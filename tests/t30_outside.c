/* em calls made outside a fiber fail with -EPERM (em_close just close()s); the same
 * calls keep working inside em_run, and a socket that outlives em_run is harmless. */
#include <fcntl.h>
#include <unistd.h>
#include "util.h"

static int kept = -1; /* a socket smuggled out of the fiber, like a thread_local would */

static int noop(void *arg) { (void)arg; return 0; }

static void outside(int fd)
{
    char b[8];
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof a;
    em_scope s;
    assert(em_sleep_until(em_now() + 1000000) == -EPERM);
    assert(em_tcp_listen((struct sockaddr *)&a, sizeof a, 8) == -EPERM);
    assert(em_udp_bind((struct sockaddr *)&a, sizeof a) == -EPERM);
    assert(em_connect((struct sockaddr *)&a, sizeof a) == -EPERM);
    assert(em_accept(fd) == -EPERM);
    assert(em_recv(fd, b, sizeof b) == -EPERM);
    assert(em_read(fd, b, sizeof b) == -EPERM);
    assert(em_send(fd, b, sizeof b) == -EPERM);
    assert(em_send_all(fd, b, 0) == -EPERM);
    assert(em_shutdown(fd, SHUT_RDWR) == -EPERM);
    assert(em_recvfrom(fd, b, sizeof b, (struct sockaddr *)&a, &sl) == -EPERM);
    assert(em_sendto(fd, b, sizeof b, (struct sockaddr *)&a, sl) == -EPERM);
    assert(em_park() == -EPERM);
    em_yield();
    em_scope_open(&s, 0);
    assert(em_spawn(&s, noop, NULL) == -EPERM);
    assert(em_scope_close(&s) == -EPERM);
    em_cancel_all();
}

static int inside(void *arg)
{
    (void)arg;
    uint16_t port;
    int l = listen_local(&port);
    int c = connect_local(port);
    assert(c >= 0);
    int a = em_accept(l);
    assert(a >= 0);
    assert(em_send_all(c, "x", 1) == 0);
    char b;
    assert(em_recv(a, &b, 1) == 1 && b == 'x');
    em_close(a);
    em_close(c);
    kept = l;
    return 0;
}

int main(void)
{
    /* before any em_run: also the very first call on this thread */
    outside(0);
    assert(em_close(open("/dev/null", O_RDONLY)) == 0);
    assert(em_close(-1) == -EBADF);

    assert(em_run(1, inside, NULL) == 0);
    assert(kept >= 0);
    /* after em_run: the fd number is still open (em_close in the fiber did not touch it) */
    outside(kept);
    assert(em_close(kept) == 0);
    assert(em_close(kept) == -EBADF); /* really closed */
    return 0;
}
