// pocket.h — the POCKET VIBESERVER's Wi-Fi side, as the daemon sees it (2026-10-10).
//
// ★★★ WHAT THIS IS. A Pi 3 A+ (or any small Pi) flashed with the pocket image has no screen, no
// keyboard and — for an iPhone owner — no terminal. It raises an OPEN "VibeServer" hotspot on
// first boot, the phone's captive-network sheet opens the EXISTING setup page, and everything —
// the admin password, the radios, up to three Wi-Fi networks in strict order, and the box's own
// fallback hotspot — is set there. See docs/POCKET-VIBESERVER-PI3A.md for the whole design.
//
// ★★★ THE DAEMON NEVER TOUCHES THE WI-FI. It runs unprivileged under NoNewPrivileges, exactly as
// on every other install. It WRITES A REQUEST into its own state directory and a root service
// (vibeserver-pocket, only enabled on the pocket image) reads it, re-validates every field, and
// drives NetworkManager. Same boundary as vibeserver-maintenance, one layer over: a compromised
// daemon can ask for "these three networks and this hotspot" and nothing else.
//
// ★★★ OFF EVERYWHERE ELSE. Nothing here does anything unless the image's marker file exists
// (/etc/vibeserver-pocket/enabled — root's directory, which the daemon cannot write to, so the
// daemon cannot switch pocket mode on for itself). A desktop or Pi install never sees a route,
// a card or a captive redirect.
//
// ★ No dependency on the shim: main.cpp adapts the shim's request to Req/Reply below, so this
//   file — the host rules, the probe answers, the validation — is testable on its own
//   (test-pocket.cpp).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace vibepocket {

/** Where everything lives. Overridable so the tests never touch /etc or /run. */
struct Paths {
    std::string marker   = "/etc/vibeserver-pocket/enabled";         // root's; written by the image
    std::string state    = "/run/vibeserver-pocket/state.json";      // root writes, we read (0640 :vibeserver)
    std::string inbox    = "/var/lib/vibeserver/pocket-wifi.request"; // we write (0600), root reads + deletes
    std::string kick     = "/var/lib/vibeserver/pocket-kick.request"; // one word: switch | scan | scan-force
    std::string portFile = "/var/lib/vibeserver/pocket-port";         // our front door's port, for the :80 redirect
};
Paths& paths();

/** The pocket image's marker is present. Checked on each call (a stat): cheap, and honest. */
bool enabled();

// ── Captive portal ────────────────────────────────────────────────────────────────────────
/** ★★ "Is this request addressed to US?" An IP literal, localhost, anything under .local, or the
 *  machine's own hostname is ours. Anything else, while the hotspot is up, can only have reached
 *  us through the DNS hijack — captive.apple.com, connectivitycheck.gstatic.com — and is a probe
 *  or a stray page load to redirect. Empty Host (HTTP/1.0) counts as ours: never redirect what
 *  we cannot classify. */
bool isOurHost(const std::string& hostHeader, const std::string& hostname);

enum class Probe { None, Apple, Android, MsConnect, MsNcsi, Firefox, FirefoxCanonical, Gnome };
/** Which OS's connectivity check this path is (host already known to be foreign). */
Probe probeFor(const std::string& path);

/** ★ A peer on a private network — RFC 1918, link-local, IPv6 ULA/link-local — and NOT loopback
 *  (the tunnel arrives as loopback). The first-run password may only be claimed from here. */
bool isPrivatePeer(const std::string& ip);
/** ★★ "weak" when the joined link is poor enough that Own Wi-Fi is worth recommending — signal −75 dBm or worse, a
 *  transmit rate of 11 Mbit/s or less, or 10 % or more of transmissions retried; "" otherwise. ONE rule, decided here,
 *  so the app and the web page cannot disagree (Stuart, 2026-10-10). A figure of −1 is "not reported" and never counts. */
std::string linkAdvice(double signal, double tx, double retry);

/** Clients that have pressed "stay connected" — their probes are answered as online, so iOS
 *  shows "Done" instead of "Cancel" (which would drop the network). Cleared when the mode changes. */
void acceptClient(const std::string& ip);
bool clientAccepted(const std::string& ip);
void clearAccepted();
/** ★ The hotspot is up (main.cpp sets it from the root service's state). Captive redirects happen
 *  only while it is; a change of mode forgets every "stay connected". */
void setApActive(bool on);
bool apActive();

// ── Validation (the root service re-checks every one of these; this is for a quick answer) ──
bool validSsid(const std::string& s);                  // 1–32 bytes, no control characters
bool validPsk(const std::string& p);                   // 8–63 printable ASCII, or 64 hex
bool validCountry(const std::string& c);               // two capital letters

struct Net { std::string ssid, psk; bool keep = false, hidden = false; };
struct WifiRequest {
    std::vector<Net> nets;                              // IN ORDER — index 0 is always tried first
    std::string apSsid, apPsk;
    bool apKeep = false;                                // keep the hotspot password already saved
    std::string country;                                // "" = leave as is
    bool switchNow = false;                             // leave the hotspot and try the networks now
};
/** Parse + validate what the setup page sent. `savedSsids` = what the box already holds, so a
 *  "keep" (no password retyped) is accepted only for a network that really has one saved. */
bool parseWifiRequest(const std::string& body, const std::vector<std::string>& savedSsids,
                      bool apSaved, WifiRequest& out, std::string& err);
/** The inbox document the root service reads. Contains passwords — written 0600, never logged. */
std::string inboxJson(const WifiRequest& r);

/** Write atomically (tmp + rename), mode 0600, refusing to follow a link at the destination. */
bool writePrivate(const std::string& path, const std::string& content, std::string& err);

// ── State, as the root service last reported it ─────────────────────────────────────────────
struct State {
    bool present = false;        // the file exists and parsed
    std::string mode;            // setup-ap | fallback-ap | client | connecting | off
    bool apSet = false;          // a secured fallback hotspot is configured
    bool internet = false;       // NetworkManager says connectivity is FULL (the tunnel may run)
    bool hold = false;           // ★ the owner chose Own Wi-Fi (the root service holds it while the radio is used)
    bool hasLink = false;        // joined to a network, with the link's figures below (-1 = not reported)
    double signal = -1, tx = -1, rx = -1, retry = -1;
    std::string ssid, apSsid;
    std::string json;            // the whole document (no passwords in it, by construction)
};
State readState();
inline bool apMode(const State& s) { return s.mode == "setup-ap" || s.mode == "fallback-ap"; }
/** The saved networks' SSIDs, in order, from the state document. */
std::vector<std::string> savedSsids(const State& s);

// ── HTTP: everything under /vibeserver/pocket/, plus captive probes ───────────────────────────
struct Req {
    std::string method, path, host, peer, body;
    bool viaTunnel = false;
    bool adminOk = false;        // the shim checked the admin proof (same handshake as /config)
};
struct Reply {
    int code = 200;
    std::string status = "OK";
    std::string contentType = "application/json";
    std::string location;        // set for a 302
    std::string body;
};
struct Hooks {
    std::function<bool()> configured;                                  // setup finished?
    std::function<bool()> adminSet;                                    // an admin password exists?
    std::function<bool(const std::string& pass, const std::string& pin, std::string& err)> claim;
    std::function<int()>  port;                                        // the front door's port
    std::function<std::string()> hostname;
    std::function<std::string()> serverName;                           // for the welcome page
};
/** Returns true when it answered. False = not ours; the shim carries on routing. */
bool handle(const Req& rq, Reply& rp, const Hooks& h);

/** The fixed address the hotspot gives this box (NetworkManager shared mode, pinned by the root
 *  service so this can never drift from it). */
constexpr const char* kApAddress = "10.42.0.1";

} // namespace vibepocket
