#ifndef RINHA_API_HTTP_H
#define RINHA_API_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    API_ROUTE_INVALID = 0,
    API_ROUTE_READY = 1,
    API_ROUTE_FRAUD_SCORE = 2,
} api_route_t;

typedef struct {
    api_route_t route;
    size_t content_length;
    bool keep_alive;
    const char *body;
    size_t total_length;
} api_request_t;

int api_parse_http_request(const char *buffer, size_t len, api_request_t *request);

#endif
