#include "common.h"
#include "index_format.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    gzFile file;
    unsigned char buffer[1 << 16];
    int len;
    int pos;
    int pushback;
} rinha_gz_reader_t;

static int rinha_reader_fill(rinha_gz_reader_t *reader) {
    reader->len = gzread(reader->file, reader->buffer, (unsigned) sizeof(reader->buffer));
    reader->pos = 0;
    return reader->len;
}

static int rinha_reader_getc(rinha_gz_reader_t *reader) {
    if (reader->pushback >= 0) {
        int ch = reader->pushback;
        reader->pushback = -1;
        return ch;
    }
    if (reader->pos >= reader->len && rinha_reader_fill(reader) <= 0) {
        return EOF;
    }
    return reader->buffer[reader->pos++];
}

static void rinha_reader_ungetc(rinha_gz_reader_t *reader, int ch) {
    reader->pushback = ch;
}

static int rinha_reader_skip_to(rinha_gz_reader_t *reader, int target) {
    int ch = 0;
    while ((ch = rinha_reader_getc(reader)) != EOF) {
        if (ch == target) {
            return ch;
        }
    }
    return EOF;
}

static int rinha_reader_skip_ws_commas(rinha_gz_reader_t *reader) {
    int ch = 0;
    while ((ch = rinha_reader_getc(reader)) != EOF) {
        if (!isspace((unsigned char) ch) && ch != ',') {
            return ch;
        }
    }
    return EOF;
}

static double rinha_reader_number(rinha_gz_reader_t *reader) {
    char text[64];
    size_t len = 0;

    int ch = rinha_reader_skip_ws_commas(reader);
    while (ch != EOF && (isdigit((unsigned char) ch) || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E')) {
        if (len + 1u < sizeof(text)) {
            text[len++] = (char) ch;
        }
        ch = rinha_reader_getc(reader);
    }

    if (ch != EOF) {
        rinha_reader_ungetc(reader, ch);
    }
    text[len] = '\0';
    return strtod(text, NULL);
}

static int rinha_reader_string(rinha_gz_reader_t *reader, char *out, size_t out_size) {
    int ch = rinha_reader_skip_ws_commas(reader);
    while (ch != EOF && ch != '"') {
        ch = rinha_reader_getc(reader);
    }
    if (ch != '"') {
        return -1;
    }

    size_t len = 0;
    while ((ch = rinha_reader_getc(reader)) != EOF && ch != '"') {
        if (len + 1u < out_size) {
            out[len++] = (char) ch;
        }
    }
    if (ch != '"') {
        return -1;
    }

    out[len] = '\0';
    return 0;
}

static uint64_t rinha_xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void rinha_decode_vector(const uint8_t *vector, float out[RINHA_DIM]) {
    for (size_t dim = 0; dim < RINHA_DIM; dim++) {
        out[dim] = rinha_dequantize_scalar(vector[dim]);
    }
}

static float rinha_distance_sq_float(const float *lhs, const float *rhs, size_t dim) {
    float sum = 0.0f;
    for (size_t i = 0; i < dim; i++) {
        float diff = lhs[i] - rhs[i];
        sum += diff * diff;
    }
    return sum;
}

static uint32_t rinha_find_nearest_centroid(
    const float *point,
    const float *centroids,
    uint32_t centroid_count,
    size_t dim
) {
    float best_dist = FLT_MAX;
    uint32_t best_index = 0u;
    for (uint32_t i = 0; i < centroid_count; i++) {
        float distance = rinha_distance_sq_float(point, centroids + (size_t) i * dim, dim);
        if (distance < best_dist) {
            best_dist = distance;
            best_index = i;
        }
    }
    return best_index;
}

static bool rinha_train_kmeans(
    const float *points,
    uint32_t point_count,
    size_t dim,
    uint32_t cluster_count,
    float *centroids,
    uint32_t *assignments,
    uint64_t *rng_state
) {
    if (point_count < cluster_count || cluster_count == 0u) {
        return false;
    }

    for (uint32_t cluster = 0; cluster < cluster_count; cluster++) {
        uint32_t source = (uint32_t) (((uint64_t) cluster * point_count) / cluster_count);
        memcpy(
            centroids + (size_t) cluster * dim,
            points + (size_t) source * dim,
            dim * sizeof(float)
        );
    }

    float *sums = calloc((size_t) cluster_count * dim, sizeof(float));
    uint32_t *counts = calloc(cluster_count, sizeof(uint32_t));
    if (sums == NULL || counts == NULL) {
        free(sums);
        free(counts);
        return false;
    }

    for (size_t iter = 0; iter < RINHA_IVF_PQ_KMEANS_ITERS; iter++) {
        memset(sums, 0, (size_t) cluster_count * dim * sizeof(float));
        memset(counts, 0, cluster_count * sizeof(uint32_t));

        for (uint32_t point = 0; point < point_count; point++) {
            const float *sample = points + (size_t) point * dim;
            uint32_t cluster = rinha_find_nearest_centroid(sample, centroids, cluster_count, dim);
            assignments[point] = cluster;
            counts[cluster] += 1u;
            float *sum = sums + (size_t) cluster * dim;
            for (size_t axis = 0; axis < dim; axis++) {
                sum[axis] += sample[axis];
            }
        }

        for (uint32_t cluster = 0; cluster < cluster_count; cluster++) {
            float *centroid = centroids + (size_t) cluster * dim;
            if (counts[cluster] == 0u) {
                uint32_t source = (uint32_t) (rinha_xorshift64(rng_state) % point_count);
                memcpy(centroid, points + (size_t) source * dim, dim * sizeof(float));
                continue;
            }

            float inv_count = 1.0f / (float) counts[cluster];
            const float *sum = sums + (size_t) cluster * dim;
            for (size_t axis = 0; axis < dim; axis++) {
                centroid[axis] = sum[axis] * inv_count;
            }
        }
    }

    free(sums);
    free(counts);
    return true;
}

static void rinha_maybe_add_training_sample(
    float *samples,
    uint32_t *sample_count,
    uint32_t seen_count,
    const uint8_t vector[RINHA_DIM],
    uint64_t *rng_state
) {
    uint32_t slot = 0u;
    if (*sample_count < RINHA_IVF_PQ_TRAIN_SAMPLES) {
        slot = (*sample_count)++;
    } else {
        uint32_t candidate = (uint32_t) (rinha_xorshift64(rng_state) % seen_count);
        if (candidate >= RINHA_IVF_PQ_TRAIN_SAMPLES) {
            return;
        }
        slot = candidate;
    }
    rinha_decode_vector(vector, samples + (size_t) slot * RINHA_DIM);
}

static bool rinha_train_ivf_pq(
    const float *samples,
    uint32_t sample_count,
    float *coarse_centroids,
    float *pq_codebooks
) {
    uint64_t rng_state = 0x123456789abcdefULL;
    uint32_t *sample_assignments = malloc((size_t) sample_count * sizeof(uint32_t));
    float *residuals = malloc((size_t) sample_count * RINHA_DIM * sizeof(float));
    float *subspace_points = malloc((size_t) sample_count * RINHA_PQ_SUBDIM * sizeof(float));
    uint32_t *sub_assignments = malloc((size_t) sample_count * sizeof(uint32_t));
    if (sample_assignments == NULL || residuals == NULL || subspace_points == NULL || sub_assignments == NULL) {
        free(sample_assignments);
        free(residuals);
        free(subspace_points);
        free(sub_assignments);
        return false;
    }

    if (!rinha_train_kmeans(
            samples,
            sample_count,
            RINHA_DIM,
            RINHA_IVF_NLIST,
            coarse_centroids,
            sample_assignments,
            &rng_state)) {
        free(sample_assignments);
        free(residuals);
        free(subspace_points);
        free(sub_assignments);
        return false;
    }

    for (uint32_t sample = 0; sample < sample_count; sample++) {
        const float *point = samples + (size_t) sample * RINHA_DIM;
        const float *centroid = coarse_centroids + (size_t) sample_assignments[sample] * RINHA_DIM;
        float *residual = residuals + (size_t) sample * RINHA_DIM;
        for (size_t dim = 0; dim < RINHA_DIM; dim++) {
            residual[dim] = point[dim] - centroid[dim];
        }
    }

    for (size_t subspace = 0; subspace < RINHA_PQ_M; subspace++) {
        size_t dim_base = subspace * RINHA_PQ_SUBDIM;
        for (uint32_t sample = 0; sample < sample_count; sample++) {
            const float *residual = residuals + (size_t) sample * RINHA_DIM + dim_base;
            float *subspace_point = subspace_points + (size_t) sample * RINHA_PQ_SUBDIM;
            for (size_t axis = 0; axis < RINHA_PQ_SUBDIM; axis++) {
                subspace_point[axis] = residual[axis];
            }
        }

        if (!rinha_train_kmeans(
                subspace_points,
                sample_count,
                RINHA_PQ_SUBDIM,
                RINHA_PQ_KSUB,
                pq_codebooks + subspace * RINHA_PQ_KSUB * RINHA_PQ_SUBDIM,
                sub_assignments,
                &rng_state)) {
            free(sample_assignments);
            free(residuals);
            free(subspace_points);
            free(sub_assignments);
            return false;
        }
    }

    free(sample_assignments);
    free(residuals);
    free(subspace_points);
    free(sub_assignments);
    return true;
}

static void rinha_encode_pq_code(
    const float point[RINHA_DIM],
    uint32_t list,
    const float *coarse_centroids,
    const float *pq_codebooks,
    uint8_t out_code[RINHA_PQ_M]
) {
    const float *centroid = coarse_centroids + (size_t) list * RINHA_DIM;
    for (size_t subspace = 0; subspace < RINHA_PQ_M; subspace++) {
        size_t dim_base = subspace * RINHA_PQ_SUBDIM;
        float best_dist = FLT_MAX;
        uint8_t best_code = 0u;
        for (size_t code = 0; code < RINHA_PQ_KSUB; code++) {
            const float *codeword = pq_codebooks +
                ((subspace * RINHA_PQ_KSUB + code) * RINHA_PQ_SUBDIM);
            float distance = 0.0f;
            for (size_t axis = 0; axis < RINHA_PQ_SUBDIM; axis++) {
                float residual = point[dim_base + axis] - centroid[dim_base + axis];
                float diff = residual - codeword[axis];
                distance += diff * diff;
            }
            if (distance < best_dist) {
                best_dist = distance;
                best_code = (uint8_t) code;
            }
        }
        out_code[subspace] = best_code;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s <references.json.gz> <index.bin>\n", argv[0]);
        return 1;
    }

    gzFile gz = gzopen(argv[1], "rb");
    if (gz == NULL) {
        fprintf(stderr, "falha ao abrir %s\n", argv[1]);
        return 1;
    }

    const uint32_t initial_capacity = 3000000u;
    uint8_t *vectors = malloc((size_t) initial_capacity * RINHA_DIM);
    uint8_t *labels = malloc(initial_capacity);
    float *samples = malloc((size_t) RINHA_IVF_PQ_TRAIN_SAMPLES * RINHA_DIM * sizeof(float));
    if (vectors == NULL || labels == NULL || samples == NULL) {
        fprintf(stderr, "sem memoria para o dataset\n");
        gzclose(gz);
        free(vectors);
        free(labels);
        free(samples);
        return 1;
    }

    rinha_gz_reader_t reader = {
        .file = gz,
        .len = 0,
        .pos = 0,
        .pushback = -1,
    };

    if (rinha_reader_skip_to(&reader, '[') == EOF) {
        fprintf(stderr, "dataset invalido\n");
        gzclose(gz);
        free(vectors);
        free(labels);
        free(samples);
        return 1;
    }

    uint32_t count = 0u;
    uint32_t sample_count = 0u;
    uint64_t sample_rng_state = 0x9e3779b97f4a7c15ULL;
    int ch = 0;
    char label_text[16];

    while ((ch = rinha_reader_skip_ws_commas(&reader)) != EOF) {
        if (ch == ']') {
            break;
        }
        if (ch != '{') {
            fprintf(stderr, "dataset invalido perto de %c\n", ch);
            gzclose(gz);
            free(vectors);
            free(labels);
            free(samples);
            return 1;
        }

        if (count >= initial_capacity) {
            fprintf(stderr, "dataset maior que a capacidade esperada\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(samples);
            return 1;
        }

        if (rinha_reader_skip_to(&reader, '[') == EOF) {
            fprintf(stderr, "vetor nao encontrado\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(samples);
            return 1;
        }

        uint8_t *vector = vectors + (size_t) count * RINHA_DIM;
        for (size_t dim = 0; dim < RINHA_DIM; dim++) {
            vector[dim] = rinha_quantize_scalar(rinha_reader_number(&reader));
        }

        if (rinha_reader_skip_to(&reader, ':') == EOF || rinha_reader_string(&reader, label_text, sizeof(label_text)) != 0) {
            fprintf(stderr, "label nao encontrada\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(samples);
            return 1;
        }
        if (rinha_reader_skip_to(&reader, '}') == EOF) {
            fprintf(stderr, "objeto de referencia incompleto\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(samples);
            return 1;
        }

        labels[count] = (uint8_t) (strcmp(label_text, "fraud") == 0 ? 1 : 0);
        rinha_maybe_add_training_sample(samples, &sample_count, count + 1u, vector, &sample_rng_state);
        count++;

        if (count % 250000u == 0u) {
            fprintf(stderr, "preprocessados %u vetores\n", count);
        }
    }

    gzclose(gz);
    fprintf(stderr, "total de vetores: %u\n", count);

    float *coarse_centroids = malloc((size_t) RINHA_IVF_NLIST * RINHA_DIM * sizeof(float));
    float *pq_codebooks = malloc((size_t) RINHA_PQ_M * RINHA_PQ_KSUB * RINHA_PQ_SUBDIM * sizeof(float));
    if (coarse_centroids == NULL || pq_codebooks == NULL) {
        fprintf(stderr, "sem memoria para treinar o indice\n");
        free(vectors);
        free(labels);
        free(samples);
        free(coarse_centroids);
        free(pq_codebooks);
        return 1;
    }

    if (!rinha_train_ivf_pq(samples, sample_count, coarse_centroids, pq_codebooks)) {
        fprintf(stderr, "falha ao treinar IVF_PQ\n");
        free(vectors);
        free(labels);
        free(samples);
        free(coarse_centroids);
        free(pq_codebooks);
        return 1;
    }
    free(samples);

    uint16_t *list_assignments = malloc((size_t) count * sizeof(uint16_t));
    uint32_t *list_sizes = calloc(RINHA_IVF_NLIST, sizeof(uint32_t));
    if (list_assignments == NULL || list_sizes == NULL) {
        fprintf(stderr, "sem memoria para particionar listas IVF\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(pq_codebooks);
        free(list_assignments);
        free(list_sizes);
        return 1;
    }

    float decoded[RINHA_DIM];
    for (uint32_t point = 0; point < count; point++) {
        const uint8_t *vector = vectors + (size_t) point * RINHA_DIM;
        rinha_decode_vector(vector, decoded);
        uint16_t list = (uint16_t) rinha_find_nearest_centroid(decoded, coarse_centroids, RINHA_IVF_NLIST, RINHA_DIM);
        list_assignments[point] = list;
        list_sizes[list] += 1u;
    }

    uint32_t *list_offsets = malloc((size_t) (RINHA_IVF_NLIST + 1u) * sizeof(uint32_t));
    if (list_offsets == NULL) {
        fprintf(stderr, "sem memoria para offsets IVF\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(pq_codebooks);
        free(list_assignments);
        free(list_sizes);
        return 1;
    }

    list_offsets[0] = 0u;
    for (size_t list = 0; list < RINHA_IVF_NLIST; list++) {
        list_offsets[list + 1u] = list_offsets[list] + list_sizes[list];
    }

    uint32_t *cursor = malloc((size_t) RINHA_IVF_NLIST * sizeof(uint32_t));
    uint8_t *grouped_vectors = malloc((size_t) count * RINHA_DIM);
    uint8_t *grouped_labels = malloc(count);
    uint8_t *grouped_codes = malloc((size_t) count * RINHA_PQ_M);
    if (cursor == NULL || grouped_vectors == NULL || grouped_labels == NULL || grouped_codes == NULL) {
        fprintf(stderr, "sem memoria para serializar IVF_PQ\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(pq_codebooks);
        free(list_assignments);
        free(list_sizes);
        free(list_offsets);
        free(cursor);
        free(grouped_vectors);
        free(grouped_labels);
        free(grouped_codes);
        return 1;
    }
    memcpy(cursor, list_offsets, (size_t) RINHA_IVF_NLIST * sizeof(uint32_t));

    for (uint32_t point = 0; point < count; point++) {
        const uint8_t *vector = vectors + (size_t) point * RINHA_DIM;
        rinha_decode_vector(vector, decoded);

        uint16_t list = list_assignments[point];
        uint32_t position = cursor[list]++;
        memcpy(grouped_vectors + (size_t) position * RINHA_DIM, vector, RINHA_DIM);
        grouped_labels[position] = labels[point];
        rinha_encode_pq_code(
            decoded,
            list,
            coarse_centroids,
            pq_codebooks,
            grouped_codes + (size_t) position * RINHA_PQ_M
        );
    }

    free(vectors);
    free(labels);
    free(list_assignments);
    free(list_sizes);
    free(cursor);

    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) {
        fprintf(stderr, "falha ao abrir saida %s: %s\n", argv[2], strerror(errno));
        free(coarse_centroids);
        free(pq_codebooks);
        free(list_offsets);
        free(grouped_vectors);
        free(grouped_labels);
        free(grouped_codes);
        return 1;
    }

    rinha_index_header_t header = {0};
    memcpy(header.magic, RINHA_INDEX_MAGIC, sizeof(header.magic));
    header.version = RINHA_INDEX_VERSION;
    header.point_count = count;
    header.dim = RINHA_DIM;
    header.nlist = RINHA_IVF_NLIST;
    header.nprobe = RINHA_IVF_NPROBE;
    header.pq_m = RINHA_PQ_M;
    header.pq_subdim = RINHA_PQ_SUBDIM;
    header.pq_ksub = RINHA_PQ_KSUB;
    header.rerank_cap = RINHA_IVF_PQ_RERANK;
    header.coarse_centroids_offset = sizeof(header);
    header.pq_codebooks_offset = header.coarse_centroids_offset +
        (uint64_t) RINHA_IVF_NLIST * RINHA_DIM * sizeof(float);
    header.list_offsets_offset = header.pq_codebooks_offset +
        (uint64_t) RINHA_PQ_M * RINHA_PQ_KSUB * RINHA_PQ_SUBDIM * sizeof(float);
    header.codes_offset = header.list_offsets_offset +
        (uint64_t) (RINHA_IVF_NLIST + 1u) * sizeof(uint32_t);
    header.labels_offset = header.codes_offset + (uint64_t) count * RINHA_PQ_M;
    header.vectors_offset = header.labels_offset + count;

    fwrite(&header, sizeof(header), 1, out);
    fwrite(coarse_centroids, sizeof(float), (size_t) RINHA_IVF_NLIST * RINHA_DIM, out);
    fwrite(pq_codebooks, sizeof(float), (size_t) RINHA_PQ_M * RINHA_PQ_KSUB * RINHA_PQ_SUBDIM, out);
    fwrite(list_offsets, sizeof(uint32_t), RINHA_IVF_NLIST + 1u, out);
    fwrite(grouped_codes, RINHA_PQ_M, count, out);
    fwrite(grouped_labels, 1, count, out);
    fwrite(grouped_vectors, RINHA_DIM, count, out);
    fclose(out);

    free(coarse_centroids);
    free(pq_codebooks);
    free(list_offsets);
    free(grouped_vectors);
    free(grouped_labels);
    free(grouped_codes);
    return 0;
}
