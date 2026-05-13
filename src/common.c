#include "common.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

const float *rinha_dequantize_lut(void) {
    static bool initialized = false;
    static float table[1u << 8];
    if (!initialized) {
        for (size_t i = 0; i < RINHA_VECTOR_QUANT_MISSING; i++) {
            table[i] = (float) i * (1.0f / (float) RINHA_VECTOR_QUANT_SCALE);
        }
        table[RINHA_VECTOR_QUANT_MISSING] = -1.0f;
        initialized = true;
    }
    return table;
}

rinha_vector_scalar_t rinha_quantize_scalar(double value) {
    if (value < 0.0) {
        return RINHA_VECTOR_QUANT_MISSING;
    }
    float clamped = rinha_clamp01(value);
    unsigned scaled = (unsigned) lroundf(clamped * (float) RINHA_VECTOR_QUANT_SCALE);
    if (scaled > RINHA_VECTOR_QUANT_SCALE) {
        scaled = RINHA_VECTOR_QUANT_SCALE;
    }
    return (rinha_vector_scalar_t) scaled;
}

uint64_t rinha_signature_for_float(const float vector[RINHA_DIM], const rinha_lsh_params_t *params) {
    uint64_t signature = 0;
    for (size_t bit = 0; bit < RINHA_SIGNATURE_BITS; bit++) {
        float dot = 0.0f;
        for (size_t dim = 0; dim < RINHA_DIM; dim++) {
            dot += vector[dim] * params->hyperplanes[bit][dim];
        }
        if (dot >= 0.0f) {
            signature |= (1ULL << bit);
        }
    }
    return signature;
}

uint64_t rinha_signature_for_quantized(const rinha_vector_scalar_t vector[RINHA_DIM], const rinha_lsh_params_t *params) {
    const float *decode = rinha_dequantize_lut();
    float decoded[RINHA_DIM];
    for (size_t dim = 0; dim < RINHA_DIM; dim++) {
        decoded[dim] = decode[vector[dim]];
    }
    return rinha_signature_for_float(decoded, params);
}

uint16_t rinha_table_key(uint64_t signature, const rinha_lsh_params_t *params, size_t table_index) {
    uint16_t key = 0;
    for (size_t bit = 0; bit < RINHA_BUCKET_BITS; bit++) {
        uint8_t source_bit = params->bit_positions[table_index][bit];
        key |= (uint16_t) (((signature >> source_bit) & 1ULL) << bit);
    }
    return key;
}

int rinha_parse_utc_timestamp(
    const char *timestamp,
    int *year,
    int *month,
    int *day,
    int *hour,
    int *minute,
    int *second
) {
    if (timestamp == NULL || strlen(timestamp) < 20) {
        return -1;
    }

    for (size_t i = 0; i < 19; i++) {
        if (i == 4 || i == 7) {
            if (timestamp[i] != '-') {
                return -1;
            }
            continue;
        }
        if (i == 10) {
            if (timestamp[i] != 'T') {
                return -1;
            }
            continue;
        }
        if (i == 13 || i == 16) {
            if (timestamp[i] != ':') {
                return -1;
            }
            continue;
        }
        if (!isdigit((unsigned char) timestamp[i])) {
            return -1;
        }
    }

    if (timestamp[19] != 'Z') {
        return -1;
    }

    *year = (timestamp[0] - '0') * 1000 + (timestamp[1] - '0') * 100 + (timestamp[2] - '0') * 10 + (timestamp[3] - '0');
    *month = (timestamp[5] - '0') * 10 + (timestamp[6] - '0');
    *day = (timestamp[8] - '0') * 10 + (timestamp[9] - '0');
    *hour = (timestamp[11] - '0') * 10 + (timestamp[12] - '0');
    *minute = (timestamp[14] - '0') * 10 + (timestamp[15] - '0');
    *second = (timestamp[17] - '0') * 10 + (timestamp[18] - '0');
    return 0;
}

int rinha_weekday_monday0(int year, int month, int day) {
    static const int month_offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year -= 1;
    }
    int weekday_sunday0 = (year + year / 4 - year / 100 + year / 400 + month_offsets[month - 1] + day) % 7;
    if (weekday_sunday0 == 0) {
        return 6;
    }
    return weekday_sunday0 - 1;
}

static int64_t rinha_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned) (year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? (unsigned) -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t) era * 146097 + (int64_t) doe - 719468;
}

int64_t rinha_epoch_minutes_utc(int year, int month, int day, int hour, int minute, int second) {
    int64_t days = rinha_days_from_civil(year, (unsigned) month, (unsigned) day);
    int64_t total_seconds = days * 86400 + (int64_t) hour * 3600 + (int64_t) minute * 60 + second;
    return total_seconds / 60;
}
