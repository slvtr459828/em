#include <stdio.h>
#include "util.h"

static int lfd;
static int sv[2];
static int acc_res, recv_res;

static int acc(void *arg) { (void)arg; acc_res = em_accept(lfd); return acc_res; }
static int rcv(void *arg) { (void)arg; char b; recv_res = (int)em_recv(sv[0], &b, 1); return recv_res; }
static int failer(void *arg) { (void)arg; em_yield(); em_yield(); return -EIO; }

static int main_fn(void *arg)
{
    (void)arg;
    uint16_t port;
    lfd = listen_local(&port);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    em_scope s;
    em_scope_open(&s, 0);
    em_spawn(&s, acc, NULL);
    em_spawn(&s, rcv, NULL);
    em_spawn(&s, failer, NULL);
    int r = em_scope_close(&s);
    assert(r == -EIO);
    assert(acc_res == -ECANCELED && recv_res == -ECANCELED);
    em_close(lfd); em_close(sv[0]); em_close(sv[1]);
    printf("failfast: close=%d accept=%d recv=%d\n", r, acc_res, recv_res);
    return 0;
}

int main(void)
{
    assert(em_run(main_fn, NULL) == 0);
    return 0;
}
