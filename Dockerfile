FROM alpine:3.22 AS builder

RUN apk add --no-cache \
    build-base \
    ca-certificates \
    curl \
    liburing-dev \
    linux-headers \
    zlib-dev \
    zlib-static

WORKDIR /app

COPY Makefile ./
COPY src ./src

RUN make clean && make all LDFLAGS="-static"

RUN mkdir -p /opt/rinha && \
    curl -fsSL https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/resources/references.json.gz -o /opt/rinha/references.json.gz

RUN /app/bin/preprocess /opt/rinha/references.json.gz /opt/rinha/index.bin && \
    rm /opt/rinha/references.json.gz

FROM scratch

COPY --from=builder /app/bin/fraud_api /usr/local/bin/fraud_api
COPY --from=builder /app/bin/fraud_lb /usr/local/bin/fraud_lb
COPY --from=builder /opt/rinha/index.bin /opt/rinha/index.bin

ENV RINHA_INDEX_PATH=/opt/rinha/index.bin
