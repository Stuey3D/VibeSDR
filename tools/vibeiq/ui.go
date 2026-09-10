package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"
)

//go:embed ui.html
var uiHTML []byte

// serveUI serves the window on the loopback and returns its URL. The page is the same on every
// platform; on Windows and Linux it opens in the default browser, on macOS VibeIQ.app hosts it.
func serveUI(b *bridge, port int) (string, error) {
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return "", fmt.Errorf("cannot open the window's port: %w", err)
	}
	/* ★★★ A LOCAL PORT IS NOT A PRIVATE ONE. Any page in any browser on this machine can POST to
	 *  127.0.0.1, and these handlers took plain JSON with no Content-Type requirement — which makes
	 *  them CORS "simple" requests, so there is no preflight to save us. A hostile page could unpair
	 *  the bridge, or RE-PAIR IT to a receiver of its choosing and quietly repoint SDR++ at somebody
	 *  else's radio; with DNS rebinding it could also read /api/status, which carries the pairing
	 *  code. (Audit, 2026-09-10.)
	 *  ★★ Two checks, because they catch different attacks. The Origin check stops an ordinary
	 *     cross-site POST — a browser always sends Origin on those and cannot forge it. The Host
	 *     check stops DNS rebinding, where the page's origin looks legitimate but the name has been
	 *     re-pointed at 127.0.0.1: our own page is only ever reached by ADDRESS, so a Host that is
	 *     not a loopback literal is not us.
	 *  ★ GET of the page itself is left open: it is a static asset with no secret in it, and a
	 *    browser that fetches it cross-origin still cannot read the response. */
	guard := func(next http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			host := r.Host
			if h, _, err := net.SplitHostPort(host); err == nil {
				host = h
			}
			host = strings.Trim(host, "[]")
			if host != "127.0.0.1" && host != "::1" && host != "localhost" {
				http.Error(w, "unexpected Host — VibeIQ is reached at 127.0.0.1 only", http.StatusForbidden)
				return
			}
			if o := r.Header.Get("Origin"); o != "" {
				u, err := url.Parse(o)
				if err != nil {
					http.Error(w, "bad Origin", http.StatusForbidden)
					return
				}
				oh := strings.Trim(u.Hostname(), "[]")
				if oh != "127.0.0.1" && oh != "::1" && oh != "localhost" {
					http.Error(w, "cross-site request refused", http.StatusForbidden)
					return
				}
			}
			next(w, r)
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("content-type", "text/html; charset=utf-8")
		w.Header().Set("cache-control", "no-store")
		_, _ = w.Write(uiHTML)
	})
	mux.HandleFunc("/api/status", guard(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("content-type", "application/json")
		w.Header().Set("cache-control", "no-store")
		_ = json.NewEncoder(w).Encode(b.status())
	}))
	mux.HandleFunc("/api/pair", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST", 405)
			return
		}
		var in struct{ Code, Host string }
		_ = json.NewDecoder(r.Body).Decode(&in)
		err := b.pair(in.Code, in.Host)
		w.Header().Set("content-type", "application/json")
		if err != nil {
			_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]bool{"ok": true})
	}))
	mux.HandleFunc("/api/unpair", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST", 405)
			return
		}
		b.unpair()
		w.Header().Set("content-type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]bool{"ok": true})
	}))
	go func() { _ = http.Serve(ln, mux) }()
	return "http://" + ln.Addr().String() + "/", nil
}
