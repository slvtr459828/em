#include <stdio.h>
#include "util.h"

static int sv[2];
static int grand_res, parked_res;
static em_scope *outer_p;

static int grandchild(void *arg) { (void)arg; char b; grand_res = (int)em_recv(sv[0], &b, 1); return 0; }
static int parker(void *arg) { (void)arg; parked_res = em_park(); return 0; }

static int child(void *arg)
{
    (void)arg;
    em_scope inner;
    em_scope_open(&inner, 0);
    em_spawn(&inner, grandchild, NULL);
    em_spawn(&inner, parker, NULL);
    return em_scope_close(&inner); /* waits here while the outer scope is cancelled */
}

static int main_fn(void *arg)
{
    (void)arg;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    em_scope outer;
    outer_p = &outer;
    em_scope_open(&outer, 0);
    em_spawn(&outer, child, NULL);
    for (int i = 0; i < 5; i++) em_yield(); /* let grandchild reach recv */
    em_scope_cancel(&outer);
    int r = em_scope_close(&outer);
    assert(grand_res == -ECANCELED && parked_res == -ECANCELED);
    assert(r == -ECANCELED); /* children returned 0, but the scope itself was cancelled */
    em_close(sv[0]); em_close(sv[1]);
    printf("nested: grandchild recv=%d park=%d\n", grand_res, parked_res);
    return 0;
}

int main(void)
{
    assert(em_run(main_fn, NULL) == 0);
    return 0;
}
