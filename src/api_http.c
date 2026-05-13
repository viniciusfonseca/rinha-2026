#include "api_http.h"

#include <string.h>

static bool api_ascii_ieq(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char) a[i];
        unsigned char cb = (unsigned char) b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char) (ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char) (cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

int api_parse_http_request(const char *buffer, size_t len, api_request_t *request) {
    const char *limit = buffer + len;
    const char *header_end = NULL;
    for (const char *p = buffer; p + 3 < limit; p++) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            header_end = p;
            break;
        }
    }
    if (header_end == NULL) {
        return 0;
    }

    const char *line_end = NULL;
    for (const char *p = buffer; p + 1 < header_end; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            line_end = p;
            break;
        }
    }
    if (line_end == NULL) {
        return -1;
    }

    request->route = API_ROUTE_INVALID;
    request->content_length = 0;
    request->keep_alive = true;

    size_t line_len = (size_t) (line_end - buffer);
    if (line_len == 19u && memcmp(buffer, "GET /ready HTTP/1.1", 19u) == 0) {
        request->route = API_ROUTE_READY;
    } else if (line_len == 26u && memcmp(buffer, "POST /fraud-score HTTP/1.1", 26u) == 0) {
        request->route = API_ROUTE_FRAUD_SCORE;
    } else {
        const char *sp1 = memchr(buffer, ' ', line_len);
        if (sp1 == NULL) {
            return -1;
        }
        const char *sp2 = memchr(sp1 + 1, ' ', (size_t) (line_end - (sp1 + 1)));
        if (sp2 == NULL) {
            return -1;
        }
        if ((size_t) (line_end - (sp2 + 1)) != 8u || memcmp(sp2 + 1, "HTTP/1.1", 8u) != 0) {
            return -1;
        }
    }

    const char *cursor = line_end + 2;
    const char *headers_limit = header_end + 2;
    while (cursor < header_end) {
        const char *next = cursor;
        while (next + 1 < headers_limit && !(next[0] == '\r' && next[1] == '\n')) {
            next++;
        }
        if (next + 1 >= headers_limit || next[0] != '\r' || next[1] != '\n') {
            return -1;
        }

        size_t header_len = (size_t) (next - cursor);
        const char *value = NULL;
        if (header_len >= 15u && api_ascii_ieq(cursor, "Content-Length:", 15u)) {
            value = cursor + 15;
            while (value < next && (*value == ' ' || *value == '\t')) {
                value++;
            }
            size_t content_length = 0;
            while (value < next && *value >= '0' && *value <= '9') {
                content_length = content_length * 10u + (size_t) (*value - '0');
                value++;
            }
            if (value != next) {
                return -1;
            }
            request->content_length = content_length;
        } else if (header_len >= 11u && api_ascii_ieq(cursor, "Connection:", 11u)) {
            value = cursor + 11;
            while (value < next && (*value == ' ' || *value == '\t')) {
                value++;
            }
            size_t value_len = (size_t) (next - value);
            if (value_len == 5 && api_ascii_ieq(value, "close", 5)) {
                request->keep_alive = false;
            }
        }

        cursor = next + 2;
    }

    request->body = header_end + 4;
    request->total_length = (size_t) ((header_end + 4) - buffer) + request->content_length;
    if (len < request->total_length) {
        return 0;
    }
    return 1;
}
