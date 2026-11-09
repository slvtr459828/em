// TLS library microbench (plans/tls-perf.md, step 2): the CPU cost of the server's TLS work alone, no
// sockets, no em. Server and client SSL live in one process; the bytes move between four memory BIOs.
// Only the server's SSL_*/BIO_new/SSL_new/SSL_free calls are measured. Uses the OpenSSL API subset that
// OpenSSL 3.0, 3.5 and AWS-LC share.
//
// usage: tls_lib <lib-name> <tickets: 0|stateless|stateful> <iters>
// stdout: CSV  lib,op,tickets,variant,ns_per_op,instr_per_op,cycles_per_op
//   op       rec128 | rec4096 (server SSL_read + SSL_write of one record pair) | full | resume
//            (a TLS 1.3 handshake incl. SSL_new/BIO creation/SSL_free; resume needs tickets)
//   variant  base   counters enabled/disabled around each measured server call group
//            sys    base + one getppid() right before each group (R4: syscall between records)
//            total  counters on for the whole loop (server + client + shuttle), divided by ops;
//                   cross-check of base without any per-call ioctl
// stderr: config and notes. instr/cycles are user mode only; "n/a" when perf_event_open fails.
// Fixed config: TLS 1.3, TLS_AES_256_GCM_SHA384, X25519, self-signed ECDSA P-256.
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CIPHER "TLS_AES_256_GCM_SHA384"

static void die(const char *what) {
    fprintf(stderr, "tls_lib: %s failed\n", what);
    ERR_print_errors_fp(stderr);
    exit(1);
}
#define CK(x) do { if (!(x)) die(#x); } while (0)

// ---- measurement: ns by clock_gettime, user instructions/cycles by a perf event group ----
enum { BASE, SYS, TOTAL };
static const char *const var_name[] = {"base", "sys", "total"};
static int var, pfd = -1, cfd = -1;
static double ns;
static struct timespec t0;

static double since(void) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
}
static void perf_open(void) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.size = sizeof a;
    a.config = PERF_COUNT_HW_INSTRUCTIONS;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.read_format = PERF_FORMAT_GROUP;
    pfd = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (pfd < 0) return;
    a.config = PERF_COUNT_HW_CPU_CYCLES;
    a.disabled = 0;
    cfd = syscall(SYS_perf_event_open, &a, 0, -1, pfd, 0);
    if (cfd < 0) { close(pfd); pfd = -1; }
}
static void pc(unsigned long req) { if (pfd >= 0) ioctl(pfd, req, PERF_IOC_FLAG_GROUP); }

// bracket one server call group
static void mb(void) {
    if (var == TOTAL) return;
    if (var == SYS) syscall(SYS_getppid);
    pc(PERF_EVENT_IOC_ENABLE);
    clock_gettime(CLOCK_MONOTONIC, &t0);
}
static void me(void) {
    if (var == TOTAL) return;
    ns += since();
    pc(PERF_EVENT_IOC_DISABLE);
}

// ---- TLS setup ----
static SSL_CTX *sctx, *cctx;

static void mkcert(SSL_CTX *c) {
    EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    CK(ec && EC_KEY_generate_key(ec));
    EVP_PKEY *k = EVP_PKEY_new();
    CK(k && EVP_PKEY_assign_EC_KEY(k, ec));
    X509 *x = X509_new();
    CK(x && X509_set_version(x, 2));
    CK(ASN1_INTEGER_set(X509_get_serialNumber(x), 1));
    CK(X509_gmtime_adj(X509_getm_notBefore(x), 0));
    CK(X509_gmtime_adj(X509_getm_notAfter(x), 31536000L));
    CK(X509_set_pubkey(x, k));
    X509_NAME *n = X509_get_subject_name(x);
    CK(X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0));
    CK(X509_set_issuer_name(x, n));
    CK(X509_sign(x, k, EVP_sha256()));
    CK(SSL_CTX_use_certificate(c, x));
    CK(SSL_CTX_use_PrivateKey(c, k));
    X509_free(x);
    EVP_PKEY_free(k);
}
static SSL_CTX *mkctx(const SSL_METHOD *m) {
    SSL_CTX *c = SSL_CTX_new(m);
    CK(c);
    CK(SSL_CTX_set_min_proto_version(c, TLS1_3_VERSION));
    CK(SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION));
    CK(SSL_CTX_set_ciphersuites(c, CIPHER));
    CK(SSL_CTX_set1_groups_list(c, "X25519"));
    return c;
}
// the client's newest resumable session. A callback because TLS 1.3 tickets arrive after the handshake and
// AWS-LC's SSL_get1_session does not return them
static SSL_SESSION *sess;
static int new_session(SSL *c, SSL_SESSION *s) {
    (void)c;
    SSL_SESSION_free(sess);
    sess = s; // we own it now
    return 1;
}
static void setup(const char *tickets) {
    sctx = mkctx(TLS_server_method());
    cctx = mkctx(TLS_client_method());
    SSL_CTX_set_session_cache_mode(cctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_sess_set_new_cb(cctx, new_session);
    mkcert(sctx);
    CK(SSL_CTX_set_session_id_context(sctx, (const unsigned char *)"tls_lib", 7));
    if (!strcmp(tickets, "0")) {
        CK(SSL_CTX_set_num_tickets(sctx, 0));
    } else {
        CK(SSL_CTX_set_num_tickets(sctx, 2));
        if (!strcmp(tickets, "stateful")) {
            SSL_CTX_set_options(sctx, SSL_OP_NO_TICKET);
            SSL_CTX_set_session_cache_mode(sctx, SSL_SESS_CACHE_SERVER);
        } else if (strcmp(tickets, "stateless")) {
            fprintf(stderr, "tickets: 0|stateless|stateful\n");
            exit(2);
        }
    }
}

// ---- one connection: server SSL (measured) + client SSL, memory BIOs between ----
struct conn { SSL *s, *c; BIO *si, *so, *ci, *co; };
static unsigned char buf[16384], msg[16384];

// move everything pending in `from` to `to`
static void pump(BIO *from, BIO *to) {
    int n;
    while ((n = BIO_read(from, buf, sizeof buf)) > 0) CK(BIO_write(to, buf, n) == n);
}
static void step(SSL *ssl, int *done) {
    int r = SSL_do_handshake(ssl);
    if (r == 1) { *done = 1; return; }
    if (SSL_get_error(ssl, r) != SSL_ERROR_WANT_READ) die("SSL_do_handshake");
}
// full or resumed handshake. take: also leave the client's newest session in `sess`
static void connect_pair(struct conn *k, int resume, int take) {
    mb();
    k->s = SSL_new(sctx);
    k->si = BIO_new(BIO_s_mem());
    k->so = BIO_new(BIO_s_mem());
    SSL_set_bio(k->s, k->si, k->so);
    SSL_set_accept_state(k->s);
    me();
    CK(k->s && k->si && k->so);
    k->c = SSL_new(cctx);
    k->ci = BIO_new(BIO_s_mem());
    k->co = BIO_new(BIO_s_mem());
    CK(k->c && k->ci && k->co);
    SSL_set_bio(k->c, k->ci, k->co);
    if (resume) CK(SSL_set_session(k->c, sess));
    if (take) { SSL_SESSION_free(sess); sess = NULL; } // new_session() must deliver a fresh one
    SSL_set_connect_state(k->c);
    int sd = 0, cd = 0;
    for (int round = 0; !sd; round++) {
        if (round > 8) die("handshake did not finish");
        if (!cd) step(k->c, &cd);
        pump(k->co, k->si);
        mb();
        step(k->s, &sd);
        me();
        pump(k->so, k->ci);
    }
#ifdef OPENSSL_IS_AWSLC
    // AWS-LC leaves the TLS 1.3 tickets unflushed until the server's next write; a zero-byte write flushes
    mb();
    SSL_write(k->s, buf, 0);
    me();
    pump(k->so, k->ci);
#endif
    if (take) {
        // the client only sees the tickets when it reads
        int r = SSL_read(k->c, buf, 1);
        if (r > 0 || SSL_get_error(k->c, r) != SSL_ERROR_WANT_READ) die("client SSL_read");
        CK(sess);
    }
}
static void disconnect(struct conn *k, int resumed) {
    if (SSL_session_reused(k->s) != resumed) {
        fprintf(stderr, "tls_lib: expected %s handshake\n", resumed ? "resumed" : "full");
        exit(1);
    }
    // em_tls shuts down before freeing; a free without it also marks the session not resumable
    mb();
    SSL_shutdown(k->s);
    SSL_free(k->s);
    me();
    SSL_set_shutdown(k->c, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
    SSL_free(k->c);
}

// ---- ops ----
static struct conn rk;
static int rsz;

static void it_rec(void) {
    CK(SSL_write(rk.c, msg, rsz) == rsz);
    pump(rk.co, rk.si);
    mb();
    int r = SSL_read(rk.s, buf, sizeof buf);
    int w = r > 0 ? SSL_write(rk.s, buf, r) : -1;
    me();
    CK(r == rsz && w == rsz);
    pump(rk.so, rk.ci);
    CK(SSL_read(rk.c, buf, sizeof buf) == rsz);
}
static void it_full(void) {
    struct conn k;
    connect_pair(&k, 0, 0);
    disconnect(&k, 0);
}
static void it_resume(void) {
    struct conn k;
    connect_pair(&k, 1, 1);
    disconnect(&k, 1);
}

static void run(const char *lib, const char *tickets, const char *op, void (*it)(void), long iters) {
    long warm = iters / 10 + 10;
    for (var = BASE; var <= TOTAL; var++) {
        ns = 0;
        for (long i = 0; i < warm; i++) it();
        pc(PERF_EVENT_IOC_RESET);
        ns = 0;
        if (var == TOTAL) { pc(PERF_EVENT_IOC_ENABLE); clock_gettime(CLOCK_MONOTONIC, &t0); }
        for (long i = 0; i < iters; i++) it();
        if (var == TOTAL) { ns = since(); pc(PERF_EVENT_IOC_DISABLE); }
        char ins[32] = "n/a", cyc[32] = "n/a";
        uint64_t g[3];
        if (pfd >= 0 && read(pfd, g, sizeof g) == sizeof g) {
            snprintf(ins, sizeof ins, "%.1f", (double)g[1] / iters);
            snprintf(cyc, sizeof cyc, "%.1f", (double)g[2] / iters);
        }
        printf("%s,%s,%s,%s,%.1f,%s,%s\n", lib, op, tickets, var_name[var], ns / iters, ins, cyc);
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: tls_lib lib-name 0|stateless|stateful iters\n"); return 2; }
    const char *lib = argv[1], *tickets = argv[2];
    long iters = atol(argv[3]);
#ifdef OPENSSL_IS_AWSLC
    if (!strcmp(tickets, "stateful")) {
        fprintf(stderr, "# lib=%s tickets=stateful skipped: AWS-LC has only stateless TLS 1.3 tickets\n", lib);
        return 0;
    }
#endif
    setup(tickets);
    perf_open();
    var = BASE; // brackets are no-ops in setup: pc() does nothing until pfd opens, and ns is reset per run
    // a first full handshake: checks the negotiated parameters and gives resume its session
    struct conn k;
    connect_pair(&k, 0, strcmp(tickets, "0") != 0); // with tickets, leaves resume its session
    fprintf(stderr, "# lib=%s (%s) tickets=%s iters=%ld proto=%s cipher=%s perf=%s\n", lib,
            OpenSSL_version(OPENSSL_VERSION), tickets, iters, SSL_get_version(k.s), SSL_get_cipher_name(k.s),
            pfd >= 0 ? "on" : "n/a");
    if (strcmp(SSL_get_version(k.s), "TLSv1.3") || strcmp(SSL_get_cipher_name(k.s), CIPHER)) die("negotiation");
    disconnect(&k, 0);
    // established connection for the record ops
    struct conn *r = &rk;
    connect_pair(r, 0, 0);
    memset(msg, 'x', sizeof msg);
    rsz = 128;
    run(lib, tickets, "rec128", it_rec, iters);
    rsz = 4096;
    run(lib, tickets, "rec4096", it_rec, iters);
    run(lib, tickets, "full", it_full, iters);
    if (strcmp(tickets, "0")) {
        // fresh session: the full ops above may have pushed the first one out of the server's cache
        connect_pair(&k, 0, 1);
        disconnect(&k, 0);
        run(lib, tickets, "resume", it_resume, iters);
    } else fprintf(stderr, "# resume skipped: no tickets, nothing to resume\n");
    return 0;
}
