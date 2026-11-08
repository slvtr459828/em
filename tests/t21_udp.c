/* 10k UDP datagrams ping-pong between fibers on two different workers; source
 * addresses checked; recvfrom under a scope deadline times out. */
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"

#define NMSG 10000

static int N, srv_id, cli_id;
static _Atomic int srv_port;
static int srv_ok;

static struct sockaddr_in local(uint16_t port)
{
    return (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
}

static int server(void)
{
    struct sockaddr_in a = local(0);
    int fd = em_udp_bind((struct sockaddr *)&a, sizeof a);
    assert(fd >= 0);
    socklen_t n = sizeof a;
    assert(getsockname(fd, (struct sockaddr *)&a, &n) == 0);
    atomic_store(&srv_port, ntohs(a.sin_port));
    struct sockaddr_in cli = {0};
    int r = 0;
    for (uint32_t i = 0; i < NMSG && !r; i++) {
        uint32_t v;
        struct sockaddr_in src;
        socklen_t sl = sizeof src;
        ssize_t got = em_recvfrom(fd, &v, sizeof v, (struct sockaddr *)&src, &sl);
        if (got != (ssize_t)sizeof v || v != i || sl != sizeof src) { r = -EIO; break; }
        if (i == 0) cli = src;
        else if (src.sin_port != cli.sin_port || src.sin_addr.s_addr != cli.sin_addr.s_addr) { r = -EIO; break; }
        v = ~v;
        if (em_sendto(fd, &v, sizeof v, (struct sockaddr *)&src, sl) != (ssize_t)sizeof v) r = -EIO;
    }
    srv_ok = !r;
    em_close(fd);
    return r;
}

static int client(void)
{
    while (!atomic_load(&srv_port)) usleep(100); /* the other worker is binding */
    struct sockaddr_in me = local(0), srv = local((uint16_t)atomic_load(&srv_port));
    int fd = em_udp_bind((struct sockaddr *)&me, sizeof me);
    assert(fd >= 0);
    int r = 0;
    for (uint32_t i = 0; i < NMSG && !r; i++) {
        if (em_sendto(fd, &i, sizeof i, (struct sockaddr *)&srv, sizeof srv) != (ssize_t)sizeof i) { r = -EIO; break; }
        uint32_t v;
        struct sockaddr_in src;
        socklen_t sl = sizeof src;
        ssize_t got = em_recvfrom(fd, &v, sizeof v, (struct sockaddr *)&src, &sl);
        if (got != (ssize_t)sizeof v || v != ~i || sl != sizeof src ||
            src.sin_port != srv.sin_port || src.sin_addr.s_addr != srv.sin_addr.s_addr) r = -EIO;
    }
    if (!r) { /* nothing more is coming */
        em_scope s;
        em_scope_open(&s, em_now() + 30000000);
        uint32_t v;
        if (em_recvfrom(fd, &v, sizeof v, NULL, NULL) != -ETIMEDOUT) r = -EIO;
        em_scope_close(&s);
    }
    em_close(fd);
    return r;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    if (id == srv_id) return server();
    if (id == cli_id) return client();
    return 0;
}

int main(void)
{
    N = test_workers();
    assert(N >= 2);
    srv_id = 1;
    cli_id = 0;
    assert(em_run(N, main_fn, NULL) == 0);
    assert(srv_ok);
    printf("udp: %d datagrams ping-pong between worker %d and %d\n", NMSG, cli_id, srv_id);
    return 0;
}
