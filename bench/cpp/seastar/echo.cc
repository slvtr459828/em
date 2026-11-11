// Seastar echo server for bench/run.sh (plans/bench-cpp.md): argv[1] = port, argv[2] = workers
// (shards, default 1). With -DTLS every connection is seastar::tls (GnuTLS). The backend comes from
// the binary's name: seastar-uring / seastar-tls-uring -> io_uring, *-epoll -> epoll.
//
// Seastar options chosen for a 1-CPU taskset (the contract: bench/run.sh pins the server, not Seastar):
//   --overprovisioned   the documented flag for shared/pinned-by-someone-else CPUs: no idle polling,
//                       no own thread pinning, no aio polling, no transparent hugepages
//   --thread-affinity 0 explicit (already implied by --overprovisioned): taskset decides the CPU
//   -m 1G               Seastar takes all memory by default; 1G is plenty for 1k connections x 16 KiB
//   (--lock-memory is off by default and left off)
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/net/api.hh>
#ifdef TLS
#include <gnutls/x509.h>
#include <seastar/net/tls.hh>
#endif
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace seastar;

static bool nodelay = true; // NODELAY=0 turns it off
static uint16_t port;
#ifdef TLS
static tls::credentials_builder tls_builder;
#ifndef TLS_PRIORITY
#define TLS_PRIORITY "NORMAL:-VERS-ALL:+VERS-TLS1.3"
#endif

// Self-signed ECDSA P-256 certificate for "localhost", generated in memory (like tests/tls_util.h).
static void tls_init()
{
    gnutls_x509_privkey_t key;
    gnutls_x509_crt_t crt;
    gnutls_datum_t kp = {}, cp = {};
    gnutls_x509_privkey_init(&key);
    gnutls_x509_privkey_generate(key, GNUTLS_PK_ECDSA, GNUTLS_CURVE_TO_BITS(GNUTLS_ECC_CURVE_SECP256R1), 0);
    gnutls_x509_crt_init(&crt);
    gnutls_x509_crt_set_key(crt, key);
    gnutls_x509_crt_set_version(crt, 3);
    gnutls_x509_crt_set_serial(crt, "\x01", 1);
    time_t now = time(nullptr);
    gnutls_x509_crt_set_activation_time(crt, now - 3600);
    gnutls_x509_crt_set_expiration_time(crt, now + 24 * 3600);
    gnutls_x509_crt_set_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME, 0, "localhost", 9);
    gnutls_x509_crt_set_subject_alt_name(crt, GNUTLS_SAN_DNSNAME, "localhost", 9, GNUTLS_FSAN_SET);
    if (gnutls_x509_crt_sign2(crt, crt, key, GNUTLS_DIG_SHA256, 0) < 0
        || gnutls_x509_crt_export2(crt, GNUTLS_X509_FMT_PEM, &cp) < 0
        || gnutls_x509_privkey_export2(key, GNUTLS_X509_FMT_PEM, &kp) < 0) {
        fprintf(stderr, "certificate generation failed\n");
        exit(1);
    }
    tls_builder.set_x509_key(std::string((char *)cp.data, cp.size), std::string((char *)kp.data, kp.size),
                             tls::x509_crt_format::PEM);
    gnutls_free(cp.data);
    gnutls_free(kp.data);
    gnutls_x509_crt_deinit(crt);
    gnutls_x509_privkey_deinit(key);
    // TLS 1.3 only, AES-256-GCM, secp256r1 (em-tls on OpenSSL negotiates TLS1.3 / AES256-GCM-SHA384 /
    // secp256r1; see the report for how this was matched against the client's "tls ..." line)
    tls_builder.set_priority_string(TLS_PRIORITY);
    const char *tk = getenv("TLS_TICKETS");
    if (tk && !strcmp(tk, "0")) {
    } else if (!tk || !strcmp(tk, "stateless")) {
        tls_builder.set_session_resume_mode(tls::session_resume_mode::TLS13_SESSION_TICKET); // 2 tickets
    } else {
        fprintf(stderr, "TLS_TICKETS=%s: want 0 or stateless (seastar has no stateful TLS 1.3 tickets)\n", tk);
        exit(2);
    }
}
#endif

static future<> echo(connected_socket s)
{
    if (nodelay) s.set_nodelay(true);
    unsigned n = 16384; // fixed: read up to 16 KiB, like every other server
    auto in = s.input(connected_socket_input_stream_config{ .buffer_size = n, .min_buffer_size = n, .max_buffer_size = n });
    auto out = s.output();
    try {
        for (;;) {
            auto b = co_await in.read();
            if (b.empty()) break;
            co_await out.write(std::move(b));
            co_await out.flush();
        }
    } catch (...) { // a dropped connection must not touch the others
    }
    try { co_await out.close(); } catch (...) {}
    try { co_await in.close(); } catch (...) {}
}

static future<> accept_loop(server_socket ss)
{
    for (;;) {
        try {
            auto ar = co_await ss.accept();
            (void)echo(std::move(ar.connection));
        } catch (const std::system_error &) { // ECONNABORTED and friends: keep accepting
        } catch (...) { // abort_requested (shutdown)
            co_return;
        }
    }
}

static future<> start_shard()
{
    listen_options lo;
    lo.reuse_address = true;
    lo.listen_backlog = 4096;
    socket_address sa = make_ipv4_address(ipv4_addr("127.0.0.1", port));
#ifdef TLS
    (void)accept_loop(tls::listen(tls_builder.build_server_credentials(), sa, lo));
#else
    (void)accept_loop(seastar::listen(sa, lo));
#endif
    return make_ready_future<>();
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s port [workers]\n", argv[0]); return 2; }
    port = (uint16_t)atoi(argv[1]);
    std::string workers = argc == 3 ? argv[2] : "1";
    std::string name = argv[0];
    name = name.substr(name.rfind('/') + 1);
    bool uring = name.size() >= 6 && name.compare(name.size() - 6, 6, "-uring") == 0;
    if (!uring && !(name.size() >= 6 && name.compare(name.size() - 6, 6, "-epoll") == 0)) {
        fprintf(stderr, "%s: name must end in -uring or -epoll (selects the reactor backend)\n", argv[0]);
        return 2;
    }
    const char *nd = getenv("NODELAY");
    if (nd && !strcmp(nd, "0")) nodelay = false;
#ifdef TLS
    tls_init();
#endif
    std::vector<std::string> args = { argv[0], "--smp", workers, "--reactor-backend", uring ? "io_uring" : "epoll",
                                      "--overprovisioned", "--thread-affinity", "0", "-m", "1G" };
    std::vector<char *> av;
    for (auto &a : args) av.push_back(a.data());
    // Seastar's own SIGTERM handling is a graceful drain that never finishes while accept() is pending;
    // the bench stops servers with kill, so leave the signals at their default action
    app_template::seastar_options opts;
    opts.auto_handle_sigint_sigterm = false;
    app_template app(std::move(opts));
    return app.run((int)av.size(), av.data(), [] {
        return smp::invoke_on_all(start_shard).then([] {
            static promise<> forever; // run until killed
            return forever.get_future();
        });
    });
}
