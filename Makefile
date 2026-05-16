CC ?= cc
LDFLAGS ?=

TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null || uname -m)
BASE_CFLAGS := -O3 -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
AVX2_CFLAGS :=
ifneq (,$(findstring x86_64,$(TARGET_TRIPLE))$(findstring i386,$(TARGET_TRIPLE))$(findstring i686,$(TARGET_TRIPLE)))
AVX2_CFLAGS := -mavx2
endif
CFLAGS ?= $(BASE_CFLAGS) $(AVX2_CFLAGS)

BIN_DIR := bin
SRC_DIR := src
TEST_DIR := test

K6 ?= k6
JQ ?= jq
COMPOSE ?= docker compose
COMPOSE_MACOS ?= docker compose -f docker-compose.yml -f docker-compose.macos.yml

COMMON_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/index_format.h
QUANTIZE_SRCS := $(SRC_DIR)/quantize.c
TIME_SRCS := $(SRC_DIR)/time_utils.c
VECTORIZE_SRCS := $(SRC_DIR)/vectorize.c $(SRC_DIR)/vector_features.c
QUANTIZE_HDRS := $(SRC_DIR)/quantize.h $(SRC_DIR)/common.h
TIME_HDRS := $(SRC_DIR)/time_utils.h
INDEX_HDRS := $(COMMON_HDRS) $(SRC_DIR)/index.h
API_HTTP_HDRS := $(SRC_DIR)/api_http.h
VECTORIZE_HDRS := $(COMMON_HDRS) $(TIME_HDRS) $(SRC_DIR)/vectorize.h $(SRC_DIR)/vectorize_payload.h

all: $(BIN_DIR)/preprocess $(BIN_DIR)/fraud_api $(BIN_DIR)/fraud_lb

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/preprocess: $(BIN_DIR) $(SRC_DIR)/preprocess.c $(QUANTIZE_SRCS) $(COMMON_HDRS) $(QUANTIZE_HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/preprocess.c $(QUANTIZE_SRCS) -lz -lm

$(BIN_DIR)/fraud_api: $(BIN_DIR) $(SRC_DIR)/api.c $(SRC_DIR)/api_http.c $(QUANTIZE_SRCS) $(TIME_SRCS) $(SRC_DIR)/index.c $(VECTORIZE_SRCS) $(API_HTTP_HDRS) $(INDEX_HDRS) $(VECTORIZE_HDRS) $(QUANTIZE_HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/api.c $(SRC_DIR)/api_http.c $(QUANTIZE_SRCS) $(TIME_SRCS) $(SRC_DIR)/index.c $(VECTORIZE_SRCS) -luring -lz -lm

$(BIN_DIR)/fraud_lb: $(BIN_DIR) $(SRC_DIR)/lb.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/lb.c -luring -lm

test:
	K6_NO_USAGE_REPORT=true $(K6) run $(TEST_DIR)/test.js > /dev/null 2>&1
	$(JQ) . $(TEST_DIR)/results.json

up:
	$(COMPOSE) up -d --build

down:
	$(COMPOSE) down

up-macos:
	$(COMPOSE_MACOS) up -d --build

down-macos:
	$(COMPOSE_MACOS) down

test-ci:
	$(COMPOSE) down
	$(COMPOSE) up -d --build
	@set -e; \
	stats_pid=""; \
	trap 'if [ -n "$$stats_pid" ]; then kill "$$stats_pid" 2>/dev/null || true; wait "$$stats_pid" 2>/dev/null || true; fi' EXIT INT TERM; \
	( while :; do \
		echo "[docker stats]"; \
		docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}" $$($(COMPOSE) ps -q) || true; \
		sleep 1; \
	done ) & \
	stats_pid=$$!; \
	K6_NO_USAGE_REPORT=true $(K6) run $(TEST_DIR)/test.js; \
	$(JQ) . $(TEST_DIR)/results.json

test-ci-macos-html:
	$(COMPOSE_MACOS) down
	$(COMPOSE_MACOS) up -d --build
	K6_WEB_DASHBOARD=true \
	K6_WEB_DASHBOARD_PORT=5665 \
	K6_WEB_DASHBOARD_PERIOD=1s \
	K6_WEB_DASHBOARD_OPEN=true \
	K6_WEB_DASHBOARD_EXPORT=report.html \
	$(K6) run $(TEST_DIR)/test.js
	$(JQ) . $(TEST_DIR)/results.json

test-ci-macos:
	$(COMPOSE_MACOS) down
	$(COMPOSE_MACOS) up -d --build
	K6_NO_USAGE_REPORT=true $(K6) run $(TEST_DIR)/test.js > /dev/null 2>&1
	$(JQ) . $(TEST_DIR)/results.json

run-test: test

clean:
	rm -rf $(BIN_DIR)

publish:
	docker buildx build --platform linux/amd64 -t distanteagle16/rinha-2026:v5 --push .

.PHONY: all test up down up-macos down-macos test-ci test-ci-macos run-test clean
