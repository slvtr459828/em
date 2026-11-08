/* A fiber on worker 0 calls em_cancel_all: every worker unwinds. */
#include <stdio.h>
#include "util.h"

#define MAXW 64
static int N;
static int result[MAXW];
static int lfd_of[MAXW];

static int waiter(void *arg)
{
    int r = em_accept(lfd_of[(int)(intptr_t)arg]);
    result[(int)(intptr_t)arg] = r;
    return r;
}

static int canceller(void *arg)
{
    (void)arg;
    em_sleep_until(em_now() + 20000000);
    em_cancel_all();
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    uint16_t port;
    lfd_of[id] = listen_local(&port);
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, waiter, (void *)(intptr_t)id) == 0);
    if (id == 0) assert(em_spawn(&s, canceller, NULL) == 0);
    int r = em_scope_close(&s);
    em_close(lfd_of[id]);
    return r;
}

int main(void)
{
    N = test_workers();
    assert(N <= MAXW);
    assert(em_run(N, main_fn, NULL) == -ECANCELED);
    for (int i = 0; i < N; i++) assert(result[i] == -ECANCELED);
    printf("cancel_all: %d workers unwound, em_run == -ECANCELED\n", N);
    return 0;
}
