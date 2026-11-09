/* N workers, one SSL_CTX shared by all: 2000 handshakes in flight at once (client and server side,
 * each worker listens on one SO_REUSEPORT port), each followed by a 1-byte echo. Prints
 * handshakes/s (lock contention inside OpenSSL would show up here). */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/resource.h>
#include "tls_util.h"

#define NCONN 2000
#define MAXW  64

static SSL_CTX *sctx, *cctx;
static int N;
static pthread_barrier_t b1, b2;
static _Atomic int port;
static _Atomic int clients_done;
static int lfd_of[MAXW];
static em_scope *srv_scope_of[MAXW];

static int echo(void *arg)
{
    int fd = (int)(intptr_t)arg;
    em_tls *t;
    int r = em_tls_accept(&t, sctx, fd);
    if (r == 0) {
        char b[256];
        for (;;) {
            ssize_t n = em_tls_recv(t, b, sizeof b);
            if (n < 0) { r = (int)n; break; }
            if (n == 0) { r = em_tls_shutdown(t); break; }
            if ((r = em_tls_send_all(t, b, (size_t)n)) < 0) break;
        }
        em_tls_free(t);
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
        if (em_spawn(srv_scope_of[id], echo, (void *)(intptr_t)fd) != 0) return -ENOMEM;
    }
}

static int client(void *arg)
{
    (void)arg;
    int fd = connect_local((uint16_t)atomic_load(&port));
    if (fd < 0) return fd;
    em_tls *t;
    int r = em_tls_connect(&t, cctx, fd, "localhost");
    if (r == 0) {
        char b = 'x', c = 0;
        if ((r = em_tls_send_all(t, &b, 1)) == 0) {
            ssize_t n = em_tls_recv(t, &c, 1);
            r = n == 1 && c == 'x' ? 0 : n < 0 ? (int)n : -EIO;
        }
        if (!r) r = em_tls_shutdown(t);
        if (!r) { ssize_t n = em_tls_recv(t, &c, 1); r = n == 0 ? 0 : n < 0 ? (int)n : -EIO; }
        em_tls_free(t);
    }
    em_close(fd);
    atomic_fetch_add(&clients_done, 1);
    return r;
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
        lfd_of[id] = em_tcp_listen((struct sockaddr *)&a, sizeof a, 4096);
        assert(lfd_of[id] >= 0);
    }
    pthread_barrier_wait(&b2); /* every listener is up */
    em_spawn(&srv, acceptor, NULL);

    em_scope cls;
    em_scope_open(&cls, 0);
    int mine = NCONN / N + (id < NCONN % N);
    for (int i = 0; i < mine; i++) assert(em_spawn(&cls, client, NULL) == 0);
    int r = em_scope_close(&cls);
    /* other workers' clients may still connect to MY listener: keep serving */
    while (!r && atomic_load(&clients_done) < NCONN) r = em_sleep_until(em_now() + 1000000);
    em_close(lfd_of[id]);
    int r2 = em_scope_close(&srv);
    return r ? r : r2;
}

int main(void)
{
    struct rlimit rl;
    assert(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    rl.rlim_cur = rl.rlim_max; /* 2 fds per connection, all in flight */
    assert(setrlimit(RLIMIT_NOFILE, &rl) == 0);
    N = test_workers();
    assert(N <= MAXW);
    tls_test_ctx(&sctx, &cctx);
    assert(pthread_barrier_init(&b1, NULL, (unsigned)N) == 0);
    assert(pthread_barrier_init(&b2, NULL, (unsigned)N) == 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(em_run(N, main_fn, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    assert(atomic_load(&clients_done) == NCONN);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("tls workers: %d handshakes over %d workers, %.0f handshakes/s (both ends of each in this process)\n",
           NCONN, N, NCONN / s);
    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    return 0;
}
