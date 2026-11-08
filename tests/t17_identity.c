/* Each worker sees a unique id in [0, N), the right count, and runs on the CPU
 * it was pinned to: the i-th allowed CPU, wrapping when N > allowed CPUs. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"

#define MAXW 64
static int N;
static cpu_set_t allowed; /* captured before em_run pins worker 0 */
static _Atomic int seen[MAXW];

static int nth_allowed(int k)
{
    k %= CPU_COUNT(&allowed);
    for (int c = 0;; c++)
        if (CPU_ISSET(c, &allowed) && k-- == 0) return c;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    assert(id >= 0 && id < N);
    assert(em_worker_count() == N);
    assert(atomic_fetch_add(&seen[id], 1) == 0);
    em_sleep_until(em_now() + 1000000); /* an op round trip must not migrate us */
    assert(sched_getcpu() == nth_allowed(id));
    return 0;
}

int main(void)
{
    N = test_workers();
    assert(N <= MAXW);
    assert(sched_getaffinity(0, sizeof allowed, &allowed) == 0);
    assert(em_worker_id() == 0 && em_worker_count() == 0); /* outside em_run */
    assert(em_run(N, main_fn, NULL) == 0);
    for (int i = 0; i < N; i++) assert(seen[i] == 1);
    cpu_set_t after;
    assert(sched_getaffinity(0, sizeof after, &after) == 0);
    assert(CPU_EQUAL(&after, &allowed)); /* caller's mask restored */
    /* nworkers = 0: one worker per allowed CPU */
    for (int i = 0; i < MAXW; i++) atomic_store(&seen[i], 0);
    N = CPU_COUNT(&allowed);
    assert(N <= MAXW);
    assert(em_run(0, main_fn, NULL) == 0);
    for (int i = 0; i < N; i++) assert(seen[i] == 1);
    printf("identity: %d workers pinned, then %d (nworkers=0)\n", test_workers(), N);
    return 0;
}
