#ifndef RINHA_COMMON_H
#define RINHA_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINHA_DIM 14u
#define RINHA_SIGNATURE_BITS 64u
#define RINHA_TABLE_COUNT 4u
/*
 * Denser buckets keep the candidate set large enough that queries avoid
 * degenerating into expensive global scans under load.
 */
#define RINHA_BUCKET_BITS 12u
#define RINHA_BUCKET_COUNT (1u << RINHA_BUCKET_BITS)
#define RINHA_ONE_BIT_PROBES (RINHA_BUCKET_BITS + 1u)

#define RINHA_IVF_NLIST 512u
#define RINHA_IVF_NPROBE 4u
#define RINHA_IVF_TRAIN_SAMPLES 65536u
#define RINHA_IVF_KMEANS_ITERS 16u
#define RINHA_VECTOR_QUANT_SCALE 65534u
#define RINHA_VECTOR_QUANT_MISSING 65535u

typedef uint16_t rinha_vector_scalar_t;

typedef struct {
    uint8_t bit_positions[RINHA_TABLE_COUNT][RINHA_BUCKET_BITS];
    float hyperplanes[RINHA_SIGNATURE_BITS][RINHA_DIM];
} rinha_lsh_params_t;

static inline float rinha_clamp01(double value) {
    if (value <= 0.0) {
        return 0.0f;
    }
    if (value >= 1.0) {
        return 1.0f;
    }
    return (float) value;
}
rinha_vector_scalar_t rinha_quantize_scalar(double value);
static inline float rinha_dequantize_scalar(rinha_vector_scalar_t value) {
    return value == RINHA_VECTOR_QUANT_MISSING ? -1.0f : (float) value * (1.0f / (float) RINHA_VECTOR_QUANT_SCALE);
}
const float *rinha_dequantize_lut(void);

uint64_t rinha_signature_for_float(const float vector[RINHA_DIM], const rinha_lsh_params_t *params);
uint64_t rinha_signature_for_quantized(const rinha_vector_scalar_t vector[RINHA_DIM], const rinha_lsh_params_t *params);
uint16_t rinha_table_key(uint64_t signature, const rinha_lsh_params_t *params, size_t table_index);

int rinha_parse_utc_timestamp(
    const char *timestamp,
    int *year,
    int *month,
    int *day,
    int *hour,
    int *minute,
    int *second
);
int rinha_weekday_monday0(int year, int month, int day);
int64_t rinha_epoch_minutes_utc(int year, int month, int day, int hour, int minute, int second);

#endif
