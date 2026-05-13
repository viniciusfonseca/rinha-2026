#include "quantize.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

const float *rinha_dequantize_lut(void) {
    static bool initialized = false;
    static float table[1u << 16];
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
