// Go net/http as the HTTP rival of bench/run.sh (plans/http.md): usage `binary port [workers] [tls]`.
// Plain net/http: one goroutine per connection, the default Server, nothing tuned (no GOGC, no
// LockOSThread). workers = GOMAXPROCS (the bench uses 1); Go's netpoller is epoll. HTTP/1.1 only: no ALPN
// "h2" (tls.NewListener, empty NextProtos, TLSNextProto empty).
// GET / -> "Hello, World!" (13 B), GET /4k -> 4096 B, both text/plain; else 404.
// Content-Length is set by hand: net/http chunks a body larger than its 2 KiB buffer when the handler
// does not set it, and the bench client wants Content-Length.
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"fmt"
	"math/big"
	"net"
	"net/http"
	"os"
	"runtime"
	"strconv"
	"time"
)

var (
	hello = []byte("Hello, World!")
	big4k = func() []byte {
		b := make([]byte, 4096)
		for i := range b {
			b[i] = 'x'
		}
		return b
	}()
)

func serve(w http.ResponseWriter, r *http.Request) {
	body, n := hello, "13"
	switch r.URL.Path {
	case "/":
	case "/4k":
		body, n = big4k, "4096"
	default:
		w.Header().Set("Content-Type", "text/plain")
		w.Header().Set("Content-Length", "10")
		w.WriteHeader(http.StatusNotFound)
		w.Write([]byte("Not Found\n"))
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Content-Length", n)
	w.Write(body)
}

// Self-signed ECDSA P-256 certificate made at startup (same as bench/go/tls.go: the bench client
// accepts any certificate). TLS 1.3 suites are not configurable in crypto/tls: Go picks AES-128-GCM.
func tlsConfig() *tls.Config {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		panic(err)
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		DNSNames:     []string{"localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	return &tls.Config{
		Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}},
		MinVersion:   tls.VersionTLS12,
	}
}

func main() {
	a := os.Args
	if len(a) < 2 || len(a) > 4 || (len(a) == 4 && a[3] != "tls") {
		fmt.Fprintf(os.Stderr, "usage: %s port [workers] [tls]\n", a[0])
		os.Exit(2)
	}
	if len(a) >= 3 {
		n, err := strconv.Atoi(a[2])
		if err != nil || n < 1 {
			fmt.Fprintf(os.Stderr, "bad workers %q\n", a[2])
			os.Exit(2)
		}
		runtime.GOMAXPROCS(n)
	}
	ln, err := net.Listen("tcp4", "127.0.0.1:"+a[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if len(a) == 4 {
		ln = tls.NewListener(ln, tlsConfig())
	}
	srv := &http.Server{Handler: http.HandlerFunc(serve), TLSNextProto: map[string]func(*http.Server, *tls.Conn, http.Handler){}}
	fmt.Fprintln(os.Stderr, srv.Serve(ln))
	os.Exit(1)
}
