/* Portal screenshot transport; all socket work stays on the compositor thread. */
#include "ipc.h"
#include "config.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static bool capture_fail(char *err, size_t err_len, const char *message, int error)
{
    if (err && err_len) snprintf(err, err_len, "%s", message);
    errno = error;
    return false;
}

static bool capture_system_error(char *err, size_t err_len, int error)
{
    return capture_fail(err, err_len, strerror(error), error);
}

/* Only a refused connection proves a socket stale. In particular, a full listen
 * backlog (EAGAIN) and permission errors must never cause an active socket's removal. */
static bool capture_remove_stale(const struct sockaddr_un *address, socklen_t address_len,
                                 char *err, size_t err_len)
{
    struct stat original;
    int result;
    do {
        result = lstat(address->sun_path, &original);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        if (errno == ENOENT) return true;
        return capture_system_error(err, err_len, errno);
    }
    if (!S_ISSOCK(original.st_mode))
        return capture_fail(err, err_len, "capture path is not a socket", EEXIST);

    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0) return capture_system_error(err, err_len, errno);
    do {
        result = connect(probe, (const struct sockaddr *)address, address_len);
    } while (result < 0 && errno == EINTR);
    int error = result < 0 ? errno : 0;
    close(probe);
    if (result == 0 || error == EAGAIN || error == EINPROGRESS || error == EALREADY ||
        error == EISCONN)
        return capture_fail(err, err_len, "capture socket is live", EADDRINUSE);
    if (error != ECONNREFUSED) return capture_system_error(err, err_len, error);
    if (original.st_uid != geteuid())
        return capture_system_error(err, err_len, EACCES);

    struct stat current;
    do {
        result = lstat(address->sun_path, &current);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        if (errno == ENOENT) return true;
        return capture_system_error(err, err_len, errno);
    }
    if (current.st_dev != original.st_dev || current.st_ino != original.st_ino ||
        !S_ISSOCK(current.st_mode))
        return capture_fail(err, err_len, "capture socket changed while binding", EADDRINUSE);
    do {
        result = unlink(address->sun_path);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return capture_system_error(err, err_len, errno);
    return true;
}

static void capture_client_close(purp_capture_client_t *client)
{
    if (client->state != PURP_CAPTURE_CLIENT_FREE && client->fd >= 0) close(client->fd);
    purp_blob_unref(client->png);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static void capture_reply(purp_capture_client_t *client, purp_blob_t *png, uint64_t now)
{
    purp_blob_unref(client->png);
    client->png = purp_blob_ref(png);
    if (png) {
        client->header_len = (size_t)snprintf(client->header, sizeof(client->header),
                                              "OK %zu\n", png->len);
    } else {
        memcpy(client->header, PURP_CAPTURE_ERR_REPLY, sizeof(PURP_CAPTURE_ERR_REPLY));
        client->header_len = sizeof(PURP_CAPTURE_ERR_REPLY) - 1;
    }
    client->header_off = 0;
    client->body_off = 0;
    client->deadline_ms = now + PURP_CAPTURE_WRITE_TIMEOUT_MS;
    client->state = PURP_CAPTURE_CLIENT_WRITING;
}

static void capture_client_write(purp_capture_client_t *client, uint64_t now)
{
    if (client->state != PURP_CAPTURE_CLIENT_WRITING) return;
    if (now >= client->deadline_ms) {
        capture_client_close(client);
        return;
    }
    for (;;) {
        const uint8_t *bytes;
        size_t remaining;
        size_t *offset;
        if (client->header_off < client->header_len) {
            bytes = (const uint8_t *)client->header;
            remaining = client->header_len - client->header_off;
            offset = &client->header_off;
        } else if (client->png && client->body_off < client->png->len) {
            bytes = client->png->data;
            remaining = client->png->len - client->body_off;
            offset = &client->body_off;
        } else {
            capture_client_close(client);
            return;
        }
        if (remaining > 65536) remaining = 65536;
        ssize_t written = send(client->fd, bytes + *offset, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            *offset += (size_t)written;
            if (purp_now_ms() >= client->deadline_ms) {
                capture_client_close(client);
                return;
            }
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        capture_client_close(client);
        return;
    }
}

/* A completed request remains READING until the batch's single render is done. */
static bool capture_client_read(purp_capture_client_t *client, uint64_t now)
{
    if (now - client->since_ms >= PURP_CAPTURE_READ_TIMEOUT_MS) {
        capture_reply(client, NULL, now);
        return false;
    }
    for (;;) {
        ssize_t count = recv(client->fd, client->in + client->in_len,
                             sizeof(client->in) - client->in_len, 0);
        if (count > 0) {
            client->in_len += (size_t)count;
            if (client->in_len > sizeof(PURP_CAPTURE_REQUEST) - 1 ||
                memcmp(client->in, PURP_CAPTURE_REQUEST, client->in_len) != 0) {
                capture_reply(client, NULL, now);
                return false;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            capture_client_close(client);
            return false;
        }
        if (client->in_len == sizeof(PURP_CAPTURE_REQUEST) - 1) {
            client->deadline_ms = now + PURP_CAPTURE_RENDER_TIMEOUT_MS;
            return true;
        }
        if (count == 0) capture_reply(client, NULL, now);
        return false;
    }
}

bool purp_capture_server_bind(purp_capture_server_t *srv, const char *path, char *err,
                              size_t err_len)
{
    if (!srv) return capture_system_error(err, err_len, EINVAL);
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;
    for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i) srv->clients[i].fd = -1;
    if (err && err_len) err[0] = '\0';
    if (!path || !path[0]) return capture_system_error(err, err_len, EINVAL);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    size_t path_len = strlen(path);
    if (path_len >= sizeof(address.sun_path))
        return capture_system_error(err, err_len, ENAMETOOLONG);
    memcpy(address.sun_path, path, path_len + 1);
    socklen_t address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    if (!capture_remove_stale(&address, address_len, err, err_len)) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return capture_system_error(err, err_len, errno);
    /* Socket nodes start at 0777; enforce 0600 from creation, not only afterwards. */
    mode_t old_mask = umask(0177);
    int result;
    do {
        result = bind(fd, (const struct sockaddr *)&address, address_len);
    } while (result < 0 && errno == EINTR);
    int error = errno;
    umask(old_mask);
    if (result < 0) {
        close(fd);
        return capture_system_error(err, err_len, error);
    }
    srv->listen_fd = fd;
    memcpy(srv->path, path, path_len + 1);
    do {
        result = chmod(path, 0600);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        do {
            result = listen(fd, PURP_CAPTURE_MAX_CLIENTS);
        } while (result < 0 && errno == EINTR);
    }
    if (result == 0) result = setenv(PURP_ENV_CAPTURE_SOCKET, path, 1);
    if (result < 0) {
        error = errno;
        purp_capture_server_close(srv);
        return capture_system_error(err, err_len, error);
    }
    return true;
}

bool purp_capture_server_poll(purp_capture_server_t *srv, bool locked, purp_capture_fn capture,
                              void *data)
{
    if (!srv || srv->listen_fd < 0) return false;
    uint64_t now = purp_now_ms();
    for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i)
        capture_client_write(&srv->clients[i], now);

    /* Bound even a flood of excess connections to finite work per poll. */
    for (size_t accepted = 0; accepted < 2 * PURP_CAPTURE_MAX_CLIENTS; ++accepted) {
        int fd;
        do {
            fd = accept4(srv->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) break;
        purp_capture_client_t *client = NULL;
        for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i) {
            if (srv->clients[i].state == PURP_CAPTURE_CLIENT_FREE) {
                client = &srv->clients[i];
                break;
            }
        }
        if (!client) {
            close(fd);
            continue;
        }
        client->fd = fd;
        client->state = PURP_CAPTURE_CLIENT_READING;
        client->since_ms = purp_now_ms();
    }

    bool waiting[PURP_CAPTURE_MAX_CLIENTS] = { false };
    bool have_waiting = false;
    now = purp_now_ms();
    for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i) {
        purp_capture_client_t *client = &srv->clients[i];
        if (client->state == PURP_CAPTURE_CLIENT_READING) {
            waiting[i] = capture_client_read(client, now);
            have_waiting |= waiting[i];
        }
    }
    bool rendered = have_waiting && !locked && capture != NULL;
    purp_blob_t *png = rendered ? capture(data) : NULL;
    now = purp_now_ms();
    for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i) {
        purp_capture_client_t *client = &srv->clients[i];
        if (waiting[i]) {
            /* The callback is synchronous: discard a render that exceeded its wait
             * deadline before starting any response, without introducing a thread. */
            capture_reply(client, now < client->deadline_ms ? png : NULL, now);
        }
        capture_client_write(client, now);
    }
    purp_blob_unref(png);
    return rendered;
}

int purp_capture_server_fd(const purp_capture_server_t *srv)
{
    return srv ? srv->listen_fd : -1;
}

void purp_capture_server_close(purp_capture_server_t *srv)
{
    if (!srv) return;
    for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i)
        capture_client_close(&srv->clients[i]);
    if (srv->listen_fd >= 0 && srv->path[0]) close(srv->listen_fd);
    if (srv->path[0]) {
        struct stat status;
        if (lstat(srv->path, &status) == 0 && S_ISSOCK(status.st_mode) &&
            status.st_uid == geteuid()) {
            int result;
            do {
                result = unlink(srv->path);
            } while (result < 0 && errno == EINTR);
        }
        const char *exported = getenv(PURP_ENV_CAPTURE_SOCKET);
        if (exported && strcmp(exported, srv->path) == 0) unsetenv(PURP_ENV_CAPTURE_SOCKET);
    }
    srv->listen_fd = -1;
    srv->path[0] = '\0';
}
