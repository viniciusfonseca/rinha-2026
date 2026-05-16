#include "vectorize.h"
#include "time_utils.h"
#include "vectorize_payload.h"

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

static float rinha_lookup_mcc_risk(uint32_t code) {
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

void rinha_payload_to_vector(const rinha_tx_payload_t *payload, float out[RINHA_DIM]) {
    float features[RINHA_DIM] = {0};
    features[0] = rinha_clamp01(payload->amount / RINHA_NORMALIZATION.max_amount);
    features[1] = rinha_clamp01((double) payload->installments / RINHA_NORMALIZATION.max_installments);

    if (payload->customer_avg_amount <= 0.0) {
        features[2] = 1.0f;
    } else {
        double ratio = (payload->amount / payload->customer_avg_amount) / RINHA_NORMALIZATION.amount_vs_avg_ratio;
        features[2] = rinha_clamp01(ratio);
    }

    features[3] = (float) payload->requested_at.hour / 23.0f;
    features[4] = (float) rinha_weekday_monday0(payload->requested_at.year, payload->requested_at.month, payload->requested_at.day) / 6.0f;

    if (!payload->has_last_transaction) {
        features[5] = -1.0f;
        features[6] = -1.0f;
    } else {
        int64_t request_minutes = rinha_epoch_minutes_utc(
            payload->requested_at.year,
            payload->requested_at.month,
            payload->requested_at.day,
            payload->requested_at.hour,
            payload->requested_at.minute,
            payload->requested_at.second
        );
        int64_t last_minutes = rinha_epoch_minutes_utc(
            payload->last_timestamp.year,
            payload->last_timestamp.month,
            payload->last_timestamp.day,
            payload->last_timestamp.hour,
            payload->last_timestamp.minute,
            payload->last_timestamp.second
        );
        double diff_minutes = (double) (request_minutes - last_minutes);
        if (diff_minutes < 0.0) {
            diff_minutes = 0.0;
        }

        features[5] = rinha_clamp01(diff_minutes / RINHA_NORMALIZATION.max_minutes);
        features[6] = rinha_clamp01(payload->km_from_current / RINHA_NORMALIZATION.max_km);
    }

    features[7] = rinha_clamp01(payload->km_from_home / RINHA_NORMALIZATION.max_km);
    features[8] = rinha_clamp01((double) payload->tx_count_24h / RINHA_NORMALIZATION.max_tx_count_24h);
    features[9] = payload->is_online ? 1.0f : 0.0f;
    features[10] = payload->card_present ? 1.0f : 0.0f;
    features[11] = payload->merchant_known ? 0.0f : 1.0f;
    features[12] = rinha_lookup_mcc_risk(payload->merchant_mcc_code);
    features[13] = rinha_clamp01(payload->merchant_avg_amount / RINHA_NORMALIZATION.max_merchant_avg_amount);

    for (size_t i = 0; i < RINHA_DIM; i++) {
        out[i] = features[rinha_feature_layout[i]];
    }
}

bool rinha_payload_force_deny_borderline(const rinha_tx_payload_t *payload) {
    if (payload->merchant_known ||
        !payload->is_online ||
        payload->card_present ||
        payload->tx_count_24h > 4 ||
        payload->requested_at.hour > 6 ||
        payload->customer_avg_amount <= 0.0) {
        return false;
    }

    double amount_ratio = payload->amount / payload->customer_avg_amount;
    return amount_ratio >= 9.0;
}

bool rinha_request_to_vector(const char *json, size_t len, float out[RINHA_DIM]) {
    rinha_tx_payload_t payload;
    if (!rinha_parse_tx_payload(json, len, &payload)) {
        return false;
    }

    rinha_payload_to_vector(&payload, out);
    return true;
}
