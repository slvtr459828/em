#include <errno.h>
#include <stdio.h>
#include "util.h"
#include "../src/internal.h"

static int woke_res[3];

static int parker(void *arg)
{
    int i = (int)(intptr_t)arg;
    woke_res[i] = em_park();
    return 0;
}

static int main_clean(void *arg)
{
    (void)arg;
    em_scope s, inner;
    em_scope_open(&s, 0);
    em_spawn(&s, parker, (void *)0);
    em_spawn(&s, parker, (void *)1);
    em_scope_open(&inner, 0);
    em_spawn(&inner, parker, (void *)2);
    em_scope_close(&inner); /* nested waiter is unwound too */
    em_scope_close(&s);
    return 0;
}

/* Ignores the first cancel and parks again: must not hang, the second park
 * returns -ECANCELED at once because the chain is still cancelled. */
static int stubborn(void *arg) { (void)arg; em_park(); return em_park() == -ECANCELED ? 0 : 1; }

static int main_stuck(void *arg)
{
    (void)arg;
    em_scope s, inner;
    em_scope_open(&s, 0);
    em_spawn(&s, stubborn, NULL);
    em_scope_open(&inner, 0);
    em_spawn(&inner, stubborn, NULL);
    em_scope_close(&inner);
    em_scope_close(&s);
    return 0;
}

int main(void)
{
    assert(em_run(main_clean, NULL) == -EDEADLK);
    for (int i = 0; i < 3; i++) assert(woke_res[i] == -ECANCELED);
    assert(em_dbg_mmap_count == em_dbg_munmap_count);
    printf("deadlock unwind: %zu mmaps == %zu munmaps\n", em_dbg_mmap_count, em_dbg_munmap_count);

    assert(em_run(main_stuck, NULL) == -EDEADLK);
    assert(em_dbg_mmap_count == em_dbg_munmap_count);
    printf("deadlock re-park: %zu mmaps == %zu munmaps\n", em_dbg_mmap_count, em_dbg_munmap_count);
    return 0;
}
