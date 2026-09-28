package main

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const directory = "https://vibeserver.vibesdr.net"

// streamRate is what the tunnel carries — the public path is always 48 kHz (the server forces
// it), and the app's SET_SAMPLE_RATE says what it expects; the bridge converts between the two.
const streamRate = 48000

type pairing struct {
	host  string // the page host: <slug>.vibeserver.vibesdr.net or the receiver's own address
	path  string // "" or "/r/<serial>/" — a radio behind a multi-radio front door
	token string // the session's credential for /ws/iq (the code itself in the --host form)
}

// bridge is the whole state of one VibeIQ: a pairing, the local rtl_tcp listener, and whatever
// app is attached. The window reads `status()`; the command line reads `onLog`.
type bridge struct {
	port  int
	onLog func(string)

	mu       sync.Mutex
	p        *pairing
	code     string
	ln       net.Listener
	app      net.Conn
	state    string // idle · paired · app · streaming · error
	detail   string // the human line under the state
	log      []string
	appRate  int
	tuneHz   uint32
	bytes    atomic.Int64
	rateBps  atomic.Int64 // bytes/s over the last second, for the meter
	appGone  chan struct{}
	unpaired chan struct{}
}

func newBridge(port int) *bridge {
	b := &bridge{port: port, state: "idle", detail: "Enter the code from the web client or the app."}
	go b.meter()
	return b
}

func (b *bridge) logf(format string, a ...any) {
	s := fmt.Sprintf(format, a...)
	b.mu.Lock()
	b.log = append(b.log, time.Now().Format("15:04:05")+"  "+s)
	if len(b.log) > 200 {
		b.log = b.log[len(b.log)-200:]
	}
	cb := b.onLog
	b.mu.Unlock()
	if cb != nil {
		cb("vibeiq: " + s)
	}
}

func (b *bridge) set(state, detail string) {
	b.mu.Lock()
	b.state, b.detail = state, detail
	b.mu.Unlock()
}

type status struct {
	State    string   `json:"state"`
	Detail   string   `json:"detail"`
	Code     string   `json:"code"`
	Receiver string   `json:"receiver"`
	Port     int      `json:"port"`
	AppRate  int      `json:"appRate"`
	TuneHz   uint32   `json:"tuneHz"`
	Bytes    int64    `json:"bytes"`
	Bps      int64    `json:"bps"`
	Log      []string `json:"log"`
}

func (b *bridge) status() status {
	b.mu.Lock()
	defer b.mu.Unlock()
	s := status{State: b.state, Detail: b.detail, Code: b.code, Port: b.port, AppRate: b.appRate,
		TuneHz: b.tuneHz, Bytes: b.bytes.Load(), Bps: b.rateBps.Load(), Log: append([]string(nil), b.log...)}
	if b.p != nil {
		s.Receiver = b.p.host + strings.TrimSuffix(b.p.path, "/")
	}
	return s
}

// meter turns the byte counter into a rate once a second, for the window's meter.
func (b *bridge) meter() {
	last := int64(0)
	for range time.Tick(time.Second) {
		now := b.bytes.Load()
		b.rateBps.Store(now - last)
		last = now
	}
}

// pair resolves the code (and optional host) and starts the local rtl_tcp listener.
func (b *bridge) pair(code, host string) error {
	code = strings.ToLower(strings.TrimSpace(code))
	p, err := resolve(code, host)
	if err != nil {
		b.set("error", err.Error())
		b.logf("%s", err)
		return err
	}
	b.unpair()
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", b.port))
	if err != nil {
		e := fmt.Errorf("cannot listen on 127.0.0.1:%d — is another rtl_tcp or VibeIQ running? (%v)", b.port, err)
		b.set("error", e.Error())
		b.logf("%s", e)
		return e
	}
	b.mu.Lock()
	b.p, b.code, b.ln = &p, code, ln
	b.unpaired = make(chan struct{})
	done := b.unpaired
	b.mu.Unlock()
	b.set("paired", fmt.Sprintf("Point your SDR app at 127.0.0.1:%d as an rtl_tcp source.", b.port))
	b.logf("paired with %s%s", p.host, strings.TrimSuffix(p.path, "/"))
	b.logf("rtl_tcp on 127.0.0.1:%d — point your SDR app there", b.port)
	go b.acceptLoop(ln, p, done)
	return nil
}

// unpair drops the app and the listener; the window's Disconnect.
func (b *bridge) unpair() {
	b.mu.Lock()
	ln, app, done := b.ln, b.app, b.unpaired
	b.ln, b.app, b.p, b.code = nil, nil, nil, ""
	b.mu.Unlock()
	if done != nil {
		close(done)
	}
	if app != nil {
		app.Close()
	}
	if ln != nil {
		ln.Close()
		b.logf("disconnected")
	}
	b.set("idle", "Enter the code from the web client or the app.")
}

func (b *bridge) acceptLoop(ln net.Listener, p pairing, done chan struct{}) {
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		b.mu.Lock()
		b.app = c
		b.mu.Unlock()
		b.logf("app connected from %s", c.RemoteAddr())
		b.set("app", "SDR app attached — connecting to the receiver…")
		b.serve(c, p, done)
		b.mu.Lock()
		if b.app == c {
			b.app = nil
		}
		still := b.ln == ln
		b.mu.Unlock()
		b.logf("app left")
		if still {
			b.set("paired", fmt.Sprintf("Point your SDR app at 127.0.0.1:%d as an rtl_tcp source.", b.port))
		}
	}
}

// resolve turns a code (and optionally a host) into the page host + credential.
func resolve(code, host string) (pairing, error) {
	if host != "" {
		h := strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(host), "https://"), "http://")
		p := pairing{host: h, token: code}
		if i := strings.Index(h, "/"); i >= 0 {
			p.host, p.path = h[:i], h[i:]
		}
		if code == "" {
			return pairing{}, errors.New("the code is needed with the address too")
		}
		return checkTarget(p)
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
	return checkTarget(pairing{host: j.Host, path: j.Path, token: j.Token})
}

// radioPath is the only path a pairing may carry: the root, or one radio behind a multi-radio
// front door. The same shape the directory accepts (IQ_PATH_RE in directory/src/index.js).
var radioPath = regexp.MustCompile(`^/(r/[A-Za-z0-9_-]{1,40}/)?$`)

// checkTarget validates a pairing's host and path before anything is fetched from them, and
// normalises the path to "/" or "/r/<serial>/".
//
// ★★★ CodeQL go/request-forgery (2026-09-28). The host is where the bridge sends its first request,
// and it comes from the window, the command line or the directory's answer. It is MEANT to be
// a receiver the user chose — this is not a proxy for arbitrary URLs — but it went into the
// URL as a raw string, so "evil@host", "host?x" or "host#x" changed which server was asked,
// and a "/../" path reached any page on it. Now the host must be a bare host[:port] (no
// userinfo, query, fragment or path of its own) and the path one of the two shapes above.
// ★ This also fixed a real fault: a pairing with NO path (a single-radio receiver, the common
// case) fetched "https://<host>vibeserver.json" — the slash was missing — and never connected.
func checkTarget(p pairing) (pairing, error) {
	bad := errors.New("that receiver address is not a plain host name or address")
	if p.host == "" || strings.ContainsAny(p.host, "@?#\\ /") {
		return pairing{}, bad
	}
	u, err := url.Parse("https://" + p.host + "/")
	if err != nil || u.User != nil || u.Host != p.host || u.Hostname() == "" || u.Path != "/" {
		return pairing{}, bad
	}
	if port := u.Port(); port != "" {
		if n, err := strconv.Atoi(port); err != nil || n < 1 || n > 65535 {
			return pairing{}, bad
		}
	}
	if p.path == "" {
		p.path = "/"
	} else if !strings.HasSuffix(p.path, "/") {
		p.path += "/"
	}
	if !radioPath.MatchString(p.path) {
		return pairing{}, errors.New("that receiver path is not /r/<radio>/")
	}
	return p, nil
}

// wsURL reads /vibeserver.json at the page host: through the directory it carries `directUrl`,
// the live tunnel hostname; on a receiver's own address it is the address itself.
func wsURL(p pairing) (string, error) {
	// ★ p was validated by checkTarget: a bare host[:port] and a path of "/" or "/r/<radio>/".
	target := url.URL{Scheme: "https", Host: p.host, Path: p.path + "vibeserver.json"}
	res, err := http.Get(target.String())
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
	if (u.Scheme != "https" && u.Scheme != "http") || u.Hostname() == "" || u.User != nil {
		return "", fmt.Errorf("the receiver gave an unusable address: %q", base)
	}
	scheme := "wss"
	if u.Scheme == "http" {
		scheme = "ws"
	}
	return fmt.Sprintf("%s://%s%sws/iq?tok=%s", scheme, u.Host, "/"+strings.TrimPrefix(p.path, "/"), url.QueryEscape(p.token)), nil
}

// serve relays one app connection for as long as it stays, reconnecting the WebSocket underneath.
func (b *bridge) serve(app net.Conn, p pairing, done chan struct{}) {
	defer app.Close()
	cmds := make(chan [5]byte, 64)
	gone := make(chan struct{})
	go func() {
		defer close(gone)
		var c [5]byte
		for {
			if _, err := io.ReadFull(app, c[:]); err != nil {
				return
			}
			select {
			case cmds <- c:
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
		case <-done:
			return
		default:
		}
		u, err := wsURL(p)
		if err != nil {
			b.set("app", "Receiver not reachable — retrying…")
			b.logf("receiver not reachable: %v", err)
		} else if err = b.relay(u, app, cmds, gone, done, &sentHeader, &appRate); err != nil {
			b.set("app", "Stream dropped — reconnecting…")
			b.logf("stream ended: %v", err)
		} else {
			return
		}
		select {
		case <-gone:
			return
		case <-done:
			return
		case <-time.After(backoff):
		}
		if backoff < 15*time.Second {
			backoff *= 2
		}
	}
}

var errAppGone = errors.New("app gone")

// relay runs one WebSocket session. Returns nil when the app left, an error when the socket did.
func (b *bridge) relay(u string, app net.Conn, cmds <-chan [5]byte, gone, done <-chan struct{}, sentHeader *bool, appRate *int) error {
	ws, err := dialWS(u)
	if err != nil {
		return err
	}
	defer ws.Close()
	b.logf("streaming")
	b.set("streaming", "Streaming to your SDR app.")
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
							b.logf("converting %d Hz → %d Hz for the app", streamRate, want)
						}
						payload = rs.convert(payload)
					}
					rsMu.Unlock()
					b.bytes.Add(int64(len(payload)))
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
		case <-done:
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
			v := binary.BigEndian.Uint32(c[1:])
			switch c[0] {
			case 0x01:
				b.mu.Lock()
				b.tuneHz = v
				b.mu.Unlock()
				b.logf("tune %.6f MHz", float64(v)/1e6)
			case 0x02:
				rsMu.Lock()
				*appRate = int(v)
				rsMu.Unlock()
				b.mu.Lock()
				b.appRate = int(v)
				b.mu.Unlock()
				b.logf("app expects %d Hz", v)
			}
		}
	}
}
