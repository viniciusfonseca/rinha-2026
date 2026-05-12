CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
LDFLAGS ?=

BIN_DIR := bin
SRC_DIR := src
TEST_DIR := test

K6 ?= k6
JQ ?= jq
COMPOSE ?= docker compose
COMPOSE_MACOS ?= docker compose -f docker-compose.yml -f docker-compose.macos.yml

COMMON_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/index_format.h
INDEX_HDRS := $(COMMON_HDRS) $(SRC_DIR)/index.h
VECTORIZE_HDRS := $(COMMON_HDRS) $(SRC_DIR)/vectorize.h

all: $(BIN_DIR)/preprocess $(BIN_DIR)/fraud_api $(BIN_DIR)/fraud_lb

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/preprocess: $(BIN_DIR) $(SRC_DIR)/preprocess.c $(SRC_DIR)/common.c $(COMMON_HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/preprocess.c $(SRC_DIR)/common.c -lz -lm

$(BIN_DIR)/fraud_api: $(BIN_DIR) $(SRC_DIR)/api.c $(SRC_DIR)/common.c $(SRC_DIR)/index.c $(SRC_DIR)/vectorize.c $(INDEX_HDRS) $(VECTORIZE_HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/api.c $(SRC_DIR)/common.c $(SRC_DIR)/index.c $(SRC_DIR)/vectorize.c -luring -lz -lm

$(BIN_DIR)/fraud_lb: $(BIN_DIR) $(SRC_DIR)/lb.c $(SRC_DIR)/common.c $(COMMON_HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/lb.c $(SRC_DIR)/common.c -luring -lm

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
	K6_NO_USAGE_REPORT=true $(K6) run $(TEST_DIR)/test.js > /dev/null 2>&1
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

.PHONY: all test up down up-macos down-macos test-ci test-ci-macos run-test clean
