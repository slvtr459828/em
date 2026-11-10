# em

Runtime concurrency viết bằng C cho tầng mạng của backend đa ngôn ngữ.
Một I/O engine dùng chung, code của người gọi trông như blocking, bên dưới là io_uring.

```c
static int echo(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    ssize_t n;
    while ((n = em_recv(fd, buf, sizeof buf)) > 0)   // trông blocking, thực ra fiber tạm dừng
        if (em_send_all(fd, buf, n) < 0) break;
    em_close(fd);
    return 0;
}
```

## Vì sao

Backend nhiều ngôn ngữ hiện phải sống chung với nhiều runtime (tokio, goroutine, Netty/Loom,
libuv, asyncio). Mỗi runtime có cách hủy, timeout, backpressure và quản lý buffer riêng.
`em` gom những thứ đó về **một I/O engine và một bộ ngữ nghĩa**, còn binding cho từng
ngôn ngữ thì mỏng và theo đúng idiom của ngôn ngữ đó:

| Ngôn ngữ | Cách viết |
|---|---|
| C, C++, Rust, Python | Fiber stackful của em, code đồng bộ, không `async` |
| Java | Virtual thread (park/unpark), code đồng bộ |
| JS/TS | `await`, Promise resolve thẳng từ io_uring |

Java và JS không dùng fiber của em vì JVM và V8 không cho đổi stack bên dưới code của chúng.
Lý do đầy đủ nằm trong [DESIGN.md](DESIGN.md).

## Nguyên tắc thiết kế

- **Chỉ Linux**, backend chính io_uring (kernel ≥ 6.1). Backend epoll (M3.5) cho môi trường
  chặn io_uring như seccomp mặc định của Docker/k8s. Chọn tường minh, không fallback lặng lẽ.
- **Mỗi core một worker, fiber không bao giờ di chuyển giữa các core.** Rust không cần
  `Send`, TLS an toàn, cache tốt.
- **Lệnh I/O nằm trên stack của fiber**, không cấp phát cho mỗi lệnh. Fiber chỉ chạy tiếp
  khi kernel đã trả kết quả của chính lệnh đó, nên buffer có thể cho kernel mượn thẳng một
  cách an toàn, kể cả khi hủy giữa chừng.
- **Structured concurrency**: mọi fiber thuộc một scope. Scope có deadline, hủy lan xuống
  fiber con, một fiber con lỗi thì anh em bị hủy (giống errgroup).

## Trạng thái

| Mốc | Nội dung | Trạng thái |
|---|---|---|
| M1 | Fiber, context switch x86_64 + aarch64, scheduler một worker | Xong |
| M2 | io_uring, TCP, scope, deadline, hủy | Xong |
| M3 | Nhiều worker, `SO_REUSEPORT`, UDP | Xong |
| M3.5 | Backend epoll (chọn tường minh auto/uring/epoll) | Xong |
| M4 | Crate Rust `em` (`std::io::Read`/`Write`, không `async`) | Xong |
| M5 | Đo sâu: syscall/round-trip và `perf`, khoảng nhiễu qua 5 vòng, latency p99, churn connection, soak 600 s với RSS phẳng | Xong |
| v0.1 | offload + DNS | Xong |
| v0.2 | TLS: `em_tls` (OpenSSL/AWS-LC qua BIO tự viết, ngoài `libem`); Rust dùng rustls trên `em::TcpStream` | Xong (CI [42615977691](https://github.com/slvtr459828/em/actions/runs/42615977691)) |

Sau v0.2: HTTP, binding Java/JS/Python.

## Build và test

Cần: Linux ≥ 6.1, gcc, make, liburing-devel. Test ASan cần thêm libasan và libubsan. TLS (`libem_tls`, test t40–t46) cần thêm OpenSSL ≥ 3.0 (`libssl-dev`/`openssl-devel`, tìm bằng `pkg-config openssl`; hoặc `make TLS_PREFIX=dir` với một prefix OpenSSL/AWS-LC tự build, xem đầu `include/em_tls.h`); thiếu thì `libem` và test của nó vẫn build, còn `make test` in dòng `SKIPPED: TLS tests ...` (không bỏ qua âm thầm).

```bash
make test         # -O2
make test-asan    # AddressSanitizer + UBSan
make test-rust    # crate Rust, cargo test trên cả hai backend
```

Mỗi lệnh chạy toàn bộ test hai lần, một lần với `EM_BACKEND=uring`, một lần với
`EM_BACKEND=epoll`. Muốn chạy một backend thôi: `make test BACKENDS=epoll`.

CI chạy cả ba lệnh trên x86_64 và arm64 (runner ARM thật của GitHub) mỗi lần push.
qemu-user không giả lập io_uring nên không dùng được để test.

Nếu `vm.max_map_count` thấp (mặc định 65530 trên Debian/Ubuntu), mỗi fiber tốn 2 vùng nhớ
nên trần là khoảng 32 nghìn fiber. Muốn nhiều hơn thì tăng sysctl này.

Trên một số kernel, bộ nhớ ring io_uring tính vào `RLIMIT_MEMLOCK` (theo UID, mặc định
8 MiB). Mỗi worker tốn khoảng 40 KiB nên 8 MiB đủ cho khoảng 200 worker. Thiếu thì `em_run`
trả `-ENOMEM`, khi đó tăng `ulimit -l`.

## Rust

Crate `rust/em` bọc API C thành kiểu an toàn: `TcpStream` implement `std::io::Read + Write`
nên code sync có sẵn chạy nguyên trạng. Không `async`. Socket là `!Send + !Sync` (fiber không
rời worker), `scope` giống `std::thread::scope` (con được mượn biến, lỗi/panic đầu tiên hủy
anh em và được trả lên).

```rust
fn main() -> std::io::Result<()> {
    em::run(0, || {                                   // chạy trên mọi worker
        let l = em::TcpListener::bind("0.0.0.0:8080".parse().unwrap())?;
        em::scope(|s| loop {
            let mut c = l.accept()?;
            s.spawn(move || {
                let mut buf = [0u8; 4096];
                loop {
                    let n = c.read(&mut buf)?;
                    if n == 0 { return Ok(()); }
                    c.write_all(&buf[..n])?;
                }
            })?;
        })
    })
}
```

Cần `cargo` và liburing-devel; `build.rs` tự biên dịch phần C. Test: `make test-rust`
(chạy cả hai backend). Gọi thư viện chặn của std trong fiber sẽ đứng cả worker, xem phần tài
liệu đầu của crate. Mọi lệnh em gọi ngoài fiber trả `-EPERM`.

## Kết quả đo (M5)

Echo ping-pong, server 1 CPU, client 3 CPU, runner GitHub 4 vCPU, 5 vòng. Tỉ lệ so với
tokio-epoll, kèm `[min–max]` theo từng vòng; ô nào khoảng chứa 1.00 thì đọc là ngang
(quy tắc trong `plans/bench.md`). Run: bench [42572531707](https://github.com/slvtr459828/em/actions/runs/42572531707),
soak [42574922764](https://github.com/slvtr459828/em/actions/runs/42574922764).

| | x86_64 | arm64 |
|---|---|---|
| em-uring, round-trip/s | 1.07–1.22x (ngang ở 1024 × 128) | 1.19–1.21x ở 16–256 conn; ngang ở 1024 |
| em-epoll, round-trip/s | 1.09–1.13x ở 16 và 1024 × 128; ngang ở 256 và 1024 × 4 KiB | 1.03–1.07x; ngang ở 1024 × 4 KiB |
| p99/p50 ở 1024 conn | em 1.0–1.2; tokio 59–67 | em 1.2–1.3; tokio 102–118 |
| churn, connection/s | em-epoll 1.18x, em-uring 0.97x | em-uring 1.51x, em-epoll 1.30x |
| soak 600 s churn (33–53 triệu connection) | RSS và số vùng map phẳng, cả uring và epoll | như x86 |

- **em-epoll** trước M5 chậm hơn tokio 1–14% khi nhiều connection. Nguyên nhân đo được: mỗi
  round-trip tốn thêm một `recv` trả `EAGAIN` (3.02 syscall so với 2.03 của tokio). Sau khi sửa
  (một `recv` ngắn trên stream thì đợi edge trước lần đọc sau), em-epoll tốn 2.03, bằng tokio.
  Profile cho thấy phần userspace của cả hai chỉ 1–4% CPU; còn lại là stack TCP của kernel.
- **Câu hỏi arm64 ở 1024 × 4 KiB:** em-uring 1.18x `[0.96–1.23]`, là ngang theo quy tắc, không thua.
- **Đuôi latency của tokio** ở 1024 conn (p99 gấp 60–120 lần p50) là của tokio: các server
  khác, gồm monoio-epoll dùng cùng client, đều ở 1.0–1.3.
- **binding Rust:** `emrs-*` nằm trong khoảng nhiễu của `em-*` ở mọi ô, kể cả churn (một
  `Box` mỗi lần spawn không hiện ra). `em_spawn_copy` vẫn hoãn.

## Backend: io_uring hay epoll

em có hai backend I/O với cùng API và cùng ngữ nghĩa (hủy, deadline, scope). Chọn bằng biến
môi trường, đọc một lần trong `em_run`:

| `EM_BACKEND` | Hành vi |
|---|---|
| `auto` (mặc định) | Dùng io_uring; nếu `io_uring_setup` bị chặn hoặc không có (`EPERM`/`ENOSYS`/`EACCES`/`ENOMEM`) thì cả process dùng epoll |
| `uring` | Ép io_uring. Không tạo được thì `em_run` trả lỗi, không fallback |
| `epoll` | Ép epoll |
| giá trị khác | `em_run` trả `-EINVAL` |

Backend được chọn **một lần lúc khởi động** và không đổi nữa. `em_backend()` trả `"uring"` hoặc
`"epoll"` để app log ra.

**Docker/k8s:** profile seccomp mặc định của Docker và containerd gần đây chặn `io_uring_setup`,
nên trong container thường thì `em_run` với io_uring trả `-EPERM`. Với `auto`, em tự chạy bằng
epoll trong trường hợp đó, không cần cấu hình gì. Muốn io_uring trong container thì phải nới
seccomp (và chấp nhận bề mặt tấn công lớn hơn của kernel); muốn chắc chắn dùng epoll thì đặt
`EM_BACKEND=epoll` trong manifest, không cần build lại. CI có một job chạy `make test` trong
container `ubuntu:24.04` với seccomp mặc định để giữ kịch bản này luôn xanh.

Khác biệt của epoll cần biết:

- Mỗi lệnh I/O thử syscall nonblocking trước, chỉ đợi khi `EAGAIN`. Socket do em tạo
  (`listen`, `accept`, `connect`, `udp_bind`) là nonblocking; `recv/send` luôn kèm `MSG_DONTWAIT`.
- `em_read` bật `O_NONBLOCK` trên fd ở lần đầu (cờ nằm trên file description nên các bản dup
  cũng thấy). Đọc file thường đi qua pool offload (xem dưới), nên không chặn worker; trên
  io_uring vẫn là `IORING_OP_READ`.
- Timer có độ phân giải 1 ms (không bao giờ nổ sớm).
- fd do em tạo thì đóng cách nào cũng được. fd tạo ngoài em (signalfd, pipe...) mà đã dùng
  với em thì phải đóng bằng `em_close` trên worker đang dùng nó; `close()` thẳng để lại trạng
  thái đăng ký cũ mà fd ngoài em tiếp theo cùng số sẽ thừa hưởng (lần đợi đầu không bao giờ thức).
- Một fiber luôn có dữ liệu sẵn sẽ nhường sau mỗi 128 lệnh xong ngay, để fiber khác và timer
  không bị đói. Cả hai backend: vòng lặp `em_yield()` thuần cũng poll không chặn mỗi 128 lần
  gọi, nên hủy từ worker khác, kết quả pool và timer vẫn tới.

## Code chặn và DNS

em không hook syscall (D6): gọi `getaddrinfo`, đọc file, hay một thư viện chặn ngay trên fiber
là đứng cả worker. Hai hàm thay thế, cùng chạy trên một pool thread chung cho cả process (tạo
khi cần, tối đa 64, sống từ `em_run` tới hết `em_run`):

- `em_offload(fn, arg, &res)` chạy `fn` trên pool (trả 0 và ghi kết quả của `fn` vào `*res`; `-EPERM`/`-EAGAIN` nghĩa là `fn` không chạy), fiber park tới khi xong, worker chạy fiber khác.
  **Không ngắt được**: `fn` có thể dùng buffer trên stack fiber nên luôn được đợi tới cùng, kể
  cả khi scope bị hủy hay quá deadline (lệnh chặn kế tiếp mới báo). Trong `fn`, mọi hàm em trả
  `-EPERM`. Rust: `em::offload(|| ...)`, mượn biến cục bộ được, panic được ném lại trên fiber.
- `em_resolve(host, port, family, out, max)` là `getaddrinfo` qua pool, **tôn trọng deadline và
  hủy**: trả về ngay, lần tra cứu chạy nốt trên pool và bị bỏ kết quả; `em_run` đợi nó xong
  trước khi trả về. Host dạng số được parse tại chỗ, không qua pool. Rust: `em::resolve`.

Trên epoll, `em_read` trên file thường cũng đi qua pool.

## TLS

**C/C++:** `include/em_tls.h` + `libem_tls.a`, dùng OpenSSL (hoặc AWS-LC) qua một BIO tự viết trên buffer của em; mọi lần chờ đi qua
`em_recv`/`em_send`. Người dùng tự cấu hình `SSL_CTX` (cert, ALPN, verify...) bằng API OpenSSL;
em không có lớp cấu hình riêng. `libem.a` không phụ thuộc OpenSSL.

```c
em_tls *t;
if (em_tls_accept(&t, ctx, fd) == 0) {            // hoặc em_tls_connect(&t, ctx, fd, "host")
    char buf[4096];
    ssize_t n;
    while ((n = em_tls_recv(t, buf, sizeof buf)) > 0)   // 0 = close_notify; cắt ngang = -ECONNRESET
        if (em_tls_send_all(t, buf, n) < 0) break;
    em_tls_shutdown(t);
    em_tls_free(t);
}
em_close(fd);
```

Đọc bị hủy lúc chờ byte thì gọi lại được (deadline đọc dùng kiểu này); hủy lúc đang gửi làm hỏng
luồng (mọi lệnh sau trả `-EPIPE`). Một kết nối cho một fiber đọc và một fiber ghi cùng lúc
(song công), fiber thứ hai cùng chiều nhận `-EBUSY`. Chi tiết ở `em_tls.h`.

**Rust:** không có `em::tls`. `rustls::StreamOwned` chạy nguyên trạng trên `em::TcpStream`
(`Read + Write`, fiber park bên trong), test ở `rust/em/tests/tls.rs`:

```rust
let tcp = em::TcpStream::connect(addr)?;
let mut tls = rustls::StreamOwned::new(rustls::ClientConnection::new(config, name)?, tcp);
tls.write_all(b"hello")?;           // dev-dependency rustls, crate em không đổi
```

**Chi phí.** Bộ nhớ (RSS của `bench/em_tls_echo` với 1000 kết nối idle sau một echo 16 KiB, đo bằng
`bench/rss_tls.sh`): **~30 KiB mỗi kết nối TLS idle trên AWS-LC, ~86 KiB trên OpenSSL** (OpenSSL giữ
thêm cặp buffer record 2 x 16 KiB của nó; `SSL_MODE_RELEASE_BUFFERS` đưa về ~53 KiB nhưng tốn 4-12% CPU
ở echo 128 B nên em_tls không bật). Kết nối idle không giữ buffer lớn: buffer đọc/ghi của em được cấp khi cần
và trả khi hết byte, lần `em_recv` đang đỗ đọc vào 512 B nằm trong `em_tls`, và stack fiber được trả
lại sau handshake. CPU: bench có thêm
em-tls-uring/em-tls-epoll/tokio-rustls ở echo 256 × 128, 256 × 4096 và churn 64 × 128, và bảng
(h) trong `bench/table.py` tính chi phí TLS = µs/round-trip của em-tls trừ của em thường cùng
ô (chạy bằng workflow `bench`, chưa có số chính thức; số kTLS dựa vào ô 4096, xem `plans/v0.2.md`).

## Tài liệu

- [DESIGN.md](DESIGN.md): các quyết định đã chốt, cái giá và điều kiện xét lại.
- [plans/](plans/): kế hoạch chi tiết từng mốc.
