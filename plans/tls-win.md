# em_tls không thua runtime nào: reader không đứng trong `send` (Bước 0) và dựng thước đo (Bước 1)

Tiếp `plans/tls-fix.md`. Trên AWS-LC em_tls đã ngang tokio-rustls, nhưng chưa có số nào so với monoio,
compio, glommio, và chưa đo bộ nhớ hay đuôi latency của đối thủ.

**Mục tiêu cuối (qua nhiều plan):** trên bản AWS-LC, em_tls không thua ô nào so với tokio, monoio, compio,
glommio cùng backend, trên x86 và arm (ưu tiên ngang nhau), ở các trục: CPU server mỗi round-trip/kết nối
(kéo theo rps khi server bão hoà), đuôi latency (p99/p50), bộ nhớ mỗi kết nối TLS idle. Thắng rõ nơi có
thể; churn chấp nhận "ngang". AWS-LC vẫn là build tuỳ chọn; OpenSSL vẫn mặc định.

**Plan này chỉ gồm Bước 0 và Bước 1.** Bước 2 (sửa các ô thua) lập plan sau khi Bước 1 có số.

## Cổng chặn

1. **Không làm thì hỏng ở đâu?**
   - Bước 0: khi peer gây lỗi giao thức trong lúc không đọc, `em_tls_recv` đứng trong `em_send` (để gửi
     alert) đến deadline thay vì trả lỗi ngay; không có deadline thì fiber kẹt đến khi peer đóng. Kết nối
     lúc đó đã chết, nên đây là lỗi "trả lỗi muộn / giữ fiber", không phải treo dưới tải bình thường.
     Chú thích DEBT ở `src/tls.c` và đoạn "Known limit" ở `include/em_tls.h` mô tả một kịch bản
     (KeyUpdate hai chiều) không xảy ra được: tài liệu sai cũng là thứ phải sửa.
   - Bước 1: không có gì hỏng, nhưng mọi tuyên bố "không thua" hiện không có số. Bench TLS chỉ có tokio,
     và tokio đang ghim provider `ring` chứ không phải mặc định của rustls.
2. **Chỗ nào đã có?** `flush()`/`run()` của `src/tls.c`; `tests/t41`, `t46`; `bench/run.sh`
   (`TLS_SERVERS`, `start()`), `bench/table.py` (bảng e–h), `bench/rss_tls.sh`, `bench/profile.sh`,
   `bench/rust/tokio_rustls_echo`, `.github/workflows/bench.yml`. Mở rộng các chỗ này, không sinh script
   hay workflow mới.
3. **Bản nhỏ nhất:**
   - Bước 0: reader gửi byte của nó bằng một lần `send` không chặn; xoá trạng thái chờ flusher; sửa chú
     thích và tài liệu cho đúng.
   - Bước 1: ba crate server TLS, một crate cấu hình chung, thêm tên vào `TLS_SERVERS`, chế độ chỉ chạy
     ô TLS, một bảng "ô thua", một bảng bộ nhớ.

## Bước 0: reader không bao giờ đứng trong `send`

### Sự thật đã kiểm (red team)

- Không thư viện nào phát câu trả lời KeyUpdate từ trong `SSL_read`: câu trả lời đi ra ở lần ghi kế tiếp,
  mỗi bên giữ tối đa một câu trả lời chờ (RFC 8446 §4.6.3). Đã chạy thử trên OpenSSL 3.5.9; OpenSSL
  3.0.13 và AWS-LC mới **đọc nguồn, chưa chạy**.
- Thứ duy nhất `SSL_read` tự phát ra là **alert**: alert fatal (lỗi giao thức), và trên TLS 1.2 có thể là
  alert cảnh báo `no_renegotiation`.
- Vậy đường "reader thành flusher" trong `run()` chỉ chạy khi gửi alert.

### Cách sửa

- Reader có byte chờ trong BIO, `!t->flushing` và `!t->broken`: gọi thẳng
  `send(fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL)`, một lần, không park.
  - `t->flushing`: writer đang có `em_send` dở, reader không đụng (như hiện tại).
  - `t->broken`: BIO có thể còn record dở của một writer bị huỷ; reader không gửi gì.
- **OpenSSL:** gửi được bao nhiêu thì `BIO_nread` bấy nhiêu. `EAGAIN` hoặc gửi thiếu: phần còn lại nằm
  yên trong BIO, lần flush kế tiếp của writer hoặc `em_tls_shutdown` gửi tiếp. Thứ tự byte giữ nguyên vì
  mọi thứ đi qua một BIO.
- **AWS-LC:** byte đã `BIO_read` ra thì không trả lại được. Reader đọc ra buffer trên stack, gửi không
  chặn; gửi không hết thì `broken = 1` (bỏ phần dư). Với alert fatal việc này vô hại vì kết nối đã chết.
  Với record không fatal thì cắt kết nối là cách duy nhất không làm hỏng luồng byte. **Không thêm stash.**
- **Lỗi của `send` không được làm mất dữ liệu đã giải mã:** nếu `SSL_read` đã trả `n > 0` thì trả `n`,
  bất kể `send` của reader ra sao. Lỗi thật của `send` (không phải `EAGAIN`) không tự đặt `broken` trên
  đường OpenSSL: writer kế tiếp sẽ gặp lại lỗi đó từ kernel.
- **`SSL_read` trả `WANT_WRITE`** (BIO ra đầy): `broken = 1`, trả `-EIO`, không chờ. Implementer đọc
  nguồn để ghi đúng vào chú thích nhánh này: không với tới trên TLS 1.3; trên TLS 1.2 có với tới được
  bằng cách dồn yêu cầu renegotiation trong lúc không đọc hay không. Không ghi gì về KeyUpdate vào hợp
  đồng `em_tls.h`.
- Reader không còn bao giờ là flusher chặn, nên writer không còn phải chờ ai: xoá trường `waiter` và
  vòng park trong `flush()`. Còn lại một cờ `flushing`. Tham số `wait` của `flush()` bỏ được nếu mọi
  nơi gọi còn lại đều truyền 1.
- Vì sao gọi `send` trực tiếp thay vì thêm API vào libem: chỉ có một người dùng, trên đường lỗi. Trên
  uring đây là syscall đồng bộ ngoài ring; không có `em_send` nào của kết nối này đang bay vì đã kiểm
  `!t->flushing`.

### Phương án đã loại

| Phương án | Lý do loại |
|---|---|
| Chỉ sửa chú thích, giữ hành vi | Vẫn còn `em_tls_recv` trả lỗi muộn đến deadline; và không xoá được `waiter`. |
| Stash 512 byte cho AWS-LC | Thiết kế cho câu trả lời KeyUpdate từ `SSL_read`, thứ không tồn tại. |
| `WANT_WRITE` trong READ → `-EPROTO` và ghi vào hợp đồng | Lý do cũ (dồn ~18 KeyUpdate) sai; ghi vào hợp đồng là mô tả hành vi không có. |
| Fiber flusher riêng cho mỗi kết nối | Thêm vòng đời fiber trong `em_tls` cho một đường lỗi. |
| Thêm `em_send` không chặn vào API libem | Một người dùng; API công khai mới cho một đường hiếm. |
| Hook `em_tls_dbg_rpend` | Ca KeyUpdate của test tự nó là máy dò (xem dưới); không cần hook. |

### Test

Sửa `tests/t46_tls_flushwait.c` tại chỗ và đổi tên (`git mv`) thành `tests/t46_tls_recv_noblock.c`: tiền
đề cũ (writer chờ sau reader-flusher) không còn, nhưng cách dựng của nó dùng lại được nguyên. Viết các
khẳng định mới TRƯỚC khi sửa `tls.c`.

- Cách dựng giữ như t46: `SO_SNDBUF`/`SO_RCVBUF` 16 KiB đặt trước handshake; làm đầy đường gửi của server
  bằng byte rác thô cho đến khi kẹt; client không đọc. Không dùng `SIOCOUTQ`, không hạ `SO_SNDBUF` sau.
- **Ca alert (phải đỏ trên code hiện tại):** đường gửi của server đầy, writer của server rảnh, client gửi
  một record rác. `em_tls_recv` với deadline 2 s phải trả `-EPROTO` trong thời gian ngắn hơn hẳn deadline
  (ngưỡng do implementer chọn, có biên rộng cho CI chậm), không phải `-ETIMEDOUT`.
- **Ca KeyUpdate (máy dò tiền đề, dự kiến xanh cả trước và sau):** đường gửi của server đầy, client gọi
  `SSL_key_update(REQUESTED)` rồi gửi 64 KiB, vẫn không đọc. `em_tls_recv` của server nhận đủ 64 KiB
  trong deadline. Sau đó client đọc hết, server ghi, client giải mã đúng.
  - Nếu ca này **đỏ trước khi sửa** ở cấu hình CI nào (ứng viên: Ubuntu OpenSSL 3.0.13) thì tiền đề "không
    thư viện nào phát từ `SSL_read`" sai ở cấu hình đó. Trên đường OpenSSL cách sửa vẫn đúng (phần dư ở
    lại BIO). Trên AWS-LC thì **dừng và báo lại**: quyết định "không stash" phải xét lại.
- **Ca reset (giữ từ t46):** writer của server đang kẹt trong `em_send`, client reset; writer phải thoát
  với lỗi, không treo.
- `tests/t41_tls_duplex.c`: logic giữ nguyên, phải xanh. Sửa chú thích đầu file cho khớp sự thật (pass 3
  không đi qua đường reader gửi).

### Phạm vi file

`src/tls.c`, `include/em_tls.h` (bỏ đoạn "Known limit"; sửa đoạn Cancellation; thêm một câu: alert do
`em_tls_recv` phát là best-effort, có thể không tới peer khi peer không đọc), `tests/t46_*` (đổi tên),
`tests/t41_tls_duplex.c` (chỉ chú thích), `tests/tls_util.h` nếu cần, `Makefile` (tên test), `README.md`
nếu có nhắc tên t46 hoặc giới hạn này.

### Xong khi

- Ca alert đỏ trên code hiện tại và xanh sau sửa, trên OpenSSL và AWS-LC, uring và epoll. Báo cáo ghi kết
  quả ca KeyUpdate **trước khi sửa** theo từng cấu hình CI (đây là lần chạy thật đầu tiên trên 3.0.13).
- Chú thích DEBT và đoạn "Known limit" đã xoá; `waiter` đã xoá; `tls.c` không dài hơn trước.
- `make test`, `make test-asan` xanh; job `awslc` xanh; `em_tls_dbg_partial == 0` ở t40.
- Hiệu năng: đường nóng không đổi (chỉ đổi nhánh "reader có byte chờ"). Kiểm bằng run của Bước 1, không
  chạy bench riêng.

## Bước 1: thước đo

Không sửa `src/`, `include/`, `tests/` trong bước này.

### 1.1 Server TLS

| Server (tên trong bảng) | Runtime | Tích hợp TLS (red team đã xác nhận qua manifest; chưa ai build) |
|---|---|---|
| `tokio-rustls` | tokio `multi_thread`, 1 worker (đã có) | tokio-rustls |
| `tokio-rustls-ct` | tokio `current_thread` (mới, cùng crate, chọn bằng biến môi trường) | tokio-rustls |
| `monoio-rustls-uring`, `monoio-rustls-epoll` | monoio đang ghim | monoio-rustls 0.4.0 |
| `compio-rustls-uring`, `compio-rustls-epoll` | compio đang ghim | compio feature `rustls` (compio-tls 0.10.0); không bật feature `ring` |
| `glommio-rustls-uring` | glommio đang ghim | futures-rustls 0.26.0 trên `TcpStream` của glommio |

- Cùng dạng `tokio_rustls_echo`: cert tự ký lúc khởi động, `NODELAY` mỗi kết nối, 1 worker, echo.
- Crate chung `bench/rust/tls_cfg` dựng `ServerConfig` cho mọi server: cert, provider, ticket. Buffer echo
  của mọi server Rust cùng một dạng cấp phát, 16 KiB.
- `rcgen`: `default-features = false, features = ["crypto", "pem", "aws_lc_rs"]`, để không kéo `ring` vào
  lại. Sau build, `cargo tree -i ring` trên các server echo phải rỗng.
- **Tên server và `start()`:** `case monoio-*` trong `bench/run.sh` (và mẫu tương tự trong
  `bench/profile.sh`, `bench/rss_tls.sh`) khớp nhầm `monoio-rustls-*`. Đổi các mẫu glob của server không
  TLS thành tên chính xác; rà mọi `case` trong ba script.
- `glommio_rustls_echo` vào đúng nhóm build có thử lại `CFLAGS` như `glommio_echo`; thủ thuật `CFLAGS` đó
  không được lọt vào build của `aws-lc-sys`.
- Tích hợp nào không build được với phiên bản runtime đang ghim: **dừng và báo lại**, không tự viết
  adapter, không nâng phiên bản runtime, không lặng lẽ bỏ server.

### 1.2 Mật mã và ticket: cố định cho mọi server

- Mọi server rustls dùng provider `aws-lc-rs`, ghim `aws-lc-sys = "=0.47.0"` (bundle AWS-LC 5.12.0, cùng
  bản với em). `tokio_rustls_echo` đổi từ `ring` sang. Hệ quả: tỉ lệ so với tokio-rustls của các run
  trước không còn nối tiếp được; ghi câu này vào phần kết quả.
- **Ticket:** mọi server rustls dùng `Ticketer` stateless và phát 2 ticket, khớp chế độ duy nhất em có
  trên AWS-LC. Mặc định của rustls là stateful; để lệch thì CPU churn lệch khoảng 27% chỉ vì chế độ.
- Client bench giữ nguyên (`ring`): bộ tham số thương lượng không đổi.
- `tls_lib_rs` (microbench thư viện) giữ `ring`: không thuộc thước đo này.

### 1.3 `bench/run.sh`, `bench/table.py`, `bench.yml`

- `run.sh`: biến `ONLY=tls` chỉ chạy các ô TLS. Thứ tự server **xoay vòng theo số vòng** (vòng r bắt đầu
  từ server thứ r), để không server nào luôn chạy ngay sau cùng một server khác.
- `bench.yml`: thêm input boolean `tlsonly` (đặt `ONLY=tls`). Ước lượng: 9 server × 4 ô × 9 vòng × ~11 s
  ≈ 59 phút, cộng build. Nếu run đầu vượt 75 phút thì nâng `timeout-minutes` của job; không giảm số vòng.
- `table.py`:
  - đọc thêm `p999_us`, in ở bảng latency và bảng churn (chỉ báo cáo);
  - bảng mới **"ô thua"**, chỉ có nghĩa với `ROUNDS >= 9` (ít hơn thì in cảnh báo thay cho phán quyết).

**Quy tắc phán (duy nhất, cho mọi trục dưới đây):** so `em-tls-uring` với từng server `*-uring`;
`em-tls-epoll` với `tokio-rustls`, `tokio-rustls-ct` và từng server `*-epoll`. Tỉ lệ tính theo từng vòng
(vòng r của em chia vòng r của đối thủ).
- **THUA:** trung vị tỉ lệ xấu hơn 5% **và** mọi vòng đều ở phía xấu hơn.
- **THẮNG:** đối xứng (tốt hơn 5% và mọi vòng cùng phía).
- **NGANG:** còn lại.
- Ghi thêm một dòng vào `plans/bench.md` (mục TLS): bảng "ô thua" dùng quy tắc này; quy tắc `[lo–hi]`
  chồng nhau của các bảng khác giữ nguyên.

**Trục được phán:**

| Trục | Số đo | Ghi chú |
|---|---|---|
| CPU server | µs mỗi round-trip (echo), µs mỗi kết nối (churn) | Trục chính. Khi server bão hoà (CPU ≥ 95%) thì rps là nghịch đảo của nó: một phán quyết cho cả hai. |
| rps / kết nối mỗi giây | chỉ báo cáo | Ở ô server chưa bão hoà, rps do client giới hạn: in số, ghi "không phán". |
| Đuôi latency | p99/p50 | Cùng quy tắc. p999 chỉ in, không phán. |
| Bộ nhớ | KiB mỗi kết nối idle (1.4) | Quy tắc riêng ở 1.4. |

### 1.4 Bộ nhớ mỗi kết nối idle

- `bench/rss_tls.sh` khởi động server bằng chính `start()` của `run.sh` (cùng binary, cùng
  `LD_LIBRARY_PATH`; hiện nó ghim cứng binary bản OpenSSL nên đang đo sai bản), cho mọi tên trong
  `TLS_SERVERS`, gồm cả hai backend của em.
- Số đo là **độ dốc**: (RSS ở 1000 kết nối − RSS ở 500 kết nối) / 500. Mỗi kết nối: handshake xong, echo
  một lần **16 KiB** (để buffer ứng dụng của mọi server đều đã được chạm), rồi để idle. Lặp 3 lần, lấy
  trung vị và khoảng.
- **Phán:** THUA khi em lớn hơn đối thủ trên 5% và khoảng 3 lần đo của hai bên không chồng nhau.
- Đo đúng server echo như đã viết, gồm buffer ứng dụng: đó là chi phí người dùng thật sẽ trả.
- `run.sh` gọi nó sau các ô TLS và đưa bảng vào summary.

### 1.5 Kiểm "cùng việc"

`bench/profile.sh` với các server mới: số syscall mỗi round-trip và mỗi kết nối. Dùng để phát hiện server
nào làm khác việc (thiếu `NODELAY`, flush thừa, ghi tách đôi), không dùng để phán thắng thua. Phán quyết
chỉ lấy từ µs/round-trip trên CI. Số lệnh PMU đo cục bộ không so với CI.

### Phạm vi file

`bench/rust/` (crate mới, `Cargo.toml`, `Cargo.lock`), `bench/run.sh`, `bench/table.py`,
`bench/rss_tls.sh`, `bench/profile.sh`, `.github/workflows/bench.yml`, `plans/bench.md`, và phần kết quả
của plan này.

### Xong khi

- `DURATION=1 ROUNDS=1 ONLY=tls bench/run.sh` chạy hết cục bộ: `start()` hoạt động với mọi tên trong
  `TLS_SERVERS`.
- Một run `bench` với `awslc=true`, `tlsonly=true`, 9 vòng, trên x86 và arm, trong đó:
  - mọi tên trong `TLS_SERVERS` có đủ hàng cho cả 4 ô TLS (script tự kiểm, thiếu thì fail);
  - `tls_info.txt` cho mọi server ra **một** bộ (phiên bản, suite, group, loại cert), script tự so và
    fail nếu lệch;
  - ô churn có resume: tỉ lệ resume ≥ 0.95 ở mọi server (script tự kiểm);
  - số syscall mỗi round-trip của các server cùng backend khớp nhau, hoặc chỗ lệch được giải thích;
  - không có WARNING client ≥ 90% ở ô nào được phán; ô có thì ghi "không phán".
- Bảng "ô thua", bảng bộ nhớ, và danh sách ô THUA kèm số run và quy trách nhiệm sơ bộ (em / thư viện /
  kernel / chưa rõ) được ghi vào mục "Kết quả".
- `make test` và CI thường không đổi.

## Giao việc

Hai task độc lập, file không giao nhau, giao song song cho hai `implementer`, mỗi task một nhánh:

| Task | Nội dung | Xác minh |
|---|---|---|
| T0 | Bước 0 | Các điều kiện "Xong khi" của Bước 0; CI `ci` xanh gồm `awslc`. |
| T1 | Bước 1 | Các điều kiện "Xong khi" của Bước 1. |

Mỗi spec giao xuống chép nguyên phần của nó trong plan này, kèm mục KHÔNG làm bên dưới. T0 không sửa
`bench/`; T1 không sửa `src/`, `include/`, `tests/`. Không task nào merge vào `main`: main thread đọc
kết quả rồi mới merge.

## KHÔNG làm

- Mọi sửa hiệu năng (thuộc Bước 2, chỉ là ứng viên cho plan sau): `SSL_MODE_RELEASE_BUFFERS`, buffer BIO
  pair cấp lười, custom BIO thay BIO pair, đo lại G4/G2, `TCP_NODELAY` lúc listen, chuyển fiber và
  branch-miss.
- Đuổi thắng trên bản OpenSSL; đo bản OpenSSL của em trong bảng "ô thua"; AWS-LC làm mặc định; vendor
  AWS-LC; bật `BORINGSSL_PREFIX` trong CI.
- kTLS, rustls-ffi.
- Chạy đối thủ trên cả `ring` lẫn `aws-lc-rs`; thêm `tokio current_thread` vào bench không TLS.
- Nâng phiên bản monoio, compio, glommio, tokio; viết adapter TLS cho runtime thiếu tích hợp.
- Đổi `tls_lib_rs` và bảng microbench của `bench/tls_libs.sh`.
- Fiber flusher, API gửi không chặn trong libem, stash cho AWS-LC, hook debug mới trong `tls.c`.
- Ghi giới hạn KeyUpdate vào hợp đồng `em_tls.h`.
- Phán theo p999, theo rps ở ô chưa bão hoà, hay theo số lệnh PMU.
- Workflow mới; đo arm trên máy thật (chưa có máy).

## Luật dừng cho các bước sau

Ô nào profile quy cho thư viện hoặc kernel thì ghi "không thắng được, lý do" và đóng. Hai ô đã biết trước:
- record 128 byte của AWS-LC: chậm 1.09x (x86) / 1.24x (arm) so với rustls-ring ngay trong microbench
  (sau Bước 1 đối thủ cũng chạy AWS-LC, nên ô này cần đọc lại);
- churn uring trên arm: spinlock loopback của kernel (`plans/tls-fix.md`, Phần 2).

## Trade-off

- **Reader gửi bằng `send` trực tiếp, ngoài libem.** Giá: `tls.c` có một lời gọi socket không qua em, và
  một syscall đồng bộ trên đường lỗi của uring. Xét lại khi có người dùng thứ hai cần gửi không chặn,
  hoặc khi bỏ BIO pair.
- **Alert từ `em_tls_recv` là best-effort.** Giá: peer không đọc thì có thể không bao giờ nhận alert, chỉ
  thấy kết nối đóng. Xét lại nếu có giao thức phía trên cần alert tới nơi.
- **AWS-LC: reader gửi thiếu thì cắt kết nối.** Giá: nếu có record không fatal phát từ `SSL_read` (đọc
  nguồn nói là không có trên TLS 1.3) mà buffer gửi đầy, kết nối bị ngắt thay vì chờ. Xét lại nếu ca
  KeyUpdate của t46 đỏ trên AWS-LC.
- **Đối thủ đo trên aws-lc-rs với ticket stateless.** Giá: mất nối tiếp với các run cũ; không phải cấu
  hình mặc định của rustls ở phần ticket; build bench lâu hơn.
- **Ngưỡng 5% và "mọi vòng cùng phía".** Giá: chênh lệch thật dưới 5% đọc là NGANG. Xét lại nếu nhiễu
  giữa các vòng trên CI nhỏ hơn hẳn 5%.
- **Bộ nhớ đo trên server echo nguyên trạng**, gồm buffer ứng dụng đã chạm. Giá: con số không tách riêng
  phần của thư viện TLS. Xét lại nếu Bước 2 cần phân rã.
- **Hai hàng tokio.** Giá: thêm một server (khoảng 6 phút mỗi run); tuyên bố "không thua tokio" phải đúng
  với cả hai.

## Nợ

- Bước 0 xoá nợ "reader flush" (bản đúng của nó: `em_tls_recv` trả lỗi muộn). Không có nợ mới.
- Bước 1: không có nợ (không đổi code thư viện).
- Điều chưa kiểm, không phải nợ: hành vi OpenSSL 3.0.13 và AWS-LC mới được đọc nguồn. Ca KeyUpdate của
  t46 là lần chạy thật; kết quả ghi vào báo cáo T0.

## Red Team Review

Hai reviewer độc lập, 13 phát hiện, chấp nhận 13 (một phần ở B1-H3 do người dùng quyết).

| # | Mức | Phát hiện | Xử lý |
|---|---|---|---|
| B0-C1 | Critical | Không thư viện nào phát câu trả lời KeyUpdate từ `SSL_read`; kịch bản treo hai đầu không tồn tại; reader chỉ gửi alert | Bước 0 thu nhỏ; cổng chặn viết lại |
| B0-H2 | High | `WANT_WRITE` trong READ gần như không với tới; ghi KeyUpdate vào hợp đồng là sai | `-EIO` + `broken`, không đổi hợp đồng |
| B0-H3 | High | Stash 512 byte cho AWS-LC thiết kế cho thứ không tồn tại | Bỏ stash; gửi thiếu thì `broken` |
| B0-H4 | High | Lỗi `send` của reader không được vứt `n` byte đã giải mã; reader không gửi khi `broken` | Đưa vào "Cách sửa" |
| B0-M5 | Medium | Dùng lại cách dựng của t46 thay vì `SIOCOUTQ` | Sửa t46 tại chỗ |
| B0-M6 | Medium | Giữ ca reset; ghi alert là best-effort | Đưa vào test và `em_tls.h` |
| B1-C1 | Critical | Quy tắc THUA mâu thuẫn `plans/bench.md`, sinh ô thua giả; rps và µs/rt là một phép đo; p999 không phân giải | Một quy tắc (5% + mọi vòng), 9 vòng, xoay thứ tự, p99/p50 |
| B1-C2 | Critical | Tên `monoio-rustls-*` khớp glob `monoio-*`; `rcgen` kéo `ring`; glommio cần nhóm `CFLAGS` | 1.1 |
| B1-H1 | High | Run vượt timeout 90 phút | `ONLY=tls`, input `tlsonly` |
| B1-H2 | High | Phép đo bộ nhớ sai bản, không nóng, buffer lệch | 1.4: độ dốc, echo 16 KiB, dùng `start()` |
| B1-H3 | High | Chế độ ticket lệch; tokio `multi_thread` 1 worker; ghim `aws-lc-sys`; resume ≥ 0.95 | 1.2; đo cả hai cấu hình tokio |
| B1-M1 | Medium | PMU không làm trọng tài được | 1.5: chỉ kiểm "cùng việc" bằng syscall |
| B1-M2 | Medium | "Xong khi" quá lỏng; có lối thoát "bỏ server" | Kiểm tự động; thiếu tích hợp thì dừng và báo |

Đứng vững sau red team: thứ tự byte qua một BIO; `flushing` che `em_send` đang bay trên uring; xử lý
edge của epoll; xoá `waiter` an toàn; ba tích hợp TLS tồn tại và khớp phiên bản đang ghim.

## Validation Log

Phỏng vấn người dùng sau red team (2026-11-10). Không câu trả lời nào đòi đổi plan thêm.

| Câu hỏi | Quyết định |
|---|---|
| Bước 0 còn lại gì sau khi tiền đề KeyUpdate sai? | Bản thu nhỏ: `send` không chặn, xoá `waiter`, sửa chú thích và tài liệu; không stash, không đổi hợp đồng; `WANT_WRITE` trong READ trả `-EIO`. |
| Áp dụng phát hiện Bước 1? | Tất cả. |
| Cấu hình tokio? | Đo cả `multi_thread` (`tokio-rustls`) và `current_thread` (`tokio-rustls-ct`); "không thua" phải đúng với cả hai. |
| Phán đuôi latency? | Theo p99/p50; p999 chỉ báo cáo. |
| Cách đo bộ nhớ? | Echo 16 KiB rồi idle, độ dốc 500→1000 kết nối. |
| Tích hợp TLS không build được? | Dừng và báo lại; người dùng quyết từng trường hợp. |
| Bước tiếp? | Commit plan (không push), giao T0 và T1 song song, hai nhánh riêng; merge do main thread quyết sau. |

## Kết quả

**Bước 0** (CI 42754655469, xanh mọi job gồm `awslc` x86/arm): t46 trên OpenSSL 3.0.13 và AWS-LC: alert →
`-EPROTO` sau 400 ms (deadline 2 s); KeyUpdate: server nhận đủ 64 KiB rồi ghi được (tiền đề "không thư viện
nào phát từ `SSL_read`" đúng thật trên cả 3.0.13 và AWS-LC); reset: reader `-EPROTO`, writer `-ECONNRESET`.

**Bước 1** (bench 42756701491: `awslc`, `tlsonly`, 9 vòng; mọi kiểm tự động qua; resume ≥ 99.6%).
Tỉ lệ so với tokio-rustls của các run trước không nối tiếp được: tokio-rustls đã đổi từ `ring` sang aws-lc-rs.

Ô THUA (em / đối thủ cùng backend, CPU server; trung vị [min–max]):

| Ô | x86 | arm | Quy trách nhiệm sơ bộ |
|---|---|---|---|
| Bộ nhớ idle: em 82.6 KiB/kết nối | 3.2x tokio/glommio, 1.4–1.6x monoio/compio | như x86 | em: buffer BIO pair, buffer SSL giữ khi idle, stack fiber đã chạm (`buf[16384]` của echo, `IN_BYTES` của AWS-LC) |
| echo 128, epoll vs tokio, tokio-ct | 1.09, 1.11 | 1.13, 1.15 | em: glue BIO pair trên epoll |
| echo 4096, epoll vs tokio, tokio-ct | 1.05, 1.07 | NGANG | em: như trên |
| echo 128, uring vs monoio | 1.08 | 1.09 | em: chưa rõ (monoio rẻ hơn ở record nhỏ) |
| churn, epoll vs tokio-ct | 1.06 (đầy đủ) | 1.09 / 1.06 | em: chi phí mỗi kết nối (accept, `epoll_ctl`, `setsockopt`, fiber) |
| churn, uring vs glommio | NGANG | 1.24 / 1.19 | uring trên arm: monoio-uring 1.24x và compio-uring 1.16x tokio cũng chịu, glommio thì không. Không chỉ là kernel: glommio làm khác gì đó |
| churn, uring vs compio | NGANG | 1.06 (đầy đủ) | như trên |
| p99/p50 churn, epoll vs tokio-ct, monoio, compio | 1.21–1.27 | 1.18–1.29 | em: độ công bằng lập lịch trên epoll khi churn |

THẮNG: echo uring vs compio và glommio (0.77–0.89) trên cả hai arch; echo epoll vs compio-epoll
(0.74–0.84); churn có resume uring vs monoio, compio trên x86 (0.94–0.95). Còn lại NGANG.

