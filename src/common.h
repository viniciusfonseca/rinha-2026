#ifndef RINHA_COMMON_H
#define RINHA_COMMON_H

#define RINHA_FEATURE_DIM 14u
#define RINHA_DIM 16u

enum {
    RINHA_SLOT_AMOUNT_RATIO = 0u,
    RINHA_SLOT_KM_FROM_CURRENT = 1u,
    RINHA_SLOT_MCC_RISK = 2u,
    RINHA_SLOT_MINUTES_SINCE_LAST = 3u,
    RINHA_SLOT_MERCHANT_UNKNOWN = 4u,
    RINHA_SLOT_TX_COUNT_24H = 5u,
    RINHA_SLOT_KM_FROM_HOME = 6u,
    RINHA_SLOT_AMOUNT = 7u,
    RINHA_SLOT_MERCHANT_AVG_AMOUNT = 8u,
    RINHA_SLOT_IS_ONLINE = 9u,
    RINHA_SLOT_CARD_PRESENT = 10u,
    RINHA_SLOT_REQUEST_HOUR = 11u,
    RINHA_SLOT_REQUEST_WEEKDAY = 12u,
    RINHA_SLOT_INSTALLMENTS = 13u,
    RINHA_SLOT_PADDING0 = 14u,
    RINHA_SLOT_PADDING1 = 15u,
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
