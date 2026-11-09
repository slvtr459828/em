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
| M5 | Đo sâu: `perf` ở 1024 conn × 4 KiB (em-uring, em-epoll, tokio), lặp bench đo nhiễu x86/arm, latency p99, bench churn connection, chạy lâu 10–30 phút với RSS phải phẳng | |

Sau v0: TLS, HTTP, binding Java/JS/Python.

## Build và test

Cần: Linux ≥ 6.1, gcc, make, liburing-devel. Test ASan cần thêm libasan và libubsan.

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
  cũng thấy). Đọc file thường chặn worker trong lúc đọc page cache; offload là việc của v0.1 (D6).
- Timer có độ phân giải 1 ms (không bao giờ nổ sớm).
- fd do em tạo thì đóng cách nào cũng được. fd tạo ngoài em (signalfd, pipe...) mà đã dùng
  với em thì phải đóng bằng `em_close` trên worker đang dùng nó; `close()` thẳng để lại trạng
  thái đăng ký cũ mà fd ngoài em tiếp theo cùng số sẽ thừa hưởng (lần đợi đầu không bao giờ thức).
- Một fiber luôn có dữ liệu sẵn sẽ nhường sau mỗi 128 lệnh xong ngay, để fiber khác và timer
  không bị đói.

## Tài liệu

- [DESIGN.md](DESIGN.md): các quyết định đã chốt, cái giá và điều kiện xét lại.
- [plans/](plans/): kế hoạch chi tiết từng mốc.
