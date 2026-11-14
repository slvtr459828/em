/* libFuzzer harness for em_http_serve (make fuzz-http). Each input is the raw byte stream of a
 * client on a socketpair: write it, shutdown(SHUT_WR), discard the output until EOF. The handler
 * picks one of several ways to use the API from the request itself (second path byte), so the
 * body reader, respond, HEAD and the error paths are all reached. Timeouts are off: the only
 * clock is the peer closing. One em_run per input. */
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <liburing.h>
#include <unistd.h>
#include "../../include/em.h"
#include "../../include/em_http.h"

#define MAX_IN 4096

static const uint8_t *in_data;
static size_t in_len;
static volatile unsigned touched; /* keeps the loops over the request alive */

static int handler(em_http_conn *c, const em_http_req *r, void *arg)
{
    (void)arg;
    static const em_http_header h[2] = { { "Content-Type", 12, "text/plain", 10 },
                                         { "X-Fuzz", 6, "1", 1 } };
    unsigned sum = (unsigned)r->minor_version + (unsigned)r->content_length; /* touch the request */
    for (size_t i = 0; i < r->method_len; i++) sum += (unsigned char)r->method[i];
    for (size_t i = 0; i < r->path_len; i++) sum += (unsigned char)r->path[i];
    for (size_t i = 0; i < r->nheaders; i++) { /* a folded line has an empty name (NULL) */
        const em_http_header *h_ = &r->headers[i];
        if (h_->name_len) sum += (unsigned char)h_->name[h_->name_len - 1];
        if (h_->value_len) sum += (unsigned char)h_->value[h_->value_len - 1];
    }

    touched = sum;
    unsigned char sel = r->path_len > 1 ? (unsigned char)r->path[1] : 0;
    char buf[300];
    size_t total = 0;
    int mode = sel % 7;
    if (mode == 1 || mode == 2 || mode == 5) {
        /* chunk size: 4096 for 1, 1..16 varying for 2, one read for 5 */
        for (size_t n = 0;; n++) {
            size_t want = mode == 1 ? sizeof buf : (mode == 2 ? 1 + (n * 7 + sel) % 16 : sizeof buf);
            ssize_t k = em_http_read_body(c, buf, want);
            if (k < 0) return (int)k; /* serve answers 413/400/408 itself when we have not responded */
            if (k == 0 || mode == 5) { total += (size_t)(k > 0 ? k : 0); break; }
            total += (size_t)k;
        }
    }
    if (mode == 3) return -EIO;
    if (mode == 6) return 0; /* no response: serve answers 500 */
    int rc = em_http_respond(c, 200, h, 2, buf, total % sizeof buf);
    if (mode == 4) { /* second respond and a read after respond: both -EALREADY */
        if (em_http_respond(c, 200, h, 1, "x", 1) != -EALREADY && rc == 0) __builtin_trap();
        if (em_http_read_body(c, buf, sizeof buf) != -EALREADY && rc == 0) __builtin_trap();
    }
    return rc;
}

static int conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    static const em_http_cfg cfg = { .max_header_bytes = 256, .max_headers = 8,
                                     .max_body_bytes = 1024, .header_timeout_ns = -1,
                                     .idle_timeout_ns = -1 };
    em_http_serve(fd, NULL, &cfg, handler, NULL);
    em_close(fd);
    return 0;
}

static int run(void *arg)
{
    (void)arg;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) __builtin_trap();
    em_scope sc;
    em_scope_open(&sc, 0);
    if (em_spawn(&sc, conn, (void *)(intptr_t)sv[1]) != 0) __builtin_trap();
    /* the whole input fits the socket buffer, so this cannot block on the server */
    if (in_len && em_send_all(sv[0], in_data, in_len) != 0) __builtin_trap();
    em_shutdown(sv[0], SHUT_WR);
    char sink[4096];
    while (em_recv(sv[0], sink, sizeof sink) > 0) {}
    em_close(sv[0]);
    return em_scope_close(&sc);
}

/* em_run failed (CI has seen -ENOMEM on x86, never locally): redo each step of its setup now and
 * say which one fails, with the memory state. Remove once the cause is known (plans/http.md, Nợ). */
static void diagnose(void)
{
    struct io_uring r;
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
              IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = 1024;
    int rc = io_uring_queue_init_params(256, &r, &p);
    fprintf(stderr, "diag: ring(256/1024) = %d\n", rc);
    if (rc == 0) io_uring_queue_exit(&r);
    void *m = mmap(NULL, 1u << 20, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    fprintf(stderr, "diag: mmap 1 MiB = %s (errno %d)\n", m == MAP_FAILED ? "failed" : "ok", errno);
    if (m != MAP_FAILED) munmap(m, 1u << 20);
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    fprintf(stderr, "diag: eventfd = %d (errno %d)\n", fd, fd < 0 ? errno : 0);
    if (fd >= 0) close(fd);
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
        fprintf(stderr, "diag: RLIMIT_MEMLOCK cur %llu max %llu\n", (unsigned long long)rl.rlim_cur,
                (unsigned long long)rl.rlim_max);
    static const char *const files[] = { "/proc/self/status", "/proc/meminfo" };
    for (size_t i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "r");
        char line[256];
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "Vm", 2) || !strncmp(line, "Mem", 3) || !strncmp(line, "Commit", 6) ||
                !strncmp(line, "Threads", 7))
                fprintf(stderr, "diag: %s", line);
        if (f) fclose(f);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > MAX_IN) return 0;
    in_data = data;
    in_len = size;
    int rc = em_run(1, run, NULL);
    if (rc != 0) { fprintf(stderr, "em_run = %d (backend %s)\n", rc, em_backend()); diagnose(); __builtin_trap(); }
    return 0;
}
