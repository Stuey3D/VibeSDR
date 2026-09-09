// VibeIQ — the bridge between a VibeServer's raw IQ out (through the tunnel) and an rtl_tcp app
// on this machine.
//
//	vibeiq CODE                  pair by the six-character code shown in the web client or the app
//	vibeiq --host H CODE         the receiver's own address, when the page was not on a directory one
//	vibeiq --port 1234 ...       where the local rtl_tcp server listens (default 127.0.0.1:1234)
//
// Point SDR++, SDR#, GQRX, DSD-FME or anything else that speaks rtl_tcp at 127.0.0.1:1234.
// Tuning from that app moves the VibeServer session's dial; the web client keeps playing audio.
//
// ★ Standard library only: one static binary per OS, nothing to install. The WebSocket client is
//
//	hand-rolled (RFC 6455, client side, ~100 lines) rather than pulling in a module for it.
//
// ★ ONE consumer at a time, as rtl_tcp itself. The WebSocket is opened when the app connects and
//
//	closed when it leaves, so a paired-but-idle bridge costs the receiver nothing.
//
// ★ The tunnel drops streams now and then (see the VibeSDR notes on Quick Tunnels): the bridge
//
//	reconnects with a short backoff while the app is still attached, and the app sees a pause
//	rather than a disconnect.
package main

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

const directory = "https://vibeserver.vibesdr.net"

type pairing struct {
	host  string // the page host: <slug>.vibeserver.vibesdr.net or the receiver's own address
	path  string // "" or "/r/<serial>/" — a radio behind a multi-radio front door
	token string // the session's credential for /ws/iq (the code itself in the --host form)
}

func main() {
	host := flag.String("host", "", "receiver address (when the code alone cannot find it)")
	port := flag.Int("port", 1234, "local rtl_tcp port")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: vibeiq [--host ADDRESS] [--port 1234] CODE")
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() > 1 {
		flag.Usage()
		os.Exit(2)
	}
	// ★ Double-clicked from Finder or Explorer there are no arguments at all — the first cut
	//   printed usage and quit before anyone could read it (Stuart, 2026-09-09: "it never asked
	//   me for the code or anything"). So ask, and keep asking until a code pairs.
	code := ""
	if flag.NArg() == 1 {
		code = strings.ToLower(strings.TrimSpace(flag.Arg(0)))
	}
	in := bufio.NewReader(os.Stdin)
	var p pairing
	for {
		if code == "" {
			fmt.Println("VibeIQ — raw IQ from a VibeServer as rtl_tcp on this machine.")
			fmt.Println("In the web client or the app, open AUDIO, switch RAW IQ OUT on, and read the six-character code.")
			fmt.Print("Code: ")
			line, err := in.ReadString('\n')
			if err != nil {
				return
			}
			code = strings.ToLower(strings.TrimSpace(line))
			if code == "" {
				continue
			}
		}
		var err error
		if p, err = resolve(code, *host); err == nil {
			break
		}
		fmt.Println("vibeiq:", err)
		code = ""
	}
	fmt.Printf("vibeiq: paired with %s%s\n", p.host, strings.TrimSuffix(p.path, "/"))

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		fmt.Fprintln(os.Stderr, "vibeiq: cannot listen:", err)
		os.Exit(1)
	}
	fmt.Printf("vibeiq: rtl_tcp on 127.0.0.1:%d — point your SDR app there\n", *port)
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		fmt.Println("vibeiq: app connected from", c.RemoteAddr())
		serve(c, p)
		fmt.Println("vibeiq: app left")
	}
}

// resolve turns a code (and optionally a host) into the page host + credential.
func resolve(code, host string) (pairing, error) {
	if host != "" {
		h := strings.TrimPrefix(strings.TrimPrefix(host, "https://"), "http://")
		p := pairing{host: h, token: code}
		if i := strings.Index(h, "/"); i >= 0 {
			p.host, p.path = h[:i], strings.TrimSuffix(h[i:], "/")+"/"
		}
		return p, nil
	}
	if len(code) != 6 {
		return pairing{}, errors.New("a code is six characters")
	}
	res, err := http.Get(directory + "/api/iq/" + url.PathEscape(code))
	if err != nil {
		return pairing{}, fmt.Errorf("directory: %w", err)
	}
	defer res.Body.Close()
	var j struct {
		Host, Path, Token, Error string
	}
	if err := json.NewDecoder(res.Body).Decode(&j); err != nil || j.Host == "" {
		if j.Error != "" {
			return pairing{}, errors.New(j.Error + " — is RAW IQ OUT still on in the web client?")
		}
		return pairing{}, errors.New("directory did not recognise that code")
	}
	return pairing{host: j.Host, path: j.Path, token: j.Token}, nil
}

// wsURL reads /vibeserver.json at the page host: through the directory it carries `directUrl`,
// the live tunnel hostname; on a receiver's own address it is the address itself.
func wsURL(p pairing) (string, error) {
	res, err := http.Get("https://" + p.host + p.path + "vibeserver.json")
	if err != nil {
		return "", err
	}
	defer res.Body.Close()
	var j struct {
		DirectUrl string `json:"directUrl"`
	}
	_ = json.NewDecoder(res.Body).Decode(&j)
	base := "https://" + p.host
	if j.DirectUrl != "" {
		base = j.DirectUrl
	}
	u, err := url.Parse(base)
	if err != nil {
		return "", err
	}
	scheme := "wss"
	if u.Scheme == "http" {
		scheme = "ws"
	}
	return fmt.Sprintf("%s://%s%sws/iq?tok=%s", scheme, u.Host, "/"+strings.TrimPrefix(p.path, "/"), url.QueryEscape(p.token)), nil
}

// serve relays one app connection for as long as it stays, reconnecting the WebSocket underneath.
func serve(app net.Conn, p pairing) {
	defer app.Close()
	cmds := make(chan [5]byte, 64)
	gone := make(chan struct{})
	go func() {
		defer close(gone)
		var b [5]byte
		for {
			if _, err := io.ReadFull(app, b[:]); err != nil {
				return
			}
			select {
			case cmds <- b:
			default:
			}
		}
	}()
	backoff := time.Second
	sentHeader := false
	appRate := 250000 // rtl_tcp apps always send SET_SAMPLE_RATE; this is only the fallback
	for {
		select {
		case <-gone:
			return
		default:
		}
		u, err := wsURL(p)
		if err != nil {
			fmt.Println("vibeiq: receiver not reachable:", err)
		} else if err = relay(u, app, cmds, gone, &sentHeader, &appRate); err != nil {
			fmt.Println("vibeiq: stream ended:", err)
		} else {
			return
		}
		select {
		case <-gone:
			return
		case <-time.After(backoff):
		}
		if backoff < 15*time.Second {
			backoff *= 2
		}
	}
}

// streamRate is what the tunnel carries — the public path is always 48 kHz (the server forces
// it), and the app's SET_SAMPLE_RATE says what it expects; the bridge converts between the two.
const streamRate = 48000

// relay runs one WebSocket session. Returns nil when the app left, an error when the socket did.
func relay(u string, app net.Conn, cmds <-chan [5]byte, gone <-chan struct{}, sentHeader *bool, appRate *int) error {
	ws, err := dialWS(u)
	if err != nil {
		return err
	}
	defer ws.Close()
	fmt.Println("vibeiq: streaming")
	errc := make(chan error, 1)
	var rs *iqResampler
	var rsMu sync.Mutex
	go func() {
		for {
			op, payload, err := ws.read()
			if err != nil {
				errc <- err
				return
			}
			switch op {
			case 0x2:
				// ★ The 12-byte RTL0 header arrives first on EVERY (re)connection; the app must
				//   see it exactly once, at the start.
				if len(payload) == 12 && string(payload[:4]) == "RTL0" {
					if *sentHeader {
						continue
					}
					*sentHeader = true
				} else {
					rsMu.Lock()
					want := *appRate
					if want > 0 && want != streamRate {
						if rs == nil || rs.out != want {
							rs = newIqResampler(streamRate, want)
							fmt.Printf("vibeiq: converting %d Hz → %d Hz for the app\n", streamRate, want)
						}
						payload = rs.convert(payload)
					}
					rsMu.Unlock()
				}
				if _, err := app.Write(payload); err != nil {
					errc <- errAppGone
					return
				}
			case 0x9:
				_ = ws.write(0xA, payload)
			case 0x8:
				errc <- errors.New("closed by the receiver")
				return
			}
		}
	}()
	for {
		select {
		case <-gone:
			return nil
		case err := <-errc:
			if errors.Is(err, errAppGone) {
				return nil
			}
			return err
		case c := <-cmds:
			// rtl_tcp command: 1 byte + big-endian u32. 0x01 SET_FREQUENCY is the one that matters;
			// the rest are forwarded and the server ignores what it owns (rate, gain, ppm).
			if err := ws.write(0x2, c[:]); err != nil {
				return err
			}
			if c[0] == 0x01 {
				fmt.Printf("vibeiq: tune %.6f MHz\n", float64(binary.BigEndian.Uint32(c[1:]))/1e6)
			}
			if c[0] == 0x02 {
				rsMu.Lock()
				*appRate = int(binary.BigEndian.Uint32(c[1:]))
				rsMu.Unlock()
				fmt.Printf("vibeiq: app expects %d Hz\n", *appRate)
			}
		}
	}
}

var errAppGone = errors.New("app gone")

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
