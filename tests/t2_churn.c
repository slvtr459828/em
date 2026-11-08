#include <assert.h>
#include <stdio.h>
#include "../include/em.h"
#include "../src/internal.h"

#define N 1000000
#define MAX_CONCURRENT 2 /* main fiber + one child at a time */

static int done;
static int child(void *arg) { (void)arg; done = 1; return 0; }

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    for (int i = 0; i < N; i++) {
        done = 0;
        assert(em_spawn(&s, child, NULL) == 0);
        while (!done) em_yield();
    }
    return em_scope_close(&s);
}

int main(void)
{
    assert(em_run(main_fn, NULL) == 0);
    printf("churn: %d fibers, %zu mmaps (max concurrent %d)\n", N, em_dbg_mmap_count, MAX_CONCURRENT);
    assert(em_dbg_mmap_count <= MAX_CONCURRENT);
    return 0;
}
