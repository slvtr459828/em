//go:build tls

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
	"os"
	"time"
)

// Self-signed ECDSA P-256 certificate made at startup; the bench client accepts any certificate.
func setup() func(net.Conn) net.Conn {
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
	cfg := &tls.Config{
		Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}},
		MinVersion:   tls.VersionTLS12,
	}
	// TLS_TICKETS: unset or stateless = Go's default (encrypted tickets, rotated key, 1 ticket per
	// TLS 1.3 connection: the count is not configurable); 0 = no tickets; stateful: Go has no
	// server-side session cache, so it is not supported (exit 2, like em on AWS-LC).
	switch v, set := os.LookupEnv("TLS_TICKETS"); {
	case !set || v == "stateless":
	case v == "0":
		cfg.SessionTicketsDisabled = true
	case v == "stateful":
		fmt.Fprintln(os.Stderr, "TLS_TICKETS=stateful: not supported by Go crypto/tls (no server-side session cache)")
		os.Exit(2)
	default:
		fmt.Fprintf(os.Stderr, "TLS_TICKETS=%s: want 0, stateless or stateful\n", v)
		os.Exit(2)
	}
	return func(c net.Conn) net.Conn { return tls.Server(c, cfg) }
}
