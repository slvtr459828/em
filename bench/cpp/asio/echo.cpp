// Asio echo server for bench/run.sh: argv[1] = port, argv[2] = workers (default 1); built plain, or with
// -DTLS for asio::ssl (OpenSSL / AWS-LC). Written the way Asio's own documentation and examples do it
// today: C++20 coroutines (co_spawn + awaitable), one coroutine per connection, no callbacks. Each worker
// is a thread with its own io_context(1) (concurrency hint 1: the scheduler takes no locks) and its own
// SO_REUSEPORT listener. Backend is a compile-time choice: epoll by default, io_uring with
// -DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL (sockets then go through io_uring_socket_service).
#include <asio.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#ifdef TLS
#include <asio/ssl.hpp>
extern "C" SSL_CTX *asio_bench_tls_ctx(int *stateful);
static int stateful;
#endif

using asio::awaitable;
using asio::ip::tcp;
constexpr auto tok = asio::as_tuple(asio::use_awaitable);

static bool nodelay = true; // NODELAY=0 turns it off, like the other servers

#ifdef TLS
static asio::ssl::context *sctx;
#endif

awaitable<void> echo(tcp::socket sock)
{
    std::error_code ec;
    sock.set_option(tcp::no_delay(nodelay), ec);
#ifdef TLS
    asio::ssl::stream<tcp::socket> s(std::move(sock), *sctx);
    auto [he] = co_await s.async_handshake(asio::ssl::stream_base::server, tok);
    if (!he) {
#else
    tcp::socket &s = sock;
    {
#endif
        char buf[16384];
        for (;;) {
            auto [re, n] = co_await s.async_read_some(asio::buffer(buf), tok);
            if (re || n == 0) break;
            auto [we, w] = co_await asio::async_write(s, asio::buffer(buf, n), tok);
            if (we) break;
        }
#ifdef TLS
        // The churn client ends with RST, no close_notify: SSL_free would then drop the session from the
        // server cache and stateful resumption would almost never hit. Same workaround as em_tls_echo.c.
        if (stateful) SSL_set_shutdown(s.native_handle(), SSL_SENT_SHUTDOWN);
#endif
    }
}

awaitable<void> listener(tcp::acceptor &acc)
{
    auto ex = co_await asio::this_coro::executor;
    for (;;) {
        auto [ec, s] = co_await acc.async_accept(tok);
        if (ec == asio::error::operation_aborted) co_return;
        if (!ec) asio::co_spawn(ex, echo(std::move(s)), asio::detached);
    }
}

static void worker(uint16_t port)
{
    asio::io_context ctx(1);
    tcp::acceptor acc(ctx);
    tcp::endpoint ep(asio::ip::address_v4::loopback(), port);
    acc.open(ep.protocol());
    acc.set_option(tcp::acceptor::reuse_address(true));
    int one = 1;
    setsockopt(acc.native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    acc.bind(ep);
    acc.listen(4096);
    asio::co_spawn(ctx, listener(acc), asio::detached);
    ctx.run();
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s port [workers]\n", argv[0]); return 2; }
    int workers = argc == 3 ? atoi(argv[2]) : 1;
    uint16_t port = (uint16_t)atoi(argv[1]);
    const char *nd = getenv("NODELAY");
    if (nd && !strcmp(nd, "0")) nodelay = false;
#ifdef TLS
    SSL_CTX *raw = asio_bench_tls_ctx(&stateful);
    if (!raw) return 2;
    asio::ssl::context c(raw); // takes ownership
    sctx = &c;
#endif
    try {
        std::vector<std::thread> th;
        for (int i = 1; i < workers; i++) th.emplace_back(worker, port);
        worker(port);
        for (auto &t : th) t.join();
    } catch (std::exception &e) { fprintf(stderr, "asio: %s\n", e.what()); return 1; }
    return 0;
}
