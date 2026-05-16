#include "api_http.h"
#include "index.h"
#include "vectorize.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <liburing.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define API_BACKLOG 1024
#define API_QUEUE_DEPTH 2048
#define API_MAX_CONNECTIONS 2048
#define API_READ_BUFFER 16384
#define API_WRITE_BUFFER 1024
#define API_CQE_BATCH 64
#define API_SOCKET_PATH_DEFAULT "/run/rinha/api.sock"
#define API_SEND_FLAGS MSG_NOSIGNAL

#if defined(IORING_OP_SEND_ZC) && defined(SO_ZEROCOPY)
#define API_HAVE_SEND_ZC 1
#else
#define API_HAVE_SEND_ZC 0
#endif

#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE 0u
#endif

#ifndef IORING_CQE_F_NOTIF
#define IORING_CQE_F_NOTIF 0u
#endif

#ifndef IORING_SEND_ZC_REPORT_USAGE
#define IORING_SEND_ZC_REPORT_USAGE 0u
#endif

typedef enum {
    API_OP_ACCEPT = 1,
    API_OP_RECV = 2,
    API_OP_SEND = 3,
} api_op_type_t;

typedef struct {
    struct sockaddr_storage addr;
    socklen_t addr_len;
} api_accept_state_t;

typedef struct {
    int fd;
    bool used;
    bool keep_alive;
    bool zerocopy_enabled;
    bool send_inflight_zerocopy;
    bool send_complete_waiting_notif;
    bool close_pending;
    uint32_t generation;
    unsigned inflight_ops;
    unsigned pending_send_notifs;
    size_t read_len;
    size_t write_len;
    size_t write_sent;
    uint64_t request_started_ns;
    uint32_t request_recv_ops;
    uint32_t request_recv_bytes;
    const char *write_ptr;
    char read_buf[API_READ_BUFFER + 1];
    char write_buf[API_WRITE_BUFFER];
} api_conn_t;

typedef struct {
    uint64_t user_data;
    int res;
    unsigned flags;
} api_cqe_record_t;

typedef struct {
    bool initialized;
    bool enabled;
    uint64_t report_every;
    uint64_t requests_completed;
    uint64_t business_calls;
    uint64_t fraud_score_requests;
    uint64_t ready_requests;
    uint64_t keep_alive_requests;
    uint64_t close_requests;
    uint64_t recv_ops;
    uint64_t recv_bytes;
    uint64_t recv_wall_ns;
    uint64_t parse_ops;
    uint64_t parse_ns;
    uint64_t business_ns;
    uint64_t business_payload_parse_ns;
    uint64_t business_vectorize_ns;
    uint64_t business_index_ns;
    uint64_t business_finalize_ns;
} api_profile_t;

static volatile sig_atomic_t api_running = 1;
static api_profile_t api_profile = {0};

static bool api_env_truthy(const char *value) {
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "false") == 0 ||
        strcmp(value, "False") == 0 ||
        strcmp(value, "FALSE") == 0 ||
        strcmp(value, "no") == 0 ||
        strcmp(value, "No") == 0 ||
        strcmp(value, "NO") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "Off") == 0 ||
        strcmp(value, "OFF") == 0) {
        return false;
    }
    return true;
}

static uint64_t api_env_u64(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0ull) {
        return fallback;
    }
    return (uint64_t) parsed;
}

static uint64_t api_now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static double api_avg_us(uint64_t total_ns, uint64_t count) {
    if (count == 0u) {
        return 0.0;
    }
    return (double) total_ns / (double) count / 1000.0;
}

static double api_avg_count(uint64_t total, uint64_t count) {
    if (count == 0u) {
        return 0.0;
    }
    return (double) total / (double) count;
}

static void api_profile_report(const char *reason) {
    if (!api_profile.enabled) {
        return;
    }

    double avg_recv_us = api_avg_us(api_profile.recv_wall_ns, api_profile.requests_completed);
    double avg_business_us = api_avg_us(api_profile.business_ns, api_profile.business_calls);
    double business_vs_recv = avg_recv_us > 0.0 ? avg_business_us / avg_recv_us : 0.0;
    double avg_payload_parse_us = api_avg_us(api_profile.business_payload_parse_ns, api_profile.business_calls);
    double avg_vectorize_us = api_avg_us(api_profile.business_vectorize_ns, api_profile.business_calls);
    double avg_index_us = api_avg_us(api_profile.business_index_ns, api_profile.business_calls);
    double avg_finalize_us = api_avg_us(api_profile.business_finalize_ns, api_profile.business_calls);

    fprintf(
        stderr,
        "[api-prof] reason=%s requests=%" PRIu64
        " fraud_requests=%" PRIu64 " ready_requests=%" PRIu64
        " keep_alive=%" PRIu64 " close=%" PRIu64
        " avg_request_recv_us=%.2f avg_parse_us=%.2f avg_handle_business_us=%.2f"
        " avg_payload_parse_us=%.2f avg_vectorize_us=%.2f avg_index_us=%.2f avg_finalize_us=%.2f"
        " handle_vs_recv=%.2fx avg_recv_ops=%.2f avg_request_bytes=%.2f\n",
        reason,
        api_profile.requests_completed,
        api_profile.fraud_score_requests,
        api_profile.ready_requests,
        api_profile.keep_alive_requests,
        api_profile.close_requests,
        avg_recv_us,
        api_avg_us(api_profile.parse_ns, api_profile.parse_ops),
        avg_business_us,
        avg_payload_parse_us,
        avg_vectorize_us,
        avg_index_us,
        avg_finalize_us,
        business_vs_recv,
        api_avg_count(api_profile.recv_ops, api_profile.requests_completed),
        api_avg_count(api_profile.recv_bytes, api_profile.requests_completed)
    );
}

static void api_profile_flush(void) {
    api_profile_report("exit");
}

static void api_profile_init(void) {
    if (api_profile.initialized) {
        return;
    }

    api_profile.initialized = true;
    api_profile.enabled = api_env_truthy(getenv("RINHA_API_PROFILE"));
    api_profile.report_every = api_env_u64("RINHA_API_PROFILE_EVERY", 1000u);
    if (api_profile.enabled) {
        atexit(api_profile_flush);
    }
}

static void api_profile_on_recv_complete(api_conn_t *conn, size_t bytes) {
    if (!api_profile.enabled) {
        return;
    }
    if (conn->request_started_ns == 0u) {
        conn->request_started_ns = api_now_ns();
        conn->request_recv_ops = 0u;
        conn->request_recv_bytes = 0u;
    }
    conn->request_recv_ops++;
    conn->request_recv_bytes += (uint32_t) bytes;
}

static void api_profile_note_parse(uint64_t elapsed_ns) {
    if (!api_profile.enabled) {
        return;
    }
    api_profile.parse_ops++;
    api_profile.parse_ns += elapsed_ns;
}

static void api_profile_complete_request(api_conn_t *conn, const api_request_t *request) {
    if (!api_profile.enabled) {
        return;
    }

    api_profile.requests_completed++;
    api_profile.recv_ops += conn->request_recv_ops;
    api_profile.recv_bytes += conn->request_recv_bytes;
    if (conn->request_started_ns != 0u) {
        api_profile.recv_wall_ns += api_now_ns() - conn->request_started_ns;
    }
    if (request != NULL) {
        if (request->route == API_ROUTE_FRAUD_SCORE) {
            api_profile.fraud_score_requests++;
        } else if (request->route == API_ROUTE_READY) {
            api_profile.ready_requests++;
        }
        if (request->keep_alive) {
            api_profile.keep_alive_requests++;
        } else {
            api_profile.close_requests++;
        }
    }

    conn->request_started_ns = 0u;
    conn->request_recv_ops = 0u;
    conn->request_recv_bytes = 0u;

    if (api_profile.report_every > 0u &&
        api_profile.requests_completed % api_profile.report_every == 0u) {
        api_profile_report("periodic");
    }
}

static void api_profile_note_business(uint64_t elapsed_ns) {
    if (!api_profile.enabled) {
        return;
    }
    api_profile.business_calls++;
    api_profile.business_ns += elapsed_ns;
}

static void api_profile_note_business_breakdown(
    uint64_t payload_parse_ns,
    uint64_t vectorize_ns,
    uint64_t index_ns,
    uint64_t finalize_ns
) {
    if (!api_profile.enabled) {
        return;
    }
    api_profile.business_payload_parse_ns += payload_parse_ns;
    api_profile.business_vectorize_ns += vectorize_ns;
    api_profile.business_index_ns += index_ns;
    api_profile.business_finalize_ns += finalize_ns;
}

static void api_on_signal(int signo) {
    (void) signo;
    api_running = 0;
}

static uint64_t api_pack_user_data(api_op_type_t type, int conn_index, uint32_t generation) {
    uint64_t packed = (uint64_t) (uint8_t) type;
    packed |= (uint64_t) (uint16_t) (conn_index + 1) << 8;
    packed |= (uint64_t) generation << 32;
    return packed;
}

static void api_unpack_user_data(uint64_t packed, api_op_type_t *type, int *conn_index, uint32_t *generation) {
    *type = (api_op_type_t) (packed & 0xffu);
    *conn_index = (int) (((packed >> 8) & 0xffffu) - 1u);
    *generation = (uint32_t) (packed >> 32);
}

static int api_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int api_open_listener(const char *socket_path) {
    if (socket_path == NULL || socket_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(((struct sockaddr_un *) 0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    if (api_set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    unlink(socket_path);

    socklen_t addr_len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
    if (bind(fd, (struct sockaddr *) &addr, addr_len) != 0 || listen(fd, API_BACKLOG) != 0) {
        unlink(socket_path);
        close(fd);
        return -1;
    }

    return fd;
}

static bool api_try_enable_zerocopy(int fd) {
#if API_HAVE_SEND_ZC
    static int disabled = -1;
    if (disabled < 0) {
        const char *env = getenv("RINHA_DISABLE_SEND_ZC");
        disabled = env != NULL && strcmp(env, "0") != 0 && env[0] != '\0';
    }
    if (disabled) {
        return false;
    }
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) == 0;
#else
    (void) fd;
    return false;
#endif
}

static void api_finalize_conn(api_conn_t *conn, int conn_index, int *free_slots, size_t *free_count) {
    bool was_used = conn->used;
    if (conn->fd >= 0) {
        close(conn->fd);
    }

    conn->fd = -1;
    conn->used = false;
    conn->keep_alive = false;
    conn->zerocopy_enabled = false;
    conn->send_inflight_zerocopy = false;
    conn->send_complete_waiting_notif = false;
    conn->close_pending = false;
    conn->inflight_ops = 0u;
    conn->pending_send_notifs = 0u;
    conn->read_len = 0;
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->request_started_ns = 0u;
    conn->request_recv_ops = 0u;
    conn->request_recv_bytes = 0u;
    conn->write_ptr = conn->write_buf;

    if (was_used && conn_index >= 0) {
        free_slots[(*free_count)++] = conn_index;
    }
}

static int api_alloc_conn(api_conn_t *connections, int *free_slots, size_t *free_count) {
    if (*free_count == 0) {
        return -1;
    }

    int i = free_slots[--(*free_count)];
    api_conn_t *conn = &connections[i];
    uint32_t next_generation = conn->generation + 1u;
    conn->used = true;
    conn->fd = -1;
    conn->keep_alive = false;
    conn->zerocopy_enabled = false;
    conn->send_inflight_zerocopy = false;
    conn->send_complete_waiting_notif = false;
    conn->close_pending = false;
    conn->generation = next_generation == 0u ? 1u : next_generation;
    conn->inflight_ops = 0u;
    conn->pending_send_notifs = 0u;
    conn->read_len = 0;
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->request_started_ns = 0u;
    conn->request_recv_ops = 0u;
    conn->request_recv_bytes = 0u;
    conn->write_ptr = conn->write_buf;
    return i;
}

static void api_request_close(api_conn_t *conn) {
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->keep_alive = false;
    conn->close_pending = true;
    conn->request_started_ns = 0u;
    conn->request_recv_ops = 0u;
    conn->request_recv_bytes = 0u;
}

static void api_maybe_finalize_conn(
    api_conn_t *conn,
    int conn_index,
    int *free_slots,
    size_t *free_count
) {
    if (conn->close_pending && conn->inflight_ops == 0u && conn->pending_send_notifs == 0u) {
        api_finalize_conn(conn, conn_index, free_slots, free_count);
    }
}

static int api_queue_accept(struct io_uring *ring, int listen_fd, api_accept_state_t *state) {
    state->addr_len = sizeof(state->addr);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_accept(sqe, listen_fd, (struct sockaddr *) &state->addr, &state->addr_len, 0);
    io_uring_sqe_set_data64(sqe, api_pack_user_data(API_OP_ACCEPT, -1, 0));
    return 0;
}

static int api_queue_recv(struct io_uring *ring, api_conn_t *conn, int conn_index) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_recv(sqe, conn->fd, conn->read_buf + conn->read_len, API_READ_BUFFER - conn->read_len, 0);
    io_uring_sqe_set_data64(sqe, api_pack_user_data(API_OP_RECV, conn_index, conn->generation));
    conn->inflight_ops++;
    return 0;
}

static int api_queue_send(struct io_uring *ring, api_conn_t *conn, int conn_index) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    const void *buf = conn->write_ptr + conn->write_sent;
    size_t len = conn->write_len - conn->write_sent;
#if API_HAVE_SEND_ZC
    if (conn->zerocopy_enabled) {
        io_uring_prep_send_zc(sqe, conn->fd, buf, len, API_SEND_FLAGS, IORING_SEND_ZC_REPORT_USAGE);
        conn->send_inflight_zerocopy = true;
    } else
#endif
    {
        io_uring_prep_send(sqe, conn->fd, buf, len, API_SEND_FLAGS);
        conn->send_inflight_zerocopy = false;
    }
    io_uring_sqe_set_data64(sqe, api_pack_user_data(API_OP_SEND, conn_index, conn->generation));
    conn->inflight_ops++;
    return 0;
}

static int api_flush_submissions(struct io_uring *ring) {
    return io_uring_submit(ring);
}

static ssize_t api_collect_cqes(struct io_uring *ring, api_cqe_record_t *cqes, size_t max_cqes) {
    if (max_cqes == 0u) {
        return 0;
    }

    struct io_uring_cqe *cqe = NULL;
    int wait_rc = io_uring_wait_cqe(ring, &cqe);
    if (wait_rc != 0) {
        if (wait_rc == -EINTR) {
            return 0;
        }
        return -1;
    }

    size_t count = 0u;
    cqes[count++] = (api_cqe_record_t) {
        .user_data = io_uring_cqe_get_data64(cqe),
        .res = cqe->res,
        .flags = cqe->flags,
    };
    io_uring_cqe_seen(ring, cqe);

    while (count < max_cqes) {
        struct io_uring_cqe *batch_cqes[API_CQE_BATCH];
        unsigned batch = io_uring_peek_batch_cqe(ring, batch_cqes, (unsigned) (max_cqes - count));
        if (batch == 0u) {
            break;
        }
        for (unsigned i = 0; i < batch && count < max_cqes; i++) {
            cqes[count++] = (api_cqe_record_t) {
                .user_data = io_uring_cqe_get_data64(batch_cqes[i]),
                .res = batch_cqes[i]->res,
                .flags = batch_cqes[i]->flags,
            };
            io_uring_cqe_seen(ring, batch_cqes[i]);
        }
        if (batch < (unsigned) (max_cqes - count + batch)) {
            break;
        }
    }

    return (ssize_t) count;
}

static char *api_copy_str(char *dst, const char *src) {
    size_t len = strlen(src);
    memcpy(dst, src, len);
    return dst + len;
}

static char *api_write_size(char *dst, size_t value) {
    char tmp[32];
    size_t len = 0;
    do {
        tmp[len++] = (char) ('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u);

    while (len > 0u) {
        *dst++ = tmp[--len];
    }
    return dst;
}

typedef struct {
    const char *data;
    size_t len;
} api_static_response_t;

#define API_STATIC_RESPONSE_LITERAL(literal) { literal, sizeof(literal) - 1u }
#define API_FRAUD_RESPONSE_CLOSE(content_length, body) \
    API_STATIC_RESPONSE_LITERAL( \
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " content_length \
        "\r\nConnection: close\r\n\r\n" body \
    )
#define API_FRAUD_RESPONSE_KEEP_ALIVE(content_length, body) \
    API_STATIC_RESPONSE_LITERAL( \
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " content_length \
        "\r\nConnection: keep-alive\r\n\r\n" body \
    )

static const api_static_response_t API_FRAUD_RESPONSES[2][6] = {
    {
        API_FRAUD_RESPONSE_CLOSE("35", "{\"approved\":true,\"fraud_score\":0.0}"),
        API_FRAUD_RESPONSE_CLOSE("35", "{\"approved\":true,\"fraud_score\":0.2}"),
        API_FRAUD_RESPONSE_CLOSE("35", "{\"approved\":true,\"fraud_score\":0.4}"),
        API_FRAUD_RESPONSE_CLOSE("36", "{\"approved\":false,\"fraud_score\":0.6}"),
        API_FRAUD_RESPONSE_CLOSE("36", "{\"approved\":false,\"fraud_score\":0.8}"),
        API_FRAUD_RESPONSE_CLOSE("36", "{\"approved\":false,\"fraud_score\":1.0}"),
    },
    {
        API_FRAUD_RESPONSE_KEEP_ALIVE("35", "{\"approved\":true,\"fraud_score\":0.0}"),
        API_FRAUD_RESPONSE_KEEP_ALIVE("35", "{\"approved\":true,\"fraud_score\":0.2}"),
        API_FRAUD_RESPONSE_KEEP_ALIVE("35", "{\"approved\":true,\"fraud_score\":0.4}"),
        API_FRAUD_RESPONSE_KEEP_ALIVE("36", "{\"approved\":false,\"fraud_score\":0.6}"),
        API_FRAUD_RESPONSE_KEEP_ALIVE("36", "{\"approved\":false,\"fraud_score\":0.8}"),
        API_FRAUD_RESPONSE_KEEP_ALIVE("36", "{\"approved\":false,\"fraud_score\":1.0}"),
    },
};

#undef API_FRAUD_RESPONSE_KEEP_ALIVE
#undef API_FRAUD_RESPONSE_CLOSE
#undef API_STATIC_RESPONSE_LITERAL

static void api_prepare_response_len(
    api_conn_t *conn,
    const char *status,
    const char *content_type,
    const char *body,
    size_t body_len,
    bool keep_alive
) {
    const char *connection = keep_alive ? "keep-alive" : "close";
    char *p = conn->write_buf;
    char *end = conn->write_buf + sizeof(conn->write_buf);

    p = api_copy_str(p, "HTTP/1.1 ");
    p = api_copy_str(p, status);
    p = api_copy_str(p, "\r\nContent-Type: ");
    p = api_copy_str(p, content_type);
    p = api_copy_str(p, "\r\nContent-Length: ");
    p = api_write_size(p, body_len);
    p = api_copy_str(p, "\r\nConnection: ");
    p = api_copy_str(p, connection);
    p = api_copy_str(p, "\r\n\r\n");

    if (body_len > 0u) {
        if (p + body_len > end) {
            body_len = (size_t) (end - p);
        }
        memcpy(p, body == NULL ? "" : body, body_len);
        p += body_len;
    }

    conn->write_ptr = conn->write_buf;
    conn->write_len = (size_t) (p - conn->write_buf);
    conn->write_sent = 0;
    conn->keep_alive = keep_alive;
}

static void api_prepare_response(
    api_conn_t *conn,
    const char *status,
    const char *content_type,
    const char *body,
    bool keep_alive
) {
    size_t body_len = body == NULL ? 0u : strlen(body);
    api_prepare_response_len(conn, status, content_type, body, body_len, keep_alive);
}

static void api_prepare_no_content(api_conn_t *conn, bool keep_alive) {
    char *p = conn->write_buf;
    p = api_copy_str(p, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: ");
    p = api_copy_str(p, keep_alive ? "keep-alive" : "close");
    p = api_copy_str(p, "\r\n\r\n");
    conn->write_ptr = conn->write_buf;
    conn->write_len = (size_t) (p - conn->write_buf);
    conn->write_sent = 0;
    conn->keep_alive = keep_alive;
}

static void api_prepare_static_response(
    api_conn_t *conn,
    const api_static_response_t *response,
    bool keep_alive
) {
    conn->write_ptr = response->data;
    conn->write_len = response->len;
    conn->write_sent = 0;
    conn->keep_alive = keep_alive;
}

static void api_handle_business(api_conn_t *conn, const api_request_t *request, rinha_index_t *index) {
    if (request->route == API_ROUTE_READY) {
        api_prepare_no_content(conn, request->keep_alive);
        return;
    }

    if (request->route != API_ROUTE_FRAUD_SCORE) {
        api_prepare_response(conn, "404 Not Found", "text/plain", "not found", false);
        return;
    }

    rinha_tx_payload_t payload;
    uint64_t step_started_ns = api_profile.enabled ? api_now_ns() : 0u;
    if (!rinha_parse_tx_payload(request->body, request->content_length, &payload)) {
        uint64_t payload_parse_ns = api_profile.enabled && step_started_ns != 0u ? api_now_ns() - step_started_ns : 0u;
        api_profile_note_business_breakdown(payload_parse_ns, 0u, 0u, 0u);
        api_prepare_response(conn, "400 Bad Request", "text/plain", "invalid payload", false);
        return;
    }
    uint64_t payload_parse_ns = api_profile.enabled && step_started_ns != 0u ? api_now_ns() - step_started_ns : 0u;

    float vector[RINHA_DIM];
    step_started_ns = api_profile.enabled ? api_now_ns() : 0u;
    rinha_payload_to_vector(&payload, vector);
    uint64_t vectorize_ns = api_profile.enabled && step_started_ns != 0u ? api_now_ns() - step_started_ns : 0u;

    step_started_ns = api_profile.enabled ? api_now_ns() : 0u;
    int fraud_count = rinha_index_fraud_count_top5(index, vector);
    uint64_t index_ns = api_profile.enabled && step_started_ns != 0u ? api_now_ns() - step_started_ns : 0u;

    step_started_ns = api_profile.enabled ? api_now_ns() : 0u;
    if (fraud_count == 2 && rinha_payload_force_deny_borderline(&payload)) {
        fraud_count = 3;
    }
    const api_static_response_t *response = &API_FRAUD_RESPONSES[request->keep_alive ? 1u : 0u][fraud_count];
    api_prepare_static_response(conn, response, request->keep_alive);
    uint64_t finalize_ns = api_profile.enabled && step_started_ns != 0u ? api_now_ns() - step_started_ns : 0u;
    api_profile_note_business_breakdown(payload_parse_ns, vectorize_ns, index_ns, finalize_ns);
}

static int api_finish_response(
    struct io_uring *ring,
    api_conn_t *conn,
    int conn_index,
    int *free_slots,
    size_t *free_count
) {
    if (conn->pending_send_notifs > 0u) {
        conn->send_complete_waiting_notif = true;
        return 0;
    }

    conn->send_complete_waiting_notif = false;
    if (!conn->keep_alive) {
        api_request_close(conn);
        api_maybe_finalize_conn(conn, conn_index, free_slots, free_count);
        return 0;
    }

    conn->read_len = 0;
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->write_ptr = conn->write_buf;
    if (api_queue_recv(ring, conn, conn_index) < 0) {
        return -1;
    }
    return 1;
}

static bool api_send_zc_should_fallback(bool used_zerocopy, int res) {
#if API_HAVE_SEND_ZC
    return used_zerocopy
        && (res == -EINVAL || res == -EOPNOTSUPP || res == -ENOBUFS || res == -ENOMEM);
#else
    (void) used_zerocopy;
    (void) res;
    return false;
#endif
}

int main(void) {
    const char *index_path = getenv("RINHA_INDEX_PATH");
    if (index_path == NULL) {
        index_path = "/opt/rinha/index.bin";
    }
    const char *socket_path = getenv("API_SOCKET_PATH");
    if (socket_path == NULL) {
        socket_path = API_SOCKET_PATH_DEFAULT;
    }

    signal(SIGINT, api_on_signal);
    signal(SIGTERM, api_on_signal);
    api_profile_init();

    rinha_index_t index;
    if (!rinha_index_open(&index, index_path)) {
        fprintf(stderr, "falha ao abrir indice %s\n", index_path);
        return 1;
    }

    int listen_fd = api_open_listener(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "falha ao abrir socket unix %s\n", socket_path);
        rinha_index_close(&index);
        return 1;
    }

    struct io_uring ring;
    if (io_uring_queue_init(API_QUEUE_DEPTH, &ring, 0) != 0) {
        fprintf(stderr, "falha ao inicializar io_uring\n");
        close(listen_fd);
        unlink(socket_path);
        rinha_index_close(&index);
        return 1;
    }

    api_conn_t *connections = calloc(API_MAX_CONNECTIONS, sizeof(api_conn_t));
    int *free_slots = malloc((size_t) API_MAX_CONNECTIONS * sizeof(int));
    if (connections == NULL || free_slots == NULL) {
        free(connections);
        free(free_slots);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        unlink(socket_path);
        rinha_index_close(&index);
        return 1;
    }
    for (int i = 0; i < API_MAX_CONNECTIONS; i++) {
        connections[i].fd = -1;
        free_slots[i] = API_MAX_CONNECTIONS - 1 - i;
    }
    size_t free_count = API_MAX_CONNECTIONS;

    api_accept_state_t accept_state;
    memset(&accept_state, 0, sizeof(accept_state));
    int accept_rc = api_queue_accept(&ring, listen_fd, &accept_state);
    if (accept_rc < 0) {
        fprintf(stderr, "falha ao submeter accept inicial no io_uring: rc=%d\n", accept_rc);
        free(connections);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        unlink(socket_path);
        rinha_index_close(&index);
        return 1;
    }
    if (api_flush_submissions(&ring) < 0) {
        fprintf(stderr, "falha ao submeter accept inicial no io_uring\n");
        free(connections);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        unlink(socket_path);
        rinha_index_close(&index);
        return 1;
    }

    api_cqe_record_t cqes[API_CQE_BATCH];
    while (api_running) {
        ssize_t cqe_count = api_collect_cqes(&ring, cqes, API_CQE_BATCH);
        if (cqe_count < 0) {
            break;
        }
        if (cqe_count == 0) {
            continue;
        }

        bool need_submit = false;
        bool fatal_error = false;
        for (ssize_t i = 0; i < cqe_count; i++) {
            uint64_t user_data = cqes[i].user_data;
            api_op_type_t type;
            int conn_index;
            uint32_t generation;
            int res = cqes[i].res;
            unsigned flags = cqes[i].flags;
            api_unpack_user_data(user_data, &type, &conn_index, &generation);

            if (type == API_OP_ACCEPT) {
                if (res >= 0) {
                    int accepted_conn_index = api_alloc_conn(connections, free_slots, &free_count);
                    if (accepted_conn_index >= 0) {
                        api_conn_t *conn = &connections[accepted_conn_index];
                        conn->fd = res;
                        api_set_nonblocking(conn->fd);
                        conn->zerocopy_enabled = api_try_enable_zerocopy(conn->fd);
                        if (api_queue_recv(&ring, conn, accepted_conn_index) < 0) {
                            api_request_close(conn);
                            api_maybe_finalize_conn(conn, accepted_conn_index, free_slots, &free_count);
                            fatal_error = true;
                        } else {
                            need_submit = true;
                        }
                    } else {
                        close(res);
                    }
                }
                if (api_queue_accept(&ring, listen_fd, &accept_state) < 0) {
                    fatal_error = true;
                } else {
                    need_submit = true;
                }
                continue;
            }

            if (conn_index < 0 || conn_index >= API_MAX_CONNECTIONS) {
                continue;
            }
            api_conn_t *conn = &connections[conn_index];
            if (!conn->used || conn->generation != generation) {
                continue;
            }
            if ((flags & IORING_CQE_F_NOTIF) == 0u && conn->inflight_ops > 0u) {
                conn->inflight_ops--;
            }

            if (type == API_OP_RECV) {
                if (res <= 0) {
                    api_request_close(conn);
                    api_maybe_finalize_conn(conn, conn_index, free_slots, &free_count);
                    continue;
                }

                api_profile_on_recv_complete(conn, (size_t) res);
                conn->read_len += (size_t) res;
                conn->read_buf[conn->read_len] = '\0';

                api_request_t request;
                uint64_t parse_started_ns = api_profile.enabled ? api_now_ns() : 0u;
                int parse_rc = api_parse_http_request(conn->read_buf, conn->read_len, &request);
                if (api_profile.enabled && parse_started_ns != 0u) {
                    api_profile_note_parse(api_now_ns() - parse_started_ns);
                }
                if (parse_rc == 0) {
                    if (conn->read_len >= API_READ_BUFFER) {
                        api_profile_complete_request(conn, NULL);
                        api_prepare_response(conn, "413 Payload Too Large", "text/plain", "payload too large", false);
                        if (api_queue_send(&ring, conn, conn_index) < 0) {
                            fatal_error = true;
                        } else {
                            need_submit = true;
                        }
                    } else if (api_queue_recv(&ring, conn, conn_index) < 0) {
                        fatal_error = true;
                    } else {
                        need_submit = true;
                    }
                    continue;
                }

                if (parse_rc < 0) {
                    api_profile_complete_request(conn, NULL);
                    api_prepare_response(conn, "400 Bad Request", "text/plain", "bad request", false);
                } else {
                    api_profile_complete_request(conn, &request);
                    uint64_t business_started_ns = api_profile.enabled ? api_now_ns() : 0u;
                    api_handle_business(conn, &request, &index);
                    if (api_profile.enabled && business_started_ns != 0u) {
                        api_profile_note_business(api_now_ns() - business_started_ns);
                    }
                }

                if (api_queue_send(&ring, conn, conn_index) < 0) {
                    fatal_error = true;
                } else {
                    need_submit = true;
                }
                continue;
            }

            if (type == API_OP_SEND) {
                if ((flags & IORING_CQE_F_NOTIF) != 0u) {
                    if (conn->pending_send_notifs > 0u) {
                        conn->pending_send_notifs--;
                    }
                    if (res != 0) {
                        conn->zerocopy_enabled = false;
                    }
                    if (conn->send_complete_waiting_notif && conn->pending_send_notifs == 0u) {
                        int finish_rc = api_finish_response(&ring, conn, conn_index, free_slots, &free_count);
                        if (finish_rc < 0) {
                            fatal_error = true;
                        } else if (finish_rc > 0) {
                            need_submit = true;
                        }
                    }
                    api_maybe_finalize_conn(conn, conn_index, free_slots, &free_count);
                    continue;
                }

                bool used_zerocopy = conn->send_inflight_zerocopy;
                conn->send_inflight_zerocopy = false;
                if (api_send_zc_should_fallback(used_zerocopy, res)) {
                    conn->zerocopy_enabled = false;
                    if (api_queue_send(&ring, conn, conn_index) < 0) {
                        fatal_error = true;
                    } else {
                        need_submit = true;
                    }
                    continue;
                }
                if (res <= 0) {
                    api_request_close(conn);
                    api_maybe_finalize_conn(conn, conn_index, free_slots, &free_count);
                    continue;
                }

                if (used_zerocopy && (flags & IORING_CQE_F_MORE) != 0u) {
                    conn->pending_send_notifs++;
                }
                conn->write_sent += (size_t) res;
                if (conn->write_sent < conn->write_len) {
                    if (api_queue_send(&ring, conn, conn_index) < 0) {
                        fatal_error = true;
                    } else {
                        need_submit = true;
                    }
                    continue;
                }

                int finish_rc = api_finish_response(&ring, conn, conn_index, free_slots, &free_count);
                if (finish_rc < 0) {
                    fatal_error = true;
                } else if (finish_rc > 0) {
                    need_submit = true;
                }
            }
        }

        if (need_submit && api_flush_submissions(&ring) < 0) {
            fatal_error = true;
        }
        if (fatal_error) {
            break;
        }
    }

    for (int i = 0; i < API_MAX_CONNECTIONS; i++) {
        if (connections[i].used) {
            api_finalize_conn(&connections[i], i, free_slots, &free_count);
        }
    }
    free(connections);
    free(free_slots);
    io_uring_queue_exit(&ring);
    close(listen_fd);
    unlink(socket_path);
    rinha_index_close(&index);
    return 0;
}
