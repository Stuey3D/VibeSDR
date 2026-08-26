#include "directory.h"
#include "solar.h"   // ★ gridToLatLon — the locator is validated HERE, see worker()

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "local_sdr_shim.h"

namespace vibedir {
namespace {

const char* kBase = "https://vibeserver.vibesdr.net";

std::mutex          g_mtx;
std::string         g_stateDir = "/var/lib/vibeserver";
Settings            g_want;
std::atomic<bool>   g_running{false};
std::atomic<bool>   g_thread{false};
std::string         g_address, g_error, g_tunnelUrl;
bool                g_listed = false;
long long           g_pingSec = 900;

/** ★★★ IS THE TUNNEL STILL THERE? A HOSTNAME IS NOT AN ANSWER TO THAT.
 *
 *  `g_tunnelUrl` was the only thing the worker consulted, and a std::string does not stop being
 *  non-empty when the process behind it exits. After Stuart's internet dropped (2026-08-25) the
 *  Pi kept re-publishing a *.trycloudflare.com name with nothing behind it — worse than being
 *  absent, because the directory went on advertising it as reachable.
 *
 *  ★★ `g_tunnelAlive` is owned by the drain thread and is the honest answer. The generation
 *     counter lets a stale drain thread know it has been superseded, so a tunnel replaced while
 *     its predecessor is still winding down cannot have its liveness cleared by the old one.
 */
std::atomic<bool>     g_tunnelAlive{false};
std::atomic<unsigned> g_tunnelGen{0};
/** ★★ The port is remembered ONLY so the kill can name it. `pkill -f` on the bare
 *     `--url http://127.0.0.1` pattern matched EVERY quick tunnel on the machine, so one radio
 *     process shutting its tunnel down would have taken every other radio's tunnel with it. */
std::atomic<int>      g_tunnelPort{0};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

std::string run(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

/** ★ curl, for the same reason eibi.cpp and geoip.cpp use it: the daemon has no TLS stack of its
 *  own, and the one certainty on both a Pi and a Mac is that curl is there. */
/** ★★★ THE STATUS MATTERS, NOT JUST THE BODY — AND CONFLATING THE TWO COST US THE LISTING.
 *
 *  This ran `curl -f` and returned stdout alone, so "the network is down" and "your row is gone"
 *  were the SAME empty string. publishOnce read that empty string as a dead row, called
 *  clearState() — and the key is sent exactly ONCE and cannot be recovered — then registered
 *  afresh and was refused by the server's own ghost entry: "that address is taken". A thirty
 *  second blip on a 5G or Starlink link therefore destroyed the friendly name permanently.
 *
 *  ★★ So `-f` is gone (an error BODY names its reason, and we want it) and curl appends the status
 *     for us. **0 means we never reached the directory at all** — the one case that must change
 *     nothing, because on a flaky link it is a weather report rather than a verdict.
 */
std::string httpPost(const std::string& path, const std::string& json, int* outStatus = nullptr) {
    if (outStatus) *outStatus = 0;
    const std::string cmd =
        "curl -sS --max-time 20 -X POST -H 'Content-Type: application/json' -d "
        + shellQuote(json) + " -w '\\n%{http_code}' "
        + shellQuote(std::string(kBase) + path) + " 2>/dev/null";
    const std::string out = run(cmd);
    // ★ curl writes the -w line even when the transfer failed (as 000), so a missing one means
    //   curl itself never ran. Both are a transport failure; both leave the status at 0.
    const size_t nl = out.find_last_of('\n');
    if (nl == std::string::npos) return {};
    const std::string code = trim(out.substr(nl + 1));
    if (code.size() != 3) return {};
    if (outStatus) *outStatus = atoi(code.c_str());
    return out.substr(0, nl);
}

std::string httpGetLocal(int port, const std::string& path) {
    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d%s", port, path.c_str());
    // ★★ NAME OURSELVES. /vibeserver/radios counts a landing-page visitor, so an unlabelled poll
    //    would show up on the owner's own admin screen as somebody choosing a radio.
    return run("curl -fsS --max-time 10 -A 'VibeServer-directory' " + shellQuote(url)
               + " 2>/dev/null");
}

// ── the smallest JSON reading that will do ───────────────────────────────────────────────────
// ★ Deliberately not a parser. We read a handful of scalars out of answers WE generate, and the
//   one field that must not be got wrong — the key — is copied whole and never interpreted.
std::string jsonStr(const std::string& j, const std::string& key) {
    const std::string k = "\"" + key + "\":\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    std::string out;
    for (size_t i = p; i < j.size(); i++) {
        if (j[i] == '\\' && i + 1 < j.size()) { out += j[i + 1]; i++; continue; }
        if (j[i] == '"') break;
        out += j[i];
    }
    return out;
}

long long jsonNum(const std::string& j, const std::string& key, long long dflt = 0) {
    const std::string k = "\"" + key + "\":";
    size_t p = j.find(k);
    if (p == std::string::npos) return dflt;
    return atoll(j.c_str() + p + k.size());
}

bool jsonBool(const std::string& j, const std::string& key) {
    const std::string k = "\"" + key + "\":";
    size_t p = j.find(k);
    return p != std::string::npos && j.compare(p + k.size(), 4, "true") == 0;
}

std::string statePath() { return g_stateDir + "/directory.json"; }

void saveState(const std::string& id, const std::string& key, const std::string& slug,
               long long until) {
    std::ofstream f(statePath(), std::ios::trunc);
    if (!f) return;
    f << "{\"id\":\"" << id << "\",\"key\":\"" << key << "\",\"slug\":\"" << slug
      << "\",\"until\":" << until << "}\n";
}

std::string loadState() {
    std::ifstream f(statePath());
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void clearState() { ::remove(statePath().c_str()); }

}  // namespace

// ── what this machine is ─────────────────────────────────────────────────────────────────────

/**
 * ★★★ NEVER INVENTED. Stuart's rule, and it is in AGENTS.md: if the machine cannot tell us, show
 *     nothing. A wrong "Debian 12" on a listing is worse than a blank, because somebody will
 *     believe it.
 */
std::string platformName() {
#if defined(__APPLE__)
    const std::string v = trim(run("sw_vers -productVersion 2>/dev/null"));
    return v.empty() ? "macOS" : "macOS " + v;
#else
    std::ifstream f("/etc/os-release");
    std::string line, name, version, pretty;
    while (std::getline(f, line)) {
        auto val = [&](const char* k) -> std::string {
            const size_t n = strlen(k);
            if (line.compare(0, n, k) != 0) return {};
            std::string v = line.substr(n);
            if (!v.empty() && v.front() == '"') v = v.substr(1, v.find_last_of('"') - 1);
            return v;
        };
        if (auto v = val("PRETTY_NAME=");  !v.empty()) pretty  = v;
        if (auto v = val("NAME=");         !v.empty()) name    = v;
        if (auto v = val("VERSION_ID=");   !v.empty()) version = v;
    }
    // ★ NAME + VERSION_ID ("Debian GNU/Linux" + "13") beats PRETTY_NAME, which carries a codename
    //   nobody outside the distribution recognises — "Debian GNU/Linux 13 (trixie)".
    if (!name.empty() && !version.empty()) {
        std::string n = name;
        const size_t sp = n.find(" GNU/Linux");
        if (sp != std::string::npos) n = n.substr(0, sp);
        return n + " " + version;
    }
    if (!pretty.empty()) return pretty;
    return {};
#endif
}

std::string hostModel() {
#if defined(__APPLE__)
    return trim(run("sysctl -n hw.model 2>/dev/null"));
#else
    // ★ A Pi states its own model here, which is far better than anything we could infer from the
    //   CPU: "Raspberry Pi 500 Rev 1.0".
    std::ifstream f("/proc/device-tree/model", std::ios::binary);
    if (f) {
        std::stringstream ss; ss << f.rdbuf();
        std::string m = ss.str();
        while (!m.empty() && (m.back() == '\0' || m.back() == '\n')) m.pop_back();
        if (!m.empty()) return m;
    }
    const std::string dmi = trim(run("cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null"));
    return dmi;
#endif
}


namespace {

/**
 * The listing's status blob, read from our OWN endpoints over loopback.
 *
 * ★★★ NOT ASSEMBLED FROM CONFIG. /vibeserver.json and /vibeserver/radios are what a listener
 *     actually meets, so a listing built from them cannot describe a server that is not there —
 *     and on a front door the radio list covers receivers owned by OTHER PROCESSES, which no
 *     amount of reading our own config could ever produce.
 * ★★ Two vocabularies meet here and must not be confused: the front door's per-radio `coverage`
 *    is NUMBERS and its `allowedNames` is WORDS, while the directory's `coverage` is words and its
 *    `ranges` is numbers. Getting this backwards printed "500,31000000" on the Android side.
 */
std::string buildStatus(int port) {
    const std::string ident  = httpGetLocal(port, "/vibeserver.json");
    const std::string radios = httpGetLocal(port, "/vibeserver/radios");

    std::string j = "{\"radios\":[";
    // ★ Walk the radio objects by brace depth rather than by regex: the values contain braces.
    size_t i = radios.find("\"radios\":[");
    bool first = true;
    if (i != std::string::npos) {
        int depth = 0; size_t start = 0;
        for (size_t k = radios.find('[', i); k < radios.size(); k++) {
            if (radios[k] == '{') { if (depth++ == 0) start = k; }
            else if (radios[k] == '}') {
                if (--depth == 0) {
                    const std::string r = radios.substr(start, k - start + 1);
                    if (!first) j += ',';
                    first = false;
                    // ★★ THE RADIO'S OWN ID, so a listener's page can ask THIS receiver how
                    //    busy it is rather than waiting on our next ping. The listing's counts are
                    //    up to fifteen minutes old, and a stale FULL turns people away from a
                    //    radio that is free. Already public — it is the /r/<id>/ route the
                    //    landing page links to — so nothing new is disclosed.
                    j += "{\"id\":\"" + jsonStr(r, "id") + "\"";
                    j += ",\"name\":\"" + jsonStr(r, "label") + "\"";
                    j += ",\"driver\":\"" + jsonStr(r, "driver") + "\"";
                    j += ",\"mode\":\"" + jsonStr(r, "mode") + "\"";
                    j += std::string(",\"locked\":") + (jsonBool(r, "locked") ? "true" : "false");
                    j += std::string(",\"shared\":") + (jsonBool(r, "locked") ? "true" : "false");
                    j += std::string(",\"restricted\":")
                       + (jsonBool(r, "restricted") ? "true" : "false");
                    // ★★★ THE FIELDS THE LANDING PAGE ITSELF READS, UNDER THE SAME NAMES. The
                    //     directory row and the receiver's own landing screen were describing the
                    //     same radio in different words — "wfm · one listener at a time" against
                    //     "500 kHz – 1766 MHz · UNRESTRICTED · one listener at a time" (Stuart,
                    //     2026-08-23: "the directory needs to mirror the landing screen"). Passing
                    //     the SAME fields through lets the directory run the SAME logic instead of
                    //     a second approximation of it, which is the only way two pages stay in
                    //     step about what a receiver is.
                    //  ★ The aerial is per RADIO here — one machine can hold three, on three
                    //    different antennas, and the machine-level one cannot say that.
                    j += ",\"antenna\":\"" + jsonStr(r, "antenna") + "\"";
                    j += ",\"centreHz\":" + std::to_string(jsonNum(r, "centreHz", 0));
                    j += ",\"spanHz\":" + std::to_string(jsonNum(r, "spanHz", 0));
                    // ★★★ AND THE CAP THIS RADIO ACTUALLY HAS. The listing carried the SERVER's
                    //     maxUsers for every radio, which on a front door is meaningless: the Pi
                    //     showed "0 OF 1 LISTENING" against receivers configured for ten, so the
                    //     directory advertised three full-looking radios on a machine with room
                    //     for twenty. `users` on the front door is the configured cap, per radio.
                    //  ★★ It is the CAP, not live occupancy — nobody counts listeners per radio in
                    //     the process that publishes this list, because each radio is a separate
                    //     process. Publishing the cap is honest; inventing an occupancy would not
                    //     be. See BRIEF-directory-network-dial.md, step 1.
                    // ★★★ AND WHO IS ACTUALLY ON IT — ASKED, NOT GUESSED. The cap alone made the
                    //     directory say FREE about a receiver its own landing page was calling
                    //     "FULL · FREE IN 25:46" (Stuart, 2026-08-23: "the website lies about
                    //     occupancy"). That is worse than saying nothing: it sends somebody to a
                    //     radio that will refuse them, and it is exactly the mistake this project
                    //     already wrote up as "a client must not decide what only the server
                    //     knows".
                    //  ★★ EACH RADIO IS A SEPARATE PROCESS ON ITS OWN PORT, which is why the front
                    //     door cannot count listeners for them — but it can ASK, over loopback,
                    //     the same way this function already asks for the machine's own figures.
                    //     The landing page does exactly this from the browser; doing it here gets
                    //     the same truth to people who have not clicked yet.
                    //  ★ Once per ping — every fifteen minutes — so the cost is nothing, and a
                    //    radio that does not answer publishes NO occupancy rather than a zero.
                    // ★★★ THROUGH THE FRONT DOOR'S OWN PROXY, NOT THE RADIO'S PORT. `port` in the
                    //     radio list is the LOGICAL port a client is told to use — the radios do
                    //     not listen on it. The door reaches them over unix sockets with
                    //     file-descriptor passing (see fd_passing.h), so probing
                    //     127.0.0.1:<port> connected to nothing and every radio published NO
                    //     occupancy at all: the directory went quiet about counts it could have
                    //     had, on a server that was answering them perfectly well through /r/<id>.
                    //  ★★ /r/<id>/vibeserver.json is exactly what the landing page asks in a
                    //     browser, so it is a route that is PROVEN to work rather than one that
                    //     looks like it should. Over loopback to ourselves it costs nothing.
                    const std::string rid = jsonStr(r, "id");
                    long long rmax = jsonNum(r, "users", 0);
                    if (!rid.empty()) {
                        const std::string ri = httpGetLocal(port, "/r/" + rid + "/vibeserver.json");
                        if (!ri.empty()) {
                            j += ",\"listeners\":" + std::to_string(jsonNum(ri, "listeners", 0));
                            const long long rm = jsonNum(ri, "maxUsers", 0);
                            if (rm > 0) rmax = rm;
                            const long long fi = jsonNum(ri, "freeInSec", -1);
                            if (fi >= 0) j += ",\"freeInSec\":" + std::to_string(fi);
                        }
                    }
                    j += ",\"maxListeners\":" + std::to_string(rmax);
                    // ★★ WORDS in coverage, NUMBERS in ranges — see the note above.
                    auto arr = [&](const std::string& key) -> std::string {
                        const std::string k2 = "\"" + key + "\":";
                        size_t a = r.find(k2);
                        if (a == std::string::npos) return {};
                        a = r.find('[', a);
                        if (a == std::string::npos) return {};
                        int d = 0;
                        for (size_t b = a; b < r.size(); b++) {
                            if (r[b] == '[') d++;
                            else if (r[b] == ']' && --d == 0) return r.substr(a, b - a + 1);
                        }
                        return {};
                    };
                    const std::string names   = arr("allowedNames");
                    const std::string allowed = arr("allowed");
                    const std::string hw      = arr("coverage");
                    // ★★ THREE FIELDS, THREE MEANINGS, THE SAME AS THE FRONT DOOR'S. `coverage` is
                    //    the hardware's reach in Hz, `allowed` is what the owner permits within
                    //    it, `allowedNames` is that in words where the band plan has words. The
                    //    directory used to squash all of it into one word-list called `coverage`,
                    //    which is why it could say less than the landing page no matter how the
                    //    page was written.
                    if (!hw.empty())      j += ",\"coverage\":" + hw;
                    if (!names.empty())   j += ",\"allowedNames\":" + names;
                    if (!allowed.empty()) j += ",\"allowed\":" + allowed;
                    // ★ What the owner PERMITS, falling back to the hardware where no list is set:
                    //   a search must never offer a band the operator has blocked.
                    const std::string ranges = !allowed.empty() ? allowed : hw;
                    if (!ranges.empty()) j += ",\"ranges\":" + ranges;
                    j += "}";
                    if (k + 1 < radios.size() && radios.find('{', k + 1) == std::string::npos) break;
                }
            } else if (radios[k] == ']' && depth == 0) break;
        }
    }
    j += "]";

    j += ",\"listeners\":"    + std::to_string(jsonNum(ident, "listeners", 0));
    j += ",\"maxListeners\":" + std::to_string(jsonNum(ident, "maxUsers", 1));
    j += ",\"limitMin\":"     + std::to_string(jsonNum(ident, "limitMin", 0));
    j += std::string(",\"pin\":") + (jsonBool(ident, "pin") ? "true" : "false");
    // ★ Said, never inferred from a timing value — the directory learnt that the hard way.
    j += ",\"temporary\":false";
    const std::string host = hostModel(), plat = platformName();
    if (!host.empty()) j += ",\"host\":\"" + host + "\"";
    if (!plat.empty()) j += ",\"platform\":\"" + plat + "\"";
    j += "}";
    return j;
}

/** ★ Escapes only what a JSON string may not contain — names come from the owner's own box. */
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) continue;
        else o += c;
    }
    return o;
}

bool publishOnce(const Settings& want, const std::string& url) {
    const std::string status = buildStatus(want.port);
    const std::string state  = loadState();
    const std::string id  = jsonStr(state, "id");
    const std::string key = jsonStr(state, "key");

    // ★★ Re-supplied on every publish: the shim keeps it in memory only, so a radio process that
    //    restarted under us must be given it again before the directory next challenges us.
    if (!key.empty()) vibe::LocalSdrShim::setDirectoryKey(key);

    std::string body;
    if (!id.empty() && !key.empty()) {
        body = "{\"id\":\"" + esc(id) + "\",\"key\":\"" + esc(key) + "\""
             + ",\"url\":\"" + esc(url) + "\",\"grid\":\"" + esc(want.locator) + "\""
             + ",\"name\":\"" + esc(want.name) + "\",\"status\":" + status;
        // ★★★ THREE STATES. -1 omits (a renewal must leave the window alone), 0 is explicitly
        //     permanent, >0 sets one. Two states were not enough on Android: with 0 doing double
        //     duty, turning a temporary share off could not make it permanent.
        if (want.shareForSec >= 0)
            body += ",\"shareForSec\":" + std::to_string(want.shareForSec);
        body += "}";
        int st = 0;
        const std::string r = httpPost("/api/directory/ping", body, &st);
        if (st == 200 && !r.empty()) {
            std::lock_guard<std::mutex> lk(g_mtx);
            const long long ps = jsonNum(r, "pingSec", 0);
            if (ps > 0) g_pingSec = ps;
            g_address = jsonStr(r, "address");
            g_listed  = jsonBool(r, "verified");
            g_error.clear();
            if (!g_listed) g_error = "the directory could not reach this address yet";
            saveState(id, key, jsonStr(state, "slug"), jsonNum(r, "until", 0));
            return true;
        }
        /** ★★★ ONLY A DIRECTORY THAT ACTUALLY ANSWERED MAY COST US OUR IDENTITY.
         *
         *  This cleared the state on ANY empty reply, and an empty reply was also what an
         *  unreachable network produced — so a blip threw away an id and a key that are issued
         *  once and cannot be recovered, and the re-registration that followed was refused by the
         *  server's own ghost entry. That is the friendly name lost to a dropped connection, which
         *  is exactly what a receiver on 5G or Starlink lives with.
         *
         *  ★★ 404 (unknown server) and 403 (bad key) are the only two answers that mean this id
         *     will never work again. A transport failure (0), a rate limit, a 5xx — none of those
         *     are a verdict on our row, so we keep the identity and try again shortly.
         */
        if (st != 404 && st != 403) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_error = st == 0 ? "could not reach the directory"
                              : "the directory could not be updated just now";
            g_listed = false;
            return false;
        }
        clearState();
    }

    body = "{\"name\":\"" + esc(want.name) + "\",\"grid\":\"" + esc(want.locator) + "\""
         + ",\"url\":\"" + esc(url) + "\",\"kind\":\"" + (want.publicUrl.empty() ? "tunnel" : "direct")
         + "\",\"locator\":\"" + esc(want.locator) + "\",\"status\":" + status;
    // ★★★ THE SHARE LENGTH TRAVELS ON THE REGISTRATION TOO. It was on the ping only, and a
    //     temporary share set up from scratch was recorded as PERMANENT — the path every new user
    //     takes first was the one path that could not work.
    if (want.shareForSec >= 0) body += ",\"shareForSec\":" + std::to_string(want.shareForSec);
    body += "}";
    const std::string r = httpPost("/api/directory/register", body);
    std::unique_lock<std::mutex> lk(g_mtx);
    if (r.empty()) { g_error = "could not reach the directory"; g_listed = false; return false; }
    const std::string newKey = jsonStr(r, "key");
    if (newKey.empty()) {
        // ★ 409 and friends come back as a body with a reason; show it rather than a shrug.
        const std::string why = jsonStr(r, "error");
        g_error = why.empty() ? "the directory refused this listing" : why;
        g_listed = false;
        return false;
    }
    saveState(jsonStr(r, "id"), newKey, jsonStr(r, "slug"), 0);
    vibe::LocalSdrShim::setDirectoryKey(newKey);
    g_address = jsonStr(r, "address");
    const long long ps = jsonNum(r, "pingSec", 0);
    if (ps > 0) g_pingSec = ps;
    g_error.clear();

    lk.unlock();
    // ★★★ PING ONCE, IMMEDIATELY, OR A NEW LISTING IS INVISIBLE FOR FIFTEEN MINUTES. The
    //     directory cannot verify at REGISTRATION — we have no key until we read the response — so
    //     proof happens on a ping, and the next is a quarter of an hour away.
    Settings again = want;
    again.shareForSec = -1;          // ★ the window is already set; do not set it twice
    return publishOnce(again, url);
}

/**
 * Where our cloudflared lives.
 *
 * ★★★ OURS FIRST, ALWAYS. We ship a pinned build — beside the binary in a .app, in lib/vibeserver
 *     from the .deb — and that is the one this was tested against. Falling straight to PATH would
 *     mean an owner's own cloudflared (any age, any config, possibly a managed named tunnel with
 *     its own credentials) silently became the thing we drive.
 * ★ PATH remains the LAST resort, so a source build with no fetched binary still works for
 *   somebody who has one installed.
 */
std::string cloudflaredPath() {
    std::vector<std::string> candidates;
#if defined(__APPLE__)
    // ★ Contents/MacOS/cloudflared — beside vibeserver-engine, which is what /proc-less macOS
    //   gives us via _NSGetExecutablePath.
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string self(n, '\0');
    if (_NSGetExecutablePath(&self[0], &n) == 0) {
        self.resize(strlen(self.c_str()));
        const size_t slash = self.find_last_of('/');
        if (slash != std::string::npos) candidates.push_back(self.substr(0, slash) + "/cloudflared");
    }
#else
    char buf[4096];
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string self(buf);
        const size_t slash = self.find_last_of('/');
        if (slash != std::string::npos) candidates.push_back(self.substr(0, slash) + "/cloudflared");
    }
    candidates.push_back("/usr/lib/vibeserver/cloudflared");
    candidates.push_back("/usr/local/lib/vibeserver/cloudflared");
#endif
    for (const auto& c : candidates)
        if (::access(c.c_str(), X_OK) == 0) return c;
    return "cloudflared";        // ★ PATH, and it may not be there — startTunnel says so.
}

/** ★★ A Quick Tunnel's hostname appears ONLY in cloudflared's own output — the edge assigns it —
 *     so the log is parsed rather than merely logged. Same as the Android side. */
void stopTunnel();   // ★ startTunnel retires the previous one first

std::string startTunnel(int port) {
    stopTunnel();
    const std::string exe = cloudflaredPath();
    char cmd[1024];
    /* ★★★ --protocol http2, AND IT IS A MEASURED CHOICE, NOT A PREFERENCE. cloudflared defaults
     *     to QUIC, and QUIC made the waterfall STICKY — not slower, burstier. Measured on spectrum
     *     frame arrivals, 80 frames per run ([[demo_server_live_on_website]]):
     *         QUIC   jitter 97.6 ms   worst stall 773 ms   mean 48.9 ms
     *         http2  jitter 34-44 ms  worst stall 151-324  mean 48.5 ms
     *         LAN    jitter  5.5 ms   worst stall  64 ms   mean 49.2 ms
     *     The mean is identical on every path: this was never latency, it was frames arriving in
     *     clumps. A 773 ms stall is what an audio stutter sounds like.
     * ★★★ AND IT HAD ONLY EVER BEEN APPLIED TO ONE MACHINE. Stuart's Pi carried it in a local
     *     systemd drop-in, so every OTHER VibeServer ever installed ran the transport the
     *     measurement rejected — and a second owner independently reported audio stutters on
     *     Cloudflare (2026-08-26). A fix that lives on the author's own box is not shipped.
     * ★★ APPENDED, NOT INSERTED. stopTunnel() below kills by matching this command line; putting a
     *    flag between `--no-autoupdate` and `--url` would break that match and orphan the tunnel.
     *    If these args are ever reordered, that pattern must move with them. */
    snprintf(cmd, sizeof cmd,
             "'%s' tunnel --no-autoupdate --url http://127.0.0.1:%d --protocol http2 2>&1",
             exe.c_str(), port);
    FILE* f = popen(cmd, "r");
    if (!f) return {};
    const unsigned gen = ++g_tunnelGen;
    g_tunnelPort = port;
    g_tunnelAlive = true;

    std::string found;
    char line[1024];
    for (int i = 0; i < 400 && fgets(line, sizeof line, f); i++) {
        const char* p = strstr(line, "https://");
        if (p && strstr(p, ".trycloudflare.com")) {
            std::string u(p);
            size_t e = u.find(".trycloudflare.com");
            u = u.substr(0, e + strlen(".trycloudflare.com"));
            found = trim(u);
            break;
        }
    }

    /** ★★★ KEEP READING FOR THE PROCESS'S WHOLE LIFE. NOBODY DID, AND THAT ALONE WEDGED IT.
     *
     *  The scrape above stopped at the hostname and the pipe was never read again. cloudflared
     *  went on logging into a 64 KB pipe buffer that nothing emptied, filled it, and blocked for
     *  ever inside write() — so it stopped forwarding. An outage is PRECISELY when it emits a
     *  retry storm, which is why the outage itself wedged the tunnel and why it stayed wedged
     *  long after the link came back.
     *
     *  ★★ This thread also OWNS the pipe and is the only thing that closes it: calling pclose()
     *     while another thread sits in fgets() on the same FILE* is undefined. Reaching EOF here
     *     is therefore the one true report that the child has gone.
     */
    std::thread([f, gen]() {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) { /* discarded on purpose — see the note above */ }
        pclose(f);
        // ★ Only if we are still the current tunnel. A newer one has its own drain thread and its
        //   own liveness, and must not be marked dead by its predecessor finishing.
        if (g_tunnelGen.load() == gen) g_tunnelAlive = false;
    }).detach();

    if (found.empty()) { stopTunnel(); return {}; }
    return found;
}

void stopTunnel() {
    // ★★ Bumping the generation first retires any drain thread still running, so its EOF cannot
    //    clear the liveness of whatever replaces this tunnel.
    g_tunnelGen++;
    g_tunnelAlive = false;
    const int port = g_tunnelPort.exchange(0);
    if (port <= 0) return;
    // ★★★ NAME THE PORT. This killed on the bare `--url http://127.0.0.1` pattern, which matches
    //     EVERY quick tunnel on the box — so one radio process stopping its tunnel would have
    //     taken down every other radio's tunnel too. The drain thread then closes the pipe.
    char pat[192];
    snprintf(pat, sizeof pat,
             "pkill -f 'tunnel --no-autoupdate --url http://127.0.0.1:%d' 2>/dev/null", port);
    run(pat);
}

void worker() {
    g_thread = true;
    int tunnelFails = 0;      // ★ consecutive failures to bring a tunnel up, for the backoff below
    while (g_running) {
        Settings want;
        { std::lock_guard<std::mutex> lk(g_mtx); want = g_want; }
        /* ★★★ AND THE LOCATOR, BECAUSE THE DIRECTORY REQUIRES IT AND WE DID NOT CHECK. Without one
         *     this worker started a tunnel, published, was refused — "a valid Maidenhead locator is
         *     required" — waited thirty seconds and did the whole thing again, for ever. An owner
         *     saw an endless wall of `publish FAILED` and no way to tell it was his own setting
         *     (a second owner's server, 2026-08-26).
         * ★★★ THE COST WAS NOT ONLY NOISE. The tunnel is started BEFORE the publish, so every lap
         *     burned a fresh *.trycloudflare.com and hammered the directory with a request that
         *     could never succeed. A permanent, owner-fixable refusal must not be retried on a
         *     timer — it must stop and SAY SO.
         * ★★ VALIDATED WITH THE SAME PARSER THE REST OF THE PRODUCT USES, so "valid here" and
         *    "valid there" cannot drift apart. Empty counts as invalid: the directory rejects it.
         * ★ Exiting is the right shape, not a giveup: apply() restarts this worker whenever the
         *   settings change, so typing a locator on the setup page picks straight back up. */
        double vlat = 0.0, vlon = 0.0;
        const bool gridOk = !want.locator.empty()
                         && vssolar::gridToLatLon(want.locator, vlat, vlon);
        if (!want.listed || want.port <= 0 || want.name.size() < 2 || !gridOk) {
            // ★★★ THE SILENT EXIT. Three ways to be unpublishable and no way to tell them apart
            //     from outside; a server simply never appeared and nobody could say why.
            std::fprintf(stderr, "[directory] worker exiting: listed=%d port=%d nameLen=%zu grid='%s' ok=%d\n",
                         want.listed ? 1 : 0, want.port, want.name.size(),
                         want.locator.c_str(), gridOk ? 1 : 0);
            // ★★ AND SAY IT WHERE THE OWNER LOOKS. statusJson() feeds the setup page; leaving
            //    g_error untouched here is what made this invisible from the browser.
            if (want.listed && !gridOk) {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_error = want.locator.empty()
                        ? "a Maidenhead locator is required to list this server (e.g. IO83 or IO83xk)"
                        : "'" + want.locator + "' is not a Maidenhead locator (e.g. IO83 or IO83xk)";
                g_listed = false;
            }
            break;
        }

        std::string url = want.publicUrl;
        const bool usingTunnel = url.empty();
        if (usingTunnel) {
            /** ★★★ A DEAD TUNNEL MUST BE REPLACED, NOT REMEMBERED.
             *
             *  This asked only whether we had ever HAD a hostname. g_tunnelUrl is a string, and a
             *  string does not stop being non-empty when the process behind it exits — so after an
             *  outage the condition was false for ever and the tunnel was never restarted. The
             *  worker simply went on re-publishing a name with nothing behind it.
             *
             *  ★★ THE REPLACEMENT HOSTNAME IS A DIFFERENT ONE, AND THAT IS FINE. The edge assigns
             *     it, so a restart always yields a new *.trycloudflare.com. publishOnce PINGS with
             *     whatever url it is given and the directory refreshes it in place — its own note
             *     says "a server may move address between pings — a Quick Tunnel does exactly that
             *     on every restart". The listing and the friendly name are keyed on id+key, so
             *     they survive the cycle untouched. The random name is disposable; the identity is
             *     not.
             */
            if (!g_tunnelAlive || g_tunnelUrl.empty()) {
                g_tunnelUrl = startTunnel(want.port);
                if (g_tunnelUrl.empty()) tunnelFails++;
                else                     tunnelFails = 0;
            }
            url = g_tunnelUrl;
        }
        if (url.empty()) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_error = "cloudflared is not installed, or no tunnel came up";
            g_listed = false;
        } else {
            // ★★ THE LOCK IS NOT HELD ACROSS THE NETWORK CALL. curl can sit for twenty seconds,
            //    and the setup page reads statusJson() on every poll — holding it here would hang
            //    the page for as long as the directory was slow, which reads as the SERVER being
            //    stuck rather than the request.
            const bool ok = publishOnce(want, url);
            // ★★ THE OUTCOME, EVERY TIME. "listed" and the reason are the two things an owner
            //    actually needs, and until now neither left the process.
            {
                std::lock_guard<std::mutex> lk2(g_mtx);
                std::fprintf(stderr, "[directory] publish %s url='%s' listed=%d addr='%s' err='%s'\n",
                             ok ? "ok" : "FAILED", url.c_str(), g_listed ? 1 : 0,
                             g_address.c_str(), g_error.c_str());
            }
            std::lock_guard<std::mutex> lk(g_mtx);
            // ★ A renewal must never move the share window — only a deliberate change from the
            //   setup page carries a number, and it has been sent by now.
            g_want.shareForSec = -1;
        }

        // ★★ A NEW TUNNEL IS UNREACHABLE FOR ABOUT NINETY SECONDS — Cloudflare answers 530 for a
        //    hostname the edge has not yet routed. So an unverified listing is retried soon rather
        //    than punished for being new, and only a proven one waits the full interval.
        long long wait = g_listed ? g_pingSec : 30;
        // ★★ Back off a tunnel that will not start, so a cloudflared that is missing or broken is
        //    not respawned every thirty seconds for ever. Capped at five minutes so a link that
        //    does come back is still picked up promptly.
        if (usingTunnel && tunnelFails > 0) {
            wait = 15LL * tunnelFails;
            if (wait > 300) wait = 300;
        }
        for (long long t = 0; t < wait && g_running; t++) {
            // ★★★ REACT TO A TUNNEL DYING IN ABOUT A SECOND, not in up to a quarter of an hour.
            //     The whole point is that a receiver on a flaky link restores itself unattended;
            //     sleeping out a 900 s ping interval with a dead tunnel is most of an outage.
            if (usingTunnel && !g_tunnelAlive && !g_tunnelUrl.empty()) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    // ★★★ RELEASE THE CLAIM. `apply()` starts a worker only if it can flip g_running false→true,
    //     so a worker that exits while leaving it TRUE makes every subsequent apply() a no-op —
    //     the listing is then dead for the life of the process and nothing says so. The exit paths
    //     above (an unset port during startup, a name not yet chosen) are exactly the ordinary
    //     early-startup states, so this is not a rare corner: it is the normal boot sequence.
    g_running = false;
    g_thread  = false;
    std::fprintf(stderr, "[directory] worker stopped\n");
}

}  // namespace

void setStateDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_stateDir = dir;
}

void apply(const Settings& s) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_want = s;
    }
    // ★★★ SAY WHAT WAS ASKED FOR. This whole subsystem was SILENT — not one log line — and
    //     `statusJson()` (the thing the comment above claims the setup page polls) is called by
    //     NOTHING in the tree, so a server that was refused by the directory, or never asked at
    //     all, looked identical to one that was listed. Two nights were spent guessing at state
    //     that the process knew all along and never said.
    std::fprintf(stderr, "[directory] apply: listed=%d port=%d name='%s' publicUrl='%s'\n",
                 s.listed ? 1 : 0, s.port, s.name.c_str(), s.publicUrl.c_str());
    if (!s.listed) { stop(); return; }
    if (g_running.exchange(true)) {
        // ★★ AND WHETHER IT ACTUALLY STARTED ONE. A worker that exits leaves g_running TRUE, so
        //    every later apply() returns here and the listing is dead for the life of the process
        //    with nothing said. g_thread tells us which of the two this is.
        std::fprintf(stderr, "[directory] apply: worker already claimed (thread alive=%d)\n",
                     g_thread.load() ? 1 : 0);
        return;
    }
    std::fprintf(stderr, "[directory] apply: starting worker\n");
    std::thread(worker).detach();
}

void stop() {
    g_running = false;
    std::string id, key;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const std::string state = loadState();
        id = jsonStr(state, "id"); key = jsonStr(state, "key");
    }
    // ★ Off means OFF: delist NOW so the public address is freed immediately, rather than leaving
    //   a listing to age out for a quarter of an hour after the owner said stop.
    if (!id.empty() && !key.empty())
        httpPost("/api/directory/delist", "{\"id\":\"" + esc(id) + "\",\"key\":\"" + esc(key) + "\"}");
    clearState();
    stopTunnel();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_listed = false; g_address.clear(); g_tunnelUrl.clear(); g_error.clear();
}

std::string statusJson() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return std::string("{\"running\":") + (g_running ? "true" : "false")
         + ",\"listed\":" + (g_listed ? "true" : "false")
         + ",\"address\":\"" + esc(g_address) + "\""
         + ",\"tunnelUrl\":\"" + esc(g_tunnelUrl) + "\""
         + ",\"error\":\"" + esc(g_error) + "\"}";
}

}  // namespace vibedir
