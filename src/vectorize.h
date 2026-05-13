#ifndef RINHA_VECTORIZE_H
#define RINHA_VECTORIZE_H

#include "common.h"
#include "vectorize_payload.h"

#include <stdbool.h>
#include <stddef.h>

void rinha_payload_to_vector(const rinha_tx_payload_t *payload, float out[RINHA_DIM]);
bool rinha_payload_force_deny_borderline(const rinha_tx_payload_t *payload);
bool rinha_request_to_vector(const char *json, size_t len, float out[RINHA_DIM]);

#endif
