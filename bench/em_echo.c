/* Echo server for bench/run.sh: argv[1] = port. 2 workers, each listens on the
 * port with SO_REUSEPORT and runs one fiber per connection. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/em.h"

#define WORKERS 2

static struct sockaddr_in addr;

static int echo(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[16384];
    for (;;) {
        ssize_t n = em_recv(fd, buf, sizeof buf);
        if (n <= 0 || em_send_all(fd, buf, (size_t)n) < 0) break;
    }
    em_close(fd);
    return 0; /* a dropped connection must not cancel the others */
}

static int worker(void *arg)
{
    (void)arg;
    int lfd = em_tcp_listen((struct sockaddr *)&addr, sizeof addr, 4096);
    if (lfd < 0) return lfd;
    em_scope sc;
    em_scope_open(&sc, 0);
    int r = 0;
    while (!r) {
        int fd = em_accept(lfd);
        if (fd < 0) { r = fd; break; }
        if (em_spawn(&sc, echo, (void *)(intptr_t)fd) != 0) { em_close(fd); r = -ENOMEM; }
    }
    em_scope_cancel(&sc);
    em_close(lfd);
    em_scope_close(&sc);
    return r;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s port\n", argv[0]); return 2; }
    addr = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int r = em_run(WORKERS, worker, NULL);
    if (r) fprintf(stderr, "em_run: %d\n", r);
    return r != 0;
}
