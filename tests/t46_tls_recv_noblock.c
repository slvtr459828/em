/* em_tls_recv never waits for the peer to read: it does not park in em_send.
 * Setup of every case: SO_SNDBUF/SO_RCVBUF 16 KiB before the handshake; after it the server's send
 * path to the client is filled with raw bytes until nothing more is accepted, and the client does
 * not read.
 *  1. alert: the client sends a garbage record; the server's em_tls_recv (deadline 2 s) has an alert
 *     to send but the path is full: it must return -EPROTO at once, not after the deadline.
 *  2. KeyUpdate (premise probe: SSL_read itself never needs to send a KeyUpdate answer): the client
 *     requests a key update and sends 64 KiB without reading; the server receives all of it inside
 *     the deadline. Then the client reads off the raw bytes, the server writes, the client decrypts
 *     it right.
 *  3. reset: the server's writer is parked in em_send; the garbage record fails the reader at once
 *     (the writer is the flusher, the reader sends nothing); the client resets and the writer
 *     returns with an error instead of hanging. */
#include <stdio.h>
#include <sys/socket.h>
#include "tls_util.h"

#define KU_BYTES 65536
#define FAST_MS  1000 /* the deadline is 2000 ms; a stuck reader would come back after it */

static SSL_CTX *sctx, *cctx;
static uint16_t port;
static size_t junk[3];       /* raw bytes the server put on the wire after the handshake */
static int filled[3], srv_done[3];
static int rres[3], wres[3];
static int64_t rms[3];
static int ku_got, ku_send;

static void small_buffers(int fd)
{
    int v = 16384;
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0);
}

static uint8_t pat(size_t i) { return (uint8_t)(i * 131 + 7); }

static void timed_recv(em_tls *t, int i)
{
    char b[16];
    em_scope s;
    int64_t t0 = em_now();
    em_scope_open(&s, t0 + 2000000000);
    rres[i] = (int)em_tls_recv(t, b, sizeof b);
    rms[i] = (em_now() - t0) / 1000000;
    em_scope_close(&s);
}

static int writer(void *arg)
{
    wres[2] = em_tls_send_all(arg, "y", 1);
    return 0;
}

static int server(void *arg)
{
    (void)arg;
    int lfd = listen_local(&port);
    for (int i = 0; i < 3; i++) {
        int fd = em_accept(lfd);
        if (fd < 0) return fd;
        small_buffers(fd);
        em_tls *t;
        int r = em_tls_accept(&t, sctx, fd);
        if (r < 0) return r;
        char junk_buf[4096] = {0};
        /* fill until nothing more is accepted for 4 rounds of 30 ms (the kernel keeps taking a little more for ~100 ms: window updates) */
        for (int quiet = 0; quiet < 4; ) {
            int sent = 0;
            ssize_t k;
            while ((k = send(fd, junk_buf, sizeof junk_buf, MSG_DONTWAIT | MSG_NOSIGNAL)) > 0) { sent = 1; junk[i] += (size_t)k; }
            while ((k = send(fd, junk_buf, 1, MSG_DONTWAIT | MSG_NOSIGNAL)) > 0) { sent = 1; junk[i] += (size_t)k; }
            quiet = sent ? 0 : quiet + 1;
            em_sleep_until(em_now() + 30000000);
        }
        filled[i] = 1;
        if (i == 0) {
            timed_recv(t, 0);
        } else if (i == 1) {
            char b[4096];
            while (ku_got < KU_BYTES) {
                em_scope s;
                em_scope_open(&s, em_now() + 2000000000);
                ssize_t n = em_tls_recv(t, b, sizeof b);
                em_scope_close(&s);
                if (n <= 0) break; /* deadline: the premise is wrong, reported below */
                ku_got += (int)n;
            }
            srv_done[1] = 1; /* the client may start reading now */
            if (ku_got == KU_BYTES) {
                unsigned char o[4096];
                for (size_t k = 0; k < sizeof o; k++) o[k] = pat(k);
                ku_send = em_tls_send_all(t, o, sizeof o);
            }
        } else {
            em_scope s;
            em_scope_open(&s, 0);
            assert(em_spawn(&s, writer, t) == 0);
            em_sleep_until(em_now() + 100000000); /* the writer is parked in em_send by now */
            timed_recv(t, 2);
            em_scope_close(&s);
        }
        srv_done[i] = 1;
        em_tls_free(t);
        em_close(fd);
    }
    em_close(lfd);
    return 0;
}

static int client(void *arg)
{
    (void)arg;
    while (!port) em_yield();
    for (int i = 0; i < 3; i++) {
        int fd = connect_local(port);
        assert(fd >= 0);
        small_buffers(fd);
        em_tls *t;
        assert(em_tls_connect(&t, cctx, fd, "localhost") == 0);
        while (!filled[i]) em_sleep_until(em_now() + 1000000);
        if (i == 1) {
            assert(SSL_key_update(em_tls_ssl(t), SSL_KEY_UPDATE_REQUESTED) == 1);
            static unsigned char big[KU_BYTES];
            assert(em_tls_send_all(t, big, sizeof big) == 0);
            while (!srv_done[1]) em_sleep_until(em_now() + 1000000);
            /* off the raw bytes, then the TLS records the server wrote */
            char b[4096];
            for (size_t left = junk[1]; left;) {
                ssize_t n = em_recv(fd, b, left < sizeof b ? left : sizeof b);
                assert(n > 0);
                left -= (size_t)n;
            }
            unsigned char in[4096];
            for (size_t got = 0; got < sizeof in;) {
                ssize_t n = em_tls_recv(t, in + got, sizeof in - got);
                assert(n > 0);
                got += (size_t)n;
            }
            for (size_t k = 0; k < sizeof in; k++) assert(in[k] == pat(k));
        } else {
            em_sleep_until(em_now() + 400000000);
            assert(em_send_all(fd, "\x99\x03\x03\x00\x05hello", 10) == 0); /* not a TLS record type */
            if (i == 2) {
                em_sleep_until(em_now() + 400000000);
                struct linger l = { 1, 0 };
                setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
                em_close(fd); /* RST: the writer's em_send fails */
                fd = -1;
            }
        }
        while (!srv_done[i]) em_sleep_until(em_now() + 1000000);
        em_tls_free(t);
        if (fd >= 0) em_close(fd);
    }
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, server, NULL) == 0);
    assert(em_spawn(&s, client, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    tls_test_ctx(&sctx, &cctx);
#ifndef OPENSSL_IS_AWSLC
    SSL_CTX_set_num_tickets(sctx, 0); /* the raw bytes must be the first thing the client reads (AWS-LC sends tickets with the next write) */
#endif
    assert(em_run(1, main_fn, NULL) == 0);
    printf("alert:     reader %d after %lld ms\n", rres[0], (long long)rms[0]);
    printf("keyupdate: server got %d of %d bytes, then wrote: %d\n", ku_got, KU_BYTES, ku_send);
    printf("reset:     reader %d after %lld ms, writer %d\n", rres[2], (long long)rms[2], wres[2]);
    int ok = 1;
#define CHECK(c) do { if (!(c)) { printf("FAILED: %s\n", #c); ok = 0; } } while (0)
    CHECK(rres[0] == -EPROTO && rms[0] < FAST_MS);
    CHECK(ku_got == KU_BYTES && ku_send == 0);
    CHECK(rres[2] == -EPROTO && rms[2] < FAST_MS && wres[2] < 0);
    fflush(stdout);
    assert(ok);
    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    return 0;
}
