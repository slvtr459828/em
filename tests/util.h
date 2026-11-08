#ifndef EM_TEST_UTIL_H
#define EM_TEST_UTIL_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "../include/em.h"

/* Worker count for multi-worker tests; fixed so results don't depend on the
 * runner's core count (workers wrap round-robin over the allowed CPUs). */
static inline int test_workers(void)
{
    const char *e = getenv("EM_TEST_WORKERS");
    int n = e ? atoi(e) : 4;
    assert(n >= 1);
    return n;
}

static inline uint64_t xs_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}

/* Listening socket on 127.0.0.1:0; returns fd, stores the port. */
static inline int listen_local(uint16_t *port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int fd = em_tcp_listen((struct sockaddr *)&a, sizeof a, 1024);
    assert(fd >= 0);
    socklen_t n = sizeof a;
    assert(getsockname(fd, (struct sockaddr *)&a, &n) == 0);
    *port = ntohs(a.sin_port);
    return fd;
}

static inline int connect_local(uint16_t port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    return em_connect((struct sockaddr *)&a, sizeof a);
}
#endif
