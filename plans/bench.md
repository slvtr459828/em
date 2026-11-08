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
- **Ghim CPU**: runner có 4 vCPU. Server dùng CPU 0–1 (2 worker/thread), client dùng CPU 2–3.
  Ở máy local có nhiều core hơn thì vẫn giữ 2 + 2 để số liệu đối chiếu được.
- **Server được đo** (tên trong bảng = runtime-backend): `em-uring`, `tokio-epoll`,
  `monoio-uring`, `monoio-epoll`, `compio-uring`, `compio-epoll`, `glommio-uring`. Khi M3.5 xong
  thì thêm `em-epoll`. Cùng backend so với nhau mới công bằng: em-uring so với monoio/compio/
  glommio-uring; bản epoll của chúng so với tokio.
- **Xen kẽ**: mỗi vòng chạy lần lượt mọi server cho mọi ô ma trận. Chạy `R` vòng.
  Nhiễu VM chia đều cho mọi server thay vì dồn vào một bên.
- Kết quả là **trung vị** qua các vòng, cùng tỉ lệ so với tokio. Số tuyệt đối trên VM dùng
  chung dao động; tỉ lệ trong cùng job mới là thứ để đọc.

## Thành phần

| Thành phần | Nội dung |
|---|---|
| `bench/em_echo.c` | Server echo dùng libem: `em_run(2, ...)`, mỗi worker listen `SO_REUSEPORT`, mỗi connection một fiber |
| `bench/rust/` | Workspace Cargo, commit `Cargo.lock` |
| `tokio_echo` | Runtime `multi_thread` 2 worker: cách dùng tokio phổ biến nhất |
| `monoio_echo` | 2 thread, mỗi thread một runtime + listener `SO_REUSEPORT`: cách monoio khuyên dùng. Hai bản: driver io_uring và driver legacy (epoll) |
| `compio_echo` | 2 thread, mỗi thread một runtime + `SO_REUSEPORT`. Hai bản: driver io_uring và driver polling (epoll), chọn bằng feature lúc build nên là hai binary |
| `glommio_echo` | 2 executor ghim 2 CPU + `SO_REUSEPORT`. Chỉ có io_uring |
| `client` | Một client chung cho mọi server. Tham số `addr C S D threads`, in ra một số: round-trip/giây |
| `bench/run.sh` | Build, ghim CPU bằng `taskset`, chạy ma trận × `R` vòng, ghi CSV, in bảng markdown |
| `.github/workflows/bench.yml` | Chỉ `workflow_dispatch` (input: duration, rounds), matrix x86_64 + arm64; bảng vào `$GITHUB_STEP_SUMMARY`, CSV vào artifact |

Client phải không phải nút cổ chai. `run.sh` cần báo khi CPU của client gần 100%, vì lúc đó
tỉ lệ giữa các server bị nén lại và không còn ý nghĩa.

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
