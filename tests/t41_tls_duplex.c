/* Full duplex on one TLS connection: each side has a fiber writing 16 MiB and a fiber reading 16 MiB
 * at the same time. Mid-stream the client asks for a KeyUpdate (the server's answer is produced
 * with its next write, which may be flushing already) and the server updates its
 * own keys. A second reader or writer on the same em_tls gets -EBUSY.
 * Third pass: 32 KiB socket buffers and a KeyUpdate request every 256 KiB, so em_send blocks all the
 * time. Each side answers a KeyUpdate from its writer (SSL_write), never from the reader. */
#include <stdatomic.h>
#include <stdio.h>
#include "tls_util.h"

#define BYTES (16u << 20)
#define STRESS_BYTES (4u << 20)

static SSL_CTX *sctx, *cctx;
static _Atomic int port;
static int nw;
static unsigned probes;
static int stress;
static uint64_t total; /* bytes each way per side */

static uint8_t pat(uint64_t seed, uint64_t i) { return (uint8_t)(((i + seed) * 0x9E3779B97F4A7C15ull) >> 40); }

struct side { em_tls *t; uint64_t tx_seed, rx_seed; int client; };

static void small_buffers(int fd)
{
    int v = 32768;
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0);
}

static int writer(void *arg)
{
    struct side *sd = arg;
    char *buf = malloc(70000); /* not static: both sides run on one worker, the buffer must outlive each park */
    assert(buf);
    uint64_t s = sd->tx_seed * 7 + 1, sent = 0;
    int updated = 0, r = 0;
    uint64_t next_update = stress ? 262144 : total / 4;
    while (sent < total && !r) {
        size_t n = 1 + xs_next(&s) % 70000;
        if (n > total - sent) n = total - sent;
        for (size_t j = 0; j < n; j++) buf[j] = (char)pat(sd->tx_seed, sent + j);
        r = em_tls_send_all(sd->t, buf, n);
        sent += n;
        if (sent > next_update && !r && (!updated || stress)) {
            updated = 1;
            next_update += 262144;
            assert(SSL_key_update(em_tls_ssl(sd->t), sd->client ? SSL_KEY_UPDATE_REQUESTED : SSL_KEY_UPDATE_NOT_REQUESTED) == 1);
        }
    }
    free(buf);
    return r ? r : em_tls_shutdown(sd->t);
}

static int reader(void *arg)
{
    struct side *sd = arg;
    char buf[16384];
    uint64_t got = 0;
    while (got < total) {
        ssize_t n = em_tls_recv(sd->t, buf, sizeof buf);
        if (n <= 0) return n < 0 ? (int)n : -EIO;
        for (ssize_t j = 0; j < n; j++)
            if ((uint8_t)buf[j] != pat(sd->rx_seed, got + (uint64_t)j)) return -EBADMSG;
        got += (uint64_t)n;
    }
    ssize_t n = em_tls_recv(sd->t, buf, 1);
    return n == 0 ? 0 : n < 0 ? (int)n : -EIO; /* peer's close_notify */
}

static int probe(void *arg) /* both directions are mid-transfer: a second fiber must be refused */
{
    struct side *sd = arg;
    char b;
    em_sleep_until(em_now() + 2000000);
    assert(em_tls_recv(sd->t, &b, 1) == -EBUSY);
    assert(em_tls_send_all(sd->t, &b, 1) == -EBUSY);
    assert(em_tls_shutdown(sd->t) == -EBUSY);
    probes++;
    return 0;
}

static int duplex(em_tls *t, int client)
{
    struct side sd = { t, client ? 1 : 2, client ? 2 : 1, client };
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, writer, &sd) == 0);
    assert(em_spawn(&s, reader, &sd) == 0);
    if (client) assert(em_spawn(&s, probe, &sd) == 0);
    return em_scope_close(&s);
}

static int server(void *arg)
{
    (void)arg;
    uint16_t p;
    int lfd = listen_local(&p);
    atomic_store(&port, p);
    int fd = em_accept(lfd);
    if (fd < 0) return fd;
    if (stress) small_buffers(fd);
    em_tls *t;
    int r = em_tls_accept(&t, sctx, fd);
    if (r < 0) return r;
    assert(SSL_version(em_tls_ssl(t)) == TLS1_3_VERSION);
    r = duplex(t, 0);
    em_tls_free(t);
    em_close(fd);
    em_close(lfd);
    return r;
}

static int client(void *arg)
{
    (void)arg;
    while (!atomic_load(&port)) em_sleep_until(em_now() + 1000000);
    int fd = connect_local((uint16_t)atomic_load(&port));
    if (fd < 0) return fd;
    if (stress) small_buffers(fd);
    em_tls *t;
    int r = em_tls_connect(&t, cctx, fd, "localhost");
    if (r < 0) return r;
    r = duplex(t, 1);
    em_tls_free(t);
    em_close(fd);
    return r;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    em_scope s;
    em_scope_open(&s, 0);
    if (nw == 1 || id == 0) assert(em_spawn(&s, server, NULL) == 0);
    if (nw == 1 || id == 1) assert(em_spawn(&s, client, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    tls_test_ctx(&sctx, &cctx);
    for (int pass = 0; pass < 3; pass++) {
        nw = pass == 1 ? 2 : 1;
        stress = pass == 2;
        total = stress ? STRESS_BYTES : BYTES;
        atomic_store(&port, 0);
        int r = em_run(nw, main_fn, NULL);
        assert(r == 0);
        printf("tls duplex: 2 x %u MiB at once over %d worker(s)%s verified, KeyUpdate mid-stream\n", (unsigned)(total >> 20), nw,
               stress ? ", 32 KiB socket buffers," : "");
    }
    assert(probes == 3);
    return 0;
}
