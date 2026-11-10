# Bench — echo em vs tokio, monoio, compio, glommio trên GitHub Actions

Mục tiêu: một lệnh chạy được ở máy local lẫn trên runner GitHub, ra bảng so sánh thông lượng
echo của em với tokio, monoio, compio và glommio. Đây là thước đo cho mọi tuyên bố hiệu năng sau này (quy tắc 7)
và cho các trade-off còn treo (kích thước ring, ~11 ns/switch).

## Vì sao cần, vì sao làm bây giờ

- Chưa có số đo nào. M3.5 (epoll) và M4 (crate Rust) cần một mốc để biết mình làm chậm đi hay không.
- Chạy local mất lâu và chiếm máy. GitHub cho runner x86_64 và arm64 miễn phí.

## Mô hình đo

- **Closed-loop ping-pong**: mỗi connection gửi một message `S` byte, đợi nhận đủ `S` byte
  echo, rồi gửi tiếp. Đo số round-trip/giây trong `D` giây. Đây cũng là kiểu monoio dùng để
  benchmark, nên dễ đối chiếu.
- Ma trận: số connection `C ∈ {16, 256, 1024}` × payload `S ∈ {128, 4096}` byte.
- **Ghim CPU**: runner có 4 vCPU. Server dùng đúng 1 CPU (CPU 0, 1 worker/thread/executor), client dùng
  3 CPU còn lại (CPU 1–3, 3 thread). Ở máy local có nhiều core hơn thì vẫn giữ 1 + 3 để số liệu đối chiếu được.
  Vì sao: lần đo đầu với 2 + 2 làm client bão hòa (CPU client >= 90% ở ~35/42 lần chạy) nên mọi tỉ lệ bị nén
  về ~1.0x. Cho server 1 CPU đo hiệu quả trên mỗi core, đúng mô hình thread-per-core của em, và 3 CPU client
  đủ để server là bên bão hòa.
- **Chỉ số thứ hai: server CPU-µs mỗi round-trip.** `run.sh` đọc thời gian bận của CPU server
  (dòng `cpu0` trong `/proc/stat`: user+nice+system+irq+softirq+steal) ngay trước và sau lần chạy client,
  rồi tính `cpu_giây * 1e6 / tổng_round-trip` (client in tổng round-trip). Không dùng utime+stime của tiến
  trình: trên loopback, stack TCP chạy trong softirq mà kế toán theo tick không tính cho task (~40% CPU 0),
  nên server bão hòa chỉ hiện ~0.55. CPU 0 chỉ có server chạy nên thời gian bận của nó là chi phí của server
  (gồm cả phần stack TCP). Thấp hơn là tốt hơn. Chỉ số này vẫn có
  nghĩa khi server chưa bão hòa, còn rps thì không. CSV có thêm cột `server_cpu` (tỉ lệ CPU duy nhất của
  server) và `us_per_rt`.
- **Server được đo** (tên trong bảng = runtime-backend): `em-uring`, `tokio-epoll`,
  `monoio-uring`, `monoio-epoll`, `compio-uring`, `compio-epoll`, `glommio-uring`. Khi M3.5 xong
  thì thêm `em-epoll`. Cùng backend so với nhau mới công bằng: em-uring so với monoio/compio/
  glommio-uring; bản epoll của chúng so với tokio.
- **Server TLS** (`TLS_SERVERS` trong `bench/run.sh`; plans/tls-win.md): `em-tls-uring`, `em-tls-epoll`,
  `tokio-rustls` (multi_thread, 1 worker), `tokio-rustls-ct` (current_thread), `monoio-rustls-uring`,
  `monoio-rustls-epoll`, `compio-rustls-uring`, `compio-rustls-epoll`, `glommio-rustls-uring`. Mọi server
  rustls dùng chung `bench/rust/tls_cfg` (aws-lc-rs, ticket stateless, 2 ticket, buffer echo 16 KiB). Bảng
  "ô thua" (`table.py`, mục (j)) **không** dùng quy tắc `[lo–hi]` chồng nhau: tỉ lệ em/đối thủ theo từng
  vòng; THUA = trung vị xấu hơn 5% và mọi vòng cùng phía xấu, THẮNG đối xứng, còn lại NGANG; cần ROUNDS >= 9.
  Bảng bộ nhớ (`bench/rss_tls.sh`, mục (i)) có quy tắc riêng (độ dốc 500 → 1000 kết nối, 3 lần lặp).
  `ONLY=tls` chỉ chạy các ô TLS.
- **Xen kẽ**: mỗi vòng chạy lần lượt mọi server cho mọi ô ma trận. Chạy `R` vòng.
  Nhiễu VM chia đều cho mọi server thay vì dồn vào một bên.
- Kết quả là **trung vị** qua các vòng, cùng tỉ lệ so với tokio. Số tuyệt đối trên VM dùng
  chung dao động; tỉ lệ trong cùng job mới là thứ để đọc.
- **Khoảng nhiễu (M5):** mặc định 5 vòng. Cạnh trung vị, bảng in `[lo–hi]`: nhỏ nhất và lớn nhất
  của tỉ lệ theo từng vòng (vòng r của server chia vòng r của tokio, hai lần chạy cách nhau vài
  phút). **Quy tắc đọc:** hai server chỉ được coi là khác nhau ở một ô khi khoảng `[lo–hi]` của
  chúng không chồng lên nhau. Chồng nhau thì đọc là "ngang".

## Thành phần

| Thành phần | Nội dung |
|---|---|
| `bench/em_echo.c` | Server echo dùng libem: `em_run(n, ...)`, mỗi worker listen `SO_REUSEPORT`, mỗi connection một fiber |
| `bench/rust/` | Workspace Cargo, commit `Cargo.lock` |
| `tokio_echo` | Runtime `multi_thread` `n` worker: cách dùng tokio phổ biến nhất |
| `monoio_echo` | `n` thread, mỗi thread một runtime + listener `SO_REUSEPORT`: cách monoio khuyên dùng. Hai bản: driver io_uring và driver legacy (epoll) |
| `compio_echo` | `n` thread, mỗi thread một runtime + `SO_REUSEPORT`. Hai bản: driver io_uring và driver polling (epoll), chọn bằng feature lúc build nên là hai binary |
| `glommio_echo` | `n` executor ghim vào `n` CPU đầu của tập CPU được phép (đọc `Cpus_allowed_list`, vì `taskset` đã giới hạn) + `SO_REUSEPORT`. Chỉ có io_uring |
| `client` | Một client chung cho mọi server. Tham số `addr C S D threads`, in ra `tổng_round-trip round-trip/giây` |
| `bench/run.sh` | Build, ghim CPU bằng `taskset`, chạy ma trận × `R` vòng, ghi CSV, in hai bảng markdown (rps; server µs/round-trip) |
| `.github/workflows/bench.yml` | Chỉ `workflow_dispatch` (input: duration, rounds), matrix x86_64 + arm64; bảng vào `$GITHUB_STEP_SUMMARY`, CSV vào artifact |

Server nhận số worker `n` ở argv sau port (mặc định 1); `run.sh` luôn chạy với 1.

Client phải không phải nút cổ chai. `table.py` báo khi CPU của client (trên 3 CPU) >= 90%, vì lúc đó
tỉ lệ giữa các server bị nén lại và không còn ý nghĩa. Nó cũng ghi chú khi CPU server < 90%: server chưa
bão hòa nên bảng rps không có nghĩa ở ô đó (bảng µs/round-trip vẫn đọc được). Với `DURATION` ngắn (1 s),
thời gian dựng connection chiếm phần đáng kể nên server khó đạt ~100%: dùng >= 4 s để đọc số.

## Xong khi

1. `bench/run.sh` chạy được ở máy local với `DURATION=1 ROUNDS=1`, ra bảng đủ 7 server × 6 ô.
2. Workflow chạy xanh trên GitHub cả x86_64 và arm64; bảng hiện trong job summary.
3. Echo server nào cũng kiểm byte trả về đúng (ít nhất ở client: số byte nhận khớp số byte gửi).

## KHÔNG làm

- Không sửa `src/` của em. Bench chỉ đo; tối ưu là việc riêng, làm sau khi có số.
- Không làm crate Rust của em (đó là M4). Server em viết bằng C.
- Không đo latency histogram (p99), không bench HTTP, không thêm smol/may/Go. Thêm khi có câu hỏi cụ thể cần trả lời.
- Không chạy bench khi push: chỉ chạy tay, vì tốn phút runner và số liệu nhiễu.
- Không lưu lịch sử kết quả, không làm dashboard. Artifact CSV là đủ.
