#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LB_PORT 9999
#define LB_BACKLOG 1024
#define LB_QUEUE_DEPTH 2048
/*
 * The LB container is capped at 48MB. Each session owns two relay buffers, so
 * overly large per-session state causes the process to OOM as concurrency grows.
 */
#define LB_MAX_SESSIONS 1024
#define LB_BUFFER_SIZE 4096
#define LB_MAX_BACKENDS 8
#define LB_CQE_BATCH 64
#define LB_SEND_FLAGS MSG_NOSIGNAL

#if defined(IORING_OP_SEND_ZC) && defined(SO_ZEROCOPY)
#define LB_HAVE_SEND_ZC 1
#else
#define LB_HAVE_SEND_ZC 0
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
    LB_OP_ACCEPT = 1,
    LB_OP_CONNECT = 2,
    LB_OP_READ = 3,
    LB_OP_WRITE = 4,
} lb_op_type_t;

typedef struct {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint64_t queued_at_ns;
} lb_accept_state_t;

typedef struct {
    struct sockaddr_storage addr;
    socklen_t addr_len;
} lb_backend_t;

typedef struct {
    int client_fd;
    int backend_fd;
    bool used;
    bool connected;
    bool zerocopy_enabled[2];
    bool send_inflight_zerocopy[2];
    bool send_complete_waiting_notif[2];
    bool close_pending;
    uint32_t generation;
    unsigned inflight_ops;
    unsigned pending_send_notifs[2];
    size_t buffer_len[2];
    size_t buffer_sent[2];
    uint64_t session_started_ns;
    uint64_t connect_started_ns;
    uint64_t read_started_ns[2];
    uint64_t write_started_ns[2];
    char buffer[2][LB_BUFFER_SIZE];
} lb_session_t;

typedef struct {
    uint64_t user_data;
    int res;
    unsigned flags;
} lb_cqe_record_t;

typedef struct {
    bool initialized;
    bool enabled;
    uint64_t report_every;
    uint64_t responses;
    uint64_t sessions_opened;
    uint64_t sessions_closed;
    uint64_t sessions_active;
    uint64_t sessions_active_max;
    uint64_t session_alloc_failures;
    uint64_t session_lifetime_ns;
    uint64_t accept_ops;
    uint64_t accept_failures;
    uint64_t accept_ns;
    uint64_t connect_ops;
    uint64_t connect_failures;
    uint64_t connect_ns;
    uint64_t read_ops[2];
    uint64_t read_failures[2];
    uint64_t read_eof[2];
    uint64_t read_bytes[2];
    uint64_t read_ns[2];
    uint64_t write_ops[2];
    uint64_t write_failures[2];
    uint64_t write_partial[2];
    uint64_t write_bytes[2];
    uint64_t write_ns[2];
    uint64_t write_zc_notifs[2];
    uint64_t write_zc_fallbacks[2];
    uint64_t wait_calls;
    uint64_t wait_ns;
    uint64_t cqe_batches;
    uint64_t cqe_total;
    uint64_t submit_calls;
    uint64_t submit_failures;
    uint64_t submit_sqes;
    uint64_t submit_ns;
} lb_profile_t;

static volatile sig_atomic_t lb_running = 1;
static lb_profile_t lb_profile = {0};

static bool lb_env_truthy(const char *value) {
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

static uint64_t lb_env_u64(const char *name, uint64_t fallback) {
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

static uint64_t lb_now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static double lb_avg_us(uint64_t total_ns, uint64_t count) {
    if (count == 0u) {
        return 0.0;
    }
    return (double) total_ns / (double) count / 1000.0;
}

static double lb_avg_count(uint64_t total, uint64_t count) {
    if (count == 0u) {
        return 0.0;
    }
    return (double) total / (double) count;
}

static void lb_profile_report(const char *reason) {
    if (!lb_profile.enabled) {
        return;
    }

    fprintf(
        stderr,
        "[lb-prof] reason=%s responses=%" PRIu64
        " sessions_opened=%" PRIu64 " sessions_closed=%" PRIu64
        " sessions_active=%" PRIu64 " max_sessions_active=%" PRIu64
        " alloc_failures=%" PRIu64
        " avg_session_ms=%.2f"
        " avg_accept_us=%.2f accept_failures=%" PRIu64
        " avg_connect_us=%.2f connect_failures=%" PRIu64
        " avg_read_client_us=%.2f avg_read_backend_us=%.2f"
        " avg_write_backend_us=%.2f avg_write_client_us=%.2f"
        " avg_wait_us=%.2f avg_cqes_per_wait=%.2f avg_sqes_per_submit=%.2f"
        " avg_client_read_bytes=%.2f avg_backend_read_bytes=%.2f"
        " avg_backend_write_bytes=%.2f avg_client_write_bytes=%.2f"
        " read_client_ops=%" PRIu64 " read_backend_ops=%" PRIu64
        " write_backend_ops=%" PRIu64 " write_client_ops=%" PRIu64
        " read_client_eof=%" PRIu64 " read_backend_eof=%" PRIu64
        " write_backend_partial=%" PRIu64 " write_client_partial=%" PRIu64
        " write_backend_zc_notifs=%" PRIu64 " write_client_zc_notifs=%" PRIu64
        " write_backend_zc_fallbacks=%" PRIu64 " write_client_zc_fallbacks=%" PRIu64 "\n",
        reason,
        lb_profile.responses,
        lb_profile.sessions_opened,
        lb_profile.sessions_closed,
        lb_profile.sessions_active,
        lb_profile.sessions_active_max,
        lb_profile.session_alloc_failures,
        lb_avg_count(lb_profile.session_lifetime_ns, lb_profile.sessions_closed) / 1000000.0,
        lb_avg_us(lb_profile.accept_ns, lb_profile.accept_ops),
        lb_profile.accept_failures,
        lb_avg_us(lb_profile.connect_ns, lb_profile.connect_ops),
        lb_profile.connect_failures,
        lb_avg_us(lb_profile.read_ns[0], lb_profile.read_ops[0]),
        lb_avg_us(lb_profile.read_ns[1], lb_profile.read_ops[1]),
        lb_avg_us(lb_profile.write_ns[0], lb_profile.write_ops[0]),
        lb_avg_us(lb_profile.write_ns[1], lb_profile.write_ops[1]),
        lb_avg_us(lb_profile.wait_ns, lb_profile.wait_calls),
        lb_avg_count(lb_profile.cqe_total, lb_profile.cqe_batches),
        lb_avg_count(lb_profile.submit_sqes, lb_profile.submit_calls),
        lb_avg_count(lb_profile.read_bytes[0], lb_profile.read_ops[0]),
        lb_avg_count(lb_profile.read_bytes[1], lb_profile.read_ops[1]),
        lb_avg_count(lb_profile.write_bytes[0], lb_profile.write_ops[0]),
        lb_avg_count(lb_profile.write_bytes[1], lb_profile.write_ops[1]),
        lb_profile.read_ops[0],
        lb_profile.read_ops[1],
        lb_profile.write_ops[0],
        lb_profile.write_ops[1],
        lb_profile.read_eof[0],
        lb_profile.read_eof[1],
        lb_profile.write_partial[0],
        lb_profile.write_partial[1],
        lb_profile.write_zc_notifs[0],
        lb_profile.write_zc_notifs[1],
        lb_profile.write_zc_fallbacks[0],
        lb_profile.write_zc_fallbacks[1]
    );
}

static void lb_profile_flush(void) {
    lb_profile_report("exit");
}

static void lb_profile_init(void) {
    if (lb_profile.initialized) {
        return;
    }

    lb_profile.initialized = true;
    lb_profile.enabled = lb_env_truthy(getenv("RINHA_LB_PROFILE"));
    lb_profile.report_every = lb_env_u64("RINHA_LB_PROFILE_EVERY", 1000u);

    if (lb_profile.enabled) {
        atexit(lb_profile_flush);
    }
}

static void lb_on_signal(int signo) {
    (void) signo;
    lb_running = 0;
}

static int lb_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int lb_open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (lb_set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, LB_BACKLOG) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool lb_try_enable_zerocopy(int fd) {
#if LB_HAVE_SEND_ZC
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

static uint64_t lb_pack_user_data(lb_op_type_t type, int session_index, int direction, uint32_t generation) {
    uint64_t packed = (uint64_t) (uint8_t) type;
    packed |= (uint64_t) (uint16_t) (session_index + 1) << 8;
    packed |= (uint64_t) (uint8_t) (direction + 1) << 24;
    packed |= (uint64_t) generation << 32;
    return packed;
}

static void lb_unpack_user_data(
    uint64_t packed,
    lb_op_type_t *type,
    int *session_index,
    int *direction,
    uint32_t *generation
) {
    *type = (lb_op_type_t) (packed & 0xffu);
    *session_index = (int) (((packed >> 8) & 0xffffu) - 1u);
    *direction = (int) (((packed >> 24) & 0xffu) - 1u);
    *generation = (uint32_t) (packed >> 32);
}

static void lb_finalize_session(lb_session_t *session) {
    uint32_t generation = session->generation;
    if (lb_profile.enabled) {
        lb_profile.sessions_closed++;
        if (lb_profile.sessions_active > 0u) {
            lb_profile.sessions_active--;
        }
        if (session->session_started_ns != 0u) {
            lb_profile.session_lifetime_ns += lb_now_ns() - session->session_started_ns;
        }
    }
    if (session->client_fd >= 0) {
        close(session->client_fd);
    }
    if (session->backend_fd >= 0) {
        close(session->backend_fd);
    }
    memset(session, 0, sizeof(*session));
    session->client_fd = -1;
    session->backend_fd = -1;
    session->generation = generation;
}

static int lb_alloc_session(lb_session_t *sessions) {
    for (int i = 0; i < LB_MAX_SESSIONS; i++) {
        if (!sessions[i].used) {
            uint32_t next_generation = sessions[i].generation + 1u;
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].used = true;
            sessions[i].client_fd = -1;
            sessions[i].backend_fd = -1;
            sessions[i].generation = next_generation == 0u ? 1u : next_generation;
            if (lb_profile.enabled) {
                lb_profile.sessions_opened++;
                lb_profile.sessions_active++;
                if (lb_profile.sessions_active > lb_profile.sessions_active_max) {
                    lb_profile.sessions_active_max = lb_profile.sessions_active;
                }
            }
            return i;
        }
    }
    if (lb_profile.enabled) {
        lb_profile.session_alloc_failures++;
    }
    return -1;
}

static void lb_request_close(lb_session_t *session) {
    if (session->client_fd >= 0) {
        close(session->client_fd);
        session->client_fd = -1;
    }
    if (session->backend_fd >= 0) {
        close(session->backend_fd);
        session->backend_fd = -1;
    }
    session->connected = false;
    session->close_pending = true;
}

static void lb_maybe_finalize_session(lb_session_t *session) {
    if (session->close_pending
        && session->inflight_ops == 0u
        && session->pending_send_notifs[0] == 0u
        && session->pending_send_notifs[1] == 0u) {
        lb_finalize_session(session);
    }
}

static int lb_queue_accept(struct io_uring *ring, int listen_fd, lb_accept_state_t *state) {
    state->addr_len = sizeof(state->addr);
    state->queued_at_ns = lb_profile.enabled ? lb_now_ns() : 0u;

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_accept(sqe, listen_fd, (struct sockaddr *) &state->addr, &state->addr_len, 0);
    io_uring_sqe_set_data64(sqe, lb_pack_user_data(LB_OP_ACCEPT, -1, -1, 0));
    return 0;
}

static int lb_queue_connect(struct io_uring *ring, lb_session_t *session, int session_index, const lb_backend_t *backend) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_connect(sqe, session->backend_fd, (const struct sockaddr *) &backend->addr, backend->addr_len);
    io_uring_sqe_set_data64(sqe, lb_pack_user_data(LB_OP_CONNECT, session_index, -1, session->generation));
    session->connect_started_ns = lb_profile.enabled ? lb_now_ns() : 0u;
    session->inflight_ops++;
    return 0;
}

static int lb_queue_read(struct io_uring *ring, lb_session_t *session, int session_index, int direction) {
    int fd = direction == 0 ? session->client_fd : session->backend_fd;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_recv(sqe, fd, session->buffer[direction], LB_BUFFER_SIZE, 0);
    io_uring_sqe_set_data64(sqe, lb_pack_user_data(LB_OP_READ, session_index, direction, session->generation));
    session->read_started_ns[direction] = lb_profile.enabled ? lb_now_ns() : 0u;
    session->inflight_ops++;
    return 0;
}

static int lb_queue_write(struct io_uring *ring, lb_session_t *session, int session_index, int direction) {
    int fd = direction == 0 ? session->backend_fd : session->client_fd;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    const void *buf = session->buffer[direction] + session->buffer_sent[direction];
    size_t len = session->buffer_len[direction] - session->buffer_sent[direction];
#if LB_HAVE_SEND_ZC
    if (session->zerocopy_enabled[direction]) {
        io_uring_prep_send_zc(sqe, fd, buf, len, LB_SEND_FLAGS, IORING_SEND_ZC_REPORT_USAGE);
        session->send_inflight_zerocopy[direction] = true;
    } else
#endif
    {
        io_uring_prep_send(sqe, fd, buf, len, LB_SEND_FLAGS);
        session->send_inflight_zerocopy[direction] = false;
    }
    io_uring_sqe_set_data64(sqe, lb_pack_user_data(LB_OP_WRITE, session_index, direction, session->generation));
    session->write_started_ns[direction] = lb_profile.enabled ? lb_now_ns() : 0u;
    session->inflight_ops++;
    return 0;
}

static int lb_flush_submissions(struct io_uring *ring) {
    if (!lb_profile.enabled) {
        return io_uring_submit(ring);
    }

    uint64_t start_ns = lb_now_ns();
    unsigned ready = io_uring_sq_ready(ring);
    int rc = io_uring_submit(ring);
    lb_profile.submit_calls++;
    lb_profile.submit_ns += lb_now_ns() - start_ns;
    if (rc < 0) {
        lb_profile.submit_failures++;
    } else if (ready > 0u) {
        lb_profile.submit_sqes += (uint64_t) rc;
    }
    return rc;
}

static ssize_t lb_collect_cqes(struct io_uring *ring, lb_cqe_record_t *cqes, size_t max_cqes) {
    if (max_cqes == 0u) {
        return 0;
    }

    struct io_uring_cqe *cqe = NULL;
    uint64_t wait_start_ns = lb_profile.enabled ? lb_now_ns() : 0u;
    int wait_rc = io_uring_wait_cqe(ring, &cqe);
    if (wait_rc != 0) {
        if (wait_rc == -EINTR) {
            return 0;
        }
        return -1;
    }
    if (lb_profile.enabled) {
        lb_profile.wait_calls++;
        lb_profile.wait_ns += lb_now_ns() - wait_start_ns;
    }

    size_t count = 0u;
    cqes[count++] = (lb_cqe_record_t) {
        .user_data = io_uring_cqe_get_data64(cqe),
        .res = cqe->res,
        .flags = cqe->flags,
    };
    io_uring_cqe_seen(ring, cqe);

    while (count < max_cqes) {
        struct io_uring_cqe *batch_cqes[LB_CQE_BATCH];
        unsigned batch = io_uring_peek_batch_cqe(ring, batch_cqes, (unsigned) (max_cqes - count));
        if (batch == 0u) {
            break;
        }
        for (unsigned i = 0; i < batch && count < max_cqes; i++) {
            cqes[count++] = (lb_cqe_record_t) {
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

    if (lb_profile.enabled) {
        lb_profile.cqe_batches++;
        lb_profile.cqe_total += (uint64_t) count;
    }
    return (ssize_t) count;
}

static bool lb_store_unix_backend(lb_backend_t *out, const char *path) {
    size_t path_len = strlen(path);
    if (path_len == 0u || path_len >= sizeof(((struct sockaddr_un *) 0)->sun_path)) {
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1u);

    memset(&out->addr, 0, sizeof(out->addr));
    memcpy(&out->addr, &addr, sizeof(addr));
    out->addr_len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
    return true;
}

static size_t lb_parse_backends(const char *env, lb_backend_t *out) {
    const char *source = env == NULL ? "unix:/run/rinha/api1.sock,unix:/run/rinha/api2.sock" : env;
    char text[256];
    snprintf(text, sizeof(text), "%s", source);

    size_t count = 0;
    char *saveptr = NULL;
    for (char *token = strtok_r(text, ",", &saveptr); token != NULL && count < LB_MAX_BACKENDS; token = strtok_r(NULL, ",", &saveptr)) {
        while (*token == ' ' || *token == '\t') {
            token++;
        }

        char *tail = token + strlen(token);
        while (tail > token && (tail[-1] == ' ' || tail[-1] == '\t')) {
            *--tail = '\0';
        }

        if (strncmp(token, "unix:", 5) == 0) {
            const char *path = token + 5;
            if (!lb_store_unix_backend(&out[count], path)) {
                fprintf(stderr, "falha ao configurar backend unix %s\n", path);
                continue;
            }
            count++;
            continue;
        }

        char *colon = strrchr(token, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        const char *host = token;
        const char *port = colon + 1;

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;

        struct addrinfo *result = NULL;
        if (getaddrinfo(host, port, &hints, &result) != 0) {
            fprintf(stderr, "falha ao resolver backend %s:%s\n", host, port);
            continue;
        }

        memcpy(&out[count].addr, result->ai_addr, result->ai_addrlen);
        out[count].addr_len = (socklen_t) result->ai_addrlen;
        freeaddrinfo(result);
        count++;
    }

    return count;
}

static int lb_finish_write(struct io_uring *ring, lb_session_t *session, int session_index, int direction) {
    if (session->pending_send_notifs[direction] > 0u) {
        session->send_complete_waiting_notif[direction] = true;
        return 0;
    }

    session->send_complete_waiting_notif[direction] = false;
    if (session->close_pending) {
        session->buffer_len[direction] = 0;
        session->buffer_sent[direction] = 0;
        lb_maybe_finalize_session(session);
        return 0;
    }
    session->buffer_len[direction] = 0;
    session->buffer_sent[direction] = 0;
    return lb_queue_read(ring, session, session_index, direction);
}

static bool lb_send_zc_should_fallback(bool used_zerocopy, int res) {
#if LB_HAVE_SEND_ZC
    return used_zerocopy
        && (res == -EINVAL || res == -EOPNOTSUPP || res == -ENOBUFS || res == -ENOMEM);
#else
    (void) used_zerocopy;
    (void) res;
    return false;
#endif
}

int main(void) {
    signal(SIGINT, lb_on_signal);
    signal(SIGTERM, lb_on_signal);
    lb_profile_init();

    lb_backend_t backends[LB_MAX_BACKENDS];
    size_t backend_count = lb_parse_backends(getenv("BACKENDS"), backends);
    if (backend_count < 2u) {
        fprintf(stderr, "configure ao menos dois backends em BACKENDS\n");
        return 1;
    }

    int listen_fd = lb_open_listener(LB_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "falha ao abrir load balancer na porta %d\n", LB_PORT);
        return 1;
    }

    struct io_uring ring;
    if (io_uring_queue_init(LB_QUEUE_DEPTH, &ring, 0) != 0) {
        fprintf(stderr, "falha ao inicializar io_uring no balanceador\n");
        close(listen_fd);
        return 1;
    }

    lb_session_t *sessions = calloc(LB_MAX_SESSIONS, sizeof(lb_session_t));
    if (sessions == NULL) {
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 1;
    }
    for (int i = 0; i < LB_MAX_SESSIONS; i++) {
        sessions[i].client_fd = -1;
        sessions[i].backend_fd = -1;
    }

    lb_accept_state_t accept_state;
    memset(&accept_state, 0, sizeof(accept_state));
    int accept_rc = lb_queue_accept(&ring, listen_fd, &accept_state);
    if (accept_rc < 0) {
        fprintf(stderr, "falha ao submeter accept inicial no io_uring do balanceador: rc=%d\n", accept_rc);
        free(sessions);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 1;
    }
    if (lb_flush_submissions(&ring) < 0) {
        fprintf(stderr, "falha ao submeter accept inicial no io_uring do balanceador\n");
        free(sessions);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        return 1;
    }

    size_t next_backend = 0;
    lb_cqe_record_t cqes[LB_CQE_BATCH];
    while (lb_running) {
        ssize_t cqe_count = lb_collect_cqes(&ring, cqes, LB_CQE_BATCH);
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
            lb_op_type_t type;
            int session_index;
            int direction;
            uint32_t generation;
            int res = cqes[i].res;
            unsigned flags = cqes[i].flags;
            uint64_t completed_ns = lb_profile.enabled ? lb_now_ns() : 0u;

            lb_unpack_user_data(user_data, &type, &session_index, &direction, &generation);

            if (type == LB_OP_ACCEPT) {
                if (lb_profile.enabled) {
                    lb_profile.accept_ops++;
                    if (accept_state.queued_at_ns != 0u) {
                        lb_profile.accept_ns += completed_ns - accept_state.queued_at_ns;
                    }
                    if (res < 0) {
                        lb_profile.accept_failures++;
                    }
                }
                if (res >= 0) {
                    int accepted_session_index = lb_alloc_session(sessions);
                    if (accepted_session_index >= 0) {
                        lb_session_t *session = &sessions[accepted_session_index];
                        session->client_fd = res;
                        lb_set_nonblocking(session->client_fd);
                        session->zerocopy_enabled[1] = lb_try_enable_zerocopy(session->client_fd);
                        session->session_started_ns = lb_profile.enabled ? completed_ns : 0u;

                        const lb_backend_t *backend = &backends[next_backend % backend_count];
                        next_backend++;

                        int backend_fd = socket(((struct sockaddr *) &backend->addr)->sa_family, SOCK_STREAM, 0);
                        if (backend_fd >= 0 && lb_set_nonblocking(backend_fd) == 0) {
                            session->backend_fd = backend_fd;
                            session->zerocopy_enabled[0] = lb_try_enable_zerocopy(session->backend_fd);
                            if (lb_queue_connect(&ring, session, accepted_session_index, backend) < 0) {
                                lb_request_close(session);
                                lb_maybe_finalize_session(session);
                                fatal_error = true;
                            } else {
                                need_submit = true;
                            }
                        } else {
                            if (backend_fd >= 0) {
                                close(backend_fd);
                            }
                            lb_request_close(session);
                            lb_maybe_finalize_session(session);
                        }
                    } else {
                        close(res);
                    }
                }
                if (lb_queue_accept(&ring, listen_fd, &accept_state) < 0) {
                    fatal_error = true;
                } else {
                    need_submit = true;
                }
                continue;
            }

            if (session_index < 0 || session_index >= LB_MAX_SESSIONS) {
                continue;
            }
            lb_session_t *session = &sessions[session_index];
            if (!session->used || session->generation != generation) {
                continue;
            }
            if ((flags & IORING_CQE_F_NOTIF) == 0u && session->inflight_ops > 0u) {
                session->inflight_ops--;
            }
            if (session->close_pending && type != LB_OP_WRITE) {
                lb_maybe_finalize_session(session);
                continue;
            }

            if (type == LB_OP_CONNECT) {
                if (lb_profile.enabled) {
                    lb_profile.connect_ops++;
                    if (session->connect_started_ns != 0u) {
                        lb_profile.connect_ns += completed_ns - session->connect_started_ns;
                    }
                    if (res < 0) {
                        lb_profile.connect_failures++;
                    }
                }
                if (res < 0) {
                    lb_request_close(session);
                    lb_maybe_finalize_session(session);
                    continue;
                }
                session->connected = true;
                if (lb_queue_read(&ring, session, session_index, 0) < 0
                    || lb_queue_read(&ring, session, session_index, 1) < 0) {
                    lb_request_close(session);
                    lb_maybe_finalize_session(session);
                    fatal_error = true;
                } else {
                    need_submit = true;
                }
                continue;
            }

            if (type == LB_OP_READ) {
                if (lb_profile.enabled) {
                    lb_profile.read_ops[direction]++;
                    if (session->read_started_ns[direction] != 0u) {
                        lb_profile.read_ns[direction] += completed_ns - session->read_started_ns[direction];
                    }
                    if (res < 0) {
                        lb_profile.read_failures[direction]++;
                    } else if (res == 0) {
                        lb_profile.read_eof[direction]++;
                    } else {
                        lb_profile.read_bytes[direction] += (uint64_t) res;
                    }
                }
                if (res <= 0) {
                    lb_request_close(session);
                    lb_maybe_finalize_session(session);
                    continue;
                }

                session->buffer_len[direction] = (size_t) res;
                session->buffer_sent[direction] = 0;
                if (lb_queue_write(&ring, session, session_index, direction) < 0) {
                    lb_request_close(session);
                    lb_maybe_finalize_session(session);
                    fatal_error = true;
                } else {
                    need_submit = true;
                }
                continue;
            }

            if (type == LB_OP_WRITE) {
                if ((flags & IORING_CQE_F_NOTIF) != 0u) {
                    if (lb_profile.enabled) {
                        lb_profile.write_zc_notifs[direction]++;
                    }
                    if (session->pending_send_notifs[direction] > 0u) {
                        session->pending_send_notifs[direction]--;
                    }
                    if (res != 0) {
                        session->zerocopy_enabled[direction] = false;
                    }
                    if (session->send_complete_waiting_notif[direction]
                        && session->pending_send_notifs[direction] == 0u) {
                        int finish_rc = lb_finish_write(&ring, session, session_index, direction);
                        if (finish_rc < 0) {
                            lb_request_close(session);
                            lb_maybe_finalize_session(session);
                            fatal_error = true;
                        } else if (finish_rc > 0) {
                            need_submit = true;
                        }
                    }
                    lb_maybe_finalize_session(session);
                    continue;
                }

                bool used_zerocopy = session->send_inflight_zerocopy[direction];
                session->send_inflight_zerocopy[direction] = false;
                if (lb_profile.enabled) {
                    lb_profile.write_ops[direction]++;
                    if (session->write_started_ns[direction] != 0u) {
                        lb_profile.write_ns[direction] += completed_ns - session->write_started_ns[direction];
                    }
                    if (res < 0) {
                        lb_profile.write_failures[direction]++;
                    } else {
                        lb_profile.write_bytes[direction] += (uint64_t) res;
                    }
                }
                if (lb_send_zc_should_fallback(used_zerocopy, res)) {
                    session->zerocopy_enabled[direction] = false;
                    if (lb_profile.enabled) {
                        lb_profile.write_zc_fallbacks[direction]++;
                    }
                    if (lb_queue_write(&ring, session, session_index, direction) < 0) {
                        lb_request_close(session);
                        lb_maybe_finalize_session(session);
                        fatal_error = true;
                    } else {
                        need_submit = true;
                    }
                    continue;
                }
                if (res <= 0) {
                    lb_request_close(session);
                    lb_maybe_finalize_session(session);
                    continue;
                }

                if (used_zerocopy && (flags & IORING_CQE_F_MORE) != 0u) {
                    session->pending_send_notifs[direction]++;
                }
                session->buffer_sent[direction] += (size_t) res;
                if (session->buffer_sent[direction] < session->buffer_len[direction]) {
                    if (lb_profile.enabled) {
                        lb_profile.write_partial[direction]++;
                    }
                    if (lb_queue_write(&ring, session, session_index, direction) < 0) {
                        lb_request_close(session);
                        lb_maybe_finalize_session(session);
                        fatal_error = true;
                    } else {
                        need_submit = true;
                    }
                    continue;
                }

                if (lb_profile.enabled && direction == 1) {
                    lb_profile.responses++;
                    if (lb_profile.report_every > 0u &&
                        lb_profile.responses % lb_profile.report_every == 0u) {
                        lb_profile_report("periodic");
                    }
                }
                int finish_rc = lb_finish_write(&ring, session, session_index, direction);
                if (finish_rc < 0) {
                    lb_request_close(session);
                    lb_maybe_finalize_session(session);
                    fatal_error = true;
                } else if (finish_rc > 0) {
                    need_submit = true;
                }
            }
        }

        if (need_submit && lb_flush_submissions(&ring) < 0) {
            fatal_error = true;
        }
        if (fatal_error) {
            break;
        }
    }

    for (int i = 0; i < LB_MAX_SESSIONS; i++) {
        if (sessions[i].used) {
            lb_finalize_session(&sessions[i]);
        }
    }
    free(sessions);
    io_uring_queue_exit(&ring);
    close(listen_fd);
    return 0;
}
