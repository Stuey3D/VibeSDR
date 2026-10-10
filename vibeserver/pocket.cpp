// pocket.cpp — see pocket.h. The pocket image's Wi-Fi, from the unprivileged daemon's side.
#include "pocket.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace vibepocket {

Paths& paths() {
    // ★ VIBESERVER_POCKET_DIR moves EVERY path under one directory — for driving the real binary in
    //   a test without /etc or /run. The daemon's environment comes from its root-owned unit, so
    //   this hands nothing to anyone who could not already edit that unit.
    static Paths p = [] {
        Paths q;
        if (const char* d = std::getenv("VIBESERVER_POCKET_DIR"); d && *d) {
            const std::string b = d;
            q.marker = b + "/enabled"; q.state = b + "/state.json"; q.inbox = b + "/pocket-wifi.request";
            q.kick = b + "/pocket-kick.request"; q.portFile = b + "/pocket-port";
        }
        return q;
    }();
    return p;
}

bool enabled() {
    struct stat st{};
    // ★ stat, not lstat: the image may make it a link to a file on the boot partition one day;
    //   the DIRECTORY is root's, which is what keeps the daemon from creating it.
    return ::stat(paths().marker.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// ── A small JSON reader ─────────────────────────────────────────────────────────────────────
// ★★ WHY ANOTHER ONE. vibeserver_config's getters are key-scanners built for a flat settings
//    file; an SSID is arbitrary text a stranger's router chose — quotes, braces, "ssid" inside a
//    name — and an array of objects. A key-scanner would match a key INSIDE a value. This is a
//    real (if small) recursive-descent parser, bounded in depth and size.
namespace {
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<JV> a;
    std::vector<std::pair<std::string, JV>> o;
    const JV* get(const char* k) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k) const { const JV* v = get(k); return v && v->t == Str ? v->s : std::string(); }
    bool flag(const char* k) const { const JV* v = get(k); return v && v->t == Bool && v->b; }
    bool has(const char* k) const { return get(k) != nullptr; }
};

struct JP {
    const std::string& s; size_t i = 0; int depth = 0; bool bad = false;
    explicit JP(const std::string& x) : s(x) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
               out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;   case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break;  case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;  case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp; if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {            // a surrogate pair
                        unsigned lo;
                        if (i + 6 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return false;
                        i += 2; if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) return false;
                    utf8(out, cp); break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool val(JV& v) {
        if (++depth > 16) return false;
        ws();
        if (i >= s.size()) return false;
        bool ok = true;
        const char c = s[i];
        if (c == '{') {
            v.t = JV::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') { i++; }
            else for (;;) {
                ws(); std::string k; if (!str(k)) { ok = false; break; }
                ws(); if (i >= s.size() || s[i] != ':') { ok = false; break; }
                i++; JV x; if (!val(x)) { ok = false; break; }
                v.o.emplace_back(std::move(k), std::move(x));
                ws(); if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v.t = JV::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') { i++; }
            else for (;;) {
                JV x; if (!val(x)) { ok = false; break; }
                v.a.push_back(std::move(x));
                ws(); if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                ok = false; break;
            }
        } else if (c == '"') { v.t = JV::Str; ok = str(v.s); }
        else if (s.compare(i, 4, "true") == 0)  { v.t = JV::Bool; v.b = true;  i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.t = JV::Bool; v.b = false; i += 5; }
        else if (s.compare(i, 4, "null") == 0)  { v.t = JV::Null; i += 4; }
        else {
            char* end = nullptr;
            const std::string rest = s.substr(i, 32);
            v.n = std::strtod(rest.c_str(), &end);
            if (end == rest.c_str()) ok = false; else { v.t = JV::Num; i += (size_t)(end - rest.c_str()); }
        }
        depth--;
        return ok;
    }
};

bool parseJson(const std::string& s, JV& out) {
    if (s.size() > 64 * 1024) return false;
    JP p(s);
    if (!p.val(out)) return false;
    p.ws();
    return p.i == s.size();
}

std::string jesc(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::mutex g_accMtx;
std::set<std::string> g_accepted;
} // namespace

// ── Captive portal ────────────────────────────────────────────────────────────────────────
bool isOurHost(const std::string& hostHeader, const std::string& hostname) {
    std::string h = lower(hostHeader);
    while (!h.empty() && (h.back() == ' ' || h.back() == '\r')) h.pop_back();
    if (h.empty()) return true;
    if (h[0] == '[') return true;                          // an IPv6 literal — always an address
    const size_t colon = h.rfind(':');
    if (colon != std::string::npos) h.resize(colon);
    while (!h.empty() && h.back() == '.') h.pop_back();
    if (h.empty() || h == "localhost") return true;
    if (h.find_first_not_of("0123456789.") == std::string::npos) return true;   // IPv4 literal
    if (h.size() > 6 && h.compare(h.size() - 6, 6, ".local") == 0) return true;
    const std::string me = lower(hostname);
    return !me.empty() && h == me;
}

Probe probeFor(const std::string& path) {
    std::string p = path;
    const size_t q = p.find_first_of("?#");
    if (q != std::string::npos) p.resize(q);
    p = lower(p);
    if (p == "/hotspot-detect.html" || p == "/library/test/success.html") return Probe::Apple;
    if (p == "/generate_204" || p == "/gen_204")                           return Probe::Android;
    if (p == "/connecttest.txt")                                           return Probe::MsConnect;
    if (p == "/ncsi.txt")                                                  return Probe::MsNcsi;
    if (p == "/success.txt")                                               return Probe::Firefox;
    if (p == "/canonical.html")                                            return Probe::FirefoxCanonical;
    if (p == "/check_network_status.txt")                                  return Probe::Gnome;
    return Probe::None;
}

bool isPrivatePeer(const std::string& ipIn) {
    std::string ip = lower(ipIn);
    if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);
    if (ip.find(':') != std::string::npos) {
        // IPv6: ULA fc00::/7, link-local fe80::/10. Loopback ::1 is not private here, on purpose.
        if (ip.rfind("fc", 0) == 0 || ip.rfind("fd", 0) == 0) return true;
        if (ip.rfind("fe8", 0) == 0 || ip.rfind("fe9", 0) == 0 || ip.rfind("fea", 0) == 0 || ip.rfind("feb", 0) == 0) return true;
        return false;
    }
    unsigned a = 0, b = 0, c = 0, d = 0; char tail = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;
    return false;                                           // 127/8 included — the tunnel is loopback
}

void acceptClient(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_accMtx);
    if (g_accepted.size() > 256) g_accepted.clear();        // ★ bounded — a hotspot is a handful of phones
    g_accepted.insert(ip);
}
bool clientAccepted(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_accMtx);
    return g_accepted.count(ip) != 0;
}
void clearAccepted() { std::lock_guard<std::mutex> lk(g_accMtx); g_accepted.clear(); }

// ── Validation ──────────────────────────────────────────────────────────────────────────────
bool validSsid(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
bool validPsk(const std::string& p) {
    if (p.size() == 64) {
        bool hex = true;
        for (char c : p) if (!std::isxdigit((unsigned char)c)) { hex = false; break; }
        if (hex) return true;
    }
    if (p.size() < 8 || p.size() > 63) return false;
    for (unsigned char c : p) if (c < 0x20 || c > 0x7E) return false;
    return true;
}
bool validCountry(const std::string& c) {
    return c.size() == 2 && c[0] >= 'A' && c[0] <= 'Z' && c[1] >= 'A' && c[1] <= 'Z';
}

bool parseWifiRequest(const std::string& body, const std::vector<std::string>& saved,
                      bool apSaved, WifiRequest& out, std::string& err) {
    out = WifiRequest{};
    JV j;
    if (!parseJson(body, j) || j.t != JV::Obj) { err = "that was not a valid request"; return false; }
    const JV* nets = j.get("networks");
    if (nets && nets->t != JV::Arr) { err = "networks must be a list"; return false; }
    if (nets && nets->a.size() > 3) { err = "at most three networks can be saved"; return false; }
    std::set<std::string> seen;
    if (nets) for (size_t k = 0; k < nets->a.size(); k++) {
        const JV& n = nets->a[k];
        const std::string which = "Network " + std::to_string(k + 1);
        if (n.t != JV::Obj) { err = which + " is not valid"; return false; }
        Net x;
        x.ssid = n.str("ssid"); x.psk = n.str("psk");
        x.keep = n.flag("keep"); x.hidden = n.flag("hidden");
        if (!validSsid(x.ssid)) { err = which + ": the network name must be 1 to 32 characters"; return false; }
        if (!seen.insert(x.ssid).second) { err = which + ": \"" + x.ssid + "\" is listed twice"; return false; }
        if (x.keep) {
            bool have = false;
            for (const auto& s : saved) if (s == x.ssid) { have = true; break; }
            if (!have) { err = which + ": type the password — this network has none saved yet"; return false; }
            x.psk.clear();
        } else if (!x.psk.empty() && !validPsk(x.psk)) {
            err = which + ": a Wi-Fi password is 8 to 63 characters"; return false;
        }
        out.nets.push_back(std::move(x));
    }
    const JV* ap = j.get("ap");
    if (!ap || ap->t != JV::Obj) { err = "set this box's own hotspot name and password"; return false; }
    out.apSsid = ap->str("ssid"); out.apPsk = ap->str("psk"); out.apKeep = ap->flag("keep");
    if (!validSsid(out.apSsid)) { err = "the hotspot name must be 1 to 32 characters"; return false; }
    // ★★ NOT THE SETUP HOTSPOT'S NAME. A phone that once joined the OPEN "VibeServer" remembers it
    //    as open; a secured network under the same name is a different network to iOS, and the
    //    owner is shown two "VibeServer"s and no way to tell which is which.
    if (lower(out.apSsid) == "vibeserver") { err = "choose a hotspot name other than \"VibeServer\" (that is the open setup hotspot)"; return false; }
    if (seen.count(out.apSsid)) { err = "the hotspot cannot share a name with one of your networks"; return false; }
    if (out.apKeep) {
        if (!apSaved) { err = "type a hotspot password — none is saved yet"; return false; }
        out.apPsk.clear();
    } else if (!validPsk(out.apPsk)) {
        // ★★★ WPA2, ALWAYS — never an open fallback. The open hotspot exists only until setup is done.
        err = "the hotspot password must be 8 to 63 characters"; return false;
    }
    out.country = j.str("country");
    if (!out.country.empty() && !validCountry(out.country)) { err = "the country must be a two-letter code, e.g. GB"; return false; }
    out.switchNow = j.flag("switch");
    return true;
}

std::string inboxJson(const WifiRequest& r) {
    std::string o = "{\"v\":1,\"networks\":[";
    for (size_t k = 0; k < r.nets.size(); k++) {
        const auto& n = r.nets[k];
        o += std::string(k ? "," : "") + "{\"ssid\":\"" + jesc(n.ssid) + "\",\"psk\":\"" + jesc(n.psk)
           + "\",\"keep\":" + (n.keep ? "true" : "false") + ",\"hidden\":" + (n.hidden ? "true" : "false") + "}";
    }
    o += "],\"ap\":{\"ssid\":\"" + jesc(r.apSsid) + "\",\"psk\":\"" + jesc(r.apPsk) + "\",\"keep\":"
       + (r.apKeep ? "true" : "false") + "},\"country\":\"" + jesc(r.country) + "\",\"switch\":"
       + (r.switchNow ? "true" : "false") + "}\n";
    return o;
}

bool writePrivate(const std::string& path, const std::string& content, std::string& err) {
    const std::string tmp = path + ".tmp";
    ::unlink(tmp.c_str());
    // ★ O_EXCL|O_NOFOLLOW: the temp file is always one this call created, never something planted.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { err = std::string("could not write the request (") + std::strerror(errno) + ")"; return false; }
    size_t off = 0;
    while (off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); err = "could not write the request"; return false; }
        off += (size_t)n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); err = "could not submit the request"; return false; }
    return true;
}

// ── State ───────────────────────────────────────────────────────────────────────────────────
State readState() {
    State st;
    const int fd = ::open(paths().state.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return st;
    std::string body; char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0 && body.size() < 64 * 1024) body.append(buf, (size_t)n);
    ::close(fd);
    JV j;
    if (!parseJson(body, j) || j.t != JV::Obj) return st;
    st.present = true;
    st.mode = j.str("mode");
    if (const JV* ap = j.get("ap")) st.apSet = ap->flag("set");
    st.json = body;
    while (!st.json.empty() && (st.json.back() == '\n' || st.json.back() == ' ')) st.json.pop_back();
    return st;
}

std::vector<std::string> savedSsids(const State& s) {
    std::vector<std::string> out;
    JV j;
    if (!s.present || !parseJson(s.json, j)) return out;
    if (const JV* a = j.get("saved"); a && a->t == JV::Arr)
        for (const auto& n : a->a) if (n.t == JV::Obj) out.push_back(n.str("ssid"));
    return out;
}

// ── HTTP ────────────────────────────────────────────────────────────────────────────────────
namespace {
void json(Reply& rp, int code, const char* status, const std::string& body) {
    rp.code = code; rp.status = status; rp.contentType = "application/json"; rp.body = body;
}
void fail(Reply& rp, int code, const char* status, const std::string& why) {
    json(rp, code, status, "{\"error\":\"" + jesc(why) + "\"}");
}

std::string htmlEsc(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
    }
    return o;
}

/* ★★ THE PAGE A CONFIGURED BOX'S HOTSPOT SHOWS IN THE PHONE'S CAPTIVE SHEET. Small on purpose: the
 *  iOS sheet is a limited web view (no audio in the background, no geolocation, closes when the
 *  network goes away), so the receiver itself belongs in Safari or the app. This page's one job is
 *  to say where, and to offer "stay connected" — which makes the probe answer "online" for this
 *  phone, so the sheet's button becomes DONE. Its other button, CANCEL, drops the network. */
std::string welcomePage(const std::string& name, const std::string& base) {
    const std::string n = htmlEsc(name.empty() ? std::string("VibeServer") : name);
    const std::string b = htmlEsc(base);
    return
        "<!doctype html><html lang=en><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>VibeServer</title>"
        "<style>:root{--bg:#080601;--fg:#ffb833;--dim:#b98a3a}"
        "body{background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif;margin:0;padding:24px 16px}"
        "h1{letter-spacing:.2em;font-size:20px;margin:0 0 8px}p{margin:10px 0}.dim{color:var(--dim)}"
        "a.btn,button{display:block;width:100%;box-sizing:border-box;text-align:center;padding:14px;margin:12px 0;"
        "border-radius:10px;border:1px solid var(--fg);background:transparent;color:var(--fg);font:inherit;text-decoration:none}"
        "button.pri{background:var(--fg);color:var(--bg);font-weight:600}code{font-size:15px;word-break:break-all}</style>"
        "<h1>VIBESERVER</h1><p>You are connected to <b>" + n + "</b>.</p>"
        "<button class=pri id=stay>Stay connected</button>"
        "<p class=dim id=after>Then tap <b>Done</b>. Open the <b>VibeSDR app</b> — it finds this box by itself — "
        "or open Safari at:</p><p><code>" + b + "</code></p>"
        "<a class=btn href='" + b + "'>Open the receiver here instead</a>"
        "<p class=dim>This hotspot has no internet. Your phone may use mobile data for everything else.</p>"
        "<script>document.getElementById('stay').onclick=async function(){"
        "try{await fetch('/vibeserver/pocket/accept',{method:'POST',cache:'no-store'});"
        "this.textContent='Done \\u2014 now tap Done at the top';this.disabled=true;}"
        "catch(e){this.textContent='Could not reach the box \\u2014 try again';}};</script></html>";
}
} // namespace

namespace { std::atomic<bool> g_apActive{false}; }
void setApActive(bool on) {
    if (g_apActive.exchange(on) != on) clearAccepted();     // ★ a new hotspot session starts clean
}
bool apActive() { return g_apActive.load(); }

bool handle(const Req& rq, Reply& rp, const Hooks& h) {
    const int port = h.port ? h.port() : 0;
    const std::string base = std::string("http://") + kApAddress + (port > 0 && port != 80 ? ":" + std::to_string(port) : "") + "/";
    const bool isPost = rq.method == "POST";

    if (rq.path.rfind("/vibeserver/pocket/", 0) == 0) {
        const std::string what = rq.path.substr(19);
        if (!enabled()) {
            if (what == "hello") { json(rp, 200, "OK", "{\"pocket\":false}"); return true; }
            fail(rp, 404, "Not Found", "not a pocket VibeServer"); return true;
        }
        const State st = readState();
        if (what == "hello" && !isPost) {
            // ★ Unauthenticated, so it says only what the page needs to choose its first screen.
            const bool claimable = !(h.adminSet && h.adminSet()) && !rq.viaTunnel && isPrivatePeer(rq.peer);
            json(rp, 200, "OK", std::string("{\"pocket\":true,\"claimable\":") + (claimable ? "true" : "false")
                 + ",\"adminSet\":" + ((h.adminSet && h.adminSet()) ? "true" : "false")
                 + ",\"configured\":" + ((h.configured && h.configured()) ? "true" : "false")
                 + ",\"mode\":\"" + jesc(st.mode) + "\",\"apSet\":" + (st.apSet ? "true" : "false")
                 + ",\"apAddress\":\"" + kApAddress + "\",\"port\":" + std::to_string(port)
                 + ",\"hostname\":\"" + jesc(h.hostname ? h.hostname() : std::string()) + "\"}");
            return true;
        }
        if (what == "claim" && isPost) {
            /* ★★★ THE FIRST-RUN PASSWORD, WITHOUT A TERMINAL. On every other install the TUI sets it
             *  (SSH is the credential). Here there is no terminal — so the first person on the box's
             *  own network sets it, exactly once, the way a new router works. Gated three ways:
             *    · no admin password exists yet (once set, this route is dead until a reset);
             *    · the peer is on a PRIVATE network and did not come through the tunnel;
             *    · the box is the pocket image (the marker, checked above).
             *  ★ Sent in clear over the open setup hotspot — there is no TLS on a fresh box. The
             *    window is minutes long and the doc says so; the TUI path over SSH is the same
             *    password in a stronger pipe, which a phone owner does not have. */
            if (h.adminSet && h.adminSet()) { fail(rp, 409, "Conflict", "an admin password is already set — sign in with it"); return true; }
            if (rq.viaTunnel || !isPrivatePeer(rq.peer)) { fail(rp, 403, "Forbidden", "the first password can only be set from the box's own network"); return true; }
            JV j;
            if (!parseJson(rq.body, j) || j.t != JV::Obj) { fail(rp, 400, "Bad Request", "that was not a valid request"); return true; }
            const std::string pass = j.str("pass"), pin = j.str("pin");
            // ★ The TUI's rule, the same number: six characters. Not stricter here than there.
            if (pass.size() < 6) { fail(rp, 400, "Bad Request", "the admin password must be at least 6 characters"); return true; }
            if (pass.size() > 128) { fail(rp, 400, "Bad Request", "that password is too long"); return true; }
            for (unsigned char c : pass) if (c < 0x20 || c == 0x7F) { fail(rp, 400, "Bad Request", "the password has a character it cannot contain"); return true; }
            if (pin.size() > 16 || pin.find_first_not_of("0123456789") != std::string::npos) {
                fail(rp, 400, "Bad Request", "the PIN is digits only, up to 16"); return true;
            }
            std::string err;
            if (!h.claim || !h.claim(pass, pin, err)) { fail(rp, 500, "Internal Server Error", err.empty() ? "could not save" : err); return true; }
            json(rp, 200, "OK", "{\"ok\":true}");
            return true;
        }
        if (what == "accept" && isPost) {
            if (!apActive()) { fail(rp, 409, "Conflict", "the hotspot is not up"); return true; }
            acceptClient(rq.peer);
            json(rp, 200, "OK", "{\"ok\":true}");
            return true;
        }
        if (what == "welcome" && !isPost) {
            rp.code = 200; rp.status = "OK"; rp.contentType = "text/html; charset=utf-8";
            rp.body = welcomePage(h.serverName ? h.serverName() : std::string(), base);
            return true;
        }
        // ── Everything below is the owner's: the admin proof, checked by the shim. ──
        if (!rq.adminOk) { fail(rp, 401, "Unauthorized", "admin password required"); return true; }
        if (what == "wifi" && !isPost) {
            json(rp, 200, "OK", st.present ? st.json : std::string("{\"present\":false}"));
            return true;
        }
        if (what == "wifi" && isPost) {
            WifiRequest w; std::string err;
            if (!parseWifiRequest(rq.body, savedSsids(st), st.apSet, w, err)) { fail(rp, 400, "Bad Request", err); return true; }
            if (!writePrivate(paths().inbox, inboxJson(w), err)) { fail(rp, 500, "Internal Server Error", err); return true; }
            // ★★★ NEVER THE PASSWORDS — the SSIDs and the count are enough to follow in a journal.
            std::printf("VibeServer: pocket Wi-Fi settings saved — %zu network(s), hotspot \"%s\"%s\n",
                        w.nets.size(), w.apSsid.c_str(), w.switchNow ? ", switching now" : "");
            json(rp, 200, "OK", std::string("{\"ok\":true,\"switching\":") + (w.switchNow ? "true" : "false") + "}");
            return true;
        }
        if ((what == "switch" || what == "scan") && isPost) {
            std::string word = what;
            if (what == "scan") {
                JV j;
                if (parseJson(rq.body, j) && j.flag("force")) word = "scan-force";
            }
            std::string err;
            if (!writePrivate(paths().kick, word + "\n", err)) { fail(rp, 500, "Internal Server Error", err); return true; }
            json(rp, 200, "OK", "{\"ok\":true}");
            return true;
        }
        fail(rp, 404, "Not Found", "no such pocket route");
        return true;
    }

    // ── The captive portal: only while our hotspot is up, only for a FOREIGN host ──────────────
    if (!enabled() || !apActive()) return false;
    if (isOurHost(rq.host, h.hostname ? h.hostname() : std::string())) return false;
    if (rq.peer.rfind("127.", 0) == 0 || rq.peer == "::1") return false;
    const Probe p = probeFor(rq.path);
    if (p != Probe::None && clientAccepted(rq.peer)) {
        // ★ The answers each OS expects from its own check, byte for byte. Anything else is "captive".
        rp.code = 200; rp.status = "OK"; rp.contentType = "text/plain";
        switch (p) {
            case Probe::Apple:
                rp.contentType = "text/html";
                rp.body = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"; break;
            case Probe::Android: rp.code = 204; rp.status = "No Content"; rp.body.clear(); break;
            case Probe::MsConnect: rp.body = "Microsoft Connect Test"; break;
            case Probe::MsNcsi:    rp.body = "Microsoft NCSI"; break;
            case Probe::Firefox:   rp.body = "success\n"; break;
            case Probe::FirefoxCanonical:
                rp.contentType = "text/html";
                rp.body = "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>"; break;
            case Probe::Gnome:     rp.body = "NetworkManager is online\n"; break;
            case Probe::None: break;
        }
        return true;
    }
    // ★★ NOT SET UP YET ⇒ THE SETUP PAGE ITSELF opens in the sheet (Stuart: "the setup page is the
    //    EXISTING setup wizard"). Set up ⇒ the small welcome page, because the receiver belongs in
    //    Safari or the app, not in a sheet that closes when the phone looks away.
    const bool configured = h.configured && h.configured();
    rp.code = 302; rp.status = "Found"; rp.contentType = "text/html";
    rp.location = configured ? base + "vibeserver/pocket/welcome" : base;
    rp.body = "<a href=\"" + htmlEsc(rp.location) + "\">VibeServer</a>";
    return true;
}

} // namespace vibepocket
