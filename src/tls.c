/* em_tls: OpenSSL / AWS-LC over a custom BIO. See include/em_tls.h for the contract.
 *
 * em owns the byte buffers the SSL talks to; the BIO callbacks never block: read hands out bytes
 * from rx (or says retry), write appends to tx (or says retry when full). em_recv writes straight
 * into rx and em_send sends straight from tx, so on both libraries the only copies are the ones
 * the SSL itself makes. One I/O path, no per-library split.
 *
 * Memory: an idle connection holds no big buffer. tx is taken by the first write and released
 * once everything in it is on the wire. rx (a whole TLS record) is taken only when a record does
 * not fit the small inline buffer `in`, and released when SSL has consumed it. A parked idle
 * em_recv reads into `in`: io_uring writes into the buffer while the fiber is parked, so the
 * buffer is owned by this connection, never shared per worker.
 *
 * Fibers only park in em_recv / em_send / em_park, never inside an SSL_* call, and a worker runs
 * one fiber at a time, so SSL_* calls of the reader and the writer never overlap. */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "em.h"
#include "internal.h"
#include "em_tls.h"

/* One SSL_write of REC_PLAIN bytes must always fit the out buffer whole (see run()). It is split
 * into records of max_send_fragment bytes, which the user (SSL_CTX_set_max_send_fragment) or the
 * peer (max_fragment_length extension) may lower to 512: up to 32 records. REC_OVH bounds one
 * record's expansion over every cipher OpenSSL offers: header 5 + IV 16 + MAC 48 (SHA384) +
 * CBC padding 16 = 85 (TLS 1.3: 22; padding stays within the fragment). CTL_ROOM is for the
 * small records the reader may add meanwhile (alerts, KeyUpdate). */
#define REC_PLAIN  16384
#define REC_OVH    96
#define REC_WORST  (REC_PLAIN + REC_PLAIN / 512 * REC_OVH)
#define CTL_ROOM   512
#define OUT_BYTES  (REC_WORST + CTL_ROOM)    /* 19.5 KiB: size of tx */
#define IN_BYTES   (16384 + 2048 + 5)       /* size of rx: the largest record a TLS 1.2 peer may send */
#define INL_BYTES  512                      /* idle recv buffer: a 128 B echo record is 150 B; anything bigger costs RSS per connection */

struct em_tls {
    SSL      *ssl;
    int       fd;
    unsigned char reading, writing, flushing, broken;
    /* Bytes SSL has not consumed yet: [rp, rp+rn), in `in` or in rx. rn > 0 only between a recv
     * and the SSL consuming it, so a recv parked in rx/in never races with a release. */
    unsigned char *rp, *rx;      /* rx: heap, NULL when not taken */
    size_t    rn;
    /* Bytes SSL produced: tx[to, tn). Fixed size OUT_BYTES, never moved while em_send reads it. */
    unsigned char *tx;
    size_t    to, tn;
    unsigned char in[INL_BYTES];
};

enum op { ACCEPT, CONNECT, READ, WRITE, SHUTDOWN };

unsigned long em_tls_dbg_partial; /* test hook: SSL_write left a record half-written (must stay 0) */

/* Errors queued by a failed SSL_* call, kept across the flush (which parks: another fiber's
 * ERR_clear_error would erase them) and put back before the caller sees -EPROTO. */
#define NSAVED 4
struct saved {
    unsigned long code[NSAVED];
    char          data[NSAVED][128];
    int           n;
#ifdef OPENSSL_IS_AWSLC
    const char   *file[NSAVED];
    int           line[NSAVED];
#endif
};

static void save_errors(struct saved *s)
{
    s->n = 0;
    unsigned long c;
    const char *data;
    int flags;
#ifdef OPENSSL_IS_AWSLC
    const char *file;
    int line;
    while (s->n < NSAVED && (c = ERR_get_error_line_data(&file, &line, &data, &flags)) != 0) {
        s->code[s->n] = c;
        s->file[s->n] = file; /* points to a string literal inside the library */
        s->line[s->n] = line;
#else
    while (s->n < NSAVED && (c = ERR_get_error_all(NULL, NULL, NULL, &data, &flags)) != 0) {
        s->code[s->n] = c;
#endif
        s->data[s->n][0] = 0;
        if (data && (flags & ERR_TXT_STRING)) snprintf(s->data[s->n], sizeof s->data[0], "%s", data);
        s->n++;
    }
}

static void restore_errors(const struct saved *s)
{
    ERR_clear_error();
    for (int i = 0; i < s->n; i++) {
#ifdef OPENSSL_IS_AWSLC
        ERR_put_error(ERR_GET_LIB(s->code[i]), 0, ERR_GET_REASON(s->code[i]), s->file[i], (unsigned)s->line[i]);
        if (s->data[i][0]) ERR_add_error_data(1, s->data[i]);
#else
        ERR_new();
        ERR_set_error(ERR_GET_LIB(s->code[i]), ERR_GET_REASON(s->code[i]), "%s", s->data[i]);
#endif
    }
}

/* ---- the BIO: callbacks never block, never park ---- */

/* Buffers of released tx / rx wait here instead of going back to malloc: a 19.5 KiB malloc+free
 * per echo costs ~2-6% of a 4 KiB round-trip (plans/tls-win.md step 2a). One size class for both
 * (OUT_BYTES >= IN_BYTES). Per worker thread, so no lock; at most POOL_N x 19.5 KiB per worker.
 * A worker thread that exits gives them back (pthread key destructor). */
#define POOL_N 8
static __thread void *pool[POOL_N];
static __thread int npool;
static pthread_key_t pool_key;

static void pool_drain(void *unused)
{
    (void)unused;
    while (npool) free(pool[--npool]);
}

static void *buf_get(void)
{
    return npool ? pool[--npool] : malloc(OUT_BYTES);
}

static void buf_put(void *p)
{
    if (!p) return;
    if (npool < POOL_N) {
        if (!npool) pthread_setspecific(pool_key, pool); /* any non-NULL value: arms the destructor for this thread */
        pool[npool++] = p;
    } else {
        free(p);
    }
}

static void rx_release(em_tls *t)
{
    buf_put(t->rx);
    t->rx = NULL;
}

static int bio_read(BIO *b, char *out, int len)
{
    em_tls *t = BIO_get_data(b);
    BIO_clear_retry_flags(b);
    if (len <= 0) return 0;
    if (t->rn == 0) { BIO_set_retry_read(b); return -1; }
    size_t n = (size_t)len < t->rn ? (size_t)len : t->rn;
    memcpy(out, t->rp, n);
    t->rp += n;
    /* (a recv into rx can only be parked while rn == 0, and then SSL gets the retry above) */
    if ((t->rn -= n) == 0 && t->rx) rx_release(t);
    return (int)n;
}

/* All or nothing: a half-taken write would leave SSL a partial record to retry. */
static int bio_write(BIO *b, const char *in, int len)
{
    em_tls *t = BIO_get_data(b);
    BIO_clear_retry_flags(b);
    if (len <= 0) return 0;
    if (!t->tx && !(t->tx = buf_get())) return -1;
    if ((size_t)len > OUT_BYTES - t->tn) { BIO_set_retry_write(b); return -1; }
    memcpy(t->tx + t->tn, in, (size_t)len);
    t->tn += (size_t)len;
    return len;
}

static long bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    (void)b; (void)num; (void)ptr;
    return cmd == BIO_CTRL_FLUSH ? 1 : 0; /* the bytes are sent by flush() / reader_send(), not by BIO_flush */
}

static int bio_create(BIO *b) { BIO_set_init(b, 1); return 1; }

static BIO_METHOD *bio_meth;
static pthread_once_t bio_once = PTHREAD_ONCE_INIT;

static void bio_meth_init(void)
{
    pthread_key_create(&pool_key, pool_drain); /* failure: buffers of an exiting worker leak; not reachable in practice */
    BIO_METHOD *m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "em_tls");
    if (m && BIO_meth_set_read(m, bio_read) && BIO_meth_set_write(m, bio_write) &&
        BIO_meth_set_ctrl(m, bio_ctrl) && BIO_meth_set_create(m, bio_create))
        bio_meth = m;
}

static void tx_release(em_tls *t)
{
    buf_put(t->tx);
    t->tx = NULL;
    t->to = t->tn = 0;
}

/* Sends all the SSL has queued; parks in em_send. Writer side only; `flushing` keeps the reader off
 * tx meanwhile (the reader may still append an alert behind the bytes em_send is reading). Failure
 * breaks the stream. */
static int flush(em_tls *t)
{
    int r = 0;
    t->flushing = 1;
    while (t->to < t->tn) {
        ssize_t s = em_send(t->fd, t->tx + t->to, t->tn - t->to);
        if (s < 0) { r = (int)s; t->broken = 1; break; }
        t->to += (size_t)s;
    }
    if (!r) tx_release(t);
    t->flushing = 0;
    return r;
}

/* What SSL_read left in tx (an alert) goes out in one send that never parks: the peer may not be
 * reading. Skipped while the writer flushes (it sends them) or the stream is broken (tx may hold a
 * cancelled record). What the kernel takes is consumed, the rest waits for the writer; a send error
 * is not recorded (the writer meets it again). */
static void reader_send(em_tls *t)
{
    if (t->to == t->tn || t->flushing || t->broken) return;
    ssize_t s = send(t->fd, t->tx + t->to, t->tn - t->to, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (s <= 0) return;
    if ((t->to += (size_t)s) == t->tn) tx_release(t);
}

/* Socket to SSL after WANT_READ (rx/in are empty). The first recv of a call reads into `in`: an
 * idle connection parks on a small buffer and a small record never needs rx. Later recvs of the
 * same call (the rest of a record) go to rx; the data is usually there already, so a sync recv
 * tries first and em_recv (which parks) only when it is not. */
static int fill(em_tls *t, int first)
{
    ssize_t k;
    if (t->rn) return -EIO; /* SSL wants input while bytes are waiting: not reachable */
    if (first) {
        rx_release(t); /* kept from a call that left it empty */
        k = em_recv(t->fd, t->in, sizeof t->in);
        t->rp = t->in;
    } else {
        if (!t->rx && !(t->rx = buf_get())) return -ENOMEM;
        do k = recv(t->fd, t->rx, IN_BYTES, MSG_DONTWAIT); while (k < 0 && errno == EINTR);
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) k = em_recv(t->fd, t->rx, IN_BYTES);
        else if (k < 0) k = -errno;
        t->rp = t->rx;
    }
    if (k <= 0) {
        rx_release(t);
        /* <0: cancelled / deadline: nothing consumed, retryable; 0: TCP EOF without close_notify */
        return k < 0 ? (int)k : -ECONNRESET;
    }
    t->rn = (size_t)k;
    return 0;
}

/* One loop for every operation. Returns bytes for READ/WRITE (> 0), 0 for the rest and for
 * close_notify on READ, or -errno. */
static int run(em_tls *t, enum op op, void *buf, int len)
{
    int first = 1; /* no recv yet in this call */
    for (;;) {
        /* A data record must never be left half-written inside SSL: an alert the reader then adds
         * would fail ("bad write retry") or interleave. So SSL_write only starts when tx has
         * room for its whole output (REC_WORST), and never gets WANT_WRITE (em_tls_dbg_partial, t40). */
        if (op == WRITE && t->tn > OUT_BYTES - REC_WORST) {
            int r = flush(t);
            if (r < 0) return r;
            continue;
        }
        if (ERR_peek_error()) ERR_clear_error(); /* the clear alone costs ~13% of a small record's instructions */
        int n;
        switch (op) {
        case ACCEPT:  n = SSL_accept(t->ssl); break;
        case CONNECT: n = SSL_connect(t->ssl); break;
        case READ:    n = SSL_read(t->ssl, buf, len); break;
        case WRITE:   n = SSL_write(t->ssl, buf, len); break;
        default:      n = SSL_shutdown(t->ssl); break; /* 0: close_notify sent, peer's not yet seen */
        }
        int e = (n > 0 || (op == SHUTDOWN && n == 0)) ? SSL_ERROR_NONE : SSL_get_error(t->ssl, n);
        struct saved se;
        if (e == SSL_ERROR_SSL) save_errors(&se);
        /* SSL_read emits only alerts (a KeyUpdate answer waits for the next SSL_write); WANT_WRITE
         * = an alert met a full tx. Not in TLS 1.3 or AWS-LC (one alert, CTL_ROOM covers it); in
         * OpenSSL TLS 1.2 ~600 renegotiation ClientHellos (a warning alert each) while we cannot
         * send fill it. The stream is lost then. */
        if (op == READ && e == SSL_ERROR_WANT_WRITE) { t->broken = 1; return -EIO; }
        int fr = op == READ ? (reader_send(t), 0) : flush(t);
        switch (e) {
        case SSL_ERROR_NONE:
            if (fr < 0) return fr;
            return (op == READ || op == WRITE) ? n : 0;
        case SSL_ERROR_SSL:
            restore_errors(&se); /* alert (if any) was sent above; its failure is not the cause */
            return -EPROTO;
        case SSL_ERROR_ZERO_RETURN:
            if (fr < 0) return fr;
            return op == READ ? 0 : op == WRITE ? -EPIPE : -ECONNRESET;
        case SSL_ERROR_WANT_WRITE: /* not READ (above): tx was full; flushed above, retry */
            if (op == WRITE) em_tls_dbg_partial++;
            if (fr < 0) return fr;
            continue;
        case SSL_ERROR_WANT_READ: {
            if (fr < 0) return fr;
            if (op == WRITE || op == SHUTDOWN) return -EPROTO; /* no renegotiation: not reachable */
            int r = fill(t, first);
            if (r < 0) return r;
            first = 0;
            continue;
        }
        default: /* SYSCALL, WANT_X509_LOOKUP...: no syscalls happen inside SSL_* here */
            return -EIO;
        }
    }
}

static void destroy(em_tls *t)
{
    SSL_free(t->ssl);
    rx_release(t);
    buf_put(t->tx);
    free(t);
}

static int start(em_tls **out, SSL_CTX *ctx, int fd, const char *host, int client)
{
    em_tls *t = calloc(1, sizeof *t);
    if (!t) return -ENOMEM;
    t->fd = fd;
    pthread_once(&bio_once, bio_meth_init);
    BIO *bio = bio_meth ? BIO_new(bio_meth) : NULL;
    t->ssl = SSL_new(ctx);
    if (!t->ssl || !bio) {
        BIO_free(bio);
        destroy(t);
        return -ENOMEM;
    }
    BIO_set_data(bio, t);
    SSL_set_bio(t->ssl, bio, bio); /* the SSL owns bio now */
    SSL_set_options(t->ssl, SSL_OP_NO_RENEGOTIATION);
    /* Not SSL_MODE_RELEASE_BUFFERS: it takes OpenSSL from 85.8 to 53.2 KiB per idle connection but
     * costs 4-12% CPU on the 256 x 128 echo cell (a 16 KiB buffer pair malloc'd per record). Reconsider
     * if idle memory matters more than small-record CPU. */
    if (client) {
        SSL_set_connect_state(t->ssl);
        if (host) {
            unsigned char ip[16];
            int is_ip = inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1;
            int ok = is_ip || SSL_set_tlsext_host_name(t->ssl, host);
            if (ok && (SSL_get_verify_mode(t->ssl) & SSL_VERIFY_PEER))
                ok = is_ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host)
                           : SSL_set1_host(t->ssl, host);
            if (!ok) { destroy(t); return -ENOMEM; }
        }
    } else {
        SSL_set_accept_state(t->ssl);
    }
    int r = run(t, client ? CONNECT : ACCEPT, NULL, 0);
    if (r < 0) { destroy(t); return r; }
    em_stack_trim(); /* the handshake's peak stack is dead now */
    *out = t;
    return 0;
}

int em_tls_accept(em_tls **out, SSL_CTX *ctx, int fd) { return start(out, ctx, fd, NULL, 0); }
int em_tls_connect(em_tls **out, SSL_CTX *ctx, int fd, const char *host) { return start(out, ctx, fd, host, 1); }

ssize_t em_tls_recv(em_tls *t, void *buf, size_t len)
{
    if (len == 0) return -EINVAL;
    if (t->reading) return -EBUSY;
    if (t->broken) return -EPIPE;
    t->reading = 1;
    int r = run(t, READ, buf, len > INT_MAX ? INT_MAX : (int)len);
    t->reading = 0;
    return r;
}

int em_tls_send_all(em_tls *t, const void *buf, size_t len)
{
    if (t->writing) return -EBUSY;
    if (t->broken) return -EPIPE;
    t->writing = 1;
    int r = 0;
    while (len && r >= 0) {
        size_t c = len > REC_PLAIN ? REC_PLAIN : len; /* one record per call, see run() */
        r = run(t, WRITE, (void *)buf, (int)c);
        buf = (const char *)buf + c;
        len -= c;
    }
    if (r < 0) t->broken = 1; /* any failure while sending leaves OpenSSL mid-record */
    t->writing = 0;
    return r < 0 ? r : 0;
}

int em_tls_shutdown(em_tls *t)
{
    if (t->writing) return -EBUSY;
    if (t->broken) return -EPIPE;
    t->writing = 1;
    int r = run(t, SHUTDOWN, NULL, 0);
    if (r < 0) t->broken = 1;
    t->writing = 0;
    return r;
}

void em_tls_free(em_tls *t)
{
    if (t) destroy(t);
}

SSL *em_tls_ssl(em_tls *t) { return t->ssl; }
