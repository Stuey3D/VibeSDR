// VibeIQ — the bridge between a VibeServer's raw IQ out (through the tunnel) and an rtl_tcp app
// on this machine.
//
//	vibeiq                        the window: a local page in your browser (the Mac app hosts it)
//	vibeiq CODE                   command line: pair by the six-character code and stream
//	vibeiq --host H CODE          the receiver's own address, when the page was not on a directory one
//	vibeiq --port 1234 ...        where the local rtl_tcp server listens (default 127.0.0.1:1234)
//	vibeiq --gui [--ui-port N] [--no-open]   force the window; the Mac app passes these
//	vibeiq --cli                  the terminal: ask for the code and stream, no window
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
// ★★ AND THAT IS WHY IT MUST NOTICE A TERMINAL. A page on the loopback is a window only if
//
//	somebody can open a browser on this machine. Over SSH nobody can: the first cut served the
//	page anyway, failed to launch a browser (silently — openBrowser discards the error), printed
//	a URL on a host the user is not sitting at, and then blocked forever on `select {}`. It looked
//	hung. So a headless run takes the TERMINAL path instead and asks for the code there
//	(Stuart, 2026-09-10: "I assume if someone SSH's in and selects the app a command line version
//	runs instead?" — it did not, and now it does). `--gui` still forces the window for anyone who
//	wants to port-forward to it.
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
	cli := flag.Bool("cli", false, "stay in the terminal: ask for the code here, never open a window")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: vibeiq [--host ADDRESS] [--port 1234] [CODE]")
		fmt.Fprintln(os.Stderr, "       vibeiq                 opens the window (asks here instead over SSH)")
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
	//
	// ★★ … UNLESS THERE IS NOBODY HERE TO SEE A WINDOW. See headless() — an SSH session, or a
	//   Linux box with no display, gets asked for the code on the terminal instead. `--gui` still
	//   overrides, and `--cli` forces the terminal even at a desk.
	if *cli || (code == "" && !*gui && headless()) {
		runTerminal(b, code, *host)
		return
	}
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

	runTerminal(b, code, *host)
}

// headless reports whether a window would be pointless — nobody is sitting at this machine's
// screen, so the loopback page has no browser to appear in.
//
// ★ The two signals are different questions and BOTH are needed. SSH_CONNECTION says the shell
//
//	came in over the network, which is true even on a Mac or a Linux desktop that HAS a display —
//	the display just is not the user's. DISPLAY/WAYLAND_DISPLAY says the Linux box has no session
//	to draw into at all, which is true of a console login on a headless Pi with no SSH in sight.
//
// ★ macOS and Windows are NOT tested for a display: a local login always has one, and the only
//
//	way to be at a Mac without a window server is over SSH, which the first test already caught.
func headless() bool {
	if os.Getenv("SSH_CONNECTION") != "" || os.Getenv("SSH_TTY") != "" || os.Getenv("SSH_CLIENT") != "" {
		return true
	}
	if runtime.GOOS == "linux" && os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "" {
		return true
	}
	return false
}

// interactive reports whether stdin is a terminal we can ask a question on. Stdlib only, because
// pulling in x/term for one predicate would break the one-static-binary promise at the top of this
// file.
//
// ★★★ A CHARACTER DEVICE IS NOT A TERMINAL. The obvious test — `Mode()&os.ModeCharDevice` — is
//
//	the one x/term makes underneath, and it says YES to /dev/null, which is a character device
//	too. So `vibeiq < /dev/null` printed the banner, printed "Pairing code: " to nobody, read EOF
//	and quit: the exact silent-question case this function exists to prevent, passing its own
//	guard. Measured, not reasoned about — the first cut of this shipped until it was run.
//
// ★ So rule /dev/null out by identity. os.DevNull is "NUL" on Windows and os.Stat handles it, so
//
//	this is one test on every platform rather than a build-tagged pair.
func interactive() bool {
	fi, err := os.Stdin.Stat()
	if err != nil || fi.Mode()&os.ModeCharDevice == 0 {
		return false
	}
	if nul, err := os.Stat(os.DevNull); err == nil && os.SameFile(fi, nul) {
		return false
	}
	return true
}

// runTerminal is the command line: pair, then stream until killed. With no code it asks for one —
// which is the whole point over SSH.
func runTerminal(b *bridge, code, host string) {
	b.onLog = func(s string) { fmt.Println(s) }
	in := bufio.NewReader(os.Stdin)

	if code == "" {
		/* ★★★ NEVER BLOCK ON A QUESTION NOBODY CAN ANSWER. If stdin is not a terminal — a cron
		 *  entry, a systemd unit, a pipe — then reading it either returns EOF at once or waits
		 *  forever on a prompt no one will ever see. Say what to pass and stop, with a non-zero
		 *  status so whatever launched it knows. This is the same fault as the old `select {}`,
		 *  one layer down. */
		if !interactive() {
			fmt.Fprintln(os.Stderr, "vibeiq: no pairing code, and stdin is not a terminal to ask on.")
			fmt.Fprintln(os.Stderr, "        pass it:  vibeiq CODE   (or --gui for the window)")
			os.Exit(2)
		}
		fmt.Println("VibeIQ — raw IQ from a VibeSDR receiver, as rtl_tcp on this machine.")
		fmt.Println("Turn RAW IQ on in the web client or the app; it shows a six-character code.")
		for code == "" {
			fmt.Print("Pairing code: ")
			line, err := in.ReadString('\n')
			if err != nil {
				fmt.Fprintln(os.Stderr, "\nvibeiq: no code given.")
				os.Exit(2)
			}
			code = strings.ToLower(strings.TrimSpace(line))
		}
	}

	if err := b.pair(code, host); err != nil {
		fmt.Fprintln(os.Stderr, "vibeiq:", err)
		os.Exit(1)
	}
	// Keep stdin open so a Ctrl-D ends it politely on a terminal.
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
