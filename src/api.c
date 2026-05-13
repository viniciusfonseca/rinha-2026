#include "api_http.h"
#include "index.h"
#include "vectorize.h"

#include <errno.h>
#include <fcntl.h>
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
#include <unistd.h>

#define API_BACKLOG 1024
#define API_QUEUE_DEPTH 2048
#define API_MAX_CONNECTIONS 2048
#define API_READ_BUFFER 16384
#define API_WRITE_BUFFER 1024
#define API_CQE_BATCH 64
#define API_SOCKET_PATH_DEFAULT "/run/rinha/api.sock"

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
    uint32_t generation;
    size_t read_len;
    size_t write_len;
    size_t write_sent;
    char read_buf[API_READ_BUFFER + 1];
    char write_buf[API_WRITE_BUFFER];
} api_conn_t;

typedef struct {
    uint64_t user_data;
    int res;
} api_cqe_record_t;

static volatile sig_atomic_t api_running = 1;

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

static void api_close_conn(api_conn_t *conn, int conn_index, int *free_slots, size_t *free_count) {
    bool was_used = conn->used;
    if (conn->fd >= 0) {
        close(conn->fd);
    }

    conn->fd = -1;
    conn->used = false;
    conn->keep_alive = false;
    conn->read_len = 0;
    conn->write_len = 0;
    conn->write_sent = 0;

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
    conn->generation = next_generation == 0u ? 1u : next_generation;
    conn->read_len = 0;
    conn->write_len = 0;
    conn->write_sent = 0;
    return i;
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
    return 0;
}

static int api_queue_send(struct io_uring *ring, api_conn_t *conn, int conn_index) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_send(
        sqe,
        conn->fd,
        conn->write_buf + conn->write_sent,
        conn->write_len - conn->write_sent,
        0
    );
    io_uring_sqe_set_data64(sqe, api_pack_user_data(API_OP_SEND, conn_index, conn->generation));
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

static void api_prepare_response(
    api_conn_t *conn,
    const char *status,
    const char *content_type,
    const char *body,
    bool keep_alive
) {
    const char *connection = keep_alive ? "keep-alive" : "close";
    char *p = conn->write_buf;
    char *end = conn->write_buf + sizeof(conn->write_buf);
    size_t body_len = body == NULL ? 0u : strlen(body);

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

    conn->write_len = (size_t) (p - conn->write_buf);
    conn->write_sent = 0;
    conn->keep_alive = keep_alive;
}

static void api_prepare_no_content(api_conn_t *conn, bool keep_alive) {
    char *p = conn->write_buf;
    p = api_copy_str(p, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: ");
    p = api_copy_str(p, keep_alive ? "keep-alive" : "close");
    p = api_copy_str(p, "\r\n\r\n");
    conn->write_len = (size_t) (p - conn->write_buf);
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
    if (!rinha_parse_tx_payload(request->body, request->content_length, &payload)) {
        api_prepare_response(conn, "400 Bad Request", "text/plain", "invalid payload", false);
        return;
    }

    float vector[RINHA_DIM];
    rinha_payload_to_vector(&payload, vector);

    int fraud_count = rinha_index_fraud_count_top5(index, vector);
    if (fraud_count == 2 && rinha_payload_force_deny_borderline(&payload)) {
        fraud_count = 3;
    }
    float fraud_score = (float) fraud_count / 5.0f;
    bool approved = fraud_score < 0.6f;

    char body[96];
    static const char *const score_texts[] = {"0.0", "0.2", "0.4", "0.6", "0.8", "1.0"};
    char *p = body;
    p = api_copy_str(p, "{\"approved\":");
    p = api_copy_str(p, approved ? "true" : "false");
    p = api_copy_str(p, ",\"fraud_score\":");
    p = api_copy_str(p, score_texts[fraud_count]);
    p = api_copy_str(p, "}");
    *p = '\0';
    api_prepare_response(conn, "200 OK", "application/json", body, request->keep_alive);
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
            api_unpack_user_data(user_data, &type, &conn_index, &generation);

            if (type == API_OP_ACCEPT) {
                if (res >= 0) {
                    int accepted_conn_index = api_alloc_conn(connections, free_slots, &free_count);
                    if (accepted_conn_index >= 0) {
                        api_conn_t *conn = &connections[accepted_conn_index];
                        conn->fd = res;
                        api_set_nonblocking(conn->fd);
                        if (api_queue_recv(&ring, conn, accepted_conn_index) < 0) {
                            api_close_conn(conn, accepted_conn_index, free_slots, &free_count);
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

            if (type == API_OP_RECV) {
                if (res <= 0) {
                    api_close_conn(conn, conn_index, free_slots, &free_count);
                    continue;
                }

                conn->read_len += (size_t) res;
                conn->read_buf[conn->read_len] = '\0';

                api_request_t request;
                int parse_rc = api_parse_http_request(conn->read_buf, conn->read_len, &request);
                if (parse_rc == 0) {
                    if (conn->read_len >= API_READ_BUFFER) {
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
                    api_prepare_response(conn, "400 Bad Request", "text/plain", "bad request", false);
                } else {
                    api_handle_business(conn, &request, &index);
                }

                if (api_queue_send(&ring, conn, conn_index) < 0) {
                    fatal_error = true;
                } else {
                    need_submit = true;
                }
                continue;
            }

            if (type == API_OP_SEND) {
                if (res <= 0) {
                    api_close_conn(conn, conn_index, free_slots, &free_count);
                    continue;
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

                if (!conn->keep_alive) {
                    api_close_conn(conn, conn_index, free_slots, &free_count);
                    continue;
                }

                conn->read_len = 0;
                conn->write_len = 0;
                conn->write_sent = 0;
                if (api_queue_recv(&ring, conn, conn_index) < 0) {
                    fatal_error = true;
                } else {
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
            api_close_conn(&connections[i], i, free_slots, &free_count);
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
