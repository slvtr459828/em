/* (1) An em-created fd closed with plain close() must not leave state that the
 * next em-created fd with the same number inherits (epoll: stale "registered"
 * slot -> the new fd is never added to epoll -> its first wait never wakes).
 * (2) Cancelling a blocked recvfrom returns -ECANCELED on both backends
 * (io_uring can complete it with -EINTR when the op runs in io-wq). */
#include <stdio.h>
#include <unistd.h>
#include "util.h"

static int udp_local(struct sockaddr_in *a)
{
    *a = (struct sockaddr_in){ .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int fd = em_udp_bind((struct sockaddr *)a, sizeof *a);
    assert(fd >= 0);
    socklen_t n = sizeof *a;
    assert(getsockname(fd, (struct sockaddr *)a, &n) == 0);
    return fd;
}

static int fd_a;
static int reader(void *arg)
{
    (void)arg;
    char b[8];
    return em_recvfrom(fd_a, b, sizeof b, NULL, NULL) == 1 ? 0 : -EIO;
}

static int blocked(void *arg)
{
    char b[8];
    *(ssize_t *)arg = em_recvfrom(fd_a, b, sizeof b, NULL, NULL);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    struct sockaddr_in a;
    /* (1) wait on fd once (registers it), then close it behind em's back */
    int old = udp_local(&a);
    em_scope s;
    em_scope_open(&s, em_now() + 20000000);
    char b[8];
    assert(em_recvfrom(old, b, sizeof b, NULL, NULL) == -ETIMEDOUT);
    em_scope_close(&s);
    close(old);
    fd_a = udp_local(&a);
    assert(fd_a == old); /* same number reused */
    em_scope_open(&s, em_now() + 2000000000ll); /* a hang shows up as -ETIMEDOUT */
    assert(em_spawn(&s, reader, NULL) == 0);
    em_yield(); /* reader parks */
    int tx = em_udp_bind((struct sockaddr *)&(struct sockaddr_in){ .sin_family = AF_INET,
                         .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }, sizeof a);
    assert(tx >= 0);
    assert(em_sendto(tx, "x", 1, (struct sockaddr *)&a, sizeof a) == 1);
    assert(em_scope_close(&s) == 0);

    /* (2) cancel a parked recvfrom, many times */
    for (int i = 0; i < 500; i++) {
        ssize_t r = 1;
        em_scope_open(&s, 0);
        assert(em_spawn(&s, blocked, &r) == 0);
        for (int k = 0; k < (i & 3); k++) em_yield();
        em_scope_cancel(&s);
        assert(em_scope_close(&s) == -ECANCELED);
        if (r != -ECANCELED) { fprintf(stderr, "iteration %d: recvfrom = %zd\n", i, r); return -EIO; }
    }
    em_close(tx);
    em_close(fd_a);
    printf("fd reuse (%s): reused fd wakes; 500 cancelled recvfrom -> -ECANCELED\n", em_backend());
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
