# em — thiết kế

Runtime concurrency bằng C cho tầng mạng của backend đa ngôn ngữ. Một I/O engine,
một bộ ngữ nghĩa (hủy, timeout, scope, buffer), binding mỏng theo idiom từng ngôn ngữ.

## Quyết định đã chốt (2026-11-08)

| # | Quyết định | Cái giá | Xét lại khi |
|---|---|---|---|
| D1 | Fiber stackful của em chỉ cho C/C++/Rust/Python. Java = virtual thread park/unpark. JS = Promise. | JS vẫn `await`, Java không dùng fiber em | Không — đây là giới hạn của JVM/V8 |
| D2 | Thread-per-core, fiber **không bao giờ** di chuyển giữa worker | Tải có thể lệch giữa core | Đo được core nóng/nguội lệch kéo dài dưới tải thật |
| D3 | Chỉ Linux. Backend chính io_uring, kernel tối thiểu 6.1 LTS (backend epoll: xem D8) | Không chạy trên mac/Windows | Có nhu cầu triển khai ngoài Linux |
| D4 | Binding đầu tiên: Rust | — | — |
| D5 | v0 chỉ TCP + UDP + timer. TLS/HTTP làm sau, dạng sans-IO cắm vào | — | — |
| D6 | Không hook syscall. Code chặn phải gọi `em_offload` | Gọi thư viện blocking mà quên offload → đứng cả worker | — |
| D7 | Stack fiber cố định (mmap + guard page), không growable | Đệ quy sâu → SIGSEGV ở guard page (rõ ràng, không hỏng bộ nhớ) | — |
| D8 | (2026-11-08) Thêm backend epoll ở M3.5, chọn tường minh: `auto` (thử io_uring, không được thì epoll, báo lại backend đang chạy) / `uring` / `epoll`. Không fallback lặng lẽ. API công khai không đổi | Hai backend, bộ test chạy gấp đôi; epoll cần timer heap riêng | em hướng tới người dùng ngoài; Docker/k8s seccomp mặc định chặn io_uring. Xét bỏ khi seccomp mặc định cho phép io_uring |

## Mô hình dữ liệu cốt lõi

**Op nằm trên stack của fiber, không cấp phát.** Khi fiber gọi `em_recv`, struct `em_op`
được khai báo trên stack của chính nó; `sqe->user_data = &op`; fiber yield. Stack đứng yên
trong lúc fiber bị treo nên con trỏ luôn hợp lệ. CQE về → `op->res = cqe->res` → fiber
vào run queue. Không malloc, không hashmap tra user_data, không slab cho op.

Bất biến bắt buộc: **một op chỉ rời khỏi frame sau khi CQE cuối cùng của nó đã về.**
Hủy = gửi `ASYNC_CANCEL` rồi *vẫn chờ* CQE gốc (thường là `-ECANCELED`). Đây là thứ làm
cho borrowed buffer an toàn với io_uring — tokio không có được vì future có thể bị drop.

**Hệ quả cho Rust:** `write(&[u8])` mượn buffer trực tiếp, không copy, không cần chuyển
ownership như monoio. `TcpStream` implement thẳng `std::io::Read + Write` → code Rust
sync có sẵn (parser, codec dùng `impl Read`) chạy được nguyên trạng.

**Buffer nhận:** `recv` dùng buffer người gọi đưa (đơn giản, v0). Provided buffer ring
(kernel tự chọn buffer, multishot recv) để sau khi benchmark chỉ ra cần — nó chỉ thắng
khi có rất nhiều connection nhàn rỗi.

## Worker

- Mỗi worker: 1 thread ghim core, 1 ring (`SINGLE_ISSUER | DEFER_TASKRUN`), 1 run queue
  FIFO intrusive (con trỏ next nằm trong fiber), 1 free-list stack fiber để tái dùng.
- Vòng lặp: chạy hết run queue → `io_uring_submit_and_wait(1)` → duyệt CQE, đánh thức fiber.
  Một syscall cho cả submit lẫn chờ.
- Gửi việc sang worker khác: `IORING_OP_MSG_RING`. Không lock, không eventfd.
- Accept: mỗi worker có listener riêng với `SO_REUSEPORT`, kernel tự chia connection.
  Không có handoff connection giữa worker.

## Fiber

- Context switch: asm riêng, chỉ lưu callee-saved (x86_64 SysV, aarch64). ~20 dòng/arch.
- Stack: mmap 256 KiB ảo + 1 guard page; RAM thật chỉ tốn cho trang đã chạm.
- Entry của fiber bắt mọi panic/exception của ngôn ngữ chủ trước khi về C
  (Rust: `catch_unwind` + `extern "C-unwind"`).

## Structured concurrency

- `em_scope`: fiber con spawn trong scope; ra khỏi scope = chờ hết con.
- Hủy lan từ scope xuống con. Mọi op chặn là điểm hủy; hủy trả lỗi `ECANCELED` lên người gọi.
- Deadline = `IORING_OP_LINK_TIMEOUT` gắn vào op. Không có timer wheel riêng. (Backend epoll
  dùng min-heap theo worker, xem plans/m3.5.md.)

## Quy tắc code (dùng khi review)

Thứ cần giữ: đường đi dữ liệu ngắn, rõ, kiểm soát được, nhanh. Hình thức (thụt lề, ngoặc)
không bàn — để formatter lo.

1. **em không copy payload.** Kernel ghi thẳng vào buffer của người gọi và đọc thẳng từ đó.
   Copy chỉ được phép trong binding, kèm lý do ở chỗ copy.
2. **Một lệnh I/O = một SQE (+ link timeout nếu có) = một CQE = một lần đánh thức.** Giữa
   CQE và fiber không có hàng đợi, callback hay lớp trung gian nào ngoài run queue.
   Bản epoll: **một lệnh I/O = một lần thử syscall; nếu `EAGAIN` thì thêm một lần đợi rồi
   một lần thử lại.** Giữa sự kiện epoll và fiber không có tầng trung gian nào ngoài danh
   sách đợi của fd và run queue.
3. **Không cấp phát, không lock, không atomic trên đường nóng** (nộp lệnh, gặt CQE, chuyển
   fiber, spawn tái dùng). Giao tiếp giữa worker chỉ qua `MSG_RING` (epoll: ghi vào eventfd của worker đích).
4. **Mỗi tài nguyên đúng một chủ:** scope sở hữu fiber, fiber sở hữu op, `em_close` sở hữu
   việc đóng fd. Không đếm tham chiếu.
5. **Trạng thái tường minh:** mọi trạng thái của fiber/op là giá trị enum, chuyển trạng thái
   ở số ít chỗ đọc được. Không có cờ ngầm suy ra trạng thái.
6. **Đường nóng đọc được từ trên xuống trong một file**, không gián tiếp qua con trỏ hàm
   (trừ entry của fiber).
7. **Tuyên bố hiệu năng phải kèm số đo.** Layout dữ liệu và số bước thì nghĩ từ lúc thiết
   kế; tối ưu vi mô thì chỉ khi benchmark chỉ ra.

## Phạm vi v0

C core (worker, fiber, scope, tcp connect/accept/recv/send/shutdown, udp sendto/recvfrom,
sleep, deadline, cancel) + crate Rust `em` + echo/ping-pong benchmark so với monoio và tokio.

**Xong khi:** echo benchmark trên cùng máy đạt ngang hoặc hơn monoio; test hủy giữa
recv/send không leak, không use-after-free (chạy dưới ASan).

## KHÔNG làm ở v0

- Offload pool / DNS — v0 nhận địa chỉ IP. Thêm ở v0.1 (cần cho người dùng thật, D6).
- Provided buffer ring, send zerocopy, SQPOLL — chỉ thêm khi benchmark chứng minh.
- Work-stealing, preemption — mâu thuẫn D2 / gây UB dưới code ngôn ngữ chủ.
- Binding Java/JS/Python, TLS, HTTP.

## Hướng cho JS (để sau, ghi lại để không quên)

"Nhẹ hơn bê cả trình duyệt vào backend": thay vì addon N-API chạy trong Node, em làm
**host** cho một engine JS nhúng (QuickJS-ng, ~1 MB). em sở hữu vòng lặp; job queue
Promise của engine được xả mỗi lượt worker; `await conn.read()` resolve thẳng từ CQE.
Mỗi worker một JSRuntime → không lock, khớp D2. Node addon vẫn là lựa chọn phụ cho ai
bắt buộc dùng Node.
