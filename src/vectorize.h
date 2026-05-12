#ifndef RINHA_VECTORIZE_H
#define RINHA_VECTORIZE_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

bool rinha_request_to_vector(const char *json, size_t len, float out[RINHA_DIM]);

#endif
