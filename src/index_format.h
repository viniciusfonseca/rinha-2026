#ifndef RINHA_INDEX_FORMAT_H
#define RINHA_INDEX_FORMAT_H

#include <stdint.h>

#define RINHA_INDEX_MAGIC "R26IDX1"
#define RINHA_INDEX_VERSION 1u

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t point_count;
    uint32_t dim;
    uint32_t table_count;
    uint32_t bucket_bits;
    uint64_t vectors_offset;
    uint64_t labels_offset;
    uint64_t bucket_offsets_offset;
    uint64_t indices_offset;
} rinha_index_header_t;

#endif
