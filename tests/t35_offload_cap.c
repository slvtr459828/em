/* 200 offloads at once from several workers: every result is right and the pool never has
 * more than EM_POOL_MAX (64) threads (counted in /proc/self/task, workers included). */
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"
#include "../src/internal.h"

#define PER_WORKER 50

static _Atomic int peak, ndone;

static int nthreads(void)
{
    DIR *d = opendir("/proc/self/task");
    assert(d);
    int n = 0;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static intptr_t job(void *arg)
{
    int n = nthreads(), p = atomic_load(&peak);
    while (n > p && !atomic_compare_exchange_weak(&peak, &p, n)) {}
    usleep(30 * 1000);
    return (intptr_t)arg * 3 + 1;
}

static int caller(void *arg)
{
    intptr_t i = (intptr_t)arg;
    assert(em_offload(job, (void *)i) == i * 3 + 1);
    atomic_fetch_add(&ndone, 1);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    for (int i = 0; i < PER_WORKER; i++)
        assert(em_spawn(&s, caller, (void *)(intptr_t)(em_worker_id() * 1000 + i)) == 0);
    assert(em_scope_close(&s) == 0);
    return 0;
}

int main(void)
{
    int n = test_workers();
    assert(em_run(n, main_fn, NULL) == 0);
    assert(ndone == n * PER_WORKER);
    assert(peak > n);                       /* the pool did grow */
    assert(peak <= EM_POOL_MAX + n);        /* workers + at most 64 pool threads (+ nothing else) */
    printf("offload cap (%s): %d results, peak %d threads (%d workers + pool, cap %d)\n",
           getenv("EM_BACKEND") ? getenv("EM_BACKEND") : "auto", ndone, peak, n, EM_POOL_MAX);
    return 0;
}
