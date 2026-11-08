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
| M3.5 | Backend epoll (chọn tường minh auto/uring/epoll) | |
| M4 | Crate Rust `em` (`std::io::Read`/`Write`, không `async`) | |
| M5 | Benchmark so với monoio, tokio | |

Sau v0: TLS, HTTP, binding Java/JS/Python.

## Build và test

Cần: Linux ≥ 6.1, gcc, make, liburing-devel. Test ASan cần thêm libasan và libubsan.

```bash
make test         # -O2
make test-asan    # AddressSanitizer + UBSan
```

CI chạy cả hai lệnh trên x86_64 và arm64 (runner ARM thật của GitHub) mỗi lần push.
qemu-user không giả lập io_uring nên không dùng được để test.

Nếu `vm.max_map_count` thấp (mặc định 65530 trên Debian/Ubuntu), mỗi fiber tốn 2 vùng nhớ
nên trần là khoảng 32 nghìn fiber. Muốn nhiều hơn thì tăng sysctl này.

## Tài liệu

- [DESIGN.md](DESIGN.md): các quyết định đã chốt, cái giá và điều kiện xét lại.
- [plans/](plans/): kế hoạch chi tiết từng mốc.
