# Đối thủ C/C++ trong bench

## Vì sao

Mọi đối thủ hiện có đều là runtime Rust dùng rustls. Vì vậy mỗi ô em thua đều lẫn hai biến số:
runtime và thư viện TLS. Bước 3 của plans/tls-win.md phải đếm số lệnh mới tách được hai thứ đó ra.

Một đối thủ C/C++ dùng cùng libssl với em (OpenSSL hệ thống, hoặc AWS-LC khi có `TLS_PREFIX`)
cho phép so runtime với runtime. Ngoài ra, người dùng C/C++ sẽ so em với thứ họ link được, chứ
không so với tokio.

## Đối thủ

| Tên | Ngôn ngữ | Backend | TLS |
|---|---|---|---|
| Photon (alibaba/PhotonLibOS) | C++ | io_uring, epoll | OpenSSL qua tls-stream của Photon |
| Boost.Asio + `asio::ssl` | C++ | epoll; io_uring nếu `BOOST_ASIO_HAS_IO_URING` dùng được cho socket | OpenSSL |
| libuv | C | epoll (libuv không dùng io_uring cho socket) | OpenSSL qua memory BIO, tự viết glue |
| Seastar | C++ | `--reactor-backend` io_uring, epoll | GnuTLS (Seastar chỉ có GnuTLS) |
| Seastar bản Rust | Rust | như Seastar | phải kiểm có net/TLS dùng được hay không |
| Go (net, goroutine) | Go | epoll (netpoll) | crypto/tls |

## Hợp đồng cho mỗi server (giống các server hiện có)

Hành vi phải giống các server hiện có, để chỉ còn khác nhau ở runtime và glue TLS:

- **Dòng lệnh:** `argv[1]` là port, `argv[2]` là số worker (mặc định 1). Lắng nghe trên
  127.0.0.1, backlog 4096. bench/run.sh ghim mỗi server vào 1 CPU với worker = 1.
- **Echo:** mỗi kết nối đọc tối đa 16 KiB rồi ghi lại toàn bộ, lặp đến khi gặp EOF hoặc lỗi. Lỗi
  trên một kết nối không được làm chết server. Đặt `TCP_NODELAY`, trừ khi `NODELAY=0`.
- **TLS:**
  - Chứng chỉ ECDSA P-256 tự ký, sinh lúc khởi động. Server OpenSSL dùng lại `tests/tls_util.h`.
  - Ticket: mặc định stateless, 2 ticket mỗi kết nối. Đọc biến môi trường `TLS_TICKETS` như
    `bench/em_tls_echo.c` và `bench/rust/tls_cfg`.
  - Client bench in ra một dòng `tls ...`. Dòng của server mới phải trùng với dòng của
    em-tls-uring: cùng version, suite, group và loại khoá chứng chỉ. table.py fail nếu có hơn
    một bộ tham số được thoả thuận.
  - Ở chế độ `churntlsres`, tỉ lệ resume phải ≥ 0.95.
- **Thư viện TLS:** server OpenSSL build theo `TLS_PREFIX` khi biến này được đặt (cách làm giống
  `build_tls_prefix` trong run.sh), không thì dùng libssl hệ thống. Không build được với AWS-LC thì
  ghi rõ trong báo cáo, không được âm thầm fallback.
- **Plain và TLS:** mỗi runtime có cả server echo thường (cho các ô `echo`/`churn`) và server TLS
  (cho các ô `echotls`/`churntls`/`churntlsres`).
- **Build:** mỗi runtime có `bench/cpp/<rt>/build.sh OUTDIR`. Script tải dependency ở một version
  hoặc commit cố định vào `build/$(uname -m)/cpp-deps/`, build bằng `-O2`, rồi đặt binary vào
  `OUTDIR` với tên theo bảng dưới. Script phải chạy được trên Ubuntu 24.04 x86 và arm64 (CI); các
  gói apt cần thiết liệt kê trong comment đầu file.

## Tên server trong bench

`photon-{uring,epoll}`, `photon-tls-{uring,epoll}`, `asio-{epoll,uring}`, `asio-tls-{epoll,uring}`,
`libuv-epoll`, `libuv-tls-epoll`, `seastar-{uring,epoll}`, `seastar-tls-{uring,epoll}`, và
`seastarrs-*` nếu bản Rust dùng được, `go-epoll`, `go-tls-epoll` (mã ở bench/go/).

Hậu tố `-uring`/`-epoll` quyết định đối thủ trong bảng phán định: table.py so em-X với các
server `*-X`. Đây là cách đặt tên có sẵn, giữ nguyên.

## Chọn nhóm đối thủ

Thêm khoảng 14 server thì một run tlsonly 9 vòng mất khoảng 2 tiếng. Vì vậy workflow có thêm
input `rivals`:

- `rust` (mặc định): như hiện nay;
- `cpp`: em, tokio (làm mốc cho các bảng ratio) và các đối thủ C/C++;
- `all`: tất cả.

Timeout được nâng theo.

## KHÔNG làm

- Không sửa client, các ô bench, hay luật phán định.
- `bench/table.py` chỉ được sửa ba chỗ:
  1. nhận thêm tên server mới;
  2. danh sách server "tham khảo" (`--ref`) được in ra nhưng không tham gia phán định hay kiểm tra;
  3. bảng phán định (j), vốn chỉ có cho em-tls-*, được mở rộng thêm cho em-uring và em-epoll ở các
     ô plain, cùng luật, đối thủ là các server `*-uring` / `*-epoll` cùng backend. Lý do: phép so
     runtime với runtime không qua TLS chính là thứ đối thủ C/C++ mang lại.
- Không thêm server HTTP, không đo multi-worker.
- Không tune đối thủ quá mức cấu hình "đúng cách" mà tài liệu của chính họ khuyên, và cũng không
  cố ý làm chậm họ.
- Không vendoring mã nguồn đối thủ vào repo: build.sh tải theo version cố định.
- Không dùng DPDK hay POSIX stack thay thế của Seastar: chỉ dùng stack kernel, giống mọi server khác.

## Quyết định: go-tls-epoll chỉ để tham khảo

Thư viện chuẩn Go không cho phép đạt hai điều trong hợp đồng TLS:

- **Suite:** ở TLS 1.3, Go bỏ qua `Config.CipherSuites`. Khi CPU có AES phần cứng, Go luôn chọn
  AES-128-GCM, trong khi mọi server khác thoả thuận AES-256-GCM.
- **Ticket:** Go gửi cố định 1 ticket cho mỗi kết nối. Client rustls chỉ giữ tối đa 8 ticket cho
  mỗi thread, nên ở ô churntlsres với 64 kết nối, tỉ lệ resume chỉ khoảng 0.6.

Cách xử lý:

- `go-epoll` (TCP thường) vào bench và được phán định như mọi server khác.
- `go-tls-epoll` chỉ chạy ở hai ô `echotls` và `churntls`, chỉ để tham khảo:
  - không nằm trong bảng phán định, không tham gia kiểm tra bộ tham số được thoả thuận, không
    tham gia kiểm tra resume;
  - không chạy ô `churntlsres`;
  - bảng ghi chú "AES-128, 1 ticket".

Đã cân nhắc và loại:

- ép client chỉ dùng AES-128: đổi mốc của toàn bộ bench, số liệu cũ không còn so được;
- sửa client để giữ nhiều ticket hơn: sửa client để chiều một đối thủ.

## Kết quả

Cách chạy: `RIVALS=cpp bench/run.sh` (hoặc `all`; workflow bench có input `rivals`, mặc định `rust`).
`run.sh` gọi `bench/cpp/{photon,asio,libuv,seastar}/build.sh` và `bench/go/build.sh` với `OUTDIR=build/$ARCH/bench`,
`TLS_PREFIX` (nếu đặt) đi theo. Seastar dùng GnuTLS nên `TLS_PREFIX` không áp dụng cho nó. Thêm một runtime = thêm tên vào
`NATIVE_SERVERS`/`NATIVE_TLS_SERVERS` và thư mục vào `NATIVE_BUILDS` ở đầu `bench/run.sh`. `table.py` in thêm bảng (j2) (em-uring/em-epoll ở các ô plain, cùng luật
với (j)) và liệt kê `go-tls-epoll` qua `--ref`. Ước lượng thời gian và timeout ở comment trong
`.github/workflows/bench.yml`. Chưa có số.
