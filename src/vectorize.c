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

typedef struct {
    const char *mcc;
    float risk;
} rinha_mcc_risk_t;

static const rinha_mcc_risk_t RINHA_MCC_RISKS[] = {
    {"5411", 0.15f},
    {"5812", 0.30f},
    {"5912", 0.20f},
    {"5944", 0.45f},
    {"7801", 0.80f},
    {"7802", 0.75f},
    {"7995", 0.85f},
    {"4511", 0.35f},
    {"5311", 0.25f},
    {"5999", 0.50f},
};

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

static const char *rinha_find_key(rinha_span_t span, const char *key) {
    char pattern[64];
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written <= 0 || (size_t) written >= sizeof(pattern)) {
        return NULL;
    }

    size_t key_len = (size_t) written;
    for (const char *p = span.start; p + key_len <= span.end; p++) {
        if (memcmp(p, pattern, key_len) == 0) {
            return p + key_len;
        }
    }
    return NULL;
}

static const char *rinha_value_after_key(rinha_span_t span, const char *key) {
    const char *p = rinha_find_key(span, key);
    if (p == NULL) {
        return NULL;
    }
    p = rinha_skip_ws(p, span.end);
    if (p >= span.end || *p != ':') {
        return NULL;
    }
    p++;
    return rinha_skip_ws(p, span.end);
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

static bool rinha_extract_object(rinha_span_t parent, const char *key, rinha_span_t *out, bool *is_null) {
    const char *value = rinha_value_after_key(parent, key);
    if (value == NULL) {
        return false;
    }

    if (value + 4 <= parent.end && memcmp(value, "null", 4) == 0) {
        *is_null = true;
        out->start = NULL;
        out->end = NULL;
        return true;
    }

    *is_null = false;
    return rinha_extract_delimited_span(value, parent.end, '{', '}', out);
}

static bool rinha_extract_array(rinha_span_t parent, const char *key, rinha_span_t *out) {
    const char *value = rinha_value_after_key(parent, key);
    if (value == NULL) {
        return false;
    }
    return rinha_extract_delimited_span(value, parent.end, '[', ']', out);
}

static bool rinha_extract_string(rinha_span_t parent, const char *key, char *out, size_t out_size) {
    const char *value = rinha_value_after_key(parent, key);
    if (value == NULL || value >= parent.end || *value != '"') {
        return false;
    }

    value++;
    const char *p = value;
    while (p < parent.end && *p != '"') {
        p++;
    }
    if (p >= parent.end) {
        return false;
    }

    size_t len = (size_t) (p - value);
    if (len + 1 > out_size) {
        return false;
    }

    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

static bool rinha_extract_double(rinha_span_t parent, const char *key, double *out) {
    const char *value = rinha_value_after_key(parent, key);
    if (value == NULL) {
        return false;
    }
    char *parsed_end = NULL;
    double parsed = strtod(value, &parsed_end);
    if (parsed_end == value) {
        return false;
    }
    *out = parsed;
    return true;
}

static bool rinha_extract_int(rinha_span_t parent, const char *key, int *out) {
    const char *value = rinha_value_after_key(parent, key);
    if (value == NULL) {
        return false;
    }
    char *parsed_end = NULL;
    long parsed = strtol(value, &parsed_end, 10);
    if (parsed_end == value) {
        return false;
    }
    *out = (int) parsed;
    return true;
}

static bool rinha_extract_bool(rinha_span_t parent, const char *key, bool *out) {
    const char *value = rinha_value_after_key(parent, key);
    if (value == NULL) {
        return false;
    }
    if (value + 4 <= parent.end && memcmp(value, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (value + 5 <= parent.end && memcmp(value, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool rinha_array_contains_string(rinha_span_t parent, const char *key, const char *needle) {
    rinha_span_t array;
    if (!rinha_extract_array(parent, key, &array)) {
        return false;
    }

    const char *p = array.start + 1;
    while (p < array.end - 1) {
        p = rinha_skip_ws(p, array.end - 1);
        if (p >= array.end - 1) {
            break;
        }
        if (*p == '"') {
            const char *value = ++p;
            while (p < array.end - 1 && *p != '"') {
                p++;
            }
            size_t len = (size_t) (p - value);
            if (strlen(needle) == len && memcmp(value, needle, len) == 0) {
                return true;
            }
            if (p < array.end - 1) {
                p++;
            }
        } else {
            p++;
        }
    }
    return false;
}

static float rinha_lookup_mcc_risk(const char *mcc) {
    for (size_t i = 0; i < sizeof(RINHA_MCC_RISKS) / sizeof(RINHA_MCC_RISKS[0]); i++) {
        if (strcmp(RINHA_MCC_RISKS[i].mcc, mcc) == 0) {
            return RINHA_MCC_RISKS[i].risk;
        }
    }
    return 0.5f;
}

static bool rinha_parse_payload(const char *json, size_t len, rinha_tx_payload_t *out) {
    memset(out, 0, sizeof(*out));
    rinha_span_t root = {.start = json, .end = json + len};

    rinha_span_t transaction;
    rinha_span_t customer;
    rinha_span_t merchant;
    rinha_span_t terminal;
    rinha_span_t last_transaction;
    bool ignored = false;
    bool last_is_null = false;

    if (!rinha_extract_object(root, "transaction", &transaction, &ignored) ||
        !rinha_extract_object(root, "customer", &customer, &ignored) ||
        !rinha_extract_object(root, "merchant", &merchant, &ignored) ||
        !rinha_extract_object(root, "terminal", &terminal, &ignored) ||
        !rinha_extract_object(root, "last_transaction", &last_transaction, &last_is_null)) {
        return false;
    }

    if (!rinha_extract_double(transaction, "amount", &out->amount) ||
        !rinha_extract_int(transaction, "installments", &out->installments) ||
        !rinha_extract_string(transaction, "requested_at", out->requested_at, sizeof(out->requested_at)) ||
        !rinha_extract_double(customer, "avg_amount", &out->customer_avg_amount) ||
        !rinha_extract_int(customer, "tx_count_24h", &out->tx_count_24h) ||
        !rinha_extract_string(merchant, "id", out->merchant_id, sizeof(out->merchant_id)) ||
        !rinha_extract_string(merchant, "mcc", out->merchant_mcc, sizeof(out->merchant_mcc)) ||
        !rinha_extract_double(merchant, "avg_amount", &out->merchant_avg_amount) ||
        !rinha_extract_bool(terminal, "is_online", &out->is_online) ||
        !rinha_extract_bool(terminal, "card_present", &out->card_present) ||
        !rinha_extract_double(terminal, "km_from_home", &out->km_from_home)) {
        return false;
    }

    out->merchant_known = rinha_array_contains_string(customer, "known_merchants", out->merchant_id);

    if (!last_is_null) {
        out->has_last_transaction = true;
        if (!rinha_extract_string(last_transaction, "timestamp", out->last_timestamp, sizeof(out->last_timestamp)) ||
            !rinha_extract_double(last_transaction, "km_from_current", &out->km_from_current)) {
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
