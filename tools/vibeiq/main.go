// VibeIQ — the bridge between a VibeServer's raw IQ out (through the tunnel) and an rtl_tcp app
// on this machine.
//
//	vibeiq                        the window: a local page in your browser (the Mac app hosts it)
//	vibeiq CODE                   command line: pair by the six-character code and stream
//	vibeiq --host H CODE          the receiver's own address, when the page was not on a directory one
//	vibeiq --port 1234 ...        where the local rtl_tcp server listens (default 127.0.0.1:1234)
//	vibeiq --gui [--ui-port N] [--no-open]   force the window; the Mac app passes these
//
// Point SDR++, SDR#, GQRX, DSD-FME or anything else that speaks rtl_tcp at 127.0.0.1:1234.
// Tuning from that app moves the VibeServer session's dial; the web client keeps playing audio.
//
// ★ Standard library only: one static binary per OS, nothing to install. The WebSocket client is
//
//	hand-rolled (RFC 6455, client side) rather than pulling in a module for it; the window is a
//	page served on the loopback rather than a GUI toolkit, so the same binary is the GUI on
//	Windows and Linux and the engine inside the Mac app.
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
	"flag"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"
)

func main() {
	host := flag.String("host", "", "receiver address (when the code alone cannot find it)")
	port := flag.Int("port", 1234, "local rtl_tcp port")
	gui := flag.Bool("gui", false, "open the window even when a code is given")
	uiPort := flag.Int("ui-port", 0, "port for the window's page (0 = pick one)")
	noOpen := flag.Bool("no-open", false, "do not open the browser (the Mac app hosts the page itself)")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: vibeiq [--host ADDRESS] [--port 1234] [CODE]")
		fmt.Fprintln(os.Stderr, "       vibeiq                 opens the window")
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() > 1 {
		flag.Usage()
		os.Exit(2)
	}
	code := ""
	if flag.NArg() == 1 {
		code = strings.ToLower(strings.TrimSpace(flag.Arg(0)))
	}

	b := newBridge(*port)

	// ★ No code = the window. Double-clicked from Finder or Explorer there are no arguments at
	//   all, and the first cut printed usage and quit before anyone could read it (Stuart,
	//   2026-09-09: "it never asked me for the code or anything"; then "for macOS just make it a
	//   GUI … same for Windows and Linux but allow for the GUI to be accessed from command line").
	if code == "" || *gui {
		u, err := serveUI(b, *uiPort)
		if err != nil {
			fmt.Fprintln(os.Stderr, "vibeiq:", err)
			os.Exit(1)
		}
		if code != "" {
			b.pair(code, *host)
		}
		fmt.Printf("VibeIQ window: %s\n", u)
		if !*noOpen {
			openBrowser(u)
		}
		select {}
	}

	// Command line: pair, then stream until killed.
	b.onLog = func(s string) { fmt.Println(s) }
	if err := b.pair(code, *host); err != nil {
		fmt.Fprintln(os.Stderr, "vibeiq:", err)
		os.Exit(1)
	}
	// Keep stdin open so a Ctrl-D ends it politely on a terminal.
	in := bufio.NewReader(os.Stdin)
	for {
		if _, err := in.ReadString('\n'); err != nil {
			select {}
		}
	}
}

func openBrowser(u string) {
	var c *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		c = exec.Command("open", u)
	case "windows":
		c = exec.Command("rundll32", "url.dll,FileProtocolHandler", u)
	default:
		c = exec.Command("xdg-open", u)
	}
	_ = c.Start()
}
