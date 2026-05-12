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
    const uint8_t *vectors;
    const uint8_t *labels;
    const uint32_t *bucket_offsets;
    const uint32_t *indices;
    rinha_lsh_params_t params;
    uint32_t *visited;
    uint32_t generation;
    uint32_t *candidates;
    size_t candidate_cap;
} rinha_index_t;

bool rinha_index_open(rinha_index_t *index, const char *path);
void rinha_index_close(rinha_index_t *index);
int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]);

#endif
