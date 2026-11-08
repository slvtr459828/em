#include <stdio.h>
#include "util.h"

static int sv[2];
static int a_res = 12345;

static int a(void *arg) { (void)arg; char b; a_res = (int)em_recv(sv[0], &b, 1); return 0; }
static int bfn(void *arg) { (void)arg; em_yield(); em_yield(); return em_close(sv[0]); }

static int main_fn(void *arg)
{
    (void)arg;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    em_scope s;
    em_scope_open(&s, 0);
    em_spawn(&s, a, NULL);
    em_spawn(&s, bfn, NULL);
    assert(em_scope_close(&s) == 0);
    assert(a_res == -ECANCELED);

    /* em_close is not cancellable: it closes even in a cancelled scope */
    int fd = sv[1];
    em_scope_open(&s, 0);
    em_scope_cancel(&s);
    assert(em_recv(fd, &(char){0}, 1) == -ECANCELED);
    assert(em_close(fd) == 0);
    em_scope_close(&s);
    assert(em_close(fd) == -EBADF); /* really gone */
    printf("close_recv: recv=%d\n", a_res);
    return 0;
}

int main(void)
{
    assert(em_run(main_fn, NULL) == 0);
    return 0;
}
