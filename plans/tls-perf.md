# TLS chậm hơn tokio-rustls: đi tìm nguyên nhân bằng số đo

Chưa sửa gì. Mục tiêu của plan này là chia khoảng cách giữa em_tls và tokio-rustls thành các phần
đo được (thư viện / glue của ta / tương tác syscall / cấu hình bench), rồi mới quyết định sửa ở đâu.

## Cổng chặn

1. **Không làm thì hỏng ở đâu?** Echo TLS 0.55–0.92x tokio-rustls, churn TLS 0.39–0.54x
   (bench 42628143821). Bước sau v0.2 là HTTP, đứng trên TLS: handshake tốn 1.8–2.5x CPU là
   lỗi người dùng thấy được ở mọi dịch vụ HTTPS nhiều kết nối ngắn.
2. **Chỗ nào đã làm gần giống?** `bench/run.sh`, `bench/profile.sh`, bảng (h) của
   `bench/table.py`. Mở rộng các chỗ đó; công cụ mới chỉ có ở chỗ chưa có gì (microbench thư viện).
3. **Phiên bản nhỏ nhất:** đủ số để gán mỗi µs chênh vào một trong các nhóm dưới, trên cả x86 và
   arm. Không sửa `src/tls.c` trong plan này.

## Số đã có (x86, profile 42629178535, ô echo 256 × 128, µs mỗi round-trip)

| | user | libcrypto | libssl | kernel |
|---|---|---|---|---|
| em-tls-uring | 2.28 | 1.14 | 0.59 | 6.6 |
| em-tls-epoll | 3.29 | 1.56 | 0.80 | 9.0 |
| tokio-rustls | 1.30 | (ring) | | 8.6 |

Arm: em-tls-uring user 3.27, epoll 3.82, tokio-rustls 1.27. Cả ba server thương lượng giống nhau:
TLS 1.3, AES-256-GCM, X25519, ecdsa-p256.

## Đọc lại ba nguyên nhân ban đầu

- **A. Tương tác TLS × syscall.** Cùng số lệnh `SSL_*` mà libcrypto trên epoll tốn hơn uring 37%.
  Bảng (h): chi phí TLS epoll 7.7 / uring 5.1 µs (x86), 8.2 / 4.2 (arm), trong khi em thường
  uring và epoll chỉ chênh ~1 µs. "uring bù trừ bằng kernel" bỏ sót số hạng này. Giả thuyết:
  syscall làm bẩn branch predictor/cache (máy dev có `BHI: SW loop`), dispatch gián tiếp của
  provider OpenSSL 3 chịu thiệt hơn code đơn hình của ring.
- **B. "Không nằm ở AES-GCM" chưa có bằng chứng.** Phần lớn mẫu libcrypto/libssl là địa chỉ hex
  (runner không có debug symbol); asm AES-GCM/GHASH là symbol local, nằm trong đó.
- **C. AWS-LC không link thẳng được.** Thiếu `BIO_nread0/nwrite0/nread/nwrite`,
  `ERR_get_error_all`, `ERR_new`/`ERR_set_error`, `EVP_EC_gen`. Port glue = đổi hai biến một lúc.
- **D. Churn so hai cấu hình ticket khác nhau.** rustls 0.23.46 mặc định: ticket stateful
  (ID + cache 256 mục), không mã hóa. OpenSSL mặc định: stateless (serialize + AES + HMAC, 2 cái
  mỗi kết nối). Resume TLS 1.3 vẫn làm X25519, OpenSSL còn giải mã ticket và phát ticket mới, nên
  "resume chỉ giảm 18% ⇒ không phải chữ ký" không suy ra được. Arm chênh 2.5x so với x86 1.8x
  gợi ý tốc độ primitive khác nhau theo kiến trúc.
- **E. Nagle không phải khoảng cách em–tokio.** `tokio_rustls_echo.rs` cũng phải bật NODELAY. Đây
  là đặc tính TLS 1.3 trên TCP; em_tls có thể làm tốt hơn tokio (gửi ticket chung lần ghi đầu,
  bớt luôn một send và một segment mỗi kết nối).

## Giả thuyết và phép đo phân biệt

| ID | Giả thuyết | Của ai | Phép đo |
|---|---|---|---|
| R1 | OpenSSL 3.0 chạy nhiều lệnh hơn mỗi record (provider, OSSL_PARAM, init IV) | thư viện | microbench trong bộ nhớ, `instructions:u`/record: 3.0, 3.5, AWS-LC, rustls |
| R2 | Glue: `ERR_clear_error` 3 lần/lượt, ~8 `BIO_ctrl`/lượt, `SSL_read` thử trước khi recv | em | ablation từng thay đổi, `instructions:u`/lượt |
| R3 | Không read_ahead: 2 `BIO_read`/record | cấu hình | `SSL_CTX_set_read_ahead(1)` |
| R4 | Syscall làm giảm IPC (A) | kiến trúc + thư viện | `perf stat` instructions/cycles/branch-misses mỗi lượt; microbench chèn `getppid()` giữa record |
| R5 | BIO pair thêm một copy mỗi chiều (chú thích đầu `tls.c` nói "no copy" là chưa đúng) | em | `memmove` trong profile có symbol, ô 4096 |
| H1 | Ticket lệch cấu hình (D) | bench | cùng chế độ ticket hai phía |
| H2 | Bộ máy handshake OpenSSL 3.0 (fetch, encoder/decoder keyshare, SSL_new/free) | thư viện | profile churn có symbol + `-g`, 3.0 vs 3.5 |
| H3 | Primitive X25519 / ECDSA P-256 chậm hơn ring, nhất là arm | thư viện | `openssl speed` / `bssl speed` hai arch |
| H4 | Số send/segment mỗi handshake (ticket flush riêng) | em | syscall mỗi kết nối, ô churn |
| N1 | Nagle với ticket | TLS 1.3 | tắt NODELAY hai server, p50 churn nhảy lên ~40 ms ở cả hai |

## Luật đo

- So sánh chỉ trong cùng một máy, cùng một lần chạy. Số cục bộ không bao giờ so với số CI.
- Đổi phiên bản OpenSSL 3.x bằng `LD_LIBRARY_PATH` trên cùng một binary (chung soname
  `libssl.so.3`); binary build với header của bản cũ nhất trong lần so.
- Máy dev (i7-10510U, turbo, SMT) nhiễu: thước đo chính là `instructions:u`, thời gian là phụ,
  chạy xen kẽ, luôn kèm tokio-rustls làm mốc. Runner CI là VM không có PMU: chỉ có thời gian và
  cpu-clock, nên arm (chỗ chênh lớn nhất) phải kiểm bằng thời gian.
- Không chạy hai bench cùng lúc trên một máy.

## Bước

0. **Thước đo.** profile.sh có symbol (debuginfod) và call graph (`-g`, frame pointer có sẵn trên
   Ubuntu 24.04 và Fedora); ô churn TLS; pass đếm `instructions:u,cycles:u,branch-misses:u` khi
   có PMU; chạy được cục bộ không root (bỏ pass tracepoint, record theo `-p`).
1. **H1 + N1 tại chỗ.** `TLS_TICKETS` (mặc định / 0 / stateless / stateful) và `NODELAY=0` cho cả
   em_tls_echo và tokio_rustls_echo; script chạy churntls/churntlsres theo từng chế độ.
2. **Microbench thư viện (R1, R4, H2, H3).** Server và client trong một process qua BIO pair, chỉ
   API chung (chạy được với OpenSSL 3.0, 3.5, AWS-LC); bản rustls-ring tương đương. Chỉ đo phía
   server: ns và `instructions:u` mỗi record 128 B / 4096 B, mỗi handshake đầy đủ, mỗi resume;
   biến thể chèn `getppid()`; cùng cert, suite, group, chế độ ticket. Kèm `openssl speed` /
   `bssl speed`.
3. **R4 tại chỗ.** Pass counters của bước 0 cho em-tls-uring, em-tls-epoll, tokio-rustls.
   Đúng nếu: lệnh/lượt uring ≈ epoll nhưng cycles epoll cao hơn.
4. **Ablation glue (R2, R3).** Mỗi thay đổi một patch trên bản sao của `tls.c` /
   `em_tls_echo.c`, build biến thể riêng, đo `instructions:u` và thời gian. G1 `ERR_clear_error`
   chỉ khi `ERR_peek_error()`; G2 bỏ `BIO_ctrl_pending` đường ghi; G3 read_ahead; G4 bỏ `SSL_read`
   thử khi chưa có byte vào; ALL. Mỗi patch phải qua t40–t46.
5. **Churn theo phiên bản (H2, H4).** Profile churn có symbol, em_tls_echo trên 3.0 và 3.5.

## Quyết định sau khi có số (theo từng phần, không phải "AWS-LC hay tls.c")

- Glue: sửa `tls.c` nếu ablation lợi rõ hơn nhiễu.
- Thư viện, 3.5 thu hẹp phần lớn: không code; ghi số, khuyến nghị OpenSSL mới, xét CI chạy bench
  trên bản mới (runner ubuntu-26.04 nếu có).
- Thư viện, chỉ AWS-LC thu hẹp: mới xét tuỳ chọn build. Giá: port glue (mất zero-copy của bio
  pair), một ô CI, người dùng dựng `SSL_CTX` bằng AWS-LC.
- R4 là phần lớn: thư viện không chữa hết; ghi như trade-off của backend epoll.
- Nagle: ứng viên sửa là gửi ticket chung lần ghi đầu trong em_tls, sau khi N1 xác nhận.

## Thực thi

Công cụ đo (microbench, script A/B, patch ablation, input mới của workflow `bench`) được commit
lên main để chạy trên CI cả x86 và arm. Chúng là công cụ đo, không đổi `libem`/`libem_tls`.
Ba nhánh làm song song (bước 0+1+3, bước 2, bước 4), gộp tuần tự vào main; đo thật chạy tuần tự
sau khi gộp.

## KHÔNG làm

- Port `tls.c` sang AWS-LC trước khi microbench cho thấy đáng.
- kTLS, custom BIO thay bio pair, rustls-ffi: là hướng sửa, chưa phải hướng đo.
- Tối ưu ô 4096 / bỏ copy của bio pair (R5) trước khi profile có symbol cho thấy `memmove` đáng kể.
- rustls với provider aws-lc-rs trong microbench (mốc ring là thứ tokio-rustls của bench dùng).

## Trade-off và nợ

- Trade-off: x86 cục bộ đo chính xác (PMU), arm chỉ có thời gian. Xét lại nếu hai arch vênh nhau.
- Nợ: không có nợ mới (plan không đổi code thư viện).
