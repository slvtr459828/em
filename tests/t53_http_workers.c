/* One HTTP server per worker (own port); every worker's clients hit the next worker's server,
 * keep-alive, answers checked: Date's __thread cache and the per-connection buffers hold up. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "http_util.h"

#define MAXW  64
#define NCONN 40 /* per worker */
#define NREQ  20 /* per connection */

static int N;
static pthread_barrier_t bar;
static uint16_t ports[MAXW];
static _Atomic int clients_done;

static int client(void *arg)
{
    intptr_t id = (intptr_t)arg;
    int wid = em_worker_id();
    rd_t *r = rd_open(ports[(wid + 1) % N]);
    for (int i = 0; i < NREQ; i++) {
        char req[96], body[64];
        snprintf(req, sizeof req, "GET /w%d/%d/%d HTTP/1.1\r\nHost: x\r\n\r\n", wid, (int)id, i);
        snprintf(body, sizeof body, "GET /w%d/%d/%d", wid, (int)id, i);
        send_str(r->fd, req);
        expect_resp(r, "HTTP/1.1 200 ", body, NULL);
    }
    rd_close(r);
    atomic_fetch_add(&clients_done, 1);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    em_scope dl, sc, cl;
    struct hsrv s;
    em_scope_open(&dl, em_now() + 30000000000ll);
    http_start(&s, &sc, &ports[id]);
    pthread_barrier_wait(&bar); /* every port is known */
    em_scope_open(&cl, 0);
    for (long i = 0; i < NCONN; i++) assert(em_spawn(&cl, client, (void *)i) == 0);
    int r = em_scope_close(&cl);
    /* other workers' clients may still be talking to this server */
    while (!r && atomic_load(&clients_done) < N * NCONN) r = em_sleep_until(em_now() + 1000000);
    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r ? r : r1 ? r1 : r2;
}

int main(void)
{
    N = test_workers();
    assert(N <= MAXW);
    assert(pthread_barrier_init(&bar, NULL, (unsigned)N) == 0);
    assert(em_run(N, main_fn, NULL) == 0);
    assert(atomic_load(&clients_done) == N * NCONN);
    printf("http workers: %d workers x %d connections x %d requests verified\n", N, NCONN, NREQ);
    return 0;
}
