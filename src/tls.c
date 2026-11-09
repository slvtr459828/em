/* em_tls: OpenSSL over a BIO pair. See include/em_tls.h for the contract.
 *
 * Data path, OpenSSL: em_recv writes straight into the BIO buffer SSL reads from (BIO_nwrite0),
 * SSL_read decrypts into the caller's buffer; SSL_write encrypts into the BIO buffer and em_send
 * sends straight from it (BIO_nread0). No copy into or out of the BIO beyond the one SSL itself
 * makes. AWS-LC (OPENSSL_IS_AWSLC) has no BIO_nread0/nwrite0: one extra copy each way, between
 * the BIO and an IN_BYTES array on the fiber's stack (never a shared buffer: io_uring uses it
 * while the fiber is parked).
 *
 * Fibers only park in em_recv / em_send / em_park, never inside an SSL_* call, and a worker runs
 * one fiber at a time, so SSL_* calls of the reader and the writer never overlap. */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include "em.h"
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
#define OUT_BYTES  (REC_WORST + CTL_ROOM)    /* 19.5 KiB */
#define IN_BYTES   (16384 + 2048 + 5)       /* largest record a TLS 1.2 peer may send */

struct em_tls {
    SSL      *ssl;
    BIO      *net;     /* our end of the pair; ssl owns the other end */
    int       fd;
    em_fiber *waiter;  /* fiber parked until the flusher is done (at most one: the other direction) */
    unsigned char reading, writing, flushing, broken;
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

#ifdef OPENSSL_IS_AWSLC
/* Sends everything in the BIO through a stack buffer (AWS-LC counterpart of the BIO_nread0 loop
 * in flush()). Bytes read out of the BIO cannot go back, so every one of them is sent. A separate
 * function so its IN_BYTES array is never live together with fill()'s one. */
__attribute__((noinline)) static int drain(em_tls *t)
{
    unsigned char buf[IN_BYTES];
    int n;
    while ((n = BIO_read(t->net, buf, sizeof buf)) > 0) {
        for (int o = 0; o < n;) {
            ssize_t s = em_send(t->fd, buf + o, (size_t)(n - o));
            if (s < 0) { t->broken = 1; return (int)s; }
            o += (int)s;
        }
    }
    return 0;
}
#endif

/* Sends everything the SSL has queued, until the BIO is empty (including bytes queued by the
 * other fiber meanwhile). Only one fiber drains at a time. wait: if another fiber is draining,
 * park until it is done (needed when this fiber's own bytes must be on their way: send_all
 * returns, or the BIO is full); otherwise return, the drainer will send them. A reader never
 * waits just for its own side effect (KeyUpdate answer): the drainer may be blocked on the peer
 * and the peer on our reads. Failure here breaks the stream. */
static int flush(em_tls *t, int wait)
{
    while (t->flushing) {
        if (!wait) return 0;
        t->waiter = em_self();
        int r = em_park();
        t->waiter = NULL;
        if (r < 0) { t->broken = 1; return r; }
        if (t->broken) return -EPIPE;
    }
    int r = 0;
#ifdef OPENSSL_IS_AWSLC
    t->flushing = 1;
    r = drain(t);
#else
    char *p;
    long n;
    t->flushing = 1;
    while ((n = BIO_nread0(t->net, &p)) > 0) {
        ssize_t s = em_send(t->fd, p, (size_t)n);
        if (s < 0) { r = (int)s; t->broken = 1; break; }
        BIO_nread(t->net, &p, (int)s);
    }
#endif
    t->flushing = 0;
    if (t->waiter) em_unpark(t->waiter);
    return r;
}

#ifdef OPENSSL_IS_AWSLC
/* Reads from the socket into the BIO (the AWS-LC counterpart of the BIO_nwrite0 block in run()).
 * Its own function so the IN_BYTES array is never live together with drain()'s one. */
__attribute__((noinline)) static int fill(em_tls *t)
{
    unsigned char in[IN_BYTES];
    size_t room = BIO_ctrl_get_write_guarantee(t->net);
    if (room == 0) return -EIO; /* SSL wants input while its input BIO is full: not reachable */
    ssize_t k = em_recv(t->fd, in, room < sizeof in ? room : sizeof in);
    if (k < 0) return (int)k;        /* cancelled / deadline: nothing consumed, retryable */
    if (k == 0) return -ECONNRESET;  /* TCP EOF without close_notify */
    if (BIO_write(t->net, in, (int)k) != k) return -EIO; /* k <= room: not reachable */
    return 0;
}
#endif

/* One loop for every operation. Returns bytes for READ/WRITE (> 0), 0 for the rest and for
 * close_notify on READ, or -errno. */
static int run(em_tls *t, enum op op, void *buf, int len)
{
    for (;;) {
        /* A data record must never be left half-written inside OpenSSL: if the reader then has to
         * send something (KeyUpdate answer), OpenSSL fails it with "bad write retry" or the records
         * interleave. So SSL_write only starts when the BIO has room for its whole output
         * (REC_WORST), and never gets WANT_WRITE (counted in em_tls_dbg_partial for t40). */
        if (op == WRITE && BIO_ctrl_pending(t->net) > OUT_BYTES - REC_WORST) {
            int r = flush(t, 1);
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
        /* flush BEFORE any wait for input: TLS 1.3 can produce records while reading
         * (KeyUpdate, alerts); forgetting means both sides wait for each other.
         * DEBT: a READ that flushes here parks in em_send while its connection's input is not
         * being read. Failure: both ends' readers flush at once (simultaneous KeyUpdate answer or
         * alert under bulk traffic both ways) with both TCP send buffers full and both writers
         * parked in em_send too: nobody reads, both connections stall until a deadline. Not yet
         * triggered by any test (OpenSSL 3.5 sends the KeyUpdate answer from SSL_write).
         * Remove when the reader's flush no longer blocks reading: hand it to a flusher fiber
         * (or to the writer) and keep reading meanwhile. */
        int fr = flush(t, op != READ || e == SSL_ERROR_WANT_WRITE);
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
        case SSL_ERROR_WANT_WRITE: /* BIO was full; flushed above, retry */
            if (op == WRITE) em_tls_dbg_partial++;
            if (fr < 0) return fr;
            continue;
        case SSL_ERROR_WANT_READ: {
            if (fr < 0) return fr;
            if (op == WRITE || op == SHUTDOWN) return -EPROTO; /* no renegotiation: not reachable */
#ifdef OPENSSL_IS_AWSLC
            int fr2 = fill(t);
            if (fr2 < 0) return fr2;
            continue;
#else
            char *p;
            long room = BIO_nwrite0(t->net, &p);
            if (room <= 0) return -EIO; /* SSL wants input while its input BIO is full: not reachable */
            ssize_t k = em_recv(t->fd, p, (size_t)room);
            if (k < 0) return (int)k;        /* cancelled / deadline: nothing consumed, retryable */
            if (k == 0) return -ECONNRESET;  /* TCP EOF without close_notify */
            BIO_nwrite(t->net, &p, (int)k);
            continue;
#endif
        }
        default: /* SYSCALL, WANT_X509_LOOKUP...: no syscalls happen inside SSL_* here */
            return -EIO;
        }
    }
}

static void destroy(em_tls *t)
{
    SSL_free(t->ssl);
    BIO_free(t->net);
    free(t);
}

static int start(em_tls **out, SSL_CTX *ctx, int fd, const char *host, int client)
{
    em_tls *t = calloc(1, sizeof *t);
    if (!t) return -ENOMEM;
    t->fd = fd;
    BIO *in = NULL;
    t->ssl = SSL_new(ctx);
    if (!t->ssl || !BIO_new_bio_pair(&in, OUT_BYTES, &t->net, IN_BYTES)) {
        BIO_free(in);
        destroy(t);
        return -ENOMEM;
    }
    SSL_set_bio(t->ssl, in, in); /* the SSL owns `in` now */
    SSL_set_options(t->ssl, SSL_OP_NO_RENEGOTIATION);
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
