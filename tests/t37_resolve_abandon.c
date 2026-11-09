/* The job of em_resolve (heap, abandoned on cancel/deadline) with a fake 500 ms lookup in
 * place of DNS (the CI resolver cannot be steered). The caller returns at once; em_run waits
 * for the abandoned jobs and frees them (LSan checks). A job that finishes in time is normal. */
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"
#include "../src/internal.h"

static _Atomic int finished;
static int r_deadline, r_cancel;
static int64_t el_deadline, el_cancel;

static intptr_t slow(void *arg)
{
    (void)arg;
    usleep(500 * 1000);
    atomic_fetch_add(&finished, 1);
    return 7;
}

static intptr_t quick(void *arg)
{
    (void)arg;
    usleep(10 * 1000);
    return 5;
}

static int by_cancel(void *arg)
{
    (void)arg;
    int64_t t0 = em_now();
    r_cancel = em_dbg_heap_job(slow, NULL);
    el_cancel = em_now() - t0;
    return 0;
}

static int canceller(void *arg)
{
    em_scope *s = arg;
    assert(em_sleep_until(em_now() + 20 * 1000000LL) == 0);
    em_scope_cancel(s);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    /* the fiber returns at the deadline (20 ms), not at 500 ms */
    em_scope d;
    em_scope_open(&d, em_now() + 20 * 1000000LL);
    int64_t t0 = em_now();
    r_deadline = em_dbg_heap_job(slow, NULL);
    el_deadline = em_now() - t0;
    em_scope_close(&d);

    em_scope c;
    em_scope_open(&c, 0);
    assert(em_spawn(&c, by_cancel, NULL) == 0);
    assert(em_spawn(&c, canceller, &c) == 0);
    em_scope_close(&c);

    assert(em_dbg_heap_job(quick, NULL) == 5);
    assert(em_dbg_heap_job(quick, NULL) == 5);
    /* and the fiber is usable afterwards */
    assert(em_sleep_until(em_now() + 1000000) == 0);
    return 0;
}

int main(void)
{
    assert(em_dbg_heap_job(quick, NULL) == -EPERM);
    int64_t t0 = em_now();
    assert(em_run(1, main_fn, NULL) == 0);
    int64_t total = em_now() - t0;
    assert(r_deadline == -ETIMEDOUT && r_cancel == -ECANCELED);
    assert(el_deadline < 400 * 1000000LL && el_cancel < 400 * 1000000LL);
    assert(atomic_load(&finished) == 2); /* em_run did not return before the abandoned lookups */
    printf("resolve abandon (%s): returned after %lld / %lld ms, em_run waited (%lld ms)\n",
           getenv("EM_BACKEND") ? getenv("EM_BACKEND") : "auto",
           (long long)(el_deadline / 1000000), (long long)(el_cancel / 1000000), (long long)(total / 1000000));
    return 0;
}
