#define _GNU_SOURCE
#include <errno.h>
#include <time.h>
#include <netinet/in.h>
#include <unistd.h>
#include "internal.h"

/* Public I/O API. Each call branches once on the worker's backend (decided at
 * em_run, never changes): io_uring.c (ur_*) or io_epoll.c (ep_*).
 * Outside a fiber (em_W.current == NULL: before/after em_run on the calling
 * thread, or a foreign thread) there is no ring and no scope to wait in, so
 * every call fails with -EPERM instead of touching stale worker state. */
#define NEED_FIBER(err) do { if (!em_W.current) return (err); } while (0)

int64_t em_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Sockets em creates must be nonblocking on epoll (accept4/recv never park the
 * worker); io_uring doesn't care. */
static int sock_flags(void)
{
    return SOCK_CLOEXEC | (em_W.epoll ? SOCK_NONBLOCK : 0);
}

int em_sleep_until(int64_t deadline_ns)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_sleep_until(deadline_ns) : ur_sleep_until(deadline_ns);
}

int em_tcp_listen(const struct sockaddr *a, socklen_t n, int backlog)
{
    NEED_FIBER(-EPERM);
    int fd = socket(a->sa_family, SOCK_STREAM | sock_flags(), 0);
    if (fd < 0) return -errno;
    if (em_W.epoll) ep_fd_new(&em_W, fd, EM_FD_STREAM); /* reused number: no stale slot */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0 || /* every worker listens; kernel spreads */
        bind(fd, a, n) < 0 || listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

int em_accept(int lfd)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_accept(lfd) : ur_accept(lfd);
}

int em_connect(const struct sockaddr *a, socklen_t n)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_connect(a, n) : ur_connect(a, n);
}

ssize_t em_recv(int fd, void *buf, size_t len)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_recv(fd, buf, len) : ur_recv(fd, buf, len);
}

ssize_t em_read(int fd, void *buf, size_t len)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_read(fd, buf, len) : ur_read(fd, buf, len);
}

ssize_t em_send(int fd, const void *buf, size_t len)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_send(fd, buf, len) : ur_send(fd, buf, len);
}

int em_send_all(int fd, const void *buf, size_t len)
{
    NEED_FIBER(-EPERM);
    const char *p = buf;
    while (len) {
        ssize_t r = em_send(fd, p, len);
        if (r < 0) return (int)r;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

int em_shutdown(int fd, int how)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_shutdown(fd, how) : ur_shutdown(fd, how);
}

int em_close(int fd)
{
    if (!em_W.current) return close(fd) < 0 ? -errno : 0; /* no per-fd state outside a fiber */
    return em_W.epoll ? ep_close(fd) : ur_close(fd);
}

int em_udp_bind(const struct sockaddr *a, socklen_t n)
{
    NEED_FIBER(-EPERM);
    int fd = socket(a->sa_family, SOCK_DGRAM | sock_flags(), 0);
    if (fd < 0) return -errno;
    if (em_W.epoll) ep_fd_new(&em_W, fd, EM_FD_OTHER); /* reused number: no stale slot */
    int one = 1;
    in_port_t port = 0;
    if (a->sa_family == AF_INET) port = ((const struct sockaddr_in *)a)->sin_port;
    else if (a->sa_family == AF_INET6) port = ((const struct sockaddr_in6 *)a)->sin6_port;
    /* Port 0 = client: with SO_REUSEPORT the kernel may hand out an ephemeral
     * port another reuseport socket already holds, and replies would be hashed
     * between the two. */
    if ((port && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) ||
        bind(fd, a, n) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

ssize_t em_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_recvfrom(fd, buf, len, src, srclen) : ur_recvfrom(fd, buf, len, src, srclen);
}

ssize_t em_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n)
{
    NEED_FIBER(-EPERM);
    return em_W.epoll ? ep_sendto(fd, buf, len, dst, n) : ur_sendto(fd, buf, len, dst, n);
}
