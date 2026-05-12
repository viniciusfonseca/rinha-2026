#include "vectorize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *start;
    const char *end;
} rinha_span_t;

typedef struct {
    double amount;
    int installments;
    char requested_at[32];

    double customer_avg_amount;
    int tx_count_24h;

    char merchant_id[32];
    char merchant_mcc[8];
    double merchant_avg_amount;
    bool merchant_known;

    bool is_online;
    bool card_present;
    double km_from_home;

    bool has_last_transaction;
    char last_timestamp[32];
    double km_from_current;
} rinha_tx_payload_t;

static const struct {
    double max_amount;
    double max_installments;
    double amount_vs_avg_ratio;
    double max_minutes;
    double max_km;
    double max_tx_count_24h;
    double max_merchant_avg_amount;
} RINHA_NORMALIZATION = {
    .max_amount = 10000.0,
    .max_installments = 12.0,
    .amount_vs_avg_ratio = 10.0,
    .max_minutes = 1440.0,
    .max_km = 1000.0,
    .max_tx_count_24h = 20.0,
    .max_merchant_avg_amount = 10000.0,
};

static const char *rinha_skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char) *p)) {
        p++;
    }
    return p;
}

static const char *rinha_skip_ws_commas(const char *p, const char *end) {
    while (p < end && (isspace((unsigned char) *p) || *p == ',')) {
        p++;
    }
    return p;
}

static bool rinha_key_equals(const char *key, size_t key_len, const char *literal) {
    size_t literal_len = strlen(literal);
    return key_len == literal_len && memcmp(key, literal, key_len) == 0;
}

static bool rinha_extract_delimited_span(const char *value, const char *limit, char open_ch, char close_ch, rinha_span_t *out) {
    if (value == NULL || value >= limit || *value != open_ch) {
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *p = value;
    while (p < limit) {
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
                    out->start = value;
                    out->end = p + 1;
                    return true;
                }
            }
        }
        p++;
    }
    return false;
}

typedef bool (*rinha_json_member_cb_t)(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx);

static bool rinha_scan_object(rinha_span_t object, rinha_json_member_cb_t cb, void *ctx) {
    if (object.start == NULL || object.end == NULL || object.end <= object.start + 1 || object.start[0] != '{' || object.end[-1] != '}') {
        return false;
    }

    const char *p = object.start + 1;
    const char *end = object.end - 1;

    while (p < end) {
        p = rinha_skip_ws_commas(p, end);
        if (p >= end || *p == '}') {
            return true;
        }
        if (*p != '"') {
            return false;
        }

        const char *key_start = ++p;
        while (p < end) {
            if (*p == '\\') {
                p += 2;
                continue;
            }
            if (*p == '"') {
                break;
            }
            p++;
        }
        if (p >= end || *p != '"') {
            return false;
        }
        size_t key_len = (size_t) (p - key_start);
        p++;

        p = rinha_skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p++;
        p = rinha_skip_ws(p, end);
        if (p >= end) {
            return false;
        }

        const char *value_start = p;
        const char *value_end = p;
        if (*p == '"') {
            p++;
            while (p < object.end) {
                if (*p == '\\') {
                    p += 2;
                    continue;
                }
                if (*p == '"') {
                    p++;
                    break;
                }
                p++;
            }
            if (p > object.end) {
                return false;
            }
            value_end = p;
        } else if (*p == '{') {
            rinha_span_t span;
            if (!rinha_extract_delimited_span(p, object.end, '{', '}', &span)) {
                return false;
            }
            value_end = span.end;
            p = value_end;
        } else if (*p == '[') {
            rinha_span_t span;
            if (!rinha_extract_delimited_span(p, object.end, '[', ']', &span)) {
                return false;
            }
            value_end = span.end;
            p = value_end;
        } else {
            while (p < end && *p != ',' && *p != '}') {
                p++;
            }
            value_end = p;
            while (value_end > value_start && isspace((unsigned char) value_end[-1])) {
                value_end--;
            }
        }

        if (!cb(key_start, key_len, value_start, value_end, ctx)) {
            return false;
        }

        p = rinha_skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
        }
    }

    return true;
}

static bool rinha_copy_json_string(const char *value_start, const char *value_end, char *out, size_t out_size) {
    if (value_start == NULL || value_end == NULL || value_end <= value_start + 1 || value_start[0] != '"' || value_end[-1] != '"') {
        return false;
    }

    size_t len = (size_t) (value_end - value_start - 2);
    if (len + 1 > out_size) {
        return false;
    }

    memcpy(out, value_start + 1, len);
    out[len] = '\0';
    return true;
}

static bool rinha_array_contains_string(rinha_span_t array, const char *needle) {
    if (array.start == NULL || array.end == NULL || array.end <= array.start + 1 || array.start[0] != '[' || array.end[-1] != ']') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    const char *p = array.start + 1;
    const char *end = array.end - 1;

    while (p < end) {
        p = rinha_skip_ws_commas(p, end);
        if (p >= end || *p == ']') {
            break;
        }
        if (*p != '"') {
            return false;
        }

        const char *value_start = ++p;
        while (p < end) {
            if (*p == '\\') {
                p += 2;
                continue;
            }
            if (*p == '"') {
                break;
            }
            p++;
        }
        if (p >= end || *p != '"') {
            return false;
        }

        size_t len = (size_t) (p - value_start);
        if (len == needle_len && memcmp(value_start, needle, len) == 0) {
            return true;
        }
        p++;
    }

    return false;
}

static float rinha_lookup_mcc_risk(const char *mcc) {
    if (mcc == NULL) {
        return 0.5f;
    }

    uint32_t code = ((uint32_t) (unsigned char) mcc[0] << 24) |
                    ((uint32_t) (unsigned char) mcc[1] << 16) |
                    ((uint32_t) (unsigned char) mcc[2] << 8) |
                    (uint32_t) (unsigned char) mcc[3];

    switch (code) {
        case ('5' << 24) | ('4' << 16) | ('1' << 8) | '1':
            return 0.15f;
        case ('5' << 24) | ('8' << 16) | ('1' << 8) | '2':
            return 0.30f;
        case ('5' << 24) | ('9' << 16) | ('1' << 8) | '2':
            return 0.20f;
        case ('5' << 24) | ('9' << 16) | ('4' << 8) | '4':
            return 0.45f;
        case ('7' << 24) | ('8' << 16) | ('0' << 8) | '1':
            return 0.80f;
        case ('7' << 24) | ('8' << 16) | ('0' << 8) | '2':
            return 0.75f;
        case ('7' << 24) | ('9' << 16) | ('9' << 8) | '5':
            return 0.85f;
        case ('4' << 24) | ('5' << 16) | ('1' << 8) | '1':
            return 0.35f;
        case ('5' << 24) | ('3' << 16) | ('1' << 8) | '1':
            return 0.25f;
        case ('5' << 24) | ('9' << 16) | ('9' << 8) | '9':
            return 0.50f;
        default:
            return 0.5f;
    }
}

typedef struct {
    rinha_span_t transaction;
    rinha_span_t customer;
    rinha_span_t merchant;
    rinha_span_t terminal;
    rinha_span_t last_transaction;
    bool last_is_null;
    bool has_transaction;
    bool has_customer;
    bool has_merchant;
    bool has_terminal;
    bool has_last_transaction;
} rinha_root_ctx_t;

static bool rinha_parse_root_member(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx) {
    rinha_root_ctx_t *root = (rinha_root_ctx_t *) ctx;

    if (rinha_key_equals(key, key_len, "transaction")) {
        if (!rinha_extract_delimited_span(value_start, value_end, '{', '}', &root->transaction)) {
            return false;
        }
        root->has_transaction = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "customer")) {
        if (!rinha_extract_delimited_span(value_start, value_end, '{', '}', &root->customer)) {
            return false;
        }
        root->has_customer = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "merchant")) {
        if (!rinha_extract_delimited_span(value_start, value_end, '{', '}', &root->merchant)) {
            return false;
        }
        root->has_merchant = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "terminal")) {
        if (!rinha_extract_delimited_span(value_start, value_end, '{', '}', &root->terminal)) {
            return false;
        }
        root->has_terminal = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "last_transaction")) {
        root->has_last_transaction = true;
        if (value_end - value_start == 4 && memcmp(value_start, "null", 4) == 0) {
            root->last_is_null = true;
            root->last_transaction.start = NULL;
            root->last_transaction.end = NULL;
            return true;
        }
        root->last_is_null = false;
        return rinha_extract_delimited_span(value_start, value_end, '{', '}', &root->last_transaction);
    }

    return true;
}

typedef struct {
    rinha_tx_payload_t *payload;
    bool amount_set;
    bool installments_set;
    bool requested_at_set;
} rinha_transaction_ctx_t;

static bool rinha_parse_transaction_member(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx) {
    rinha_transaction_ctx_t *state = (rinha_transaction_ctx_t *) ctx;

    if (rinha_key_equals(key, key_len, "amount")) {
        char *end = NULL;
        double parsed = strtod(value_start, &end);
        if (end == value_start) {
            return false;
        }
        state->payload->amount = parsed;
        state->amount_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "installments")) {
        char *end = NULL;
        long parsed = strtol(value_start, &end, 10);
        if (end == value_start) {
            return false;
        }
        state->payload->installments = (int) parsed;
        state->installments_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "requested_at")) {
        if (!rinha_copy_json_string(value_start, value_end, state->payload->requested_at, sizeof(state->payload->requested_at))) {
            return false;
        }
        state->requested_at_set = true;
        return true;
    }

    return true;
}

typedef struct {
    rinha_tx_payload_t *payload;
    bool avg_amount_set;
    bool tx_count_set;
    bool known_merchants_set;
} rinha_customer_ctx_t;

static bool rinha_parse_customer_member(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx) {
    rinha_customer_ctx_t *state = (rinha_customer_ctx_t *) ctx;

    if (rinha_key_equals(key, key_len, "avg_amount")) {
        char *end = NULL;
        double parsed = strtod(value_start, &end);
        if (end == value_start) {
            return false;
        }
        state->payload->customer_avg_amount = parsed;
        state->avg_amount_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "tx_count_24h")) {
        char *end = NULL;
        long parsed = strtol(value_start, &end, 10);
        if (end == value_start) {
            return false;
        }
        state->payload->tx_count_24h = (int) parsed;
        state->tx_count_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "known_merchants")) {
        state->payload->merchant_known = rinha_array_contains_string((rinha_span_t) {.start = value_start, .end = value_end}, state->payload->merchant_id);
        state->known_merchants_set = true;
        return true;
    }

    return true;
}

typedef struct {
    rinha_tx_payload_t *payload;
    bool id_set;
    bool mcc_set;
    bool avg_amount_set;
} rinha_merchant_ctx_t;

static bool rinha_parse_merchant_member(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx) {
    rinha_merchant_ctx_t *state = (rinha_merchant_ctx_t *) ctx;

    if (rinha_key_equals(key, key_len, "id")) {
        if (!rinha_copy_json_string(value_start, value_end, state->payload->merchant_id, sizeof(state->payload->merchant_id))) {
            return false;
        }
        state->id_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "mcc")) {
        if (!rinha_copy_json_string(value_start, value_end, state->payload->merchant_mcc, sizeof(state->payload->merchant_mcc))) {
            return false;
        }
        state->mcc_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "avg_amount")) {
        char *end = NULL;
        double parsed = strtod(value_start, &end);
        if (end == value_start) {
            return false;
        }
        state->payload->merchant_avg_amount = parsed;
        state->avg_amount_set = true;
        return true;
    }

    return true;
}

typedef struct {
    rinha_tx_payload_t *payload;
    bool is_online_set;
    bool card_present_set;
    bool km_from_home_set;
} rinha_terminal_ctx_t;

static bool rinha_parse_terminal_member(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx) {
    rinha_terminal_ctx_t *state = (rinha_terminal_ctx_t *) ctx;

    if (rinha_key_equals(key, key_len, "is_online")) {
        if (value_end - value_start == 4 && memcmp(value_start, "true", 4) == 0) {
            state->payload->is_online = true;
        } else if (value_end - value_start == 5 && memcmp(value_start, "false", 5) == 0) {
            state->payload->is_online = false;
        } else {
            return false;
        }
        state->is_online_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "card_present")) {
        if (value_end - value_start == 4 && memcmp(value_start, "true", 4) == 0) {
            state->payload->card_present = true;
        } else if (value_end - value_start == 5 && memcmp(value_start, "false", 5) == 0) {
            state->payload->card_present = false;
        } else {
            return false;
        }
        state->card_present_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "km_from_home")) {
        char *end = NULL;
        double parsed = strtod(value_start, &end);
        if (end == value_start) {
            return false;
        }
        state->payload->km_from_home = parsed;
        state->km_from_home_set = true;
        return true;
    }

    return true;
}

typedef struct {
    rinha_tx_payload_t *payload;
    bool timestamp_set;
    bool km_from_current_set;
} rinha_last_ctx_t;

static bool rinha_parse_last_member(const char *key, size_t key_len, const char *value_start, const char *value_end, void *ctx) {
    rinha_last_ctx_t *state = (rinha_last_ctx_t *) ctx;

    if (rinha_key_equals(key, key_len, "timestamp")) {
        if (!rinha_copy_json_string(value_start, value_end, state->payload->last_timestamp, sizeof(state->payload->last_timestamp))) {
            return false;
        }
        state->timestamp_set = true;
        return true;
    }
    if (rinha_key_equals(key, key_len, "km_from_current")) {
        char *end = NULL;
        double parsed = strtod(value_start, &end);
        if (end == value_start) {
            return false;
        }
        state->payload->km_from_current = parsed;
        state->km_from_current_set = true;
        return true;
    }

    return true;
}

static bool rinha_parse_payload(const char *json, size_t len, rinha_tx_payload_t *out) {
    memset(out, 0, sizeof(*out));

    rinha_span_t root_span;
    if (!rinha_extract_delimited_span(json, json + len, '{', '}', &root_span)) {
        return false;
    }

    rinha_root_ctx_t root = {0};
    if (!rinha_scan_object(root_span, rinha_parse_root_member, &root)) {
        return false;
    }
    if (!root.has_transaction || !root.has_customer || !root.has_merchant || !root.has_terminal || !root.has_last_transaction) {
        return false;
    }

    rinha_merchant_ctx_t merchant_ctx = {
        .payload = out,
    };
    if (!rinha_scan_object(root.merchant, rinha_parse_merchant_member, &merchant_ctx) ||
        !merchant_ctx.id_set ||
        !merchant_ctx.mcc_set ||
        !merchant_ctx.avg_amount_set) {
        return false;
    }

    rinha_transaction_ctx_t transaction_ctx = {
        .payload = out,
    };
    if (!rinha_scan_object(root.transaction, rinha_parse_transaction_member, &transaction_ctx) ||
        !transaction_ctx.amount_set ||
        !transaction_ctx.installments_set ||
        !transaction_ctx.requested_at_set) {
        return false;
    }

    rinha_customer_ctx_t customer_ctx = {
        .payload = out,
    };
    if (!rinha_scan_object(root.customer, rinha_parse_customer_member, &customer_ctx) ||
        !customer_ctx.avg_amount_set ||
        !customer_ctx.tx_count_set ||
        !customer_ctx.known_merchants_set) {
        return false;
    }

    rinha_terminal_ctx_t terminal_ctx = {
        .payload = out,
    };
    if (!rinha_scan_object(root.terminal, rinha_parse_terminal_member, &terminal_ctx) ||
        !terminal_ctx.is_online_set ||
        !terminal_ctx.card_present_set ||
        !terminal_ctx.km_from_home_set) {
        return false;
    }

    if (!root.last_is_null) {
        out->has_last_transaction = true;
        rinha_last_ctx_t last_ctx = {
            .payload = out,
        };
        if (!rinha_scan_object(root.last_transaction, rinha_parse_last_member, &last_ctx) ||
            !last_ctx.timestamp_set ||
            !last_ctx.km_from_current_set) {
            return false;
        }
    }

    return true;
}

bool rinha_request_to_vector(const char *json, size_t len, float out[RINHA_DIM]) {
    rinha_tx_payload_t payload;
    if (!rinha_parse_payload(json, len, &payload)) {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (rinha_parse_utc_timestamp(payload.requested_at, &year, &month, &day, &hour, &minute, &second) != 0) {
        return false;
    }

    out[0] = rinha_clamp01(payload.amount / RINHA_NORMALIZATION.max_amount);
    out[1] = rinha_clamp01((double) payload.installments / RINHA_NORMALIZATION.max_installments);

    if (payload.customer_avg_amount <= 0.0) {
        out[2] = 1.0f;
    } else {
        double ratio = (payload.amount / payload.customer_avg_amount) / RINHA_NORMALIZATION.amount_vs_avg_ratio;
        out[2] = rinha_clamp01(ratio);
    }

    out[3] = (float) hour / 23.0f;
    out[4] = (float) rinha_weekday_monday0(year, month, day) / 6.0f;

    if (!payload.has_last_transaction) {
        out[5] = -1.0f;
        out[6] = -1.0f;
    } else {
        int last_year = 0;
        int last_month = 0;
        int last_day = 0;
        int last_hour = 0;
        int last_minute = 0;
        int last_second = 0;
        if (rinha_parse_utc_timestamp(payload.last_timestamp, &last_year, &last_month, &last_day, &last_hour, &last_minute, &last_second) != 0) {
            return false;
        }

        int64_t request_minutes = rinha_epoch_minutes_utc(year, month, day, hour, minute, second);
        int64_t last_minutes = rinha_epoch_minutes_utc(last_year, last_month, last_day, last_hour, last_minute, last_second);
        double diff_minutes = (double) (request_minutes - last_minutes);
        if (diff_minutes < 0.0) {
            diff_minutes = 0.0;
        }

        out[5] = rinha_clamp01(diff_minutes / RINHA_NORMALIZATION.max_minutes);
        out[6] = rinha_clamp01(payload.km_from_current / RINHA_NORMALIZATION.max_km);
    }

    out[7] = rinha_clamp01(payload.km_from_home / RINHA_NORMALIZATION.max_km);
    out[8] = rinha_clamp01((double) payload.tx_count_24h / RINHA_NORMALIZATION.max_tx_count_24h);
    out[9] = payload.is_online ? 1.0f : 0.0f;
    out[10] = payload.card_present ? 1.0f : 0.0f;
    out[11] = payload.merchant_known ? 0.0f : 1.0f;
    out[12] = rinha_lookup_mcc_risk(payload.merchant_mcc);
    out[13] = rinha_clamp01(payload.merchant_avg_amount / RINHA_NORMALIZATION.max_merchant_avg_amount);
    return true;
}
