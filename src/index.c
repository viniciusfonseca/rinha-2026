#include "index.h"

#include <math.h>
#include <float.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

typedef struct {
    float centroid_dist_sq;
    uint32_t list;
} rinha_list_bound_t;

typedef struct {
    float lower_bound_sq;
    uint32_t list;
} rinha_list_candidate_t;

static float rinha_distance_sq_scalar(
    const float query[RINHA_DIM],
    const rinha_vector_scalar_t *vector,
    const float *decode,
    float cutoff
) {
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
    float sum = 0.0f;
    diff0 *= diff0;
    sum += diff0;
    if (sum >= cutoff) return sum;
    diff1 *= diff1;
    sum += diff1;
    if (sum >= cutoff) return sum;
    diff2 *= diff2;
    sum += diff2;
    if (sum >= cutoff) return sum;
    diff3 *= diff3;
    sum += diff3;
    if (sum >= cutoff) return sum;
    diff4 *= diff4;
    sum += diff4;
    if (sum >= cutoff) return sum;
    diff5 *= diff5;
    sum += diff5;
    if (sum >= cutoff) return sum;
    diff6 *= diff6;
    sum += diff6;
    if (sum >= cutoff) return sum;
    diff7 *= diff7;
    sum += diff7;
    if (sum >= cutoff) return sum;
    diff8 *= diff8;
    sum += diff8;
    if (sum >= cutoff) return sum;
    diff9 *= diff9;
    sum += diff9;
    if (sum >= cutoff) return sum;
    diff10 *= diff10;
    sum += diff10;
    if (sum >= cutoff) return sum;
    diff11 *= diff11;
    sum += diff11;
    if (sum >= cutoff) return sum;
    diff12 *= diff12;
    sum += diff12;
    if (sum >= cutoff) return sum;
    diff13 *= diff13;
    sum += diff13;
    return sum;
}

static float rinha_distance_sq_float_scalar(const float *lhs, const float *rhs, size_t dim) {
    float sum = 0.0f;
    for (size_t i = 0; i < dim; i++) {
        float diff = lhs[i] - rhs[i];
        sum += diff * diff;
    }
    return sum;
}

#if defined(__x86_64__) || defined(__i386__)
static bool rinha_cpu_has_avx2(void) {
    static bool initialized = false;
    static bool has_avx2 = false;
    if (!initialized) {
        __builtin_cpu_init();
        has_avx2 = __builtin_cpu_supports("avx2");
        initialized = true;
    }
    return has_avx2;
}

__attribute__((target("avx2")))
static float rinha_reduce_m256(__m256 value) {
    float lanes[8];
    _mm256_storeu_ps(lanes, value);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] + lanes[6] + lanes[7];
}

__attribute__((target("avx2")))
static float rinha_reduce_m128(__m128 value) {
    float lanes[4];
    _mm_storeu_ps(lanes, value);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

__attribute__((target("avx2")))
static __m256 rinha_decode8_avx2(const rinha_vector_scalar_t *vector) {
    const __m256 scale = _mm256_set1_ps(1.0f / (float) RINHA_VECTOR_QUANT_SCALE);
    const __m256 missing_value = _mm256_set1_ps(-1.0f);
    const __m256i missing = _mm256_set1_epi32((int) RINHA_VECTOR_QUANT_MISSING);

    __m128i packed = _mm_loadu_si128((const __m128i *) vector);
    __m256i values_i32 = _mm256_cvtepu16_epi32(packed);
    __m256 values = _mm256_mul_ps(_mm256_cvtepi32_ps(values_i32), scale);
    __m256 mask = _mm256_castsi256_ps(_mm256_cmpeq_epi32(values_i32, missing));
    return _mm256_blendv_ps(values, missing_value, mask);
}

__attribute__((target("avx2")))
static __m128 rinha_decode4_avx2(const rinha_vector_scalar_t *vector) {
    const __m128 scale = _mm_set1_ps(1.0f / (float) RINHA_VECTOR_QUANT_SCALE);
    const __m128 missing_value = _mm_set1_ps(-1.0f);
    const __m128i missing = _mm_set1_epi32((int) RINHA_VECTOR_QUANT_MISSING);

    __m128i packed = _mm_loadl_epi64((const __m128i *) vector);
    __m128i values_i32 = _mm_cvtepu16_epi32(packed);
    __m128 values = _mm_mul_ps(_mm_cvtepi32_ps(values_i32), scale);
    __m128 mask = _mm_castsi128_ps(_mm_cmpeq_epi32(values_i32, missing));
    return _mm_blendv_ps(values, missing_value, mask);
}

__attribute__((target("avx2")))
static float rinha_distance_sq_avx2(
    const float query[RINHA_DIM],
    const rinha_vector_scalar_t *vector,
    const float *decode,
    float cutoff
) {
    (void) decode;
    __m256 query0 = _mm256_loadu_ps(query);
    __m256 values0 = rinha_decode8_avx2(vector);
    __m256 diff0 = _mm256_sub_ps(query0, values0);
    __m256 sum0 = _mm256_mul_ps(diff0, diff0);
    float partial0 = rinha_reduce_m256(sum0);
    if (partial0 >= cutoff) {
        return partial0;
    }

    __m128 query1 = _mm_loadu_ps(query + 8);
    __m128 values1 = rinha_decode4_avx2(vector + 8);
    __m128 diff1 = _mm_sub_ps(query1, values1);
    __m128 sum1 = _mm_mul_ps(diff1, diff1);
    float partial1 = partial0 + rinha_reduce_m128(sum1);
    if (partial1 >= cutoff) {
        return partial1;
    }

    float diff12 = query[12] - rinha_dequantize_scalar(vector[12]);
    float diff13 = query[13] - rinha_dequantize_scalar(vector[13]);
    return partial1 + diff12 * diff12 + diff13 * diff13;
}

__attribute__((target("avx2")))
static float rinha_distance_sq_float_avx2(const float *lhs, const float *rhs) {
    __m256 diff0 = _mm256_sub_ps(_mm256_loadu_ps(lhs), _mm256_loadu_ps(rhs));
    __m256 sum0 = _mm256_mul_ps(diff0, diff0);

    __m128 diff1 = _mm_sub_ps(_mm_loadu_ps(lhs + 8), _mm_loadu_ps(rhs + 8));
    __m128 sum1 = _mm_mul_ps(diff1, diff1);

    float diff12 = lhs[12] - rhs[12];
    float diff13 = lhs[13] - rhs[13];
    return rinha_reduce_m256(sum0) + rinha_reduce_m128(sum1) + diff12 * diff12 + diff13 * diff13;
}
#endif

static float rinha_distance_sq(
    const float query[RINHA_DIM],
    const rinha_vector_scalar_t *vector,
    const float *decode,
    float cutoff
) {
#if defined(__x86_64__) || defined(__i386__)
    if (rinha_cpu_has_avx2()) {
        return rinha_distance_sq_avx2(query, vector, decode, cutoff);
    }
#endif
    return rinha_distance_sq_scalar(query, vector, decode, cutoff);
}

static float rinha_distance_sq_float(const float *lhs, const float *rhs, size_t dim) {
#if defined(__x86_64__) || defined(__i386__)
    if (dim == RINHA_DIM && rinha_cpu_has_avx2()) {
        return rinha_distance_sq_float_avx2(lhs, rhs);
    }
#endif
    return rinha_distance_sq_float_scalar(lhs, rhs, dim);
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

static void rinha_insert_candidate_list(
    rinha_list_candidate_t *candidate,
    float lower_bound_sq,
    uint32_t list
) {
    candidate->lower_bound_sq = lower_bound_sq;
    candidate->list = list;
}

static float rinha_list_lower_bound_sq(float centroid_dist_sq, float radius) {
    float centroid_dist = sqrtf(centroid_dist_sq);
    if (centroid_dist <= radius) {
        return 0.0f;
    }
    float lower_bound = centroid_dist - radius;
    return lower_bound * lower_bound;
}

static int rinha_compare_candidate_lists(const void *lhs_ptr, const void *rhs_ptr) {
    const rinha_list_candidate_t *lhs = (const rinha_list_candidate_t *) lhs_ptr;
    const rinha_list_candidate_t *rhs = (const rinha_list_candidate_t *) rhs_ptr;
    if (lhs->lower_bound_sq < rhs->lower_bound_sq) {
        return -1;
    }
    if (lhs->lower_bound_sq > rhs->lower_bound_sq) {
        return 1;
    }
    return 0;
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
        float exact_distance = rinha_distance_sq(query, vector, decode, best_dist[4]);
        rinha_insert_top5(best_dist, best_label, exact_distance, index->labels[item]);
    }
}

int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]) {
    const float *decode = rinha_dequantize_lut();
    float centroid_dist_sq[RINHA_IVF_NLIST];
    rinha_list_bound_t probe_lists[RINHA_IVF_NPROBE];
    rinha_list_candidate_t candidate_lists[RINHA_IVF_NLIST - RINHA_IVF_NPROBE];
    bool selected_lists[RINHA_IVF_NLIST] = {false};
    size_t probe_count = rinha_plan_probe_lists(index, query, centroid_dist_sq, probe_lists);

    float best_dist[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    uint8_t best_label[5] = {0u, 0u, 0u, 0u, 0u};

    for (size_t i = 0; i < probe_count; i++) {
        uint32_t list = probe_lists[i].list;
        selected_lists[list] = true;
        float lower_bound_sq = rinha_list_lower_bound_sq(probe_lists[i].centroid_dist_sq, index->list_radii[list]);
        if (lower_bound_sq >= best_dist[4]) {
            continue;
        }
        rinha_scan_list(index, query, decode, list, best_dist, best_label);
    }

    size_t candidate_count = 0u;
    float prune_distance = best_dist[4] < FLT_MAX ? sqrtf(best_dist[4]) : FLT_MAX;
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
        float lower_bound_sq = rinha_list_lower_bound_sq(centroid_dist_sq[list], index->list_radii[list]);
        if (lower_bound_sq >= best_dist[4]) {
            continue;
        }
        rinha_insert_candidate_list(&candidate_lists[candidate_count++], lower_bound_sq, list);
    }

    if (candidate_count > 1u) {
        qsort(candidate_lists, candidate_count, sizeof(candidate_lists[0]), rinha_compare_candidate_lists);
    }

    for (size_t i = 0; i < candidate_count; i++) {
        if (candidate_lists[i].lower_bound_sq >= best_dist[4]) {
            break;
        }
        rinha_scan_list(index, query, decode, candidate_lists[i].list, best_dist, best_label);
    }

    int fraud_count = 0;
    for (size_t i = 0; i < 5; i++) {
        fraud_count += best_label[i] ? 1 : 0;
    }
    return fraud_count;
}
