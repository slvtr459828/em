//go:build !tls

package main

import "net"

func setup() func(net.Conn) net.Conn { return func(c net.Conn) net.Conn { return c } }
