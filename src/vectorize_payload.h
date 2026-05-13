#ifndef RINHA_VECTORIZE_PAYLOAD_H
#define RINHA_VECTORIZE_PAYLOAD_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *start;
    const char *end;
} rinha_span_t;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} rinha_timestamp_t;

typedef struct {
    double amount;
    int installments;
    rinha_timestamp_t requested_at;

    double customer_avg_amount;
    int tx_count_24h;
    rinha_span_t known_merchants;

    char merchant_id[32];
    size_t merchant_id_len;
    uint32_t merchant_mcc_code;
    double merchant_avg_amount;
    bool merchant_known;

    bool is_online;
    bool card_present;
    double km_from_home;

    bool has_last_transaction;
    rinha_timestamp_t last_timestamp;
    double km_from_current;

    bool amount_set;
    bool installments_set;
    bool requested_at_set;
    bool customer_avg_amount_set;
    bool tx_count_24h_set;
    bool known_merchants_set;
    bool merchant_id_set;
    bool merchant_mcc_set;
    bool merchant_avg_amount_set;
    bool is_online_set;
    bool card_present_set;
    bool km_from_home_set;
    bool last_timestamp_set;
    bool km_from_current_set;
    bool transaction_set;
    bool customer_set;
    bool merchant_set;
    bool terminal_set;
    bool last_transaction_set;
} rinha_tx_payload_t;

bool rinha_parse_tx_payload(const char *json, size_t len, rinha_tx_payload_t *payload);

#endif
