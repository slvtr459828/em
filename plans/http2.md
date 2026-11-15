# HTTP — đợt 2: server HTTP/2

Đợt 1 (`plans/http.md`) đã xong: server HTTP/1.0 và 1.1, plain và TLS. Đợt này thêm HTTP/2 cho cùng
handler. Mục tiêu đặt lúc khởi động (2026-11-15): **web trước, gRPC sau**. Body stream hai chiều và
trailers để sang đợt sau, khi đã có số đo của đợt này.

## Cổng chặn

1. **Không làm thì hỏng ở đâu?**
   - Trình duyệt và CDN nói chuyện thẳng với em qua TLS sẽ bị hạ xuống HTTP/1.1. Một trang có
     nhiều tài nguyên phải mở 6 kết nối, mỗi kết nối một lần bắt tay TLS. Bắt tay chính là chỗ đắt
     nhất của em: ô TLS churn tốn khoảng 100–190 µs mỗi kết nối, trong khi một request keep-alive
     chỉ tốn vài µs.
   - Proxy phía trước (Envoy, nginx `grpc_pass`, các load balancer của cloud) muốn nói h2 với
     upstream thì không dùng em được.
   - gRPC bắt buộc h2. Đợt này chưa làm gRPC, nhưng chọn sai dạng dữ liệu bây giờ thì đợt sau
     phải viết lại.
2. **Chỗ nào đã làm gần giống?**
   - `em_http`: handler, `em_http_req`, `em_http_respond`/`em_http_read_body`, cấu hình giới hạn
     và timeout. Handler của người dùng phải chạy nguyên trạng trên h2; đây là người dùng thứ hai
     có thật của API đó, không phải người dùng giả định.
   - `em_tls`: đã chạy song công (một fiber đọc, một fiber ghi). ALPN do người dùng tự cấu hình
     trên `SSL_CTX`.
   - `em_park`/`em_unpark`: đủ để một fiber chờ fiber khác trên cùng worker. Không cần primitive mới.
   - nghttp2 (thư viện của curl, nginx cũ, h2o, Apache): framing, HPACK, flow control, trạng thái
     stream, có sẵn các biện pháp chống Rapid Reset và CONTINUATION flood. Nó không tự làm I/O:
     ta đưa byte vào (`nghttp2_session_mem_recv2`) và lấy byte ra (`nghttp2_session_mem_send2`)
     trên buffer của em.
3. **Phiên bản nhỏ nhất:**
   - `em_http_serve` tự nhận ra h2: TLS mà ALPN chọn "h2", hoặc plain mà 24 byte đầu là preface h2
     (h2c prior-knowledge).
   - Mỗi stream chạy handler hiện có trên một fiber riêng.
   - Response trọn gói như đợt 1.
   - Người dùng chỉ cần thêm "h2" vào ALPN của `SSL_CTX`. Code handler không đổi.

## Quyết định

**Code nằm trong `src/http2.c` và build thành `libem_http2.a`, tách khỏi `libem_http`.**
- `em_http_serve` tham chiếu điểm vào h2 bằng weak symbol, đúng cách đợt 1 làm với em_tls.
  Chương trình chỉ dùng h1 không phải link nghttp2.
- Khi thiếu nghttp2, ALPN không chọn được "h2". Preface h2c thì nhận 400 rồi đóng kết nối.

**nghttp2 lấy từ hệ thống**, tìm bằng `pkg-config libnghttp2`. Có thêm `NGHTTP2_PREFIX=dir`, giống
`TLS_PREFIX`.
- Bắt buộc phiên bản ≥ 1.57 (có chống Rapid Reset, CVE-2023-44487), kiểm lúc compile.
- Chống CONTINUATION flood (CVE-2024-27983/28182) có từ 1.61. Ubuntu 24.04 đóng gói 1.59.
  **Giữ sàn 1.57, không nâng lên 1.61 và không kiểm phiên bản cho lỗi này**: distro backport bản
  vá mà không đổi số phiên bản, nên số phiên bản không trả lời được câu hỏi. Câu trả lời là test
  `CONTINUATION flood` riêng (xem mục Kiểm thử), chạy trên CI Ubuntu với gói hệ thống. Bản 1.59
  của Ubuntu chưa được backport thì test đó đỏ và ta biết ngay; lúc đó mới quyết giữa nâng sàn
  hay tự chặn ở fiber đọc.
- Đã cân nhắc và loại:
  - Vendor nghttp2: khoảng 30 file C, phải theo dõi CVE thay cho distro.
  - Tự viết h2 và HPACK: sai thì thành lỗ bảo mật, đúng y lý do đợt 1 không tự viết parser.

**Mô hình fiber cho mỗi kết nối h2:**
- **Fiber đọc** (fiber của `em_http_serve`): recv, đưa byte vào nghttp2. Các callback của nghttp2
  chỉ ghi vào trạng thái của stream và unpark, không bao giờ park.
- **Fiber ghi:** park cho đến khi có việc cần ghi, rồi gọi `mem_send2` và gửi, cho đến khi
  nghttp2 không còn gì.
  - em_tls cho phép một fiber đọc và một fiber ghi chạy cùng lúc.
  - Trên plain socket, em đã cho recv và send chạy song song trên cùng fd.
- **Mỗi stream một fiber**, spawn trong scope của kết nối khi header đã đủ. Số stream đồng thời
  bị chặn bởi `SETTINGS_MAX_CONCURRENT_STREAMS` (mặc định 100).
- Cả ba fiber chạy trên cùng một worker và nhường nhau theo kiểu cooperative. Session nghttp2
  không có khoá, và không gọi nghttp2 khi đang dở một lời gọi khác.
- Đã cân nhắc và loại: chạy các handler lần lượt trên fiber đọc. Cách đó gây head-of-line
  blocking, mất lý do tồn tại của h2.

**Đường đi của dữ liệu:**
- **Nhận:** socket → buffer đọc của kết nối (hoặc em_tls) → nghttp2.
  - nghttp2 trả payload DATA bằng con trỏ vào buffer đó.
  - Ta copy một lần vào buffer của stream; `em_http_read_body` copy ra buffer của người dùng.
  - Buffer stream bị chặn bởi flow-control window: chỉ gửi WINDOW_UPDATE khi handler đã đọc. Nhờ
    vậy bộ nhớ có trần và backpressure tự có, không cần cơ chế riêng.
- **Gửi:**
  - `em_http_respond` nộp response cho nghttp2, đánh thức fiber ghi, rồi park cho đến khi body đã
    gửi xong hoặc stream bị huỷ. Ngữ nghĩa "body phải còn sống đến khi respond trả về" giữ nguyên
    như đợt 1.
  - Body lớn đi thẳng từ con trỏ của người dùng (`NGHTTP2_DATA_FLAG_NO_COPY`, frame header và
    payload trong một lần gửi). Body nhỏ đi qua buffer của `mem_send2`.

**Ánh xạ cấu hình và ngữ nghĩa** (dùng chung `em_http_cfg`, không thêm cấu trúc mới):

| `em_http_cfg` | h2 |
|---|---|
| `max_header_bytes` | `SETTINGS_MAX_HEADER_LIST_SIZE`. Vượt thì RST_STREAM (hoặc 431 nếu còn trả được) |
| `max_headers` | đếm trong callback header. Vượt thì 431 |
| `max_body_bytes` | Content-Length vượt thì 413 trước handler. Body DATA vượt thì `read_body` trả `-EMSGSIZE`, như chunked ở đợt 1 |
| `max_requests_per_conn` | tổng số stream. Đạt ngưỡng thì gửi GOAWAY, các stream đang chạy vẫn hoàn tất |
| `header_timeout_ns` | từ lúc mở kết nối đến khi xong preface và SETTINGS; với mỗi stream, từ HEADERS đầu đến END_HEADERS |
| `idle_timeout_ns` | không có stream nào đang chạy và không có frame nào trong khoảng này thì GOAWAY rồi đóng. Mỗi lần `read_body` chờ cũng tính khoảng này, hết hạn thì RST_STREAM |
| `send_timeout_ns` | áp cho từng lần send của fiber ghi, hết hạn thì đóng kết nối |

- `em_http_req` cho h2:
  - `:method` → `method`, `:path` → `path`.
  - `:authority` được đưa vào danh sách header dưới tên `host`, để handler viết cho h1 vẫn chạy
    được.
  - Thêm trường `int version` (11 hoặc 20). Trước 1.0 đổi struct công khai vẫn chấp nhận được.
- Response h2 không có `Connection` hay `Keep-Alive`. `Date` và `content-length` do em thêm, như
  đợt 1.
- HEAD giữ nguyên ngữ nghĩa.

**Trade-off đã biết:**
- Mỗi stream tốn một fiber và một buffer stream. Không còn "không cấp phát gì cho mỗi request" như
  h1: stack fiber lấy từ free list của worker, buffer lấy từ pool của kết nối.
- Mỗi DATA nhận được copy thêm một lần (vào buffer stream).
- Xét lại cả hai nếu bench cho thấy chi phí mỗi stream > 1.2x chi phí mỗi request h1
  keep-alive.

## API (bản nháp, chốt khi implement)

```c
/* em_http.h: không thêm hàm mới. em_http_serve tự chuyển sang h2 khi ALPN = "h2"
 * hoặc khi gặp preface h2c, nếu libem_http2.a đã được link. */
typedef struct em_http_req { /* ... như đợt 1 ... */ int version; /* 11 hoặc 20 */ } em_http_req;

/* em_http_cfg thêm đúng một trường: */
unsigned max_concurrent_streams; /* SETTINGS_MAX_CONCURRENT_STREAMS; mặc định 100 */
```

Người dùng TLS tự thêm "h2" vào ALPN bằng `SSL_CTX_set_alpn_select_cb`. README có sẵn một
callback 6 dòng. Không có helper trong libem_http, vì helper đó buộc libem_http phải tham chiếu
OpenSSL.

## Ngữ nghĩa phải đúng (đều có test)

- Handler h1 chạy không đổi trên h2: cùng một test, chạy qua h1, h2 qua TLS và h2c.
- Nhiều stream đồng thời: một handler chậm không chặn các stream khác.
- Flow control:
  - Client có window nhỏ thì response lớn vẫn đi hết.
  - Body request lớn hơn window thì `read_body` vẫn đọc đủ.
  - Bộ nhớ của mỗi stream không vượt window.
- RST_STREAM từ client trong lúc handler chạy:
  - `read_body` và `respond` của stream đó trả `-ECANCELED`.
  - Fiber kết thúc, không rò rỉ.
- GOAWAY:
  - Khi đạt `max_requests_per_conn` hoặc idle, stream đang chạy vẫn hoàn tất.
  - Khi peer gửi GOAWAY, serve kết thúc sau khi xong các stream.
- Các giới hạn và timeout ở bảng trên, mỗi dòng một test.
- Scope huỷ hoặc deadline của caller: mọi fiber stream đều dừng và serve trả về.
- Tấn công:
  - Rapid Reset: mở rồi huỷ stream liên tục mà số fiber và RSS vẫn có trần.
  - CONTINUATION flood: header chia nhỏ vô hạn (test riêng, xem mục Kiểm thử).
  - HPACK bomb: header nén nhỏ, giải nén ra lớn.
  - SETTINGS flood và PING flood.

## Kiểm thử

- Test `t6x` trong `make test`/`make test-asan`, trên cả hai backend, mỗi test chạy cả h2c lẫn h2
  qua TLS. Client test là nghttp2 phía client, đã có sẵn trong cùng thư viện, không cần thêm
  dependency.
- **h2spec** (bộ kiểm conformance chuẩn, binary Go, tải cố định một phiên bản trên CI): chạy
  toàn bộ, mọi mục phải pass. Mục nào bỏ qua phải ghi lý do trong plan.
- **Test CONTINUATION flood**, tách riêng, tên nói rõ CVE-2024-27983/28182:
  - Client là byte thô, không phải nghttp2 client (client đúng chuẩn không sinh được frame này):
    preface, SETTINGS, rồi HEADERS không có END_HEADERS, theo sau là 10.000 frame CONTINUATION
    nhỏ không bao giờ có END_HEADERS. Gửi ở cả h2c lẫn h2 qua TLS.
  - Pass khi: server gửi GOAWAY hoặc đóng kết nối trước khi client gửi hết; handler không bao
    giờ được gọi; RSS của server tăng dưới một trần cố định ghi trong test.
  - Không skip, không điều kiện theo phiên bản. Chạy trong `make test` trên mọi job CI, nên job
    Ubuntu kiểm đúng gói 1.59 mà người dùng Ubuntu sẽ link.
- `curl --http2` và `--http2-prior-knowledge` trong `tests/http_curl.sh`.
- Fuzz: harness libFuzzer gửi byte thô vào serve qua h2c (preface + frame). Corpus mồi lấy từ
  output của h2spec.

## Bench

- Client: **h2load** (gói `nghttp2-client`). CPU server đo như cũ (`/proc/stat` của CPU server).
- Ô: h2 qua TLS và h2c, 16 và 256 kết nối × 10 stream mỗi kết nối, GET 13 B và 4 KiB; thêm churn
  h2 qua TLS (mỗi kết nối một vài request).
- Đối thủ: nginx (`http2 on`), h2o, haproxy, hyper (h2), Go net/http (h2 qua TLS). Cấu hình TLS
  cố định như đợt 1. Luật THUA áp cho mọi đối thủ.
- Thêm một ô đối chiếu: em h1 keep-alive với cùng số request. Ô này để đo chi phí mỗi stream so
  với mỗi request h1 (điều kiện xét lại trade-off ở trên).

## Thứ tự làm

1. **M1:** build `libem_http2.a`, phát hiện ALPN và h2c, mô hình ba loại fiber, GET không body,
   response trọn gói. Có test, chạy một phần h2spec, bench GET so với nginx và h2o. Test
   CONTINUATION flood làm ngay ở M1, không đợi M2: kết quả của nó quyết định sàn phiên bản. **Dừng xem số**
   trước khi làm tiếp: nếu chi phí mỗi stream đứng sai chỗ thì sửa kiến trúc trước.
2. **M2:** body và flow control, giới hạn, timeout, GOAWAY, RST, test tấn công, h2spec đầy đủ,
   fuzz.
3. **M3:** các đối thủ còn lại, ô churn, README, doc trong `em_http.h`.

## KHÔNG làm (đợt 2)

- **Body stream và trailers.** Đây là gRPC, làm ở đợt sau khi đã có số đo của đợt này. Dạng dữ
  liệu đợt này đã chừa chỗ: mỗi stream một fiber, nên stream hai chiều chỉ là thêm hàm, không
  phải đổi mô hình.
- **Server push:** trình duyệt đã bỏ, RFC 9113 không khuyến khích.
- **h2c qua `Upgrade: h2c`:** RFC 9113 đã bỏ. Chỉ hỗ trợ prior-knowledge.
- **Priority:** cả scheme cũ của RFC 7540 lẫn RFC 9218. Gửi theo thứ tự sẵn sàng.
- HTTP/2 client, HTTP/3.
- Tự viết HPACK hay framing, vendor nghttp2.
- Helper ALPN trong libem_http.
- Rust binding cho em_http.

## Trade-off và nợ

Trade-off: xem các mục ở trên (fiber và buffer cho mỗi stream, copy DATA một lần, nghttp2 lấy từ
hệ thống).

Nợ: không có, vì chưa implement.
