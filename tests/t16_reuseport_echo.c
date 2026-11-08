/* N workers listen on one port (SO_REUSEPORT); clients on every worker open
 * NCONN connections in total, 64 KiB echoed and compared byte for byte. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
#include "util.h"

#define NCONN   10000
#define BYTES   (64u * 1024u)
#define CHUNK   16384u
#define WAVE    64 /* concurrent clients per worker: bounds fibers and fds */
#define MAXW    64

static int N;
static pthread_barrier_t b1, b2;
static _Atomic int port;
static _Atomic int clients_done;
static unsigned accepted[MAXW]; /* [worker]: written by its own thread only */

static int lfd_of[MAXW];
static em_scope *srv_scope_of[MAXW];

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
    int id = em_worker_id();
    for (;;) {
        int fd = em_accept(lfd_of[id]);
        if (fd == -ECANCELED) return 0; /* listener closed */
        if (fd < 0) return fd;
        accepted[id]++;
        if (em_spawn(srv_scope_of[id], echo, (void *)(intptr_t)fd) != 0) return -ENOMEM;
    }
}

struct cl { uint64_t seed; int fd; };

static void fill(char *buf, uint64_t seed)
{
    uint64_t s = seed;
    for (unsigned i = 0; i < BYTES; i += 8) { uint64_t v = xs_next(&s); memcpy(buf + i, &v, 8); }
}

static int writer(void *arg)
{
    struct cl *c = arg;
    char buf[BYTES];
    fill(buf, c->seed);
    return em_send_all(c->fd, buf, BYTES);
}

static int client(void *arg)
{
    struct cl c = { .seed = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1 };
    c.fd = connect_local((uint16_t)atomic_load(&port));
    if (c.fd < 0) return c.fd;
    em_scope ws;
    em_scope_open(&ws, 0);
    em_spawn(&ws, writer, &c);
    char want[BYTES], got[CHUNK];
    fill(want, c.seed);
    unsigned pos = 0;
    int r = 0;
    while (pos < BYTES) {
        ssize_t n = em_recv(c.fd, got, sizeof got);
        if (n <= 0) { r = n < 0 ? (int)n : -EIO; break; }
        if ((unsigned)n > BYTES - pos || memcmp(got, want + pos, (size_t)n) != 0) { r = -EIO; break; }
        pos += (unsigned)n;
    }
    if (r) em_scope_cancel(&ws);
    int wr = em_scope_close(&ws);
    em_close(c.fd);
    atomic_fetch_add(&clients_done, 1);
    return r ? r : wr;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    uint16_t p;
    em_scope srv;
    em_scope_open(&srv, 0);
    srv_scope_of[id] = &srv;
    if (id == 0) {
        lfd_of[0] = listen_local(&p);
        atomic_store(&port, p);
    }
    pthread_barrier_wait(&b1); /* port known */
    if (id != 0) {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atomic_load(&port)),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        lfd_of[id] = em_tcp_listen((struct sockaddr *)&a, sizeof a, 1024);
        assert(lfd_of[id] >= 0);
    }
    pthread_barrier_wait(&b2); /* every listener is up */
    em_spawn(&srv, acceptor, NULL);

    int mine = NCONN / N + (id < NCONN % N);
    int r = 0;
    for (int done = 0; done < mine && !r; ) {
        em_scope wave;
        em_scope_open(&wave, 0);
        for (int i = 0; i < WAVE && done < mine; i++, done++)
            assert(em_spawn(&wave, client, (void *)(intptr_t)(done * N + id)) == 0);
        r = em_scope_close(&wave);
    }
    /* Other workers' clients may still connect to MY listener: keep serving. */
    while (!r && atomic_load(&clients_done) < NCONN) r = em_sleep_until(em_now() + 1000000);
    em_close(lfd_of[id]); /* acceptor wakes with -ECANCELED */
    int r2 = em_scope_close(&srv);
    return r ? r : r2;
}

int main(void)
{
    struct rlimit rl;
    assert(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    rl.rlim_cur = rl.rlim_max; /* 2 fds per connection, in flight */
    assert(setrlimit(RLIMIT_NOFILE, &rl) == 0);
    N = test_workers();
    assert(N <= MAXW);
    assert(pthread_barrier_init(&b1, NULL, (unsigned)N) == 0);
    assert(pthread_barrier_init(&b2, NULL, (unsigned)N) == 0);
    assert(em_run(N, main_fn, NULL) == 0);
    assert(atomic_load(&clients_done) == NCONN);
    unsigned total = 0;
    for (int i = 0; i < N; i++) {
        printf("worker %d accepted %u\n", i, accepted[i]);
        assert(accepted[i] > 0);
        total += accepted[i];
    }
    assert(total == NCONN);
    printf("reuseport echo: %d connections x %u bytes over %d workers verified\n", NCONN, BYTES, N);
    return 0;
}
