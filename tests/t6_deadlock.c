#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "../include/em.h"

static int parker(void *arg) { (void)arg; return em_park(); }

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, parker, NULL) == 0);
    assert(em_spawn(&s, parker, NULL) == 0);
    int r = em_park();
    assert(r == -ECANCELED);
    em_scope_close(&s);
    return r;
}

int main(void)
{
    assert(em_run(main_fn, NULL) == -EDEADLK);
    printf("deadlock: em_run returned -EDEADLK\n");
    return 0;
}
