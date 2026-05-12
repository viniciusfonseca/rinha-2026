#include "common.h"
#include "index_format.h"

#include <ctype.h>
#include <errno.h>
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
        if (len + 1 < sizeof(text)) {
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
        if (len + 1 < out_size) {
            out[len++] = (char) ch;
        }
    }
    if (ch != '"') {
        return -1;
    }

    out[len] = '\0';
    return 0;
}

static uint32_t *rinha_build_bucket_offsets(const uint64_t *signatures, uint32_t count, const rinha_lsh_params_t *params) {
    size_t offsets_len = RINHA_TABLE_COUNT * (RINHA_BUCKET_COUNT + 1u);
    uint32_t *offsets = calloc(offsets_len, sizeof(uint32_t));
    if (offsets == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < count; i++) {
        for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
            uint16_t key = rinha_table_key(signatures[i], params, table);
            offsets[table * (RINHA_BUCKET_COUNT + 1u) + key + 1u] += 1u;
        }
    }

    for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
        uint32_t *base = offsets + table * (RINHA_BUCKET_COUNT + 1u);
        for (size_t bucket = 1; bucket <= RINHA_BUCKET_COUNT; bucket++) {
            base[bucket] += base[bucket - 1];
        }
    }

    return offsets;
}

static uint32_t *rinha_build_index_arrays(const uint64_t *signatures, uint32_t count, const uint32_t *bucket_offsets, const rinha_lsh_params_t *params) {
    uint32_t *indices = malloc((size_t) RINHA_TABLE_COUNT * count * sizeof(uint32_t));
    uint32_t *cursor = malloc((size_t) RINHA_TABLE_COUNT * RINHA_BUCKET_COUNT * sizeof(uint32_t));
    if (indices == NULL || cursor == NULL) {
        free(indices);
        free(cursor);
        return NULL;
    }

    for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
        memcpy(
            cursor + table * RINHA_BUCKET_COUNT,
            bucket_offsets + table * (RINHA_BUCKET_COUNT + 1u),
            RINHA_BUCKET_COUNT * sizeof(uint32_t)
        );
    }

    for (uint32_t point = 0; point < count; point++) {
        for (size_t table = 0; table < RINHA_TABLE_COUNT; table++) {
            uint16_t key = rinha_table_key(signatures[point], params, table);
            uint32_t pos = cursor[table * RINHA_BUCKET_COUNT + key]++;
            indices[table * count + pos] = point;
        }
    }

    free(cursor);
    return indices;
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
    uint64_t *signatures = malloc((size_t) initial_capacity * sizeof(uint64_t));
    if (vectors == NULL || labels == NULL || signatures == NULL) {
        fprintf(stderr, "sem memoria para o dataset\n");
        gzclose(gz);
        free(vectors);
        free(labels);
        free(signatures);
        return 1;
    }

    rinha_lsh_params_t params;
    rinha_init_lsh_params(&params);

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
        free(signatures);
        return 1;
    }

    uint32_t count = 0;
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
            free(signatures);
            return 1;
        }

        if (count >= initial_capacity) {
            fprintf(stderr, "dataset maior que a capacidade esperada\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(signatures);
            return 1;
        }

        if (rinha_reader_skip_to(&reader, '[') == EOF) {
            fprintf(stderr, "vetor nao encontrado\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(signatures);
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
            free(signatures);
            return 1;
        }
        if (rinha_reader_skip_to(&reader, '}') == EOF) {
            fprintf(stderr, "objeto de referencia incompleto\n");
            gzclose(gz);
            free(vectors);
            free(labels);
            free(signatures);
            return 1;
        }

        labels[count] = (uint8_t) (strcmp(label_text, "fraud") == 0 ? 1 : 0);
        signatures[count] = rinha_signature_for_quantized(vector, &params);
        count++;

        if (count % 250000u == 0) {
            fprintf(stderr, "preprocessados %u vetores\n", count);
        }
    }

    gzclose(gz);
    fprintf(stderr, "total de vetores: %u\n", count);

    uint32_t *bucket_offsets = rinha_build_bucket_offsets(signatures, count, &params);
    uint32_t *indices = rinha_build_index_arrays(signatures, count, bucket_offsets, &params);
    if (bucket_offsets == NULL || indices == NULL) {
        fprintf(stderr, "falha ao construir o indice\n");
        free(vectors);
        free(labels);
        free(signatures);
        free(bucket_offsets);
        free(indices);
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) {
        fprintf(stderr, "falha ao abrir saida %s: %s\n", argv[2], strerror(errno));
        free(vectors);
        free(labels);
        free(signatures);
        free(bucket_offsets);
        free(indices);
        return 1;
    }

    rinha_index_header_t header = {0};
    memcpy(header.magic, RINHA_INDEX_MAGIC, sizeof(header.magic));
    header.version = RINHA_INDEX_VERSION;
    header.point_count = count;
    header.dim = RINHA_DIM;
    header.table_count = RINHA_TABLE_COUNT;
    header.bucket_bits = RINHA_BUCKET_BITS;
    header.vectors_offset = sizeof(header);
    header.labels_offset = header.vectors_offset + (uint64_t) count * RINHA_DIM;
    header.bucket_offsets_offset = header.labels_offset + count;
    header.indices_offset = header.bucket_offsets_offset + (uint64_t) RINHA_TABLE_COUNT * (RINHA_BUCKET_COUNT + 1u) * sizeof(uint32_t);

    fwrite(&header, sizeof(header), 1, out);
    fwrite(vectors, RINHA_DIM, count, out);
    fwrite(labels, 1, count, out);
    fwrite(bucket_offsets, sizeof(uint32_t), (size_t) RINHA_TABLE_COUNT * (RINHA_BUCKET_COUNT + 1u), out);
    fwrite(indices, sizeof(uint32_t), (size_t) RINHA_TABLE_COUNT * count, out);
    fclose(out);

    free(vectors);
    free(labels);
    free(signatures);
    free(bucket_offsets);
    free(indices);
    return 0;
}
