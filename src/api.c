#include "index.h"
#include "vectorize.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define API_PORT 9999
#define API_BACKLOG 1024
#define API_QUEUE_DEPTH 2048
#define API_MAX_CONNECTIONS 2048
#define API_READ_BUFFER 16384
#define API_WRITE_BUFFER 1024

typedef enum {
    API_OP_ACCEPT = 1,
    API_OP_RECV = 2,
    API_OP_SEND = 3,
} api_op_type_t;

typedef struct {
    api_op_type_t type;
    int conn_index;
} api_op_t;

typedef struct {
    api_op_t op;
    struct sockaddr_storage addr;
    socklen_t addr_len;
} api_accept_state_t;

typedef struct {
    int fd;
    bool used;
    bool keep_alive;
    size_t read_len;
    size_t write_len;
    size_t write_sent;
    char read_buf[API_READ_BUFFER + 1];
    char write_buf[API_WRITE_BUFFER];
    api_op_t recv_op;
    api_op_t send_op;
} api_conn_t;

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

static volatile sig_atomic_t api_running = 1;

static void api_on_signal(int signo) {
    (void) signo;
    api_running = 0;
}

static int api_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int api_open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (api_set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, API_BACKLOG) != 0) {
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

    conn->used = false;

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
    conn->used = true;
    conn->fd = -1;
    conn->keep_alive = false;
    conn->read_len = 0;
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->recv_op.type = API_OP_RECV;
    conn->recv_op.conn_index = i;
    conn->send_op.type = API_OP_SEND;
    conn->send_op.conn_index = i;
    return i;
}

static int api_submit_accept(struct io_uring *ring, int listen_fd, api_accept_state_t *state) {
    state->op.type = API_OP_ACCEPT;
    state->op.conn_index = -1;
    state->addr_len = sizeof(state->addr);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_accept(sqe, listen_fd, (struct sockaddr *) &state->addr, &state->addr_len, 0);
    io_uring_sqe_set_data(sqe, &state->op);
    return io_uring_submit(ring);
}

static int api_submit_recv(struct io_uring *ring, api_conn_t *conn) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        return -1;
    }
    io_uring_prep_recv(sqe, conn->fd, conn->read_buf + conn->read_len, API_READ_BUFFER - conn->read_len, 0);
    io_uring_sqe_set_data(sqe, &conn->recv_op);
    return io_uring_submit(ring);
}

static int api_submit_send(struct io_uring *ring, api_conn_t *conn) {
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
    io_uring_sqe_set_data(sqe, &conn->send_op);
    return io_uring_submit(ring);
}

static bool api_ascii_ieq(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char) a[i];
        unsigned char cb = (unsigned char) b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char) (ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char) (cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
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

static int api_parse_http_request(const char *buffer, size_t len, api_request_t *request) {
    const char *limit = buffer + len;
    const char *header_end = NULL;
    for (const char *p = buffer; p + 3 < limit; p++) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            header_end = p;
            break;
        }
    }
    if (header_end == NULL) {
        return 0;
    }

    const char *line_end = NULL;
    for (const char *p = buffer; p + 1 < header_end; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            line_end = p;
            break;
        }
    }
    if (line_end == NULL) {
        return -1;
    }

    request->route = API_ROUTE_INVALID;
    request->content_length = 0;
    request->keep_alive = true;

    const char *sp1 = memchr(buffer, ' ', (size_t) (line_end - buffer));
    if (sp1 == NULL) {
        return -1;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t) (line_end - (sp1 + 1)));
    if (sp2 == NULL) {
        return -1;
    }

    size_t method_len = (size_t) (sp1 - buffer);
    size_t path_len = (size_t) (sp2 - (sp1 + 1));
    if (method_len == 3 && memcmp(buffer, "GET", 3) == 0 && path_len == 6 && memcmp(sp1 + 1, "/ready", 6) == 0) {
        request->route = API_ROUTE_READY;
    } else if (method_len == 4 && memcmp(buffer, "POST", 4) == 0 && path_len == 12 && memcmp(sp1 + 1, "/fraud-score", 12) == 0) {
        request->route = API_ROUTE_FRAUD_SCORE;
    }

    const char *cursor = line_end + 2;
    while (cursor < header_end) {
        const char *next = cursor;
        while (next + 1 < header_end && !(next[0] == '\r' && next[1] == '\n')) {
            next++;
        }
        if (next + 1 >= header_end) {
            return -1;
        }

        const char *colon = memchr(cursor, ':', (size_t) (next - cursor));
        if (colon == NULL) {
            return -1;
        }

        size_t name_len = (size_t) (colon - cursor);
        const char *value = colon + 1;
        while (value < next && (*value == ' ' || *value == '\t')) {
            value++;
        }

        if (name_len == 14 && api_ascii_ieq(cursor, "Content-Length", 14)) {
            size_t content_length = 0;
            while (value < next && *value >= '0' && *value <= '9') {
                content_length = content_length * 10u + (size_t) (*value - '0');
                value++;
            }
            request->content_length = content_length;
        } else if (name_len == 10 && api_ascii_ieq(cursor, "Connection", 10)) {
            size_t value_len = (size_t) (next - value);
            if (value_len == 5 && api_ascii_ieq(value, "close", 5)) {
                request->keep_alive = false;
            }
        }

        cursor = next + 2;
    }

    request->body = header_end + 4;
    request->total_length = (size_t) ((header_end + 4) - buffer) + request->content_length;
    if (len < request->total_length) {
        return 0;
    }
    return 1;
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

    float vector[RINHA_DIM];
    if (!rinha_request_to_vector(request->body, request->content_length, vector)) {
        api_prepare_response(conn, "400 Bad Request", "text/plain", "invalid payload", false);
        return;
    }

    int fraud_count = rinha_index_fraud_count_top5(index, vector);
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

    signal(SIGINT, api_on_signal);
    signal(SIGTERM, api_on_signal);

    rinha_index_t index;
    if (!rinha_index_open(&index, index_path)) {
        fprintf(stderr, "falha ao abrir indice %s\n", index_path);
        return 1;
    }

    int listen_fd = api_open_listener(API_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "falha ao abrir porta %d\n", API_PORT);
        rinha_index_close(&index);
        return 1;
    }

    struct io_uring ring;
    if (io_uring_queue_init(API_QUEUE_DEPTH, &ring, 0) != 0) {
        fprintf(stderr, "falha ao inicializar io_uring\n");
        close(listen_fd);
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
    int accept_rc = api_submit_accept(&ring, listen_fd, &accept_state);
    if (accept_rc < 0) {
        fprintf(stderr, "falha ao submeter accept inicial no io_uring: rc=%d\n", accept_rc);
        free(connections);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        rinha_index_close(&index);
        return 1;
    }

    while (api_running) {
        struct io_uring_cqe *cqe = NULL;
        int wait_rc = io_uring_wait_cqe(&ring, &cqe);
        if (wait_rc != 0) {
            if (wait_rc == -EINTR) {
                continue;
            }
            break;
        }

        api_op_t *op = io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (op == NULL) {
            continue;
        }

        if (op->type == API_OP_ACCEPT) {
            if (res >= 0) {
                int conn_index = api_alloc_conn(connections, free_slots, &free_count);
                if (conn_index >= 0) {
                    api_conn_t *conn = &connections[conn_index];
                    conn->fd = res;
                    api_set_nonblocking(conn->fd);
                    if (api_submit_recv(&ring, conn) < 0) {
                        api_close_conn(conn, conn_index, free_slots, &free_count);
                    }
                } else {
                    close(res);
                }
            }
            api_submit_accept(&ring, listen_fd, &accept_state);
            continue;
        }

        api_conn_t *conn = &connections[op->conn_index];
        if (!conn->used) {
            continue;
        }

        if (op->type == API_OP_RECV) {
            if (res <= 0) {
                api_close_conn(conn, op->conn_index, free_slots, &free_count);
                continue;
            }

            conn->read_len += (size_t) res;
            conn->read_buf[conn->read_len] = '\0';

            api_request_t request;
            int parse_rc = api_parse_http_request(conn->read_buf, conn->read_len, &request);
            if (parse_rc == 0) {
                if (conn->read_len >= API_READ_BUFFER) {
                    api_prepare_response(conn, "413 Payload Too Large", "text/plain", "payload too large", false);
                    api_submit_send(&ring, conn);
                } else {
                    api_submit_recv(&ring, conn);
                }
                continue;
            }

            if (parse_rc < 0) {
                api_prepare_response(conn, "400 Bad Request", "text/plain", "bad request", false);
            } else {
                api_handle_business(conn, &request, &index);
            }

            api_submit_send(&ring, conn);
            continue;
        }

        if (op->type == API_OP_SEND) {
            if (res <= 0) {
                api_close_conn(conn, op->conn_index, free_slots, &free_count);
                continue;
            }

            conn->write_sent += (size_t) res;
            if (conn->write_sent < conn->write_len) {
                api_submit_send(&ring, conn);
                continue;
            }

            if (!conn->keep_alive) {
                api_close_conn(conn, op->conn_index, free_slots, &free_count);
                continue;
            }

            conn->read_len = 0;
            conn->write_len = 0;
            conn->write_sent = 0;
            api_submit_recv(&ring, conn);
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
    rinha_index_close(&index);
    return 0;
}
