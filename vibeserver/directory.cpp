#include "directory.h"

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
FILE*               g_tunnel = nullptr;

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
std::string httpPost(const std::string& path, const std::string& json) {
    const std::string cmd =
        "curl -fsS --max-time 20 -X POST -H 'Content-Type: application/json' -d "
        + shellQuote(json) + " " + shellQuote(std::string(kBase) + path) + " 2>/dev/null";
    return run(cmd);
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
                    j += "{\"name\":\"" + jsonStr(r, "label") + "\"";
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
                    const long long rport = jsonNum(r, "port", 0);
                    long long rmax = jsonNum(r, "users", 0);
                    if (rport > 0) {
                        const std::string ri = httpGetLocal((int)rport, "/vibeserver.json");
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
        const std::string r = httpPost("/api/directory/ping", body);
        if (!r.empty()) {
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
        // ★ A ping that fails outright may mean the row is gone. Fall through and register afresh
        //   rather than retrying a dead id for ever.
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
std::string startTunnel(int port) {
    if (g_tunnel) { pclose(g_tunnel); g_tunnel = nullptr; }
    const std::string exe = cloudflaredPath();
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "'%s' tunnel --no-autoupdate --url http://127.0.0.1:%d 2>&1", exe.c_str(), port);
    g_tunnel = popen(cmd, "r");
    if (!g_tunnel) return {};
    char line[1024];
    for (int i = 0; i < 400 && fgets(line, sizeof line, g_tunnel); i++) {
        const char* p = strstr(line, "https://");
        if (p && strstr(p, ".trycloudflare.com")) {
            std::string u(p);
            size_t e = u.find(".trycloudflare.com");
            u = u.substr(0, e + strlen(".trycloudflare.com"));
            while (!u.empty() && (u.back() == '\n' || u.back() == '\r' || u.back() == ' ')) u.pop_back();
            return u;
        }
    }
    return {};
}

void stopTunnel() {
    if (!g_tunnel) return;
    pclose(g_tunnel);
    g_tunnel = nullptr;
    // ★ pclose waits for the child, but cloudflared ignores the pipe closing — make sure.
    run("pkill -f 'tunnel --no-autoupdate --url http://127.0.0.1' 2>/dev/null");
}

void worker() {
    g_thread = true;
    while (g_running) {
        Settings want;
        { std::lock_guard<std::mutex> lk(g_mtx); want = g_want; }
        if (!want.listed || want.port <= 0 || want.name.size() < 2) break;

        std::string url = want.publicUrl;
        if (url.empty()) {
            if (g_tunnelUrl.empty()) g_tunnelUrl = startTunnel(want.port);
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
            publishOnce(want, url);
            std::lock_guard<std::mutex> lk(g_mtx);
            // ★ A renewal must never move the share window — only a deliberate change from the
            //   setup page carries a number, and it has been sent by now.
            g_want.shareForSec = -1;
        }

        // ★★ A NEW TUNNEL IS UNREACHABLE FOR ABOUT NINETY SECONDS — Cloudflare answers 530 for a
        //    hostname the edge has not yet routed. So an unverified listing is retried soon rather
        //    than punished for being new, and only a proven one waits the full interval.
        const long long wait = g_listed ? g_pingSec : 30;
        for (long long t = 0; t < wait && g_running; t++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    g_thread = false;
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
    if (!s.listed) { stop(); return; }
    if (g_running.exchange(true)) return;      // already up; the worker will pick the new wishes up
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
