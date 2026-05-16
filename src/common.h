#ifndef RINHA_COMMON_H
#define RINHA_COMMON_H

#include <stdint.h>

#define RINHA_FEATURE_DIM 14u
#define RINHA_DIM 16u

static const uint8_t rinha_feature_layout[RINHA_DIM] = {
    2u, 6u, 12u, 5u, 11u, 8u, 7u, 0u,
    13u, 9u, 10u, 3u, 4u, 1u, 14u, 15u
};

static const uint8_t rinha_feature_layout_inverse[RINHA_FEATURE_DIM] = {
    7u, 13u, 0u, 11u, 12u, 3u, 1u, 6u,
    5u, 9u, 10u, 4u, 2u, 8u
};

#define RINHA_IVF_NLIST 2048u
#define RINHA_IVF_NPROBE 4u
#define RINHA_IVF_TRAIN_SAMPLES 131072u
#define RINHA_IVF_KMEANS_ITERS 16u
#define RINHA_IVF_BLOCK_SIZE 64u

static inline float rinha_clamp01(double value) {
    if (value <= 0.0) {
        return 0.0f;
    }
    if (value >= 1.0) {
        return 1.0f;
    }
    return (float) value;
}

#endif
