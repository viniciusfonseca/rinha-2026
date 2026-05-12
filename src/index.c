#include "index.h"

#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static float rinha_distance_sq(const float query[RINHA_DIM], const uint8_t *vector) {
    float sum = 0.0f;
    for (size_t dim = 0; dim < RINHA_DIM; dim++) {
        float diff = query[dim] - rinha_dequantize_scalar(vector[dim]);
        sum += diff * diff;
    }
    return sum;
}

static void rinha_insert_top5(float best_dist[5], uint8_t best_label[5], float distance, uint8_t label) {
    for (size_t i = 0; i < 5; i++) {
        if (distance >= best_dist[i]) {
            continue;
        }

        for (size_t j = 4; j > i; j--) {
            best_dist[j] = best_dist[j - 1];
            best_label[j] = best_label[j - 1];
        }
        best_dist[i] = distance;
        best_label[i] = label;
        break;
    }
}

bool rinha_index_open(rinha_index_t *index, const char *path) {
    memset(index, 0, sizeof(*index));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }

    void *mapping = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        return false;
    }

    const rinha_index_header_t *header = (const rinha_index_header_t *) mapping;
    if (memcmp(header->magic, RINHA_INDEX_MAGIC, sizeof(header->magic)) != 0 ||
        header->version != RINHA_INDEX_VERSION ||
        header->dim != RINHA_DIM ||
        header->table_count != RINHA_TABLE_COUNT ||
        header->bucket_bits != RINHA_BUCKET_BITS) {
        munmap(mapping, (size_t) st.st_size);
        return false;
    }

    index->mapping = mapping;
    index->mapping_len = (size_t) st.st_size;
    index->point_count = header->point_count;
    index->vectors = (const uint8_t *) ((const unsigned char *) mapping + header->vectors_offset);
    index->labels = (const uint8_t *) ((const unsigned char *) mapping + header->labels_offset);
    index->bucket_offsets = (const uint32_t *) ((const unsigned char *) mapping + header->bucket_offsets_offset);
    index->indices = (const uint32_t *) ((const unsigned char *) mapping + header->indices_offset);
    index->generation = 1;
    index->candidate_cap = 65536u;
    index->visited = calloc(index->point_count, sizeof(uint32_t));
    index->candidates = malloc(index->candidate_cap * sizeof(uint32_t));
    if (index->visited == NULL || index->candidates == NULL) {
        rinha_index_close(index);
        return false;
    }

    rinha_init_lsh_params(&index->params);
    return true;
}

void rinha_index_close(rinha_index_t *index) {
    if (index->mapping != NULL) {
        munmap(index->mapping, index->mapping_len);
    }
    free(index->visited);
    free(index->candidates);
    memset(index, 0, sizeof(*index));
}

static void rinha_collect_bucket(
    rinha_index_t *index,
    size_t table,
    uint16_t key,
    size_t *candidate_count
) {
    const uint32_t *bucket_offsets = index->bucket_offsets;
    const uint32_t *indices = index->indices;
    uint32_t *visited = index->visited;
    uint32_t generation = index->generation;
    size_t point_base = table * index->point_count;
    size_t bucket_base = table * (RINHA_BUCKET_COUNT + 1u);

    uint32_t start = bucket_offsets[bucket_base + key];
    uint32_t end = bucket_offsets[bucket_base + key + 1u];

    for (uint32_t i = start; i < end; i++) {
        uint32_t point = indices[point_base + i];
        if (visited[point] == generation) {
            continue;
        }
        visited[point] = generation;
        if (*candidate_count < index->candidate_cap) {
            index->candidates[(*candidate_count)++] = point;
        }
    }
}

static void rinha_collect_candidates(rinha_index_t *index, uint64_t signature, size_t *candidate_count) {
    uint16_t keys[RINHA_TABLE_COUNT];
    for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
        keys[table] = rinha_table_key(signature, &index->params, table);
        rinha_collect_bucket(index, table, keys[table], candidate_count);
    }

    if (*candidate_count >= 64u) {
        return;
    }

    for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
        for (size_t bit = 0; bit < RINHA_BUCKET_BITS; bit++) {
            uint16_t probe = (uint16_t) (keys[table] ^ (1u << bit));
            rinha_collect_bucket(index, table, probe, candidate_count);
        }
    }

    if (*candidate_count >= 32u) {
        return;
    }

    for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
        for (size_t bit_a = 0; bit_a < RINHA_BUCKET_BITS; bit_a++) {
            for (size_t bit_b = bit_a + 1u; bit_b < RINHA_BUCKET_BITS; bit_b++) {
                uint16_t probe = (uint16_t) (keys[table] ^ (1u << bit_a) ^ (1u << bit_b));
                rinha_collect_bucket(index, table, probe, candidate_count);
            }
        }
    }
}

int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]) {
    if (index->generation == UINT32_MAX) {
        memset(index->visited, 0, index->point_count * sizeof(uint32_t));
        index->generation = 1;
    } else {
        index->generation += 1;
    }

    size_t candidate_count = 0;
    uint64_t signature = rinha_signature_for_float(query, &index->params);
    rinha_collect_candidates(index, signature, &candidate_count);

    float best_dist[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    uint8_t best_label[5] = {0, 0, 0, 0, 0};

    for (size_t i = 0; i < candidate_count; i++) {
        uint32_t point = index->candidates[i];
        float distance = rinha_distance_sq(query, index->vectors + ((size_t) point * RINHA_DIM));
        rinha_insert_top5(best_dist, best_label, distance, index->labels[point]);
    }

    int fraud_count = 0;
    for (size_t i = 0; i < 5; i++) {
        fraud_count += best_label[i] ? 1 : 0;
    }
    return fraud_count;
}
