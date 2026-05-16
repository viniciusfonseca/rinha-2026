#ifndef RINHA_COMMON_H
#define RINHA_COMMON_H

#define RINHA_DIM 14u

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
