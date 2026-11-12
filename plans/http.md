# HTTP — đợt 1: server HTTP/1.0 và HTTP/1.1

Các đợt sau (chưa nằm trong plan này): đợt 2 là HTTP/2 (nghttp2 + ALPN); đợt 3 là HTTP/3, chỉ
làm nếu em nhắm tới edge/CDN, và sẽ quyết sau khi có số của đợt 1 và 2.

## Cổng chặn

1. **Không làm thì hỏng ở đâu?** Người dùng C/C++ chỉ có TCP và TLS. Muốn phục vụ HTTP, họ phải
   tự ghép parser, keep-alive, chunked, giới hạn, timeout. Phần dễ sai nhất lại là phần bảo mật:
   request smuggling khi gặp cùng lúc Content-Length và Transfer-Encoding, slowloris, header quá
   lớn. HTTP cũng là workload mà người ta thực sự so runtime với nhau; bench echo không thuyết
   phục được ai đổi runtime.
2. **Chỗ nào đã làm gần giống?** Không có gì trong em. Ở ngoài:
   - picohttpparser (h2o) chỉ parse request line, header và chunked; không có I/O, không có
     state.
   - Phần còn lại (vòng keep-alive, body, giới hạn, response) phải viết, nhưng chỉ dùng API công
     khai của em và em_tls.
3. **Phiên bản nhỏ nhất:** một hàm phục vụ một kết nối (plain hoặc TLS). Hàm đó parse request,
   gọi handler của người dùng, ghi response, rồi lặp keep-alive. Handler là code đồng bộ chạy
   trên fiber, đúng thế mạnh của em.

## Quyết định

**`libem_http.a` nằm trong repo nhưng tách khỏi `libem`**, giống em_tls.
- Gồm `src/http.c` và `include/em_http.h`.
- Không có OpenSSL thì vẫn build được bản chỉ có plain. Phần TLS chỉ được bật khi em_tls có mặt.

**picohttpparser được vendor tại `third_party/picohttpparser/`**, ghim một commit, kèm LICENSE
(MIT). Nó gồm 2 file và được thiết kế để copy vào dự án; h2o, Fluent Bit và các dự án khác đều làm
vậy. Lý do chọn nó:
- Không cấp phát bộ nhớ và không callback: parse thẳng trên buffer em sở hữu.
- Header trả về dưới dạng con trỏ vào buffer đó, không copy.
- Có đường SSE4.2 và AVX2.

Đã cân nhắc và loại:
- llhttp: dùng callback, nặng hơn, phải sinh mã bằng TypeScript.
- Tự viết parser: vô ích, và sai thì thành lỗ bảo mật.

**Transport là plain hoặc em_tls, một con trỏ `em_tls *` có thể NULL.** Hai người dùng thật có từ
ngày đầu (http và https), nhưng chỉ khác nhau ở hai lời gọi recv/send, nên chỉ cần một nhánh `if`,
không cần vtable.

**Buffer.** Mỗi kết nối có một buffer đọc, mặc định 8 KiB và tối đa bằng `max_header_bytes`.
Request line và header nằm trọn trong đó; struct request trỏ vào buffer này nên chỉ sống trong
lúc handler chạy.
- Response được gom vào một buffer ghi (header + body nhỏ) rồi gửi bằng một lần send, chấp nhận
  copy một lần. Body lớn thì gửi thẳng từ con trỏ của người dùng, không copy.
- Pipelining: phần byte còn thừa sau một request được giữ lại cho vòng sau.
- Không cấp phát gì cho mỗi request.

## API (bản nháp, sẽ chốt khi implement)

```c
typedef struct em_http_req {
    const char *method, *path;  size_t method_len, path_len;
    int minor_version;                       /* 0 = HTTP/1.0, 1 = HTTP/1.1 */
    const struct em_http_header *headers;    /* name/value: pointers into the conn buffer */
    size_t nheaders;
    int64_t content_length;                  /* -1: chunked or none */
} em_http_req;

typedef struct em_http_conn em_http_conn;   /* opaque: buffers, keep-alive state, transport */

/* Body: reads (content-length or chunked, decoded) until 0 = end. Unread body is drained before
 * the next request (bounded by max_body_bytes, else the connection closes). */
ssize_t em_http_read_body(em_http_conn *c, void *buf, size_t len);

/* Response: one call, or a streamed form with chunked encoding. */
int em_http_respond(em_http_conn *c, int status, const struct em_http_header *h, size_t nh,
                    const void *body, size_t len);
int em_http_start(em_http_conn *c, int status, const struct em_http_header *h, size_t nh); /* chunked */
int em_http_write(em_http_conn *c, const void *buf, size_t len);
int em_http_end(em_http_conn *c);

typedef int (*em_http_handler)(em_http_conn *c, const em_http_req *r, void *arg);

/* Serves one connection until close/error; does not close fd. tls may be NULL. */
int em_http_serve(int fd, em_tls *tls, const em_http_cfg *cfg, em_http_handler h, void *arg);
```

`em_http_cfg` gồm: `max_header_bytes` (8 KiB), `max_headers` (64), `max_body_bytes`,
`header_timeout_ns` (slowloris), `idle_timeout_ns` (keep-alive), `max_requests_per_conn`.

## Ngữ nghĩa phải đúng (đều có test)

- **Phiên bản.** HTTP/1.0 mặc định đóng kết nối, trừ khi client gửi `Connection: keep-alive`.
  HTTP/1.1 mặc định giữ kết nối, trừ khi gặp `Connection: close`. Response trả về cùng
  minor_version với request.
- **Request smuggling** (RFC 9112 §6.3). Có cả `Transfer-Encoding` lẫn `Content-Length` thì trả 400
  và đóng. Content-Length lặp lại với giá trị khác nhau, hoặc không hợp lệ, cũng trả 400.
  Transfer-Encoding khác `chunked` thì trả 501.
- **Giới hạn.**
  - Header quá lớn: 431. Body vượt giới hạn: 413.
  - Hết `header_timeout`: đóng kết nối, có thể kèm 408.
  - Idle quá lâu giữa hai request: đóng im lặng.
  - Mọi timeout dùng deadline của scope em, không dùng timer riêng.
- **`Expect: 100-continue`.** Server chỉ gửi `100 Continue` khi handler bắt đầu đọc body. Nếu
  handler trả lời mà không đọc body thì body được drain có giới hạn, hoặc kết nối bị đóng.
- **Request HEAD.** Response không có body nhưng vẫn giữ Content-Length.
- **Pipelining.** Response trả về đúng thứ tự request.
- **Handler trả lỗi hoặc không gọi respond:** server trả 500 và đóng kết nối. Handler đã ghi được
  một phần response mà gặp lỗi: đóng kết nối.
- **Header `Date`:** chỉ tính lại mỗi giây một lần cho mỗi worker.

## Kiểm thử

- Test `t5x_http_*` trong `tests/`. Mỗi mục ở phần ngữ nghĩa có ít nhất một test, cho cả plain và
  TLS. Client trong test viết bằng em và gửi byte thô, để kiểm được cả input sai.
- Fuzz phần quanh parser (vòng request, chunked, drain body) bằng libFuzzer, có ASan. Chạy ngắn
  trên CI và chạy dài ở local trước khi merge.
- Chạy được với curl (`--http1.0`, `--http1.1`, keep-alive, chunked upload) như một bước smoke test
  trên CI.

## Bench

- **Các ô đo:**
  - GET nhỏ, keep-alive, body 13 B kiểu "Hello, World!" của TechEmpower plaintext; có và không có
    pipelining (độ sâu 16).
  - Response 4 KiB.
  - Churn: mỗi kết nối chỉ một request.
  - Bản TLS của ô keep-alive và ô churn.
- **Client:** mở rộng client hiện có (chế độ `http`) để giữ nguyên cách đo CPU, ghim CPU và
  histogram. Client parse response bằng crate httparse. Không dùng wrk, vì nó sẽ đổi cách đo.
- **Đối thủ:**
  - **nginx** (`return 200`): làm mốc.
  - **h2o:** chính là người dùng picohttpparser.
  - **hyper** (tokio).
  - **Go net/http:** goroutine, tức mô hình "code đồng bộ trên luồng nhẹ" gần em nhất.
  - **ASP.NET Core Kestrel:** luôn ở top TechEmpower plaintext.
  - **Photon HTTP** và **Boost.Beast:** nếu bench C/C++ (plans/bench-cpp.md) đã có runtime của
    hai đối thủ này.
  - Tất cả chạy 1 worker, ghim 1 CPU, giống bench echo.

## Thứ tự làm

1. Vendor picohttpparser. Viết `http.c`: chỉ GET và keep-alive, chưa có body. Test và bench ô
   GET nhỏ so với nginx và h2o, để biết sớm đường nóng đứng ở đâu.
2. Thêm body (Content-Length, chunked, 100-continue), các giới hạn, timeout, HEAD, pipelining.
   Kèm đủ test ngữ nghĩa và fuzz.
3. TLS (`em_tls`), các ô TLS, rồi lần lượt thêm các đối thủ còn lại.
4. README, `include/em_http.h` có tài liệu đầy đủ, ví dụ.

## Quyết định lúc khởi động (2026-11-12)

- **Dừng sau bước 1 (M1).** Làm xong GET + keep-alive, bench so với nginx, h2o và haproxy, rồi xem
  số trước khi làm bước 2–4. Nếu đường nóng đứng sai chỗ thì sửa kiến trúc trước khi thêm body/TLS.
- **Đối thủ.**
  - Thêm **haproxy** (`http-request return status 200 ... string "Hello, World!"`, `nbthread 1`).
  - **Kestrel hoãn** sang sau đợt 1 (phải cài .NET SDK trên CI bench; Go + hyper + h2o đã đại diện
    đủ các mô hình).
  - **Photon HTTP và Boost.Beast là tuỳ chọn:** chỉ làm nếu dựng rẻ từ bench/cpp, và không chặn
    việc xong đợt 1.
- **Phán định.** Luật THUA (trung vị > 1.05 và mọi vòng > 1) áp cho **mọi** đối thủ HTTP, kể cả
  nginx và haproxy. Không còn đối thủ nào chỉ được in để tham khảo.
- **SIMD.** picohttpparser chọn SSE4.2 lúc compile, không có dispatch lúc chạy.
  - Makefile mặc định portable (không `-msse4.2`); `HTTP_SSE42=1` bật nó.
  - Bench luôn bật `HTTP_SSE42=1`. README nói rõ người dùng phải tự bật.
  - Trade-off: bản build mặc định chậm hơn ở parse. Xét lại khi baseline của các distro là
    x86-64-v2.
- `max_requests_per_conn` giữ nguyên như plan.

## KHÔNG làm (đợt 1)

- HTTP client. Sẽ làm khi có nhu cầu proxy hoặc gọi API.
- HTTP/2, HTTP/3, WebSocket, upgrade nói chung.
- Router, middleware, URL decode, query parse, cookie, nén. Đó là việc của framework phía trên.
- Phục vụ file tĩnh, sendfile, kTLS.
- Binding Rust hay ngôn ngữ khác cho em_http.
- Nhiều worker trong bench (giữ 1 CPU như bench hiện tại).
- Viết lại hay tối ưu picohttpparser.

## Kết quả

(chưa có)
