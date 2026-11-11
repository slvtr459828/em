// Go echo servers for bench/run.sh (plans/bench-cpp.md): usage `binary port [workers]`.
// Plain Go style: net.Listen, one goroutine per connection, a 16 KiB buffer per connection.
// workers = GOMAXPROCS (the bench uses 1). Go's netpoller is epoll. Nothing is tuned: no GOGC,
// no LockOSThread, no buffer pool. The binary is built with -tags tls for the TLS server.
package main

import (
	"fmt"
	"net"
	"os"
	"runtime"
	"strconv"
)

const echoBuf = 16384

func main() {
	if len(os.Args) < 2 || len(os.Args) > 3 {
		fmt.Fprintf(os.Stderr, "usage: %s port [workers]\n", os.Args[0])
		os.Exit(2)
	}
	if len(os.Args) == 3 {
		n, err := strconv.Atoi(os.Args[2])
		if err != nil || n < 1 {
			fmt.Fprintf(os.Stderr, "bad workers %q\n", os.Args[2])
			os.Exit(2)
		}
		runtime.GOMAXPROCS(n)
	}
	wrap := setup() // TLS: builds the config (may exit 2); plain: identity
	nodelay := os.Getenv("NODELAY") != "0"
	ln, err := net.Listen("tcp4", "127.0.0.1:"+os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			continue // e.g. EMFILE: a failed accept must not kill the server
		}
		go echo(c, wrap, nodelay)
	}
}

func echo(c net.Conn, wrap func(net.Conn) net.Conn, nodelay bool) {
	defer c.Close()
	c.(*net.TCPConn).SetNoDelay(nodelay)
	rw := wrap(c)
	buf := make([]byte, echoBuf)
	for {
		n, err := rw.Read(buf)
		if n > 0 {
			if _, werr := rw.Write(buf[:n]); werr != nil {
				return
			}
		}
		if err != nil {
			return
		}
	}
}
