# Sửa khoảng cách TLS: glue G1(+G4) và tuỳ chọn build AWS-LC

Dựa trên kết quả của `plans/tls-perf.md`: handshake chậm 1.7–2.5x là do bộ máy OpenSSL 3 (AWS-LC ngang
rustls trên cả x86 và arm), record nhỏ phần lớn là thư viện, glue ≤ 10% và chỉ ở epoll.

## Cổng chặn

1. **Không làm thì hỏng ở đâu?** Mỗi kết nối HTTPS ngắn của người dùng C tốn 1.7x (x86) / 2.5x (arm)
   CPU so với rustls, và với OpenSSL thì em không sửa được: 3.5 không đỡ. HTTP là bước kế tiếp.
2. **Chỗ nào đã có?** Vòng `run()` của `src/tls.c` giữ nguyên. Khác biệt dồn vào `flush()`, nhánh
   `WANT_READ` và `save_errors`/`restore_errors`. Cũng dùng lại `build_lib` trong `bench/tls_libs.sh`,
   `TLS_PREFIX` của `bench/run.sh`, và cache `tlslibs` của `bench.yml`.
3. **Bản nhỏ nhất:**
   - G1 (+G4 nếu số đo cho phép);
   - `#ifdef OPENSSL_IS_AWSLC` trong 3 chỗ của `tls.c` và 1 chỗ của `tests/tls_util.h`;
   - một knob `make TLS_PREFIX=`;
   - một job CI;
   - một đoạn tài liệu.

Ai được lợi: chỉ người dùng C/C++ của em_tls. Rust đã có rustls. Python dùng `ssl` của CPython (OpenSSL,
trade-off của Python). Java/JS dùng TLS của runtime.

## Phần 1: glue (G1, có thể kèm G4)

Số CI hiện có (epoll 256×128, rps): G1 +2.5–3.8%, G4 +2.4–3.4%, ALL +11% (x86) / +6.5% (arm). Tổng các
phần lẻ không ra ALL, nên có tương tác. Nhưng trên uring, G4 riêng lẻ nằm trong nhiễu, và smoke cục bộ
từng thấy G4 +44% cycles ở ô epoll 4096. **Đo trước, rồi mới đưa vào.**

1. Thêm biến thể `G14` (G1+G4) và `G124` (thêm G2) vào `bench/ablate/` + `VARIANTS` của
   `tls_ablate.sh`. Chạy CI `tlsablate` với `ROUNDS=9`. Chạy cục bộ (có PMU, đo `instructions:u`).
2. Luật chọn:
   - Lấy tập nhỏ nhất trong {G1, G14, G124} mà trung vị rps của epoll 256×128 đạt ≥ 80% mức lợi của
     ALL trên cả hai arch, và không ô nào (uring/epoll × 128/4096) có trung vị xấu hơn base quá 2%.
   - Không tập nào đạt: chỉ G1.
   - G3 (read_ahead) không đưa vào em_tls: đó là cấu hình `SSL_CTX` của người dùng, và nằm trong nhiễu.
3. Áp vào `src/tls.c`. Phải qua `make test`, `make test-asan` (t40–t46), và `em_tls_dbg_partial == 0`.
4. Dọn: xoá `bench/ablate/`, `bench/tls_ablate.sh` và input `tlsablate`. Kết quả đã ghi trong plan; các
   patch sẽ không còn áp được lên `tls.c` mới, giữ lại chỉ thành code mục.

## Phần 2: tuỳ chọn build AWS-LC

### Code

- **`flush()`**: `BIO_read` từ `t->net` vào buffer trên stack, rồi gửi hết bằng vòng `em_send` (dữ liệu
  đã rời BIO thì không trả lại được). Lỗi giữa chừng thì `broken = 1`, như hiện tại. Thứ tự byte giữ
  nguyên vì chỉ một fiber flush tại một thời điểm.
- **`WANT_READ`**: `room = BIO_ctrl_get_write_guarantee(t->net)`, `em_recv` vào buffer stack tối đa
  `room` byte, rồi `BIO_write` hết. Chỉ fiber đọc ghi vào chiều này, nên luôn vừa.
- **Buffer**: một mảng `IN_BYTES` (~18 KiB) đặt trong `run()`, dùng chung cho flush và recv (hai việc
  này không lồng nhau trong cùng một fiber). Không dùng buffer chung cho cả worker: io_uring đọc/ghi
  buffer trong lúc fiber đang park.
- **Lỗi**:
  - lưu bằng `ERR_get_error_line_data(&file, &line, &data, &flags)`;
  - khôi phục bằng `ERR_put_error(lib, 0, reason, file, line)` rồi `ERR_add_error_data(1, data)` khi
    `flags & ERR_FLAG_STRING`.
- **Đầu `tls.c`**: sửa chú thích "no copy" cho đúng với cả hai đường (R5).
- **`tests/tls_util.h`**: thay `EVP_EC_gen` bằng `EC_KEY_new_by_curve_name` + `EC_KEY_generate_key` +
  `EVP_PKEY_assign_EC_KEY` dưới `#ifdef`. Nhánh OpenSSL giữ nguyên, tránh cảnh báo deprecated
  (`-Werror`).
- **`bench/em_tls_echo.c`**: chế độ `TLS_TICKETS=stateful` trên AWS-LC thì báo không hỗ trợ và thoát 2.

### Build và CI

- **`Makefile`**: `TLS_PREFIX=dir` thay pkg-config:
  `TLS_CF=-I$(TLS_PREFIX)/include`,
  `TLS_LIBS=-L$(TLS_PREFIX)/lib -Wl,-rpath,$(TLS_PREFIX)/lib -lssl -lcrypto`.
  Dùng chung cho mọi thư viện có API OpenSSL (OpenSSL 3.5 tự build cũng được). Thư viện được nhận
  diện trong code bằng `OPENSSL_IS_AWSLC`, không thêm biến make thứ hai.
- **`bench/tls_libs.sh`**: `BUILD_ONLY=1 LIBS=aws-lc-5.12.0` chỉ build prefix, rồi thoát.
- **`bench/run.sh`**: bỏ dòng từ chối AWS-LC trong `build_tls_prefix`.
- **`ci.yml`**: job `awslc` chạy ma trận x86 + arm:
  - cache `build/*/tlslibs` (cùng key với bench);
  - `make test` và `make test-asan` với `TLS_PREFIX`.
- **`bench.yml`**: input `awslc` build prefix rồi export `TLS_PREFIX` cho script được chọn
  (`run.sh` / `tls_ab.sh` / `profile.sh`).

### Tài liệu

Trong `include/em_tls.h` và README:
- cách build;
- `SSL_CTX` của người dùng phải tạo từ cùng thư viện;
- khác biệt hành vi:
  - TLS 1.3 không có ticket stateful;
  - server gửi ticket cùng lần ghi đầu tiên;
  - không renegotiation (em vốn đã tắt).
- **Trùng symbol**: AWS-LC trùng tên hàm với OpenSSL. Một process vừa link em_tls+AWS-LC vừa kéo
  OpenSSL hệ thống qua thư viện khác (libcurl, libpq…) thì một bên sẽ gọi nhầm bên kia. Cách tránh:
  build AWS-LC với `BORINGSSL_PREFIX`. Tài liệu nói rõ điều này; plan không làm hay kiểm chế độ này.

### Tiêu chí xong

- `make test` + `make test-asan` xanh trên cả hai thư viện, cả hai arch (CI).
- CI `tlsab` với `awslc=true`: churn em-tls-uring trên AWS-LC ≤ 1.15x CPU/kết nối của tokio-rustls, trên
  cả x86 và arm (microbench dự đoán 0.9–1.03x). Bench mặc định với `awslc=true`: ô echo TLS 128 và 4096
  không chậm hơn build OpenSSL của cùng lần chạy (chi phí copy).
- Build mặc định (pkg-config, OpenSSL) không đổi hành vi. Diff của nhánh OpenSSL ngoài `#ifdef` = 0
  (trừ G1/G4 và chú thích).

## Red team

| Rủi ro | Xử lý |
|---|---|
| Copy làm chậm ô 4096 | memcpy 4 KiB ≪ 1.7 µs/record. Tiêu chí bench ở trên bắt được. |
| Buffer stack 18 KiB làm tăng RSS (trang đã chạm thì giữ cùng fiber) | ~18 KiB mỗi fiber dùng TLS, chỉ ở build AWS-LC. Ghi trade-off. Đặt buffer theo kết nối cũng tốn bằng vậy. |
| Hành vi khác làm test đỏ (thời điểm trả lời KeyUpdate ở t46, mã lỗi verify ở t42) | Hợp đồng của `em_tls.h` phải giữ. Chỉ sửa test khi test mã hoá chi tiết riêng của OpenSSL, mỗi chỗ ghi lý do. Không bao giờ nới kiểm `em_tls_dbg_partial == 0`. |
| REC_WORST sai trên AWS-LC (cách chia record khác, ví dụ split 1/n-1 của CBC TLS 1.0) | Kiểm bộ cipher/phiên bản mặc định của AWS-LC. t40 đếm `em_tls_dbg_partial`. Nếu vượt: tăng `REC_OVH`/`CTL_ROOM` có lý do, không bỏ invariant. |
| Nợ "reader flush" trong `run()` thành ra dễ chạm hơn (AWS-LC xếp hàng câu trả lời KeyUpdate khác) | Implementer đọc lại kịch bản trong chú thích DEBT với AWS-LC và nêu kết quả trong báo cáo. Không sửa nợ đó trong plan này. |
| AWS-LC giữ ticket tới lần ghi kế tiếp: server chỉ đọc thì không phát ticket | Ghi vào tài liệu. Với echo/HTTP, câu trả lời mang ticket đi luôn (còn bớt được bẫy Nagle). |
| CI build AWS-LC chậm (tarball 147 MB, cmake + go) | Cache theo arch + hash script. Cache miss chỉ khi đổi phiên bản. |

## Thứ tự và giao việc

1. Phần 1 bước 1 (main thread): tạo patch G14/G124, push, chạy CI `tlsablate` + đo cục bộ.
2. Song song, một `implementer` làm Phần 2 trên worktree: code + test util + Makefile + tls_libs + CI +
   tài liệu. Xong thì gộp vào main, push, chạy `ci` và `bench` với `awslc=true`.
3. Khi có số Phần 1: áp G1(+G4) vào `tls.c` sau khi Phần 2 đã gộp (tránh conflict trong `run()`), rồi
   dọn ablation.

## KHÔNG làm

- **Custom BIO thay bio pair:** cho được một đường code không copy cho cả hai thư viện, nhưng phải viết
  lại lõi dữ liệu và giả lập `BIO_ctrl_pending` mà invariant REC_WORST dựa vào. Xét lại nếu profile
  AWS-LC cho thấy memcpy đáng kể.
- **AWS-LC làm mặc định, vendor hay submodule AWS-LC:** OpenSSL vẫn là mặc định; người dùng tự cung cấp
  prefix.
- **Build và kiểm `BORINGSSL_PREFIX` trên CI:** chỉ tài liệu hoá.
- **BoringSSL gốc:** chưa kiểm, không hứa hỗ trợ (dù `#ifdef` có thể đủ).
- **Đặt read_ahead (G3), số ticket hay chế độ ticket trong em_tls:** đó là cấu hình `SSL_CTX` của
  người dùng.
- **G2 riêng lẻ** (nằm trong nhiễu), trừ khi `G124` thắng theo luật ở Phần 1.
- **Đổi binding Python/Rust;** kTLS; rustls-ffi.
- **Sửa nợ "reader flush"** trong `run()`.

## Trade-off và nợ

- **Trade-off: build AWS-LC copy một lần mỗi chiều mỗi record.** Giá: một memcpy ≤ 16 KiB mỗi record và
  ~18 KiB stack đã chạm mỗi fiber TLS. Xét lại khi memcpy hiện trong profile AWS-LC, hoặc khi bỏ bio pair.
- **Trade-off: hai thư viện có hai đường I/O.** Đường AWS-LC chỉ được kiểm bởi job CI `awslc`. Xét lại
  nếu job đó bị bỏ.
- **Trade-off: trùng symbol khi trộn với OpenSSL hệ thống.** Là giới hạn đã tài liệu hoá của tuỳ chọn,
  cách tránh là `BORINGSSL_PREFIX`.
- **Nợ:** không có nợ mới. Nợ "reader flush" có sẵn được giữ nguyên, kèm kiểm tra trong red team.

## Kết quả Phần 1 (CI 42656222023, 9 vòng; cục bộ có PMU)

rps epoll 256×128 so với base (x86 / arm): G1 1.031 / 1.048, G14 1.078 / 1.022, G124 1.054 / 1.077,
ALL 1.072 / 1.044. Không tập nào đạt ≥ 80% mức lợi của ALL trên cả hai arch, nên chỉ lấy G1. Min-max của CI
±10–15%, rps không phân giải được vài phần trăm. `instructions:u` cục bộ trên ô 128: G1 -13%,
G14 -17–20%, G124 -20%, ALL -21%. G1 không đổi hành vi (hàng lỗi rỗng thì không có gì để xoá). Đã áp vào
`tls.c`; công cụ ablation đã xoá. Xét lại G4 nếu có máy đo rps ổn định hơn nhiễu 5%.

## Kết quả Phần 2 (AWS-LC 5.12.0; tlsab 42660662000, bench 42660671263, profile 42668959769)

CPU server µs mỗi kết nối, em / tokio-rustls cùng run (bench 42660671263, 5 vòng):

| | x86 uring | x86 epoll | arm uring | arm epoll |
|---|---|---|---|---|
| churntls (OpenSSL trước đó) | 1.76x | | 2.64x | |
| churntls AWS-LC | 1.06x | 1.02x | 1.19x | 1.00x |
| churntlsres AWS-LC | 1.06x | 1.02x | 1.17x | 0.99x |
| echo 256×128 (OpenSSL r2 → AWS-LC) | 1.09 → 1.01x | 1.30 → 1.17x | 1.21 → 0.97x | 1.82 → 1.15x |
| echo 256×4096 (OpenSSL r2 → AWS-LC) | 1.07 → 0.94x | 1.31 → 1.12x | 0.95 → 0.93x | 1.30 → 1.08x |

- Tiêu chí ≤ 1.15x: đạt trên x86 và với epoll trên arm. Uring trên arm trượt (1.17–1.19x).
- Echo không chậm hơn bản OpenSSL ở ô nào: copy một lần mỗi chiều không lộ ra.
- NODELAY=0 không còn bẫy Nagle (ticket đi chung lần ghi đầu): p50 em 17 ms x86 / 12 ms arm, tokio ~41 ms.
- Phần dư uring trên arm không nằm ở TLS hay glue. PMU arm, churntls mỗi kết nối:
  - uring / epoll `instructions:u` 841k / 848k và `instructions:k` 91.5k / 89.0k, gần bằng nhau;
  - `cycles:k` 166k / 115k (+45%), `cycles:u` 413k / 385k (+7%), `branch-misses:u` +46%.
  - Cùng lượng việc nhưng tốn thêm chu kỳ. Điểm nóng kernel: `_raw_spin_unlock_irqrestore` từ
    `sock_def_readable` (loopback rx softirq chạy trong ngữ cảnh gửi), 5.1% so với 3.5%.
  - Churn không TLS trên arm thì uring rẻ hơn epoll (12.3 so với 13.7 µs), nên đây là tương tác
    uring–kernel khi mỗi kết nối nặng CPU, không phải đường TLS.
  - Không xử lý trong plan này. Xét lại nếu uring churn trên máy arm thật (không phải VM) vẫn > 1.1x
    so với epoll.
