#ifndef RINHA_INDEX_FORMAT_H
#define RINHA_INDEX_FORMAT_H

#include <stdint.h>

#define RINHA_INDEX_MAGIC "R26IVF9"
#define RINHA_INDEX_VERSION 9u

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t point_count;
    uint32_t dim;
    uint32_t nlist;
    uint32_t nprobe;
    uint64_t coarse_centroids_offset;
    uint64_t list_offsets_offset;
    uint64_t list_radii_offset;
    uint64_t labels_offset;
    uint64_t vectors_offset;
} rinha_index_header_t;

#endif
