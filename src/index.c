#include "index.h"

#include <math.h>
#include <float.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    float centroid_dist_sq;
    uint32_t list;
} rinha_list_bound_t;

static float rinha_distance_sq(const float query[RINHA_DIM], const rinha_vector_scalar_t *vector, const float *decode) {
    float diff0 = query[0] - decode[vector[0]];
    float diff1 = query[1] - decode[vector[1]];
    float diff2 = query[2] - decode[vector[2]];
    float diff3 = query[3] - decode[vector[3]];
    float diff4 = query[4] - decode[vector[4]];
    float diff5 = query[5] - decode[vector[5]];
    float diff6 = query[6] - decode[vector[6]];
    float diff7 = query[7] - decode[vector[7]];
    float diff8 = query[8] - decode[vector[8]];
    float diff9 = query[9] - decode[vector[9]];
    float diff10 = query[10] - decode[vector[10]];
    float diff11 = query[11] - decode[vector[11]];
    float diff12 = query[12] - decode[vector[12]];
    float diff13 = query[13] - decode[vector[13]];
    return diff0 * diff0 +
        diff1 * diff1 +
        diff2 * diff2 +
        diff3 * diff3 +
        diff4 * diff4 +
        diff5 * diff5 +
        diff6 * diff6 +
        diff7 * diff7 +
        diff8 * diff8 +
        diff9 * diff9 +
        diff10 * diff10 +
        diff11 * diff11 +
        diff12 * diff12 +
        diff13 * diff13;
}

static float rinha_distance_sq_float(const float *lhs, const float *rhs, size_t dim) {
    float sum = 0.0f;
    for (size_t i = 0; i < dim; i++) {
        float diff = lhs[i] - rhs[i];
        sum += diff * diff;
    }
    return sum;
}

static void rinha_insert_top5(
    float best_dist[5],
    uint8_t best_label[5],
    float distance,
    uint8_t label
) {
    for (size_t i = 0; i < 5; i++) {
        if (distance >= best_dist[i]) {
            continue;
        }
        for (size_t j = 4u; j > i; j--) {
            best_dist[j] = best_dist[j - 1u];
            best_label[j] = best_label[j - 1u];
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
        header->nlist != RINHA_IVF_NLIST ||
        header->nprobe != RINHA_IVF_NPROBE) {
        munmap(mapping, (size_t) st.st_size);
        return false;
    }

    index->mapping = mapping;
    index->mapping_len = (size_t) st.st_size;
    index->point_count = header->point_count;
    index->nlist = header->nlist;
    index->nprobe = header->nprobe;
    index->coarse_centroids = (const float *) ((const unsigned char *) mapping + header->coarse_centroids_offset);
    index->list_offsets = (const uint32_t *) ((const unsigned char *) mapping + header->list_offsets_offset);
    index->list_radii = (const float *) ((const unsigned char *) mapping + header->list_radii_offset);
    index->labels = (const uint8_t *) ((const unsigned char *) mapping + header->labels_offset);
    index->vectors = (const rinha_vector_scalar_t *) ((const unsigned char *) mapping + header->vectors_offset);
    return true;
}

void rinha_index_close(rinha_index_t *index) {
    if (index->mapping != NULL) {
        munmap(index->mapping, index->mapping_len);
    }
    memset(index, 0, sizeof(*index));
}

static void rinha_insert_probe_list(rinha_list_bound_t bounds[RINHA_IVF_NPROBE], size_t count, float centroid_dist_sq, uint32_t list) {
    size_t slot = count;
    while (slot > 0u && centroid_dist_sq < bounds[slot - 1u].centroid_dist_sq) {
        bounds[slot] = bounds[slot - 1u];
        slot--;
    }
    bounds[slot].centroid_dist_sq = centroid_dist_sq;
    bounds[slot].list = list;
}

static size_t rinha_plan_probe_lists(
    const rinha_index_t *index,
    const float query[RINHA_DIM],
    float centroid_dist_sq[RINHA_IVF_NLIST],
    rinha_list_bound_t probe_lists[RINHA_IVF_NPROBE]
) {
    size_t probe_count = 0;
    for (uint32_t list = 0; list < index->nlist; list++) {
        const float *centroid = index->coarse_centroids + (size_t) list * RINHA_DIM;
        float dist_sq = rinha_distance_sq_float(query, centroid, RINHA_DIM);
        centroid_dist_sq[list] = dist_sq;

        if (probe_count < index->nprobe) {
            rinha_insert_probe_list(probe_lists, probe_count, dist_sq, list);
            probe_count++;
            continue;
        }
        if (dist_sq >= probe_lists[probe_count - 1u].centroid_dist_sq) {
            continue;
        }
        rinha_insert_probe_list(probe_lists, probe_count - 1u, dist_sq, list);
    }
    return probe_count;
}

static void rinha_scan_list(
    const rinha_index_t *index,
    const float query[RINHA_DIM],
    const float *decode,
    uint32_t list,
    float best_dist[5],
    uint8_t best_label[5]
) {
    uint32_t start = index->list_offsets[list];
    uint32_t end = index->list_offsets[list + 1u];
    if (start >= end) {
        return;
    }

    for (uint32_t item = start; item < end; item++) {
        const rinha_vector_scalar_t *vector = index->vectors + (size_t) item * RINHA_DIM;
        float exact_distance = rinha_distance_sq(query, vector, decode);
        rinha_insert_top5(best_dist, best_label, exact_distance, index->labels[item]);
    }
}

int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]) {
    const float *decode = rinha_dequantize_lut();
    float centroid_dist_sq[RINHA_IVF_NLIST];
    rinha_list_bound_t probe_lists[RINHA_IVF_NPROBE];
    bool selected_lists[RINHA_IVF_NLIST] = {false};
    size_t probe_count = rinha_plan_probe_lists(index, query, centroid_dist_sq, probe_lists);

    float best_dist[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    uint8_t best_label[5] = {0u, 0u, 0u, 0u, 0u};
    float prune_distance = FLT_MAX;

    for (size_t i = 0; i < probe_count; i++) {
        uint32_t list = probe_lists[i].list;
        selected_lists[list] = true;

        if (prune_distance < FLT_MAX) {
            float max_centroid_dist = prune_distance + index->list_radii[list];
            if (probe_lists[i].centroid_dist_sq >= max_centroid_dist * max_centroid_dist) {
                continue;
            }
        }

        float previous_worst = best_dist[4];
        rinha_scan_list(index, query, decode, list, best_dist, best_label);
        if (best_dist[4] < previous_worst) {
            prune_distance = sqrtf(best_dist[4]);
        }
    }

    for (uint32_t list = 0; list < index->nlist; list++) {
        if (selected_lists[list]) {
            continue;
        }

        if (prune_distance < FLT_MAX) {
            float max_centroid_dist = prune_distance + index->list_radii[list];
            if (centroid_dist_sq[list] >= max_centroid_dist * max_centroid_dist) {
                continue;
            }
        }

        float previous_worst = best_dist[4];
        rinha_scan_list(index, query, decode, list, best_dist, best_label);
        if (best_dist[4] < previous_worst) {
            prune_distance = sqrtf(best_dist[4]);
        }
    }

    int fraud_count = 0;
    for (size_t i = 0; i < 5; i++) {
        fraud_count += best_label[i] ? 1 : 0;
    }
    return fraud_count;
}
