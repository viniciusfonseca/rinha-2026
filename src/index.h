#ifndef RINHA_INDEX_H
#define RINHA_INDEX_H

#include "common.h"
#include "index_format.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *mapping;
    size_t mapping_len;
    uint32_t point_count;
    uint32_t nlist;
    uint32_t nprobe;
    uint32_t pq_m;
    uint32_t pq_subdim;
    uint32_t pq_ksub;
    uint32_t rerank_cap;
    const float *coarse_centroids;
    const float *pq_codebooks;
    const uint32_t *list_offsets;
    const float *list_radii;
    const uint8_t *codes;
    const uint8_t *labels;
    const rinha_vector_scalar_t *vectors;
} rinha_index_t;

bool rinha_index_open(rinha_index_t *index, const char *path);
void rinha_index_close(rinha_index_t *index);
int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]);

#endif
