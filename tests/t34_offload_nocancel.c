/* em_offload is not interruptible: a scope deadline or cancel that hits during the call does
 * not cut it short (fn owns buffers on the caller's stack); the next cancel point reports it. */
#include <stdio.h>
#include <unistd.h>
#include "util.h"

static intptr_t slow(void *arg)
{
    (void)arg;
    usleep(100 * 1000);
    return 42;
}

static int cancel_later(void *arg)
{
    em_scope *s = arg;
    assert(em_sleep_until(em_now() + 20 * 1000000LL) == 0);
    em_scope_cancel(s);
    return 0;
}

static int off(void *arg)
{
    (void)arg;
    int64_t t0 = em_now();
    assert(em_offload(slow, NULL) == 42);
    assert(em_now() - t0 >= 95 * 1000000LL);
    assert(em_sleep_until(em_now() + 1000000000LL) == -ECANCELED);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    /* deadline */
    em_scope s;
    em_scope_open(&s, em_now() + 20 * 1000000LL);
    int64_t t0 = em_now();
    assert(em_offload(slow, NULL) == 42);
    assert(em_now() - t0 >= 95 * 1000000LL);
    assert(em_sleep_until(em_now() + 1000000000LL) == -ETIMEDOUT);
    assert(em_scope_close(&s) == 0);

    /* cancel from a sibling */
    em_scope c;
    em_scope_open(&c, 0);
    assert(em_spawn(&c, off, NULL) == 0);
    assert(em_spawn(&c, cancel_later, &c) == 0);
    em_scope_close(&c);
    printf("offload not interruptible (%s)\n", em_backend());
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
