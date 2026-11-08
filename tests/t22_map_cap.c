/* Process-wide mapping cap: workers share one budget; going over it is -ENOMEM
 * on whichever worker asks, and the total never exceeds the cap. */
#include <pthread.h>
#include <stdio.h>
#include "util.h"
#include "internal.h"

#define CAP 64
#define TRIES 200
#define MAXW 64
static int N;
static pthread_barrier_t bar;
static int ok[MAXW], nomem[MAXW];

static int child(void *arg)
{
    (void)arg;
    em_park(); /* returns -ECANCELED at once: the scope is cancelled before we run */
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    /* Every root exists before anyone spawns children: otherwise a greedy worker
     * could take the whole budget and another root would fail with -ENOMEM. */
    pthread_barrier_wait(&bar);
    em_scope s;
    em_scope_open(&s, 0);
    for (int i = 0; i < TRIES; i++) {
        int r = em_spawn(&s, child, NULL);
        if (r == 0) ok[id]++;
        else { assert(r == -ENOMEM); nomem[id]++; }
    }
    em_scope_cancel(&s);
    em_scope_close(&s);
    return 0;
}

int main(void)
{
    N = test_workers();
    assert(N <= MAXW && N <= CAP);
    assert(pthread_barrier_init(&bar, NULL, (unsigned)N) == 0);
    em_dbg_map_cap = CAP;
    assert(em_run(N, main_fn, NULL) == 0);
    size_t total = 0;
    for (int i = 0; i < N; i++) {
        assert(nomem[i] > 0);
        total += (size_t)ok[i];
    }
    /* each worker's main fiber took a mapping too */
    assert(em_dbg_mmap_count <= CAP);
    assert(em_dbg_mmap_count == em_dbg_munmap_count);
    printf("map cap: %zu children + %d roots mapped <= cap %d, every worker saw -ENOMEM\n", total, N, CAP);
    return 0;
}
