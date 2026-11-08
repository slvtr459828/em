/* Several fibers wait on one UDP socket: every datagram is delivered to exactly
 * one of them, none is lost, nobody hangs. */
#include <stdio.h>
#include "util.h"

#define N 1000
#define RECEIVERS 4

static int rfd;
static uint16_t port;
static int got[N], total;

static int receiver(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t seq;
        ssize_t r = em_recvfrom(rfd, &seq, sizeof seq, NULL, NULL);
        assert(r == (ssize_t)sizeof seq);
        if (seq >= N) return 0; /* sentinel: one per receiver */
        got[seq]++;
        total++;
    }
}

static int sender(void *arg)
{
    (void)arg;
    int fd = em_udp_bind((struct sockaddr *)&(struct sockaddr_in){ .sin_family = AF_INET,
                         .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }, sizeof(struct sockaddr_in));
    assert(fd >= 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    for (uint32_t i = 0; i < N + RECEIVERS; i++) { /* N datagrams, then one sentinel (seq >= N) per receiver */
        assert(em_sendto(fd, &i, sizeof i, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof i);
        if (i % 8 == 7) em_yield(); /* let the receivers drain: the socket buffer is finite */
    }
    em_close(fd);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    rfd = em_udp_bind((struct sockaddr *)&a, sizeof a);
    assert(rfd >= 0);
    socklen_t l = sizeof a;
    assert(getsockname(rfd, (struct sockaddr *)&a, &l) == 0);
    port = ntohs(a.sin_port);
    em_scope s;
    em_scope_open(&s, em_now() + 10000000000LL); /* a hang shows up as -ETIMEDOUT, not a stuck test */
    for (int i = 0; i < RECEIVERS; i++) assert(em_spawn(&s, receiver, NULL) == 0);
    assert(em_spawn(&s, sender, NULL) == 0);
    assert(em_scope_close(&s) == 0);
    assert(total == N);
    for (int i = 0; i < N; i++) assert(got[i] == 1);
    em_close(rfd);
    printf("udp waiters (%s): %d datagrams over %d fibers, none lost\n", em_backend(), N, RECEIVERS);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
