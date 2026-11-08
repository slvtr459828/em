#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include "../include/em.h"

static int a_ok, b_ok;

static int a(void *arg)
{
    (void)arg;
    assert(fesetround(FE_UPWARD) == 0);
    em_yield();
    a_ok = fegetround() == FE_UPWARD;
    em_yield();
    a_ok = a_ok && fegetround() == FE_UPWARD;
    return 0;
}

static int b(void *arg)
{
    (void)arg;
    int r1 = fegetround() == FE_TONEAREST;
    em_yield();
    int r2 = fegetround() == FE_TONEAREST;
    em_yield();
    b_ok = r1 && r2 && fegetround() == FE_TONEAREST;
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, a, NULL) == 0);
    assert(em_spawn(&s, b, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    assert(em_run(main_fn, NULL) == 0);
    assert(a_ok && b_ok);
    assert(fegetround() == FE_TONEAREST); /* scheduler context untouched */
    printf("fp: rounding mode isolated per fiber\n");
    return 0;
}
