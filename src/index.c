#include "index.h"

#include <float.h>
#include <fcntl.h>
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

static float rinha_distance_sq_float(const float *lhs, const float *rhs, size_t dim) {
    float sum = 0.0f;
    for (size_t i = 0; i < dim; i++) {
        float diff = lhs[i] - rhs[i];
        sum += diff * diff;
    }
    return sum;
}

static void rinha_insert_top_ids(
    float best_dist[RINHA_IVF_NPROBE],
    uint32_t best_id[RINHA_IVF_NPROBE],
    float distance,
    uint32_t id
) {
    for (size_t i = 0; i < RINHA_IVF_NPROBE; i++) {
        if (distance >= best_dist[i]) {
            continue;
        }
        for (size_t j = RINHA_IVF_NPROBE - 1u; j > i; j--) {
            best_dist[j] = best_dist[j - 1u];
            best_id[j] = best_id[j - 1u];
        }
        best_dist[i] = distance;
        best_id[i] = id;
        break;
    }
}

static void rinha_insert_rerank_candidates(
    float best_dist[RINHA_IVF_PQ_RERANK],
    uint32_t best_index[RINHA_IVF_PQ_RERANK],
    float distance,
    uint32_t index
) {
    for (size_t i = 0; i < RINHA_IVF_PQ_RERANK; i++) {
        if (distance >= best_dist[i]) {
            continue;
        }
        for (size_t j = RINHA_IVF_PQ_RERANK - 1u; j > i; j--) {
            best_dist[j] = best_dist[j - 1u];
            best_index[j] = best_index[j - 1u];
        }
        best_dist[i] = distance;
        best_index[i] = index;
        break;
    }
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
    index->codes = (const uint8_t *) ((const unsigned char *) mapping + header->codes_offset);
    index->labels = (const uint8_t *) ((const unsigned char *) mapping + header->labels_offset);
    index->vectors = (const uint8_t *) ((const unsigned char *) mapping + header->vectors_offset);
    return true;
}

void rinha_index_close(rinha_index_t *index) {
    if (index->mapping != NULL) {
        munmap(index->mapping, index->mapping_len);
    }
    memset(index, 0, sizeof(*index));
}

static void rinha_select_lists(
    const rinha_index_t *index,
    const float query[RINHA_DIM],
    uint32_t selected_lists[RINHA_IVF_NPROBE]
) {
    float best_dist[RINHA_IVF_NPROBE];
    for (size_t i = 0; i < RINHA_IVF_NPROBE; i++) {
        best_dist[i] = FLT_MAX;
        selected_lists[i] = 0u;
    }

    for (uint32_t list = 0; list < index->nlist; list++) {
        const float *centroid = index->coarse_centroids + (size_t) list * RINHA_DIM;
        float distance = rinha_distance_sq_float(query, centroid, RINHA_DIM);
        rinha_insert_top_ids(best_dist, selected_lists, distance, list);
    }
}

static void rinha_build_distance_table(
    const rinha_index_t *index,
    const float query[RINHA_DIM],
    uint32_t list,
    float table[RINHA_PQ_M][RINHA_PQ_KSUB]
) {
    const float *centroid = index->coarse_centroids + (size_t) list * RINHA_DIM;
    for (size_t subspace = 0; subspace < RINHA_PQ_M; subspace++) {
        size_t dim_base = subspace * RINHA_PQ_SUBDIM;
        float residual[RINHA_PQ_SUBDIM];
        for (size_t dim = 0; dim < RINHA_PQ_SUBDIM; dim++) {
            residual[dim] = query[dim_base + dim] - centroid[dim_base + dim];
        }

        for (size_t code = 0; code < RINHA_PQ_KSUB; code++) {
            const float *codeword = index->pq_codebooks +
                ((subspace * RINHA_PQ_KSUB + code) * RINHA_PQ_SUBDIM);
            float distance = 0.0f;
            for (size_t dim = 0; dim < RINHA_PQ_SUBDIM; dim++) {
                float diff = residual[dim] - codeword[dim];
                distance += diff * diff;
            }
            table[subspace][code] = distance;
        }
    }
}

int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]) {
    uint32_t selected_lists[RINHA_IVF_NPROBE];
    rinha_select_lists(index, query, selected_lists);

    float rerank_best_dist[RINHA_IVF_PQ_RERANK];
    uint32_t rerank_best_index[RINHA_IVF_PQ_RERANK];
    for (size_t i = 0; i < RINHA_IVF_PQ_RERANK; i++) {
        rerank_best_dist[i] = FLT_MAX;
        rerank_best_index[i] = 0u;
    }

    for (size_t probe = 0; probe < RINHA_IVF_NPROBE; probe++) {
        uint32_t list = selected_lists[probe];
        uint32_t start = index->list_offsets[list];
        uint32_t end = index->list_offsets[list + 1u];
        if (start >= end) {
            continue;
        }

        float distance_table[RINHA_PQ_M][RINHA_PQ_KSUB];
        rinha_build_distance_table(index, query, list, distance_table);

        for (uint32_t item = start; item < end; item++) {
            const uint8_t *code = index->codes + (size_t) item * RINHA_PQ_M;
            float distance = 0.0f;
            for (size_t subspace = 0; subspace < RINHA_PQ_M; subspace++) {
                distance += distance_table[subspace][code[subspace]];
            }
            rinha_insert_rerank_candidates(rerank_best_dist, rerank_best_index, distance, item);
        }
    }

    float best_dist[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    uint8_t best_label[5] = {0u, 0u, 0u, 0u, 0u};
    for (size_t i = 0; i < RINHA_IVF_PQ_RERANK; i++) {
        if (rerank_best_dist[i] == FLT_MAX) {
            break;
        }
        uint32_t item = rerank_best_index[i];
        const uint8_t *vector = index->vectors + (size_t) item * RINHA_DIM;
        float distance = rinha_distance_sq(query, vector);
        rinha_insert_top5(best_dist, best_label, distance, index->labels[item]);
    }

    int fraud_count = 0;
    for (size_t i = 0; i < 5; i++) {
        fraud_count += best_label[i] ? 1 : 0;
    }
    return fraud_count;
}
