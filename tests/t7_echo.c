#include <stdio.h>
#include <sys/resource.h>
#include "util.h"

#define NCONN 1000
#define BYTES (1u << 20)
#define CHUNK 16384u

static em_scope *srv_scope;
static int lfd;
static uint16_t port;
static unsigned done_clients;

static int echo(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[CHUNK];
    int r = 0;
    for (;;) {
        ssize_t n = em_recv(fd, buf, sizeof buf);
        if (n < 0) { r = (int)n; break; }
        if (n == 0) break;
        if ((r = em_send_all(fd, buf, (size_t)n)) < 0) break;
    }
    em_close(fd);
    return r;
}

static int acceptor(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = em_accept(lfd);
        if (fd == -ECANCELED) return 0; /* listener closed by main */
        if (fd < 0) return fd;
        if (em_spawn(srv_scope, echo, (void *)(intptr_t)fd) != 0) return -ENOMEM;
    }
}

struct cl { uint64_t seed; int fd; };

static int writer(void *arg)
{
    struct cl *c = arg;
    uint64_t s = c->seed;
    char buf[CHUNK];
    for (unsigned sent = 0; sent < BYTES; sent += CHUNK) {
        for (unsigned i = 0; i < CHUNK; i += 8) { uint64_t v = xs_next(&s); memcpy(buf + i, &v, 8); }
        int r = em_send_all(c->fd, buf, CHUNK);
        if (r < 0) return r;
    }
    return em_shutdown(c->fd, SHUT_WR);
}

static int client(void *arg)
{
    struct cl c = { .seed = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1 };
    c.fd = connect_local(port);
    if (c.fd < 0) return c.fd;
    em_scope ws;
    em_scope_open(&ws, 0);
    em_spawn(&ws, writer, &c);
    uint64_t s = c.seed;
    char buf[CHUNK], want[8];
    unsigned got = 0, pos = 0; /* pos: offset in the current 8-byte word */
    uint64_t v = 0;
    int r = 0;
    while (got < BYTES) {
        ssize_t n = em_recv(c.fd, buf, sizeof buf);
        if (n <= 0) { r = n < 0 ? (int)n : -EIO; break; }
        for (ssize_t i = 0; i < n; i++, got++) {
            if (pos == 0) { v = xs_next(&s); memcpy(want, &v, 8); }
            if (buf[i] != want[pos]) { r = -EIO; goto out; }
            pos = (pos + 1) & 7;
        }
    }
    if (!r) { ssize_t n = em_recv(c.fd, buf, 1); if (n != 0) r = -EIO; } /* EOF after echo */
out:
    if (r) em_scope_cancel(&ws);
    int wr = em_scope_close(&ws);
    em_close(c.fd);
    done_clients++;
    return r ? r : wr;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope srv, cls;
    em_scope_open(&srv, 0);
    srv_scope = &srv;
    lfd = listen_local(&port);
    em_spawn(&srv, acceptor, NULL);
    em_scope_open(&cls, 0);
    for (long i = 0; i < NCONN; i++) assert(em_spawn(&cls, client, (void *)i) == 0);
    int r = em_scope_close(&cls);
    em_close(lfd); /* acceptor wakes with -ECANCELED */
    int r2 = em_scope_close(&srv);
    return r ? r : r2;
}

int main(void)
{
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max; /* 2 fds per connection */
    setrlimit(RLIMIT_NOFILE, &rl);
    assert(em_run(main_fn, NULL) == 0);
    assert(done_clients == NCONN);
    printf("echo: %d connections x %u bytes verified\n", NCONN, BYTES);
    return 0;
}
