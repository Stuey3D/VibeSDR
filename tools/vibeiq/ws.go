package main

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// ── a minimal RFC 6455 client ────────────────────────────────────────────────────────────────

type wsConn struct {
	c net.Conn
	r *bufio.Reader
}

func dialWS(raw string) (*wsConn, error) {
	u, err := url.Parse(raw)
	if err != nil {
		return nil, err
	}
	host := u.Host
	var c net.Conn
	if u.Scheme == "wss" {
		if !strings.Contains(host, ":") {
			host += ":443"
		}
		c, err = tls.DialWithDialer(&net.Dialer{Timeout: 10 * time.Second}, "tcp", host, &tls.Config{ServerName: u.Hostname()})
	} else {
		if !strings.Contains(host, ":") {
			host += ":80"
		}
		c, err = net.DialTimeout("tcp", host, 10*time.Second)
	}
	if err != nil {
		return nil, err
	}
	var key [16]byte
	_, _ = rand.Read(key[:])
	k := base64.StdEncoding.EncodeToString(key[:])
	req := fmt.Sprintf("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: VibeIQ/1.0\r\n\r\n",
		u.RequestURI(), u.Host, k)
	if _, err := c.Write([]byte(req)); err != nil {
		c.Close()
		return nil, err
	}
	r := bufio.NewReaderSize(c, 1<<16)
	res, err := http.ReadResponse(r, nil)
	if err != nil {
		c.Close()
		return nil, err
	}
	if res.StatusCode != 101 {
		c.Close()
		switch res.StatusCode {
		case 403:
			return nil, errors.New("the receiver does not recognise this code — turn RAW IQ OUT off and on again for a new one")
		case 409:
			return nil, errors.New("another VibeIQ is already attached to this session")
		}
		return nil, fmt.Errorf("receiver answered %s", res.Status)
	}
	want := base64.StdEncoding.EncodeToString(func() []byte { h := sha1.Sum([]byte(k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")); return h[:] }())
	if res.Header.Get("Sec-WebSocket-Accept") != want {
		c.Close()
		return nil, errors.New("bad websocket handshake")
	}
	return &wsConn{c: c, r: r}, nil
}

func (w *wsConn) Close() { w.c.Close() }

func (w *wsConn) read() (byte, []byte, error) {
	var h [2]byte
	if _, err := io.ReadFull(w.r, h[:]); err != nil {
		return 0, nil, err
	}
	op := h[0] & 0x0f
	masked := h[1]&0x80 != 0
	n := uint64(h[1] & 0x7f)
	switch n {
	case 126:
		var b [2]byte
		if _, err := io.ReadFull(w.r, b[:]); err != nil {
			return 0, nil, err
		}
		n = uint64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err := io.ReadFull(w.r, b[:]); err != nil {
			return 0, nil, err
		}
		n = binary.BigEndian.Uint64(b[:])
	}
	if n > 1<<24 {
		return 0, nil, errors.New("frame too large")
	}
	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(w.r, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	p := make([]byte, n)
	if _, err := io.ReadFull(w.r, p); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range p {
			p[i] ^= mask[i&3]
		}
	}
	return op, p, nil
}

func (w *wsConn) write(op byte, p []byte) error {
	// ★ Client frames MUST be masked (RFC 6455 §5.3) or the server drops the connection.
	var mask [4]byte
	_, _ = rand.Read(mask[:])
	h := []byte{0x80 | op}
	switch {
	case len(p) < 126:
		h = append(h, 0x80|byte(len(p)))
	case len(p) < 1<<16:
		h = append(h, 0x80|126, byte(len(p)>>8), byte(len(p)))
	default:
		h = append(h, 0x80|127)
		var b [8]byte
		binary.BigEndian.PutUint64(b[:], uint64(len(p)))
		h = append(h, b[:]...)
	}
	h = append(h, mask[:]...)
	body := make([]byte, len(p))
	for i := range p {
		body[i] = p[i] ^ mask[i&3]
	}
	_ = w.c.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if _, err := w.c.Write(append(h, body...)); err != nil {
		return err
	}
	return nil
}
