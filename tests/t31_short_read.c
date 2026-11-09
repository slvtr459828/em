/* epoll skips the recv after a short stream read (rd_idle) until a new edge.
 * Must not hang where a short read says nothing: datagram sockets read with
 * em_recv (several datagrams queued), data that arrives later on a stream, and
 * EOF after a short read, and data plus FIN in one edge (t7 hung on that). Each case runs under a 2 s scope deadline. */
#include <stdio.h>
#include "util.h"

static int sv[2];

/* The reader parks first, so the fd is registered and the skip path is live. */
static int stream_reader(void *arg)
{
    (void)arg;
    char b[64];
    assert(em_recv(sv[0], b, sizeof b) == 2);        /* short: rd_idle set */
    assert(em_recv(sv[0], b, sizeof b) == 3);        /* waits; the later send is a new edge */
    assert(em_recv(sv[0], b, sizeof b) == 0);        /* EOF after a short read */
    assert(em_recv(sv[0], b, sizeof b) == 0);        /* EOF stays readable */
    return 0;
}

static int stream_writer(void *arg)
{
    (void)arg;
    em_yield();                                      /* reader is parked */
    assert(em_send(sv[1], "ab", 2) == 2);
    assert(em_sleep_until(em_now() + 5000000) == 0); /* reader drains "ab" meanwhile */
    assert(em_send(sv[1], "cde", 3) == 3);
    assert(em_sleep_until(em_now() + 5000000) == 0);
    assert(em_shutdown(sv[1], SHUT_WR) == 0);
    return 0;
}

/* FIN arrives in the same edge as the bytes: no edge follows the short read. */
static int fin_reader(void *arg)
{
    (void)arg;
    char b[64];
    assert(em_recv(sv[0], b, sizeof b) == 2);
    assert(em_recv(sv[0], b, sizeof b) == 0);
    return 0;
}

static int fin_writer(void *arg)
{
    (void)arg;
    em_yield();                                      /* reader is parked */
    assert(em_send(sv[1], "ab", 2) == 2);
    assert(em_shutdown(sv[1], SHUT_WR) == 0);        /* no yield in between */
    return 0;
}

static int dgram_reader(void *arg)
{
    (void)arg;
    char b[64];
    for (int i = 0; i < 3; i++) assert(em_recv(sv[0], b, sizeof b) == 1); /* each short, rest still queued */
    return 0;
}

static int dgram_writer(void *arg)
{
    (void)arg;
    em_yield();                                      /* reader is parked */
    for (int i = 0; i < 3; i++) assert(em_send(sv[1], "x", 1) == 1); /* one edge for all three */
    return 0;
}

static int run_case(int type, int (*rd)(void *), int (*wr)(void *))
{
    assert(socketpair(AF_UNIX, type, 0, sv) == 0);
    em_scope s;
    em_scope_open(&s, em_now() + 2000000000LL);
    assert(em_spawn(&s, rd, NULL) == 0);
    assert(em_spawn(&s, wr, NULL) == 0);
    int r = em_scope_close(&s);
    em_close(sv[0]);
    em_close(sv[1]);
    return r;
}

static int main_fn(void *arg)
{
    (void)arg;
    assert(run_case(SOCK_STREAM, stream_reader, stream_writer) == 0);
    assert(run_case(SOCK_STREAM, fin_reader, fin_writer) == 0);
    assert(run_case(SOCK_DGRAM, dgram_reader, dgram_writer) == 0);
    assert(run_case(SOCK_SEQPACKET, dgram_reader, dgram_writer) == 0);
    printf("short read (%s): stream late data + EOF, data+FIN in one edge, dgram and seqpacket queues\n", em_backend());
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
