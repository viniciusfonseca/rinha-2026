#include "common.h"
#include "index_format.h"
#include "quantize.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
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

typedef struct {
    float radius;
    uint32_t order;
    uint8_t label;
    rinha_vector_scalar_t vector[RINHA_DIM];
} rinha_grouped_item_t;

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

static void rinha_pack_vector(
    const float features[RINHA_FEATURE_DIM],
    rinha_vector_scalar_t vector[RINHA_DIM]
) {
    vector[RINHA_SLOT_AMOUNT_RATIO] = rinha_quantize_scalar(features[2]);
    vector[RINHA_SLOT_KM_FROM_CURRENT] = rinha_quantize_scalar(features[6]);
    vector[RINHA_SLOT_MCC_RISK] = rinha_quantize_scalar(features[12]);
    vector[RINHA_SLOT_MINUTES_SINCE_LAST] = rinha_quantize_scalar(features[5]);
    vector[RINHA_SLOT_MERCHANT_UNKNOWN] = rinha_quantize_scalar(features[11]);
    vector[RINHA_SLOT_TX_COUNT_24H] = rinha_quantize_scalar(features[8]);
    vector[RINHA_SLOT_KM_FROM_HOME] = rinha_quantize_scalar(features[7]);
    vector[RINHA_SLOT_AMOUNT] = rinha_quantize_scalar(features[0]);
    vector[RINHA_SLOT_MERCHANT_AVG_AMOUNT] = rinha_quantize_scalar(features[13]);
    vector[RINHA_SLOT_IS_ONLINE] = rinha_quantize_scalar(features[9]);
    vector[RINHA_SLOT_CARD_PRESENT] = rinha_quantize_scalar(features[10]);
    vector[RINHA_SLOT_REQUEST_HOUR] = rinha_quantize_scalar(features[3]);
    vector[RINHA_SLOT_REQUEST_WEEKDAY] = rinha_quantize_scalar(features[4]);
    vector[RINHA_SLOT_INSTALLMENTS] = rinha_quantize_scalar(features[1]);
    vector[RINHA_SLOT_PADDING0] = rinha_quantize_scalar(0.0);
    vector[RINHA_SLOT_PADDING1] = rinha_quantize_scalar(0.0);
}

static void rinha_decode_vector(const rinha_vector_scalar_t *vector, float out[RINHA_DIM]) {
    const float *decode = rinha_dequantize_lut();
    for (size_t dim = 0; dim < RINHA_DIM; dim++) {
        out[dim] = decode[vector[dim]];
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

    for (size_t iter = 0; iter < RINHA_IVF_KMEANS_ITERS; iter++) {
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
    const rinha_vector_scalar_t vector[RINHA_DIM],
    uint64_t *rng_state
) {
    uint32_t slot = 0u;
    if (*sample_count < RINHA_IVF_TRAIN_SAMPLES) {
        slot = (*sample_count)++;
    } else {
        uint32_t candidate = (uint32_t) (rinha_xorshift64(rng_state) % seen_count);
        if (candidate >= RINHA_IVF_TRAIN_SAMPLES) {
            return;
        }
        slot = candidate;
    }
    rinha_decode_vector(vector, samples + (size_t) slot * RINHA_DIM);
}

static int rinha_compare_grouped_items(const void *lhs_ptr, const void *rhs_ptr) {
    const rinha_grouped_item_t *lhs = (const rinha_grouped_item_t *) lhs_ptr;
    const rinha_grouped_item_t *rhs = (const rinha_grouped_item_t *) rhs_ptr;
    if (lhs->radius < rhs->radius) {
        return -1;
    }
    if (lhs->radius > rhs->radius) {
        return 1;
    }
    if (lhs->order < rhs->order) {
        return -1;
    }
    if (lhs->order > rhs->order) {
        return 1;
    }
    return 0;
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
    rinha_vector_scalar_t *vectors = malloc((size_t) initial_capacity * RINHA_DIM * sizeof(rinha_vector_scalar_t));
    uint8_t *labels = malloc(initial_capacity);
    float *samples = malloc((size_t) RINHA_IVF_TRAIN_SAMPLES * RINHA_DIM * sizeof(float));
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

        float features[RINHA_FEATURE_DIM];
        for (size_t dim = 0; dim < RINHA_FEATURE_DIM; dim++) {
            features[dim] = (float) rinha_reader_number(&reader);
        }
        rinha_vector_scalar_t *vector = vectors + (size_t) count * RINHA_DIM;
        rinha_pack_vector(features, vector);

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
    uint32_t *sample_assignments = malloc((size_t) sample_count * sizeof(uint32_t));
    if (coarse_centroids == NULL || sample_assignments == NULL) {
        fprintf(stderr, "sem memoria para treinar o indice IVF\n");
        free(vectors);
        free(labels);
        free(samples);
        free(coarse_centroids);
        free(sample_assignments);
        return 1;
    }

    uint64_t coarse_rng_state = 0x123456789abcdefULL;
    if (!rinha_train_kmeans(
            samples,
            sample_count,
            RINHA_DIM,
            RINHA_IVF_NLIST,
            coarse_centroids,
            sample_assignments,
            &coarse_rng_state)) {
        fprintf(stderr, "falha ao treinar IVF\n");
        free(vectors);
        free(labels);
        free(samples);
        free(coarse_centroids);
        free(sample_assignments);
        return 1;
    }
    free(samples);
    free(sample_assignments);

    uint16_t *list_assignments = malloc((size_t) count * sizeof(uint16_t));
    float *vector_radii = malloc((size_t) count * sizeof(float));
    uint32_t *list_sizes = calloc(RINHA_IVF_NLIST, sizeof(uint32_t));
    float *list_radii = calloc(RINHA_IVF_NLIST, sizeof(float));
    if (list_assignments == NULL || vector_radii == NULL || list_sizes == NULL || list_radii == NULL) {
        fprintf(stderr, "sem memoria para particionar listas IVF\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(list_assignments);
        free(vector_radii);
        free(list_sizes);
        free(list_radii);
        return 1;
    }

    float decoded[RINHA_DIM];
    for (uint32_t point = 0; point < count; point++) {
        const rinha_vector_scalar_t *vector = vectors + (size_t) point * RINHA_DIM;
        rinha_decode_vector(vector, decoded);
        uint16_t list = (uint16_t) rinha_find_nearest_centroid(decoded, coarse_centroids, RINHA_IVF_NLIST, RINHA_DIM);
        list_assignments[point] = list;
        list_sizes[list] += 1u;
        float radius_sq = rinha_distance_sq_float(
            decoded,
            coarse_centroids + (size_t) list * RINHA_DIM,
            RINHA_DIM
        );
        float radius = sqrtf(radius_sq);
        vector_radii[point] = radius;
        if (radius > list_radii[list]) {
            list_radii[list] = radius;
        }
    }

    uint32_t *list_offsets = malloc((size_t) (RINHA_IVF_NLIST + 1u) * sizeof(uint32_t));
    if (list_offsets == NULL) {
        fprintf(stderr, "sem memoria para offsets IVF\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(list_assignments);
        free(vector_radii);
        free(list_sizes);
        free(list_radii);
        return 1;
    }

    list_offsets[0] = 0u;
    for (size_t list = 0; list < RINHA_IVF_NLIST; list++) {
        list_offsets[list + 1u] = list_offsets[list] + list_sizes[list];
    }

    uint32_t *cursor = malloc((size_t) RINHA_IVF_NLIST * sizeof(uint32_t));
    float *grouped_radii = malloc((size_t) count * sizeof(float));
    rinha_vector_scalar_t *grouped_vectors = malloc((size_t) count * RINHA_DIM * sizeof(rinha_vector_scalar_t));
    uint8_t *grouped_labels = malloc(count);
    if (cursor == NULL || grouped_radii == NULL || grouped_vectors == NULL || grouped_labels == NULL) {
        fprintf(stderr, "sem memoria para serializar IVF\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(list_assignments);
        free(vector_radii);
        free(list_sizes);
        free(list_radii);
        free(list_offsets);
        free(cursor);
        free(grouped_radii);
        free(grouped_vectors);
        free(grouped_labels);
        return 1;
    }
    memcpy(cursor, list_offsets, (size_t) RINHA_IVF_NLIST * sizeof(uint32_t));

    for (uint32_t point = 0; point < count; point++) {
        uint16_t list = list_assignments[point];
        uint32_t position = cursor[list]++;
        const rinha_vector_scalar_t *vector = vectors + (size_t) point * RINHA_DIM;
        grouped_radii[position] = vector_radii[point];
        memcpy(grouped_vectors + (size_t) position * RINHA_DIM, vector, RINHA_DIM * sizeof(rinha_vector_scalar_t));
        grouped_labels[position] = labels[point];
    }

    uint32_t max_list_size = 0u;
    for (size_t list = 0; list < RINHA_IVF_NLIST; list++) {
        if (list_sizes[list] > max_list_size) {
            max_list_size = list_sizes[list];
        }
    }

    rinha_grouped_item_t *sort_buffer = max_list_size > 0u ?
        malloc((size_t) max_list_size * sizeof(rinha_grouped_item_t)) : NULL;
    if (max_list_size > 0u && sort_buffer == NULL) {
        fprintf(stderr, "sem memoria para ordenar listas IVF\n");
        free(vectors);
        free(labels);
        free(coarse_centroids);
        free(list_assignments);
        free(vector_radii);
        free(list_sizes);
        free(list_radii);
        free(list_offsets);
        free(cursor);
        free(grouped_radii);
        free(grouped_vectors);
        free(grouped_labels);
        return 1;
    }

    for (size_t list = 0; list < RINHA_IVF_NLIST; list++) {
        uint32_t start = list_offsets[list];
        uint32_t end = list_offsets[list + 1u];
        uint32_t size = end - start;
        if (size <= 1u) {
            continue;
        }

        for (uint32_t i = 0; i < size; i++) {
            uint32_t position = start + i;
            sort_buffer[i].radius = grouped_radii[position];
            sort_buffer[i].order = position;
            sort_buffer[i].label = grouped_labels[position];
            memcpy(
                sort_buffer[i].vector,
                grouped_vectors + (size_t) position * RINHA_DIM,
                RINHA_DIM * sizeof(rinha_vector_scalar_t)
            );
        }

        qsort(sort_buffer, size, sizeof(sort_buffer[0]), rinha_compare_grouped_items);

        for (uint32_t i = 0; i < size; i++) {
            uint32_t position = start + i;
            grouped_radii[position] = sort_buffer[i].radius;
            grouped_labels[position] = sort_buffer[i].label;
            memcpy(
                grouped_vectors + (size_t) position * RINHA_DIM,
                sort_buffer[i].vector,
                RINHA_DIM * sizeof(rinha_vector_scalar_t)
            );
        }
    }

    free(vectors);
    free(labels);
    free(list_assignments);
    free(vector_radii);
    free(cursor);

    uint32_t *list_block_offsets = malloc((size_t) (RINHA_IVF_NLIST + 1u) * sizeof(uint32_t));
    if (list_block_offsets == NULL) {
        fprintf(stderr, "sem memoria para blocos IVF\n");
        free(coarse_centroids);
        free(list_sizes);
        free(list_offsets);
        free(list_radii);
        free(sort_buffer);
        free(grouped_radii);
        free(grouped_vectors);
        free(grouped_labels);
        return 1;
    }

    list_block_offsets[0] = 0u;
    for (size_t list = 0; list < RINHA_IVF_NLIST; list++) {
        uint32_t list_size = list_sizes[list];
        uint32_t block_count = (list_size + RINHA_IVF_BLOCK_SIZE - 1u) / RINHA_IVF_BLOCK_SIZE;
        list_block_offsets[list + 1u] = list_block_offsets[list] + block_count;
    }

    uint32_t total_blocks = list_block_offsets[RINHA_IVF_NLIST];
    float *block_min_radii = malloc((size_t) total_blocks * sizeof(float));
    float *block_max_radii = malloc((size_t) total_blocks * sizeof(float));
    if (block_min_radii == NULL || block_max_radii == NULL) {
        fprintf(stderr, "sem memoria para metadados de blocos IVF\n");
        free(coarse_centroids);
        free(list_sizes);
        free(list_offsets);
        free(list_radii);
        free(list_block_offsets);
        free(block_min_radii);
        free(block_max_radii);
        free(sort_buffer);
        free(grouped_radii);
        free(grouped_vectors);
        free(grouped_labels);
        return 1;
    }

    for (size_t list = 0; list < RINHA_IVF_NLIST; list++) {
        uint32_t list_start = list_offsets[list];
        uint32_t list_end = list_offsets[list + 1u];
        uint32_t block_start = list_block_offsets[list];
        uint32_t block_end = list_block_offsets[list + 1u];
        for (uint32_t block = block_start; block < block_end; block++) {
            uint32_t local_block = block - block_start;
            uint32_t item_start = list_start + local_block * RINHA_IVF_BLOCK_SIZE;
            uint32_t item_end = item_start + RINHA_IVF_BLOCK_SIZE;
            if (item_end > list_end) {
                item_end = list_end;
            }
            block_min_radii[block] = grouped_radii[item_start];
            block_max_radii[block] = grouped_radii[item_end - 1u];
        }
    }

    free(grouped_radii);
    free(list_sizes);
    free(sort_buffer);

    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) {
        fprintf(stderr, "falha ao abrir saida %s: %s\n", argv[2], strerror(errno));
        free(coarse_centroids);
        free(list_offsets);
        free(list_radii);
        free(list_block_offsets);
        free(block_min_radii);
        free(block_max_radii);
        free(grouped_vectors);
        free(grouped_labels);
        return 1;
    }

    rinha_index_header_t header = {0};
    memcpy(header.magic, RINHA_INDEX_MAGIC, sizeof(header.magic));
    header.version = RINHA_INDEX_VERSION;
    header.point_count = count;
    header.dim = RINHA_DIM;
    header.nlist = RINHA_IVF_NLIST;
    header.nprobe = RINHA_IVF_NPROBE;
    header.block_size = RINHA_IVF_BLOCK_SIZE;
    header.coarse_centroids_offset = sizeof(header);
    header.list_offsets_offset = header.coarse_centroids_offset +
        (uint64_t) RINHA_IVF_NLIST * RINHA_DIM * sizeof(float);
    header.list_radii_offset = header.list_offsets_offset +
        (uint64_t) (RINHA_IVF_NLIST + 1u) * sizeof(uint32_t);
    header.list_block_offsets_offset = header.list_radii_offset +
        (uint64_t) RINHA_IVF_NLIST * sizeof(float);
    header.block_min_radii_offset = header.list_block_offsets_offset +
        (uint64_t) (RINHA_IVF_NLIST + 1u) * sizeof(uint32_t);
    header.block_max_radii_offset = header.block_min_radii_offset +
        (uint64_t) total_blocks * sizeof(float);
    header.labels_offset = header.block_max_radii_offset +
        (uint64_t) total_blocks * sizeof(float);
    header.vectors_offset = header.labels_offset + count;

    fwrite(&header, sizeof(header), 1, out);
    fwrite(coarse_centroids, sizeof(float), (size_t) RINHA_IVF_NLIST * RINHA_DIM, out);
    fwrite(list_offsets, sizeof(uint32_t), RINHA_IVF_NLIST + 1u, out);
    fwrite(list_radii, sizeof(float), RINHA_IVF_NLIST, out);
    fwrite(list_block_offsets, sizeof(uint32_t), RINHA_IVF_NLIST + 1u, out);
    fwrite(block_min_radii, sizeof(float), total_blocks, out);
    fwrite(block_max_radii, sizeof(float), total_blocks, out);
    fwrite(grouped_labels, 1, count, out);
    fwrite(grouped_vectors, sizeof(rinha_vector_scalar_t), (size_t) count * RINHA_DIM, out);
    fclose(out);

    free(coarse_centroids);
    free(list_offsets);
    free(list_radii);
    free(list_block_offsets);
    free(block_min_radii);
    free(block_max_radii);
    free(grouped_vectors);
    free(grouped_labels);
    return 0;
}
