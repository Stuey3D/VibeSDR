package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
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
	mux.HandleFunc("/api/status", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("content-type", "application/json")
		w.Header().Set("cache-control", "no-store")
		_ = json.NewEncoder(w).Encode(b.status())
	})
	mux.HandleFunc("/api/pair", func(w http.ResponseWriter, r *http.Request) {
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
	})
	mux.HandleFunc("/api/unpair", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST", 405)
			return
		}
		b.unpair()
		w.Header().Set("content-type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]bool{"ok": true})
	})
	go func() { _ = http.Serve(ln, mux) }()
	return "http://" + ln.Addr().String() + "/", nil
}
