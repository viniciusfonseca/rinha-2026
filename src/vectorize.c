#include "vectorize_payload.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *rinha_skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char) *p)) {
        p++;
    }
    return p;
}

static bool rinha_span_equals(rinha_span_t span, const char *literal, size_t literal_len) {
    return (size_t) (span.end - span.start) == literal_len && memcmp(span.start, literal, literal_len) == 0;
}

static bool rinha_parse_json_string_span(const char **cursor, const char *end, rinha_span_t *out) {
    const char *p = *cursor;
    if (p >= end || *p != '"') {
        return false;
    }

    const char *start = ++p;
    bool escaped = false;
    while (p < end) {
        char ch = *p;
        if (escaped) {
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            out->start = start;
            out->end = p;
            *cursor = p + 1;
            return true;
        }
        p++;
    }
    return false;
}

static bool rinha_copy_plain_span(rinha_span_t span, char *out, size_t out_size, size_t *out_len) {
    size_t len = (size_t) (span.end - span.start);
    if (len + 1u > out_size) {
        return false;
    }
    if (memchr(span.start, '\\', len) != NULL) {
        return false;
    }
    memcpy(out, span.start, len);
    out[len] = '\0';
    if (out_len != NULL) {
        *out_len = len;
    }
    return true;
}

static bool rinha_skip_delimited(const char **cursor, const char *end, char open_ch, char close_ch) {
    const char *p = *cursor;
    if (p >= end || *p != open_ch) {
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    while (p < end) {
        char ch = *p;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
        } else {
            if (ch == '"') {
                in_string = true;
            } else if (ch == open_ch) {
                depth++;
            } else if (ch == close_ch) {
                depth--;
                if (depth == 0) {
                    *cursor = p + 1;
                    return true;
                }
            }
        }
        p++;
    }
    return false;
}

static bool rinha_skip_json_value(const char **cursor, const char *end) {
    const char *p = rinha_skip_ws(*cursor, end);
    if (p >= end) {
        return false;
    }

    if (*p == '"') {
        rinha_span_t ignored;
        if (!rinha_parse_json_string_span(&p, end, &ignored)) {
            return false;
        }
        *cursor = p;
        return true;
    }
    if (*p == '{') {
        if (!rinha_skip_delimited(&p, end, '{', '}')) {
            return false;
        }
        *cursor = p;
        return true;
    }
    if (*p == '[') {
        if (!rinha_skip_delimited(&p, end, '[', ']')) {
            return false;
        }
        *cursor = p;
        return true;
    }

    while (p < end && !isspace((unsigned char) *p) && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    *cursor = p;
    return true;
}

static bool rinha_parse_uint_token(const char **cursor, const char *end, unsigned *out) {
    const char *p = *cursor;
    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }

    unsigned value = 0u;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10u + (unsigned) (*p - '0');
        p++;
    }

    *out = value;
    *cursor = p;
    return true;
}

static bool rinha_parse_int_token(const char **cursor, const char *end, int *out) {
    const char *p = *cursor;
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = *p == '-';
        p++;
    }

    unsigned value = 0u;
    if (!rinha_parse_uint_token(&p, end, &value)) {
        return false;
    }

    *out = negative ? -(int) value : (int) value;
    *cursor = p;
    return true;
}

static bool rinha_parse_double_token(const char **cursor, const char *end, double *out) {
    const char *p = *cursor;
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = *p == '-';
        p++;
    }

    const char *number_start = p;
    uint64_t integer = 0u;
    bool has_integer = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_integer = true;
        integer = integer * 10u + (uint64_t) (*p - '0');
        p++;
    }

    double value = (double) integer;
    bool has_fraction = false;
    if (p < end && *p == '.') {
        p++;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            has_fraction = true;
            value += (double) (*p - '0') * scale;
            scale *= 0.1;
            p++;
        }
    }

    if (!has_integer && !has_fraction) {
        return false;
    }

    if (p < end && (*p == 'e' || *p == 'E')) {
        char buffer[64];
        size_t len = 0u;
        const char *q = *cursor;
        while (q < end && len + 1u < sizeof(buffer) && !isspace((unsigned char) *q) && *q != ',' && *q != '}' && *q != ']') {
            buffer[len++] = *q++;
        }
        buffer[len] = '\0';

        char *parsed_end = NULL;
        double fallback = strtod(buffer, &parsed_end);
        if (parsed_end == buffer || *parsed_end != '\0') {
            return false;
        }
        *out = fallback;
        *cursor = q;
        return true;
    }

    if (negative) {
        value = -value;
    }
    *out = value;
    *cursor = p;
    (void) number_start;
    return true;
}

static bool rinha_parse_bool_token(const char **cursor, const char *end, bool *out) {
    const char *p = *cursor;
    if ((size_t) (end - p) >= 4u && memcmp(p, "true", 4) == 0) {
        *out = true;
        *cursor = p + 4;
        return true;
    }
    if ((size_t) (end - p) >= 5u && memcmp(p, "false", 5) == 0) {
        *out = false;
        *cursor = p + 5;
        return true;
    }
    return false;
}

static bool rinha_parse_timestamp_span(rinha_span_t span, rinha_timestamp_t *timestamp) {
    if ((size_t) (span.end - span.start) != 20u) {
        return false;
    }

    const char *text = span.start;
    for (size_t i = 0; i < 19u; i++) {
        if (i == 4u || i == 7u) {
            if (text[i] != '-') {
                return false;
            }
            continue;
        }
        if (i == 10u) {
            if (text[i] != 'T') {
                return false;
            }
            continue;
        }
        if (i == 13u || i == 16u) {
            if (text[i] != ':') {
                return false;
            }
            continue;
        }
        if (!isdigit((unsigned char) text[i])) {
            return false;
        }
    }
    if (text[19] != 'Z') {
        return false;
    }

    timestamp->year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
    timestamp->month = (text[5] - '0') * 10 + (text[6] - '0');
    timestamp->day = (text[8] - '0') * 10 + (text[9] - '0');
    timestamp->hour = (text[11] - '0') * 10 + (text[12] - '0');
    timestamp->minute = (text[14] - '0') * 10 + (text[15] - '0');
    timestamp->second = (text[17] - '0') * 10 + (text[18] - '0');
    return true;
}

static bool rinha_parse_timestamp_value(const char **cursor, const char *end, rinha_timestamp_t *timestamp) {
    rinha_span_t span;
    if (!rinha_parse_json_string_span(cursor, end, &span)) {
        return false;
    }
    return rinha_parse_timestamp_span(span, timestamp);
}

static bool rinha_array_contains_string(rinha_span_t array, const char *needle, size_t needle_len) {
    const char *p = array.start;
    const char *end = array.end;
    p = rinha_skip_ws(p, end);
    if (p >= end || *p != '[') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == ']') {
            return false;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t value;
        if (!rinha_parse_json_string_span(&p, end, &value)) {
            return false;
        }
        if ((size_t) (value.end - value.start) == needle_len && memcmp(value.start, needle, needle_len) == 0) {
            return true;
        }
    }
}

static bool rinha_parse_transaction_object(const char **cursor, const char *end, rinha_tx_payload_t *payload) {
    const char *p = rinha_skip_ws(*cursor, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == '}') {
            *cursor = p + 1;
            return payload->amount_set && payload->installments_set && payload->requested_at_set;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t key;
        if (!rinha_parse_json_string_span(&p, end, &key)) {
            return false;
        }
        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = rinha_skip_ws(p + 1, end);
        if (p >= end) {
            return false;
        }

        if (rinha_span_equals(key, "amount", 6u)) {
            if (!rinha_parse_double_token(&p, end, &payload->amount)) {
                return false;
            }
            payload->amount_set = true;
        } else if (rinha_span_equals(key, "installments", 12u)) {
            if (!rinha_parse_int_token(&p, end, &payload->installments)) {
                return false;
            }
            payload->installments_set = true;
        } else if (rinha_span_equals(key, "requested_at", 12u)) {
            if (!rinha_parse_timestamp_value(&p, end, &payload->requested_at)) {
                return false;
            }
            payload->requested_at_set = true;
        } else if (!rinha_skip_json_value(&p, end)) {
            return false;
        }
    }
}

static bool rinha_parse_customer_object(const char **cursor, const char *end, rinha_tx_payload_t *payload) {
    const char *p = rinha_skip_ws(*cursor, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == '}') {
            *cursor = p + 1;
            return payload->customer_avg_amount_set && payload->tx_count_24h_set && payload->known_merchants_set;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t key;
        if (!rinha_parse_json_string_span(&p, end, &key)) {
            return false;
        }
        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = rinha_skip_ws(p + 1, end);
        if (p >= end) {
            return false;
        }

        if (rinha_span_equals(key, "avg_amount", 10u)) {
            if (!rinha_parse_double_token(&p, end, &payload->customer_avg_amount)) {
                return false;
            }
            payload->customer_avg_amount_set = true;
        } else if (rinha_span_equals(key, "tx_count_24h", 12u)) {
            if (!rinha_parse_int_token(&p, end, &payload->tx_count_24h)) {
                return false;
            }
            payload->tx_count_24h_set = true;
        } else if (rinha_span_equals(key, "known_merchants", 15u)) {
            const char *array_start = p;
            if (!rinha_skip_delimited(&p, end, '[', ']')) {
                return false;
            }
            payload->known_merchants.start = array_start;
            payload->known_merchants.end = p;
            payload->known_merchants_set = true;
        } else if (!rinha_skip_json_value(&p, end)) {
            return false;
        }
    }
}

static bool rinha_parse_merchant_object(const char **cursor, const char *end, rinha_tx_payload_t *payload) {
    const char *p = rinha_skip_ws(*cursor, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == '}') {
            *cursor = p + 1;
            return payload->merchant_id_set && payload->merchant_mcc_set && payload->merchant_avg_amount_set;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t key;
        if (!rinha_parse_json_string_span(&p, end, &key)) {
            return false;
        }
        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = rinha_skip_ws(p + 1, end);
        if (p >= end) {
            return false;
        }

        if (rinha_span_equals(key, "id", 2u)) {
            rinha_span_t value;
            if (!rinha_parse_json_string_span(&p, end, &value) ||
                !rinha_copy_plain_span(value, payload->merchant_id, sizeof(payload->merchant_id), &payload->merchant_id_len)) {
                return false;
            }
            payload->merchant_id_set = true;
        } else if (rinha_span_equals(key, "mcc", 3u)) {
            rinha_span_t value;
            if (!rinha_parse_json_string_span(&p, end, &value)) {
                return false;
            }
            if ((size_t) (value.end - value.start) != 4u || memchr(value.start, '\\', 4u) != NULL) {
                return false;
            }
            payload->merchant_mcc_code = ((uint32_t) (unsigned char) value.start[0] << 24) |
                                         ((uint32_t) (unsigned char) value.start[1] << 16) |
                                         ((uint32_t) (unsigned char) value.start[2] << 8) |
                                         (uint32_t) (unsigned char) value.start[3];
            payload->merchant_mcc_set = true;
        } else if (rinha_span_equals(key, "avg_amount", 10u)) {
            if (!rinha_parse_double_token(&p, end, &payload->merchant_avg_amount)) {
                return false;
            }
            payload->merchant_avg_amount_set = true;
        } else if (!rinha_skip_json_value(&p, end)) {
            return false;
        }
    }
}

static bool rinha_parse_terminal_object(const char **cursor, const char *end, rinha_tx_payload_t *payload) {
    const char *p = rinha_skip_ws(*cursor, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == '}') {
            *cursor = p + 1;
            return payload->is_online_set && payload->card_present_set && payload->km_from_home_set;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t key;
        if (!rinha_parse_json_string_span(&p, end, &key)) {
            return false;
        }
        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = rinha_skip_ws(p + 1, end);
        if (p >= end) {
            return false;
        }

        if (rinha_span_equals(key, "is_online", 9u)) {
            if (!rinha_parse_bool_token(&p, end, &payload->is_online)) {
                return false;
            }
            payload->is_online_set = true;
        } else if (rinha_span_equals(key, "card_present", 12u)) {
            if (!rinha_parse_bool_token(&p, end, &payload->card_present)) {
                return false;
            }
            payload->card_present_set = true;
        } else if (rinha_span_equals(key, "km_from_home", 12u)) {
            if (!rinha_parse_double_token(&p, end, &payload->km_from_home)) {
                return false;
            }
            payload->km_from_home_set = true;
        } else if (!rinha_skip_json_value(&p, end)) {
            return false;
        }
    }
}

static bool rinha_parse_last_transaction_object(const char **cursor, const char *end, rinha_tx_payload_t *payload) {
    const char *p = rinha_skip_ws(*cursor, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == '}') {
            *cursor = p + 1;
            return payload->last_timestamp_set && payload->km_from_current_set;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t key;
        if (!rinha_parse_json_string_span(&p, end, &key)) {
            return false;
        }
        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = rinha_skip_ws(p + 1, end);
        if (p >= end) {
            return false;
        }

        if (rinha_span_equals(key, "timestamp", 9u)) {
            if (!rinha_parse_timestamp_value(&p, end, &payload->last_timestamp)) {
                return false;
            }
            payload->last_timestamp_set = true;
        } else if (rinha_span_equals(key, "km_from_current", 15u)) {
            if (!rinha_parse_double_token(&p, end, &payload->km_from_current)) {
                return false;
            }
            payload->km_from_current_set = true;
        } else if (!rinha_skip_json_value(&p, end)) {
            return false;
        }
    }
}

bool rinha_parse_tx_payload(const char *json, size_t len, rinha_tx_payload_t *payload) {
    memset(payload, 0, sizeof(*payload));

    const char *p = rinha_skip_ws(json, json + len);
    const char *end = json + len;
    if (p >= end || *p != '{') {
        return false;
    }
    p++;

    bool first = true;
    while (true) {
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }
        if (*p == '}') {
            break;
        }
        if (!first) {
            if (*p != ',') {
                return false;
            }
            p = rinha_skip_ws(p + 1, end);
            if (p >= end) {
                return false;
            }
        }
        first = false;

        rinha_span_t key;
        if (!rinha_parse_json_string_span(&p, end, &key)) {
            return false;
        }
        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = rinha_skip_ws(p + 1, end);
        if (p >= end) {
            return false;
        }

        if (rinha_span_equals(key, "transaction", 11u)) {
            if (!rinha_parse_transaction_object(&p, end, payload)) {
                return false;
            }
            payload->transaction_set = true;
        } else if (rinha_span_equals(key, "customer", 8u)) {
            if (!rinha_parse_customer_object(&p, end, payload)) {
                return false;
            }
            payload->customer_set = true;
        } else if (rinha_span_equals(key, "merchant", 8u)) {
            if (!rinha_parse_merchant_object(&p, end, payload)) {
                return false;
            }
            payload->merchant_set = true;
        } else if (rinha_span_equals(key, "terminal", 8u)) {
            if (!rinha_parse_terminal_object(&p, end, payload)) {
                return false;
            }
            payload->terminal_set = true;
        } else if (rinha_span_equals(key, "last_transaction", 16u)) {
            if ((size_t) (end - p) >= 4u && memcmp(p, "null", 4) == 0) {
                p += 4;
                payload->has_last_transaction = false;
            } else {
                payload->has_last_transaction = true;
                if (!rinha_parse_last_transaction_object(&p, end, payload)) {
                    return false;
                }
            }
            payload->last_transaction_set = true;
        } else if (!rinha_skip_json_value(&p, end)) {
            return false;
        }
    }

    if (!payload->transaction_set ||
        !payload->customer_set ||
        !payload->merchant_set ||
        !payload->terminal_set ||
        !payload->last_transaction_set ||
        !payload->known_merchants_set ||
        !payload->merchant_id_set) {
        return false;
    }

    payload->merchant_known = rinha_array_contains_string(
        payload->known_merchants,
        payload->merchant_id,
        payload->merchant_id_len
    );
    return true;
}
