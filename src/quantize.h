#ifndef RINHA_QUANTIZE_H
#define RINHA_QUANTIZE_H

#include "common.h"

#include <stdint.h>

#define RINHA_VECTOR_QUANT_SCALE 65534u
#define RINHA_VECTOR_QUANT_MISSING 65535u

typedef uint16_t rinha_vector_scalar_t;

rinha_vector_scalar_t rinha_quantize_scalar(double value);
static inline float rinha_dequantize_scalar(rinha_vector_scalar_t value) {
    return value == RINHA_VECTOR_QUANT_MISSING ? -1.0f : (float) value * (1.0f / (float) RINHA_VECTOR_QUANT_SCALE);
}
const float *rinha_dequantize_lut(void);

#endif
