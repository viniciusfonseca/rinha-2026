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

static float rinha_distance_sq(const float query[RINHA_DIM], const rinha_vector_scalar_t *vector) {
    float sum = 0.0f;
    for (size_t dim = 0; dim < RINHA_DIM; dim++) {
        float diff = query[dim] - rinha_dequantize_scalar(vector[dim]);
        sum += diff * diff;
    }
    return sum;
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
        header->nprobe != RINHA_IVF_NPROBE ||
        header->pq_m != RINHA_PQ_M ||
        header->pq_subdim != RINHA_PQ_SUBDIM ||
        header->pq_ksub != RINHA_PQ_KSUB ||
        header->rerank_cap != RINHA_IVF_PQ_RERANK) {
        munmap(mapping, (size_t) st.st_size);
        return false;
    }

    index->mapping = mapping;
    index->mapping_len = (size_t) st.st_size;
    index->point_count = header->point_count;
    index->nlist = header->nlist;
    index->nprobe = header->nprobe;
    index->pq_m = header->pq_m;
    index->pq_subdim = header->pq_subdim;
    index->pq_ksub = header->pq_ksub;
    index->rerank_cap = header->rerank_cap;
    index->coarse_centroids = (const float *) ((const unsigned char *) mapping + header->coarse_centroids_offset);
    index->pq_codebooks = (const float *) ((const unsigned char *) mapping + header->pq_codebooks_offset);
    index->list_offsets = (const uint32_t *) ((const unsigned char *) mapping + header->list_offsets_offset);
    index->list_radii = (const float *) ((const unsigned char *) mapping + header->list_radii_offset);
    index->codes = (const uint8_t *) ((const unsigned char *) mapping + header->codes_offset);
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
        float exact_distance = rinha_distance_sq(query, vector);
        rinha_insert_top5(best_dist, best_label, exact_distance, index->labels[item]);
    }
}

int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]) {
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
        rinha_scan_list(index, query, list, best_dist, best_label);
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
        rinha_scan_list(index, query, list, best_dist, best_label);
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
