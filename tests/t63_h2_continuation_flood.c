/* CVE-2024-27983 / CVE-2024-28182 (CONTINUATION flood). The client is raw bytes, because a conforming
 * client cannot produce this: preface, SETTINGS, a HEADERS frame without END_HEADERS, then 10000
 * CONTINUATION frames, none of which ever ends the header block. Passes when the server sends GOAWAY
 * or closes before the client has sent them all, never runs the handler, and its memory stays under
 * a fixed cap. Over h2c, and over TLS with -DHTTP_TLS. No version check: the answer must be the
 * same whichever nghttp2 build, with or without a distro backport, is linked. */
#include <unistd.h>
#include "h2_util.h"

#define FRAMES   10000
#define BATCH    100
#define RSS_CAP  (32u << 20) /* growth of the process (server and client are in it) */

static size_t rss(void)
{
    long pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    assert(f && fscanf(f, "%*s %ld", &pages) == 1);
    fclose(f);
    return (size_t)pages * (size_t)sysconf(_SC_PAGESIZE);
}

static size_t frame(uint8_t *o, uint8_t type, uint8_t flags, uint32_t sid, const void *pl, size_t n)
{
    o[0] = (uint8_t)(n >> 16); o[1] = (uint8_t)(n >> 8); o[2] = (uint8_t)n; o[3] = type; o[4] = flags;
    o[5] = (uint8_t)(sid >> 24); o[6] = (uint8_t)(sid >> 16); o[7] = (uint8_t)(sid >> 8); o[8] = (uint8_t)sid;
    if (n) memcpy(o + 9, pl, n);
    return 9 + n;
}

/* Looks for the server's reaction without waiting for it. 1 = GOAWAY seen or connection ended. */
static int reacted(rd_t *r, uint8_t *acc, size_t *alen)
{
    em_scope s;
    em_scope_open(&s, em_now() + 1000000); /* 1 ms: the server shares this worker and runs while we wait */
    ssize_t n = rd_recv(r, acc + *alen, 4096 - *alen);
    em_scope_close(&s);
    if (n == -ETIMEDOUT) return 0;
    if (n <= 0) return 1;              /* closed or reset */
    *alen += (size_t)n;
    for (size_t p = 0; *alen - p >= 9; ) {   /* whole frames in acc: GOAWAY is type 7 */
        size_t len = (size_t)acc[p] << 16 | (size_t)acc[p + 1] << 8 | acc[p + 2];
        if (acc[p + 3] == 7) return 1;
        if (*alen - p < 9 + len) break;
        p += 9 + len;
        memmove(acc, acc + p, *alen - p); *alen -= p; p = 0;
    }
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll);
    http_start(&s, &sc, &port);

    rd_t *r = h2_connect(port);
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const uint8_t hpack[] = { 0x82, 0x86, 0x84, 0x41, 0x01, 'x' }; /* GET http / authority x */
    uint8_t out[sizeof preface + 9 + 9 + sizeof hpack], acc[4096];
    size_t n = sizeof preface - 1, alen = 0;
    memcpy(out, preface, n);
    n += frame(out + n, 4, 0, 0, NULL, 0);                    /* SETTINGS */
    n += frame(out + n, 1, 0, 1, hpack, sizeof hpack);        /* HEADERS, stream 1, no END_HEADERS */
    assert(rd_send(r, out, n) == 0);

    size_t before = rss();
    static uint8_t cont[BATCH * 9];
    for (int i = 0; i < BATCH; i++) frame(cont + i * 9, 9, 0, 1, NULL, 0); /* CONTINUATION, empty, no END_HEADERS */
    int cut = 0, sent = 0;
    while (sent < FRAMES && !cut) {
        if (rd_send(r, cont, sizeof cont) < 0) cut = 1;       /* the server is gone */
        else { sent += BATCH; cut = reacted(r, acc, &alen); }
    }
    if (!cut) { fprintf(stderr, "server took all %d CONTINUATION frames\n", sent); abort(); }
    assert(sent < FRAMES);
    assert(atomic_load(&handler_calls) == 0);
    size_t after = rss();
    fprintf(stderr, "flood: server reacted after %d frames, RSS %zu -> %zu\n", sent, before, after);
    assert(after < before + RSS_CAP);
    rd_close(r);

    int r1 = http_stop(&s);
    assert(r1 == 0);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http2 continuation flood: cut off, handler never called");
    return 0;
}
