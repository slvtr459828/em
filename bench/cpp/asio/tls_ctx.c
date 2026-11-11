/* Server SSL_CTX of the Asio TLS echo server: the same certificate (tests/tls_util.h) and TLS_TICKETS
 * handling as bench/em_tls_echo.c, in C because tls_util.h is C. Returns NULL (message on stderr) on a bad
 * TLS_TICKETS. *stateful: the caller must mark sessions shut down before freeing them (see echo.cpp). */
#include <stdio.h>
#include <string.h>
#include "../../../tests/tls_util.h"

SSL_CTX *asio_bench_tls_ctx(int *stateful);

SSL_CTX *asio_bench_tls_ctx(int *stateful)
{
    SSL_CTX *sctx, *cctx;
    tls_test_ctx(&sctx, &cctx);
    SSL_CTX_free(cctx);
    *stateful = 0;
    const char *tk = getenv("TLS_TICKETS");
    if (!tk) return sctx;
    if (!strcmp(tk, "0")) SSL_CTX_set_num_tickets(sctx, 0);
    else if (!strcmp(tk, "stateless")) SSL_CTX_set_num_tickets(sctx, 2);
    else if (!strcmp(tk, "stateful")) {
#ifdef OPENSSL_IS_AWSLC
        fprintf(stderr, "TLS_TICKETS=stateful: not supported on AWS-LC (no stateful TLS 1.3 tickets)\n");
        return NULL;
#endif
        SSL_CTX_set_options(sctx, SSL_OP_NO_TICKET);
        SSL_CTX_set_session_cache_mode(sctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_set_session_id_context(sctx, (const unsigned char *)"em", 2);
        SSL_CTX_set_num_tickets(sctx, 2);
        *stateful = 1;
    } else { fprintf(stderr, "TLS_TICKETS=%s: want 0, stateless or stateful\n", tk); return NULL; }
    return sctx;
}
