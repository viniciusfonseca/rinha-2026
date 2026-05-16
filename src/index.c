#include "index.h"

#include <float.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
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

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t block_begin;
    uint32_t candidate_block_begin;
    uint32_t candidate_block_end;
} rinha_scan_window_t;

typedef struct {
    bool initialized;
    bool enabled;
    uint64_t report_every;
    uint64_t calls;
    uint64_t total_ns;
    uint64_t plan_ns;
    uint64_t probe_scan_ns;
    uint64_t candidate_build_ns;
    uint64_t candidate_sort_ns;
    uint64_t candidate_scan_ns;
    uint64_t probe_lists_total;
    uint64_t probe_lists_scanned;
    uint64_t probe_lists_pruned_bound;
    uint64_t candidate_lists_considered;
    uint64_t candidate_lists_selected;
    uint64_t candidate_lists_scanned;
    uint64_t candidate_lists_pruned_radius;
    uint64_t candidate_lists_pruned_bound;
    uint64_t candidate_lists_skipped_after_sort;
    uint64_t vectors_scanned_probe;
    uint64_t vectors_scanned_candidate;
} rinha_index_profile_t;

static rinha_index_profile_t rinha_index_profile = {0};

static bool rinha_env_truthy(const char *value) {
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "False") == 0 ||
        strcmp(value, "FALSE") == 0 ||
        strcmp(value, "no") == 0 ||
        strcmp(value, "No") == 0 ||
        strcmp(value, "NO") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "Off") == 0 ||
        strcmp(value, "OFF") == 0) {
        return false;
    }
    return true;
}

static uint64_t rinha_env_u64(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0ull) {
        return fallback;
    }
    return (uint64_t) parsed;
}

static uint64_t rinha_now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static void rinha_index_profile_report(const char *reason) {
    if (!rinha_index_profile.enabled || rinha_index_profile.calls == 0u) {
        return;
    }

    double calls = (double) rinha_index_profile.calls;
    double avg_total_us = (double) rinha_index_profile.total_ns / calls / 1000.0;
    double avg_plan_us = (double) rinha_index_profile.plan_ns / calls / 1000.0;
    double avg_probe_scan_us = (double) rinha_index_profile.probe_scan_ns / calls / 1000.0;
    double avg_candidate_build_us = (double) rinha_index_profile.candidate_build_ns / calls / 1000.0;
    double avg_candidate_sort_us = (double) rinha_index_profile.candidate_sort_ns / calls / 1000.0;
    double avg_candidate_scan_us = (double) rinha_index_profile.candidate_scan_ns / calls / 1000.0;
    double avg_vectors_probe = (double) rinha_index_profile.vectors_scanned_probe / calls;
    double avg_vectors_candidate = (double) rinha_index_profile.vectors_scanned_candidate / calls;

    fprintf(
        stderr,
        "[index-prof] reason=%s calls=%" PRIu64
        " avg_total_us=%.2f avg_plan_us=%.2f avg_probe_scan_us=%.2f"
        " avg_candidate_build_us=%.2f avg_candidate_sort_us=%.2f avg_candidate_scan_us=%.2f"
        " avg_probe_lists=%.2f avg_probe_scanned=%.2f avg_probe_pruned_bound=%.2f"
        " avg_candidate_considered=%.2f avg_candidate_selected=%.2f avg_candidate_scanned=%.2f"
        " avg_candidate_pruned_radius=%.2f avg_candidate_pruned_bound=%.2f avg_candidate_skipped_after_sort=%.2f"
        " avg_vectors_probe=%.2f avg_vectors_candidate=%.2f avg_vectors_total=%.2f\n",
        reason,
        rinha_index_profile.calls,
        avg_total_us,
        avg_plan_us,
        avg_probe_scan_us,
        avg_candidate_build_us,
        avg_candidate_sort_us,
        avg_candidate_scan_us,
        (double) rinha_index_profile.probe_lists_total / calls,
        (double) rinha_index_profile.probe_lists_scanned / calls,
        (double) rinha_index_profile.probe_lists_pruned_bound / calls,
        (double) rinha_index_profile.candidate_lists_considered / calls,
        (double) rinha_index_profile.candidate_lists_selected / calls,
        (double) rinha_index_profile.candidate_lists_scanned / calls,
        (double) rinha_index_profile.candidate_lists_pruned_radius / calls,
        (double) rinha_index_profile.candidate_lists_pruned_bound / calls,
        (double) rinha_index_profile.candidate_lists_skipped_after_sort / calls,
        avg_vectors_probe,
        avg_vectors_candidate,
        avg_vectors_probe + avg_vectors_candidate
    );
}

static void rinha_index_profile_flush(void) {
    rinha_index_profile_report("exit");
}

static void rinha_index_profile_init(void) {
    if (rinha_index_profile.initialized) {
        return;
    }

    rinha_index_profile.initialized = true;
    rinha_index_profile.enabled = rinha_env_truthy(getenv("RINHA_INDEX_PROFILE"));
    rinha_index_profile.report_every = rinha_env_u64("RINHA_INDEX_PROFILE_EVERY", 1000u);

    if (rinha_index_profile.enabled) {
        atexit(rinha_index_profile_flush);
    }
}

static inline float rinha_distance_sq_scalar_preloaded(
    float query0,
    float query1,
    float query2,
    float query3,
    float query4,
    float query5,
    float query6,
    float query7,
    float query8,
    float query9,
    float query10,
    float query11,
    float query12,
    float query13,
    const rinha_vector_scalar_t *vector,
    const float *decode,
    float cutoff
) {
    float diff2 = query2 - decode[vector[2]];
    float diff6 = query6 - decode[vector[6]];
    float diff12 = query12 - decode[vector[12]];
    float diff5 = query5 - decode[vector[5]];
    float diff11 = query11 - decode[vector[11]];
    float diff8 = query8 - decode[vector[8]];
    float diff7 = query7 - decode[vector[7]];
    float diff0 = query0 - decode[vector[0]];
    float diff13 = query13 - decode[vector[13]];
    float diff9 = query9 - decode[vector[9]];
    float diff10 = query10 - decode[vector[10]];
    float diff3 = query3 - decode[vector[3]];
    float diff4 = query4 - decode[vector[4]];
    float diff1 = query1 - decode[vector[1]];
    float sum = 0.0f;
    diff2 *= diff2;
    sum += diff2;
    if (sum >= cutoff) return sum;
    diff6 *= diff6;
    sum += diff6;
    if (sum >= cutoff) return sum;
    diff12 *= diff12;
    sum += diff12;
    if (sum >= cutoff) return sum;
    diff5 *= diff5;
    sum += diff5;
    if (sum >= cutoff) return sum;
    diff11 *= diff11;
    sum += diff11;
    if (sum >= cutoff) return sum;
    diff8 *= diff8;
    sum += diff8;
    if (sum >= cutoff) return sum;
    diff7 *= diff7;
    sum += diff7;
    if (sum >= cutoff) return sum;
    diff0 *= diff0;
    sum += diff0;
    if (sum >= cutoff) return sum;
    diff13 *= diff13;
    sum += diff13;
    if (sum >= cutoff) return sum;
    diff9 *= diff9;
    sum += diff9;
    if (sum >= cutoff) return sum;
    diff10 *= diff10;
    sum += diff10;
    if (sum >= cutoff) return sum;
    diff3 *= diff3;
    sum += diff3;
    if (sum >= cutoff) return sum;
    diff4 *= diff4;
    sum += diff4;
    if (sum >= cutoff) return sum;
    diff1 *= diff1;
    sum += diff1;
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
static float rinha_distance_sq_avx2_preloaded(
    __m256 query0,
    __m128 query1,
    float query12,
    float query13,
    const rinha_vector_scalar_t *vector,
    float cutoff
) {
    __m256 values0 = rinha_decode8_avx2(vector);
    __m256 diff0 = _mm256_sub_ps(query0, values0);
    __m256 sum0 = _mm256_mul_ps(diff0, diff0);
    float partial0 = rinha_reduce_m256(sum0);
    if (partial0 >= cutoff) {
        return partial0;
    }

    __m128 values1 = rinha_decode4_avx2(vector + 8);
    __m128 diff1 = _mm_sub_ps(query1, values1);
    __m128 sum1 = _mm_mul_ps(diff1, diff1);
    float partial1 = partial0 + rinha_reduce_m128(sum1);
    if (partial1 >= cutoff) {
        return partial1;
    }

    float diff12 = query12 - rinha_dequantize_scalar(vector[12]);
    float diff13 = query13 - rinha_dequantize_scalar(vector[13]);
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
    rinha_index_profile_init();

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
        header->block_size != RINHA_IVF_BLOCK_SIZE) {
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
    index->list_block_offsets = (const uint32_t *) ((const unsigned char *) mapping + header->list_block_offsets_offset);
    index->block_min_radii = (const float *) ((const unsigned char *) mapping + header->block_min_radii_offset);
    index->block_max_radii = (const float *) ((const unsigned char *) mapping + header->block_max_radii_offset);
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

static float rinha_list_lower_bound_sq_for_distance(float centroid_dist, float radius) {
    if (centroid_dist <= radius) {
        return 0.0f;
    }
    float lower_bound = centroid_dist - radius;
    return lower_bound * lower_bound;
}

static float rinha_list_lower_bound_sq(float centroid_dist_sq, float radius) {
    return rinha_list_lower_bound_sq_for_distance(sqrtf(centroid_dist_sq), radius);
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

static uint32_t rinha_find_first_block_with_max_ge(
    const float *block_max_radii,
    uint32_t begin,
    uint32_t end,
    float lower
) {
    while (begin < end) {
        uint32_t mid = begin + (end - begin) / 2u;
        if (block_max_radii[mid] < lower) {
            begin = mid + 1u;
        } else {
            end = mid;
        }
    }
    return begin;
}

static uint32_t rinha_find_first_block_with_min_gt(
    const float *block_min_radii,
    uint32_t begin,
    uint32_t end,
    float upper
) {
    while (begin < end) {
        uint32_t mid = begin + (end - begin) / 2u;
        if (block_min_radii[mid] <= upper) {
            begin = mid + 1u;
        } else {
            end = mid;
        }
    }
    return begin;
}

static bool rinha_prepare_scan_window(
    const rinha_index_t *index,
    uint32_t list,
    float centroid_dist,
    float cutoff_sq,
    rinha_scan_window_t *window
) {
    uint32_t start = index->list_offsets[list];
    uint32_t end = index->list_offsets[list + 1u];
    if (start >= end) {
        return false;
    }

    uint32_t block_begin = index->list_block_offsets[list];
    uint32_t block_end = index->list_block_offsets[list + 1u];
    if (block_begin >= block_end) {
        return false;
    }

    float initial_cutoff_dist = cutoff_sq < FLT_MAX ? sqrtf(cutoff_sq) : FLT_MAX;
    float lower = initial_cutoff_dist < FLT_MAX && centroid_dist > initial_cutoff_dist ?
        centroid_dist - initial_cutoff_dist : 0.0f;
    float upper = initial_cutoff_dist < FLT_MAX ? centroid_dist + initial_cutoff_dist : FLT_MAX;
    uint32_t candidate_block_begin = lower > 0.0f ?
        rinha_find_first_block_with_max_ge(index->block_max_radii, block_begin, block_end, lower) : block_begin;
    uint32_t candidate_block_end = upper < FLT_MAX ?
        rinha_find_first_block_with_min_gt(index->block_min_radii, candidate_block_begin, block_end, upper) : block_end;
    if (candidate_block_begin >= candidate_block_end) {
        return false;
    }

    window->start = start;
    window->end = end;
    window->block_begin = block_begin;
    window->candidate_block_begin = candidate_block_begin;
    window->candidate_block_end = candidate_block_end;
    return true;
}

static void rinha_block_item_range(
    const rinha_scan_window_t *window,
    uint32_t block,
    uint32_t *item_start,
    uint32_t *item_end
) {
    uint32_t local_block = block - window->block_begin;
    *item_start = window->start + local_block * RINHA_IVF_BLOCK_SIZE;
    *item_end = *item_start + RINHA_IVF_BLOCK_SIZE;
    if (*item_end > window->end) {
        *item_end = window->end;
    }
}

static uint32_t rinha_scan_window_scalar(
    const rinha_index_t *index,
    const rinha_scan_window_t *window,
    const float query[RINHA_DIM],
    const float *decode,
    float centroid_dist,
    float best_dist[5],
    uint8_t best_label[5]
) {
    const rinha_vector_scalar_t *vectors = index->vectors;
    const uint8_t *labels = index->labels;
    const float *block_min_radii = index->block_min_radii;
    const float *block_max_radii = index->block_max_radii;
    float query0 = query[0];
    float query1 = query[1];
    float query2 = query[2];
    float query3 = query[3];
    float query4 = query[4];
    float query5 = query[5];
    float query6 = query[6];
    float query7 = query[7];
    float query8 = query[8];
    float query9 = query[9];
    float query10 = query[10];
    float query11 = query[11];
    float query12 = query[12];
    float query13 = query[13];
    uint32_t scanned = 0u;
    for (uint32_t block = window->candidate_block_begin; block < window->candidate_block_end; block++) {
        if (best_dist[4] < FLT_MAX) {
            float cutoff_dist = sqrtf(best_dist[4]);
            float current_lower = centroid_dist > cutoff_dist ? centroid_dist - cutoff_dist : 0.0f;
            float current_upper = centroid_dist + cutoff_dist;
            if (block_max_radii[block] < current_lower) {
                continue;
            }
            if (block_min_radii[block] > current_upper) {
                break;
            }
        }

        uint32_t item_start = 0u;
        uint32_t item_end = 0u;
        rinha_block_item_range(window, block, &item_start, &item_end);
        scanned += item_end - item_start;
        for (uint32_t item = item_start; item < item_end; item++) {
            const rinha_vector_scalar_t *vector = vectors + (size_t) item * RINHA_DIM;
            float exact_distance = rinha_distance_sq_scalar_preloaded(
                query0, query1, query2, query3, query4, query5, query6,
                query7, query8, query9, query10, query11, query12, query13,
                vector, decode, best_dist[4]
            );
            if (exact_distance < best_dist[4]) {
                rinha_insert_top5(best_dist, best_label, exact_distance, labels[item]);
            }
        }
    }
    return scanned;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static uint32_t rinha_scan_window_avx2(
    const rinha_index_t *index,
    const rinha_scan_window_t *window,
    const float query[RINHA_DIM],
    float centroid_dist,
    float best_dist[5],
    uint8_t best_label[5]
) {
    const rinha_vector_scalar_t *vectors = index->vectors;
    const uint8_t *labels = index->labels;
    const float *block_min_radii = index->block_min_radii;
    const float *block_max_radii = index->block_max_radii;
    __m256 query0 = _mm256_loadu_ps(query);
    __m128 query1 = _mm_loadu_ps(query + 8);
    float query12 = query[12];
    float query13 = query[13];
    uint32_t scanned = 0u;
    for (uint32_t block = window->candidate_block_begin; block < window->candidate_block_end; block++) {
        if (best_dist[4] < FLT_MAX) {
            float cutoff_dist = sqrtf(best_dist[4]);
            float current_lower = centroid_dist > cutoff_dist ? centroid_dist - cutoff_dist : 0.0f;
            float current_upper = centroid_dist + cutoff_dist;
            if (block_max_radii[block] < current_lower) {
                continue;
            }
            if (block_min_radii[block] > current_upper) {
                break;
            }
        }

        uint32_t item_start = 0u;
        uint32_t item_end = 0u;
        rinha_block_item_range(window, block, &item_start, &item_end);
        scanned += item_end - item_start;
        for (uint32_t item = item_start; item < item_end; item++) {
            const rinha_vector_scalar_t *vector = vectors + (size_t) item * RINHA_DIM;
            float exact_distance = rinha_distance_sq_avx2_preloaded(query0, query1, query12, query13, vector, best_dist[4]);
            if (exact_distance < best_dist[4]) {
                rinha_insert_top5(best_dist, best_label, exact_distance, labels[item]);
            }
        }
    }
    return scanned;
}
#endif

static uint32_t rinha_scan_list(
    const rinha_index_t *index,
    const float query[RINHA_DIM],
    const float *decode,
    uint32_t list,
    float centroid_dist,
    bool use_avx2,
    float best_dist[5],
    uint8_t best_label[5]
) {
    rinha_scan_window_t window;
    if (!rinha_prepare_scan_window(index, list, centroid_dist, best_dist[4], &window)) {
        return 0u;
    }

#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2) {
        return rinha_scan_window_avx2(index, &window, query, centroid_dist, best_dist, best_label);
    }
#else
    (void) use_avx2;
#endif
    return rinha_scan_window_scalar(index, &window, query, decode, centroid_dist, best_dist, best_label);
}

int rinha_index_fraud_count_top5(rinha_index_t *index, const float query[RINHA_DIM]) {
    bool profile_enabled = rinha_index_profile.enabled;
    uint64_t total_start_ns = profile_enabled ? rinha_now_ns() : 0u;
    uint64_t phase_start_ns = 0u;
    uint64_t plan_ns = 0u;
    uint64_t probe_scan_ns = 0u;
    uint64_t candidate_build_ns = 0u;
    uint64_t candidate_sort_ns = 0u;
    uint64_t candidate_scan_ns = 0u;
    uint64_t probe_lists_scanned = 0u;
    uint64_t probe_lists_pruned_bound = 0u;
    uint64_t candidate_lists_considered = 0u;
    uint64_t candidate_lists_selected = 0u;
    uint64_t candidate_lists_scanned = 0u;
    uint64_t candidate_lists_pruned_radius = 0u;
    uint64_t candidate_lists_pruned_bound = 0u;
    uint64_t candidate_lists_skipped_after_sort = 0u;
    uint64_t vectors_scanned_probe = 0u;
    uint64_t vectors_scanned_candidate = 0u;

    const float *decode = rinha_dequantize_lut();
#if defined(__x86_64__) || defined(__i386__)
    bool use_avx2 = rinha_cpu_has_avx2();
#else
    bool use_avx2 = false;
#endif
    float centroid_dist_sq[RINHA_IVF_NLIST];
    rinha_list_bound_t probe_lists[RINHA_IVF_NPROBE];
    rinha_list_candidate_t candidate_lists[RINHA_IVF_NLIST - RINHA_IVF_NPROBE];
    bool selected_lists[RINHA_IVF_NLIST] = {false};

    if (profile_enabled) {
        phase_start_ns = rinha_now_ns();
    }
    size_t probe_count = rinha_plan_probe_lists(index, query, centroid_dist_sq, probe_lists);
    if (profile_enabled) {
        plan_ns = rinha_now_ns() - phase_start_ns;
        phase_start_ns = rinha_now_ns();
    }

    float best_dist[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    uint8_t best_label[5] = {0u, 0u, 0u, 0u, 0u};

    for (size_t i = 0; i < probe_count; i++) {
        uint32_t list = probe_lists[i].list;
        selected_lists[list] = true;
        float centroid_dist = sqrtf(probe_lists[i].centroid_dist_sq);
        float lower_bound_sq = rinha_list_lower_bound_sq_for_distance(centroid_dist, index->list_radii[list]);
        if (lower_bound_sq >= best_dist[4]) {
            probe_lists_pruned_bound++;
            continue;
        }
        probe_lists_scanned++;
        vectors_scanned_probe += rinha_scan_list(index, query, decode, list, centroid_dist, use_avx2, best_dist, best_label);
    }
    if (profile_enabled) {
        probe_scan_ns = rinha_now_ns() - phase_start_ns;
        phase_start_ns = rinha_now_ns();
    }

    size_t candidate_count = 0u;
    float prune_distance = best_dist[4] < FLT_MAX ? sqrtf(best_dist[4]) : FLT_MAX;
    for (uint32_t list = 0; list < index->nlist; list++) {
        if (selected_lists[list]) {
            continue;
        }
        candidate_lists_considered++;
        if (prune_distance < FLT_MAX) {
            float max_centroid_dist = prune_distance + index->list_radii[list];
            if (centroid_dist_sq[list] >= max_centroid_dist * max_centroid_dist) {
                candidate_lists_pruned_radius++;
                continue;
            }
        }
        float lower_bound_sq = rinha_list_lower_bound_sq(centroid_dist_sq[list], index->list_radii[list]);
        if (lower_bound_sq >= best_dist[4]) {
            candidate_lists_pruned_bound++;
            continue;
        }
        rinha_insert_candidate_list(&candidate_lists[candidate_count++], lower_bound_sq, list);
        candidate_lists_selected++;
    }
    if (profile_enabled) {
        candidate_build_ns = rinha_now_ns() - phase_start_ns;
        phase_start_ns = rinha_now_ns();
    }

    if (candidate_count > 1u) {
        qsort(candidate_lists, candidate_count, sizeof(candidate_lists[0]), rinha_compare_candidate_lists);
    }
    if (profile_enabled) {
        candidate_sort_ns = rinha_now_ns() - phase_start_ns;
        phase_start_ns = rinha_now_ns();
    }

    for (size_t i = 0; i < candidate_count; i++) {
        if (candidate_lists[i].lower_bound_sq >= best_dist[4]) {
            candidate_lists_skipped_after_sort += candidate_count - i;
            break;
        }
        candidate_lists_scanned++;
        uint32_t list = candidate_lists[i].list;
        float centroid_dist = sqrtf(centroid_dist_sq[list]);
        vectors_scanned_candidate += rinha_scan_list(index, query, decode, list, centroid_dist, use_avx2, best_dist, best_label);
    }
    if (profile_enabled) {
        candidate_scan_ns = rinha_now_ns() - phase_start_ns;
    }

    int fraud_count = 0;
    for (size_t i = 0; i < 5; i++) {
        fraud_count += best_label[i] ? 1 : 0;
    }

    if (profile_enabled) {
        rinha_index_profile.calls++;
        rinha_index_profile.total_ns += rinha_now_ns() - total_start_ns;
        rinha_index_profile.plan_ns += plan_ns;
        rinha_index_profile.probe_scan_ns += probe_scan_ns;
        rinha_index_profile.candidate_build_ns += candidate_build_ns;
        rinha_index_profile.candidate_sort_ns += candidate_sort_ns;
        rinha_index_profile.candidate_scan_ns += candidate_scan_ns;
        rinha_index_profile.probe_lists_total += probe_count;
        rinha_index_profile.probe_lists_scanned += probe_lists_scanned;
        rinha_index_profile.probe_lists_pruned_bound += probe_lists_pruned_bound;
        rinha_index_profile.candidate_lists_considered += candidate_lists_considered;
        rinha_index_profile.candidate_lists_selected += candidate_lists_selected;
        rinha_index_profile.candidate_lists_scanned += candidate_lists_scanned;
        rinha_index_profile.candidate_lists_pruned_radius += candidate_lists_pruned_radius;
        rinha_index_profile.candidate_lists_pruned_bound += candidate_lists_pruned_bound;
        rinha_index_profile.candidate_lists_skipped_after_sort += candidate_lists_skipped_after_sort;
        rinha_index_profile.vectors_scanned_probe += vectors_scanned_probe;
        rinha_index_profile.vectors_scanned_candidate += vectors_scanned_candidate;

        if (rinha_index_profile.report_every > 0u &&
            rinha_index_profile.calls % rinha_index_profile.report_every == 0u) {
            rinha_index_profile_report("periodic");
        }
    }
    return fraud_count;
}
