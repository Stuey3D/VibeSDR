// vibe_uberhfdl.h — V12 beta96 follow-up: AN UBERSDR's HFDL ADD-ON AS A FEED SOURCE (docs/v12/ADSB-FEED.md §UberSDR HFDL,
// docs/v12/FEED-TEST.md). Stuart's UberSDR is headless and he cannot edit its docker-compose, so dumphfdl's BaseStation
// push is not an option for him — but the UberSDR already serves its HFDL add-on's aircraft as JSON over plain HTTP:
//     GET http://<ubersdr>:8080/addon/hfdl/aircraft  →  [{"key","icao","reg","flight","lat","lon","freq_khz","gs_id",
//         "msg_count","sig_level","last_seen" (Unix s),"aircraft_time","alt_ft","alt_valid",…}, …]
// (fixture: test/fixtures/uberhfdl/). Nothing to change on the UberSDR: the owner types its address.
//
// ★★★ HFDL, EXACTLY AS AN SBS HFDL SOURCE: each row becomes a vibe_sbs.h Msg into the source's own sbs::Table (label
//     hfdl) — the same rows, the same 30-minute datalink window, the channel (freq_khz) as "HFDL 8942", never counted as
//     ADS-B and never sent to an ADS-B aggregator (only Beast reaches the hub). A row with no ICAO address (keyed by
//     flight or registration), at 0,0, or with a fix older than the window is skipped.
// ★★ POLITE AND BOUNDED: one GET every kPollSec (5 s, a floor — never faster), HTTP/1.0 with Connection: close and no
//    compression asked for, a plain User-Agent naming the software only, connect/read deadlines, the body capped at
//    kMaxBody (2 MB), at most sbs::kMaxRows rows read. On the source's own thread (Network), never audio/DSP.
// ★ Also the small, deadline-bounded network helpers the feed test uses (feednet::resolve / connectTo / httpGet).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vibe_antswitch.h"
#include "vibe_sbs.h"
#include "vibe_thread.h"

namespace vibe {
namespace feednet {

inline double mono() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/** A name lookup with a deadline. An address needs no lookup. ★ getaddrinfo cannot be interrupted, so a NAME is looked up
 *  on a detached helper holding ONLY its own shared state (no server object); at the deadline it is abandoned and frees
 *  itself when it returns. 0 = found; EAI_AGAIN = no answer in time; else getaddrinfo's code. */
inline int resolve(const std::string& host, int port, double secs, const std::function<bool()>& cancelled, std::vector<sockaddr_storage>& out) {
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    const std::string ps = std::to_string(port);
    {
        addrinfo h2 = hints; h2.ai_flags = AI_NUMERICHOST; addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), ps.c_str(), &h2, &res) == 0 && res) {
            for (addrinfo* a = res; a && out.size() < 4; a = a->ai_next) { sockaddr_storage s{}; std::memcpy(&s, a->ai_addr, std::min<size_t>(a->ai_addrlen, sizeof s)); out.push_back(s); }
            ::freeaddrinfo(res); return 0;
        }
        if (res) ::freeaddrinfo(res);
    }
    struct St { std::mutex m; std::condition_variable cv; bool done = false; int rc = EAI_FAIL; std::vector<sockaddr_storage> v; };
    auto st = std::make_shared<St>();
    std::thread([st, host, ps, hints] {
        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), ps.c_str(), &hints, &res);
        std::vector<sockaddr_storage> v;
        for (addrinfo* a = res; rc == 0 && a && v.size() < 4; a = a->ai_next) { sockaddr_storage s{}; std::memcpy(&s, a->ai_addr, std::min<size_t>(a->ai_addrlen, sizeof s)); v.push_back(s); }
        if (res) ::freeaddrinfo(res);
        std::lock_guard<std::mutex> lk(st->m); st->rc = rc; st->v = std::move(v); st->done = true; st->cv.notify_all();
    }).detach();
    const double end = mono() + secs;
    std::unique_lock<std::mutex> lk(st->m);
    while (!st->done && mono() < end && !(cancelled && cancelled())) st->cv.wait_for(lk, std::chrono::milliseconds(100));
    if (!st->done) return EAI_AGAIN;
    if (st->rc == 0) out = st->v;
    return st->rc;
}

/** Non-blocking connect to the first address that answers, within `secs` in all. fd ≥ 0, or -1 and `err` (errno). */
inline int connectTo(const std::vector<sockaddr_storage>& addrs, double secs, const std::function<bool()>& cancelled, int& err) {
    int fd = -1; err = ECONNREFUSED;
    const double end = mono() + secs;
    for (const auto& s : addrs) {
        if (fd >= 0 || (cancelled && cancelled()) || mono() >= end) break;
        const sockaddr* sa = reinterpret_cast<const sockaddr*>(&s);
        fd = ::socket(sa->sa_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
        const socklen_t sl = sa->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        if (::connect(fd, sa, sl) != 0 && errno != EINPROGRESS) { err = errno; ::close(fd); fd = -1; continue; }
        bool done = false;
        while (!done && !(cancelled && cancelled()) && mono() < end) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            int e = 0; socklen_t el = sizeof e; getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
            if (e) { err = e; ::close(fd); fd = -1; }
            done = true;
        }
        if (!done && fd >= 0) { err = ETIMEDOUT; ::close(fd); fd = -1; }
    }
    return fd;
}

/** What one plain-HTTP GET came to. `fail`: "" (an answer, any status) | noname | refused | unreachable | timeout |
 *  toolarge | badhttp | cancelled. */
struct HttpResult { std::string fail, err, body; int status = 0; };

inline bool dechunk(const std::string& in, std::string& out) {
    out.clear(); size_t i = 0;
    for (;;) {
        const size_t e = in.find("\r\n", i);
        if (e == std::string::npos) return false;
        const unsigned long n = std::strtoul(in.substr(i, e - i).c_str(), nullptr, 16);
        i = e + 2;
        if (n == 0) return true;
        if (i + n > in.size()) return false;
        out.append(in, i, n); i += n + 2;
    }
}

/** The request line + headers for one GET (a plain User-Agent naming the software only — no person, no address). */
inline std::string httpRequest(const std::string& host, int port, const std::string& path) {
    const std::string hostHdr = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return "GET " + path + " HTTP/1.0\r\nHost: " + hostHdr + ":" + std::to_string(port)
         + "\r\nUser-Agent: VibeServer (aircraft feed; +https://vibesdr.net)\r\nAccept: application/json"
           "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
}
/** Send a GET on an open socket and read the answer until the far end closes, `end` (mono) or `maxBody`. */
inline HttpResult httpOn(int fd, const std::string& host, int port, const std::string& path, double end, size_t maxBody,
                         const std::function<bool()>& cancelled) {
    HttpResult r;
    const std::string req = httpRequest(host, port, path);
    size_t sent = 0;
    while (sent < req.size() && mono() < end) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0
#ifdef MSG_NOSIGNAL
                                 | MSG_NOSIGNAL
#endif
        );
        if (n > 0) { sent += size_t(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) { pollfd p{fd, POLLOUT, 0}; ::poll(&p, 1, 100); continue; }
        break;
    }
    std::string raw; std::vector<char> buf(16384);
    bool eof = false;
    while (!eof && mono() < end && !(cancelled && cancelled())) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0) continue;
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) { raw.append(buf.data(), size_t(n)); if (raw.size() > maxBody + 16384) { r.fail = "toolarge"; return r; } continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        eof = true;
    }
    if (cancelled && cancelled()) { r.fail = "cancelled"; return r; }
    const size_t he = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/", 0) != 0 || he == std::string::npos) { r.fail = raw.empty() && !eof ? "timeout" : "badhttp"; return r; }
    const size_t sp = raw.find(' ');
    r.status = sp != std::string::npos && sp < he ? std::atoi(raw.c_str() + sp + 1) : 0;
    std::string head = raw.substr(0, he);
    for (char& c : head) c = char(std::tolower((unsigned char)c));
    r.body = raw.substr(he + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) { std::string o; if (!dechunk(r.body, o)) { r.fail = eof ? "badhttp" : "timeout"; return r; } r.body.swap(o); }
    else if (!eof) { r.fail = "timeout"; return r; }
    if (head.find("content-encoding:") != std::string::npos && head.find("content-encoding: identity") == std::string::npos) { r.fail = "badhttp"; r.err = "compressed"; return r; }
    if (r.body.size() > maxBody) { r.fail = "toolarge"; return r; }
    return r;
}
/** ★ GET http://host:port<path>: HTTP/1.0, Connection: close, identity encoding, the body capped at `maxBody`, every step
 *  within `secs` in all. */
inline HttpResult httpGet(const std::string& host, int port, const std::string& path, double secs, size_t maxBody,
                          const std::function<bool()>& cancelled) {
    HttpResult r;
    const double end = mono() + secs;
    std::vector<sockaddr_storage> addrs;
    const int grc = resolve(host, port, std::min(3.0, secs), cancelled, addrs);
    if (grc != 0 || addrs.empty()) { r.fail = "noname"; return r; }
    int e = 0;
    const int fd = connectTo(addrs, std::max(0.2, std::min(3.0, end - mono())), cancelled, e);
    if (fd < 0) { r.fail = e == ECONNREFUSED ? "refused" : "unreachable"; r.err = std::strerror(e); return r; }
    r = httpOn(fd, host, port, path, end, maxBody, cancelled);
    ::close(fd);
    return r;
}

}  // namespace feednet

namespace uberhfdl {

constexpr const char* kPath = "/addon/hfdl/aircraft";
constexpr int kDefaultPort = 8080;
constexpr double kPollSec = 5;              ///< ★ a floor: never faster
constexpr size_t kMaxBody = 2 * 1024 * 1024;
constexpr double kFetchSec = 4;              ///< one GET, all of it

/** What one answer held. */
struct Parsed { size_t rows = 0, withIcao = 0, withPos = 0; std::vector<sbs::Msg> msgs; };

/** The add-on's JSON → Msgs. False (and `err`) when it is not UberSDR HFDL JSON. `nowMs` / `keepMs` = the datalink window:
 *  a fix older than it is skipped (the row is still counted as an aircraft). */
inline bool parse(const std::string& body, int64_t nowMs, int64_t keepMs, Parsed& out, std::string& err) {
    using J = vibe::antswitch::Json;
    /* ★ ELEMENT BY ELEMENT: the list is split at its top level and each object parsed on its own (vibe_antswitch.h's
     *  strict reader) — never the whole 2 MB as one tree, and a junk element costs only itself. */
    size_t i = 0;
    auto ws = [&] { while (i < body.size() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) ++i; };
    ws();
    if (i >= body.size() || body[i] != '[') { err = "not UberSDR HFDL JSON"; return false; }
    ++i; ws();
    bool shaped = i < body.size() && body[i] == ']';
    std::vector<std::string> elems;
    while (i < body.size() && body[i] != ']') {
        if (body[i] != '{') { err = "not UberSDR HFDL JSON"; out = Parsed(); return false; }
        const size_t a = i; int depth = 0; bool str = false, esc = false;
        for (; i < body.size(); ++i) {
            const char c = body[i];
            if (str) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') str = false; continue; }
            if (c == '"') str = true; else if (c == '{' || c == '[') ++depth; else if (c == '}' || c == ']') { if (--depth == 0) { ++i; break; } }
        }
        if (depth != 0 || i - a > 8192) { err = "not UberSDR HFDL JSON"; out = Parsed(); return false; }
        if (elems.size() < sbs::kMaxRows) elems.push_back(body.substr(a, i - a));
        ws();
        if (i < body.size() && body[i] == ',') { ++i; ws(); }
    }
    for (const std::string& es : elems) {
        J e;
        if (!vibe::antswitch::parseJson(es, e) || e.kind != J::Obj) continue;
        const J* ls = e.get("last_seen");
        if (!(e.get("key") || e.get("icao")) || !ls || ls->kind != J::Num) continue;
        if (e.get("freq_khz") || e.get("gs_id") || e.get("msg_count")) shaped = true;
        ++out.rows;
        const std::string hx = e.str("icao");
        uint32_t ic = 0; bool good = hx.size() == 6;
        for (char c : hx) { const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (v < 0) good = false; ic = ic << 4 | uint32_t(v < 0 ? 0 : v); }
        if (!good || !ic || ic == 0xFFFFFF) continue;   // ★ keyed by flight or registration only: not an aircraft we can name
        ++out.withIcao;
        sbs::Msg m; m.type = 3; m.icao = ic;
        { std::string c; for (char ch : e.str("flight")) if (ch > 0x20 && ch < 0x7F && ch != '"' && ch != '\\') c += ch; if (c.size() <= 8) m.callsign = c; }
        { std::string g; for (char ch : e.str("reg")) if (ch > 0x20 && ch < 0x7F && ch != '"' && ch != '\\') g += ch; if (g.size() <= 10) m.reg = g; }
        if (const J* f = e.get("freq_khz"); f && f->kind == J::Num && f->n >= 2000 && f->n <= 30000) m.squawk = int(std::lround(f->n));   // ★ the HF channel (Table: "HFDL 8942")
        if (const J* a = e.get("alt_ft"); a && a->kind == J::Num) {
            const J* av = e.get("alt_valid");
            if ((!av || (av->kind == J::Bool && av->b)) && a->n >= -2000 && a->n <= 100000) { m.hasAlt = true; m.altFt = int(std::lround(a->n)); }
        }
        const int64_t seenMs = int64_t(ls->n) * 1000;
        m.genMs = seenMs > 0 ? seenMs : 0;
        const J* la = e.get("lat"); const J* lo = e.get("lon");
        if (la && lo && la->kind == J::Num && lo->kind == J::Num && std::isfinite(la->n) && std::isfinite(lo->n)
            && std::fabs(la->n) <= 90 && std::fabs(lo->n) <= 180 && !(la->n == 0 && lo->n == 0)
            && seenMs > 0 && nowMs - seenMs <= keepMs && seenMs <= nowMs + 60 * 1000) {
            m.hasPos = true; m.lat = la->n; m.lon = lo->n; ++out.withPos;
        }
        out.msgs.push_back(std::move(m));
    }
    if (!shaped) { err = "not UberSDR HFDL JSON"; out = Parsed(); return false; }
    return true;
}

/** "404: this UberSDR has no HFDL add-on" … — one GET's failure in words ("" = it is the add-on's answer). */
inline std::string failWords(const feednet::HttpResult& h, const std::string& where) {
    if (h.fail == "noname") return "Name not found";
    if (h.fail == "refused") return "Nothing is listening on " + where;
    if (h.fail == "unreachable") return "No route to " + where + ", or it timed out";
    if (h.fail == "timeout") return where + " answered too slowly";
    if (h.fail == "toolarge") return "the answer is over 2 MB \xe2\x80\x94 not UberSDR HFDL JSON";
    if (h.fail == "cancelled") return "stopped";
    if (h.fail == "badhttp") return "not a web server (no HTTP answer)";
    if (h.status == 404) return "404: this UberSDR has no HFDL add-on";
    if (h.status != 200) return "HTTP " + std::to_string(h.status) + " from " + where;
    return "";
}

/** ★ THE POLLER: its own thread, one GET every kPollSec, rows handed to `sink`. Status for the Source's line and down(). */
class Poller {
public:
    using Sink = std::function<void(const Parsed&)>;
    ~Poller() { stop(); }
    void setLog(std::function<void(const std::string&)> f) { std::lock_guard<std::mutex> lk(m_); log_ = std::move(f); }
    void start(const std::string& host, int port, int64_t keepMs, Sink sink, double everySec = kPollSec) {
        stop();
        std::lock_guard<std::mutex> lk(m_);
        host_ = host; port_ = port; keepMs_ = keepMs; sink_ = std::move(sink); every_ = std::max(kPollSec, everySec);
        if (const char* t = std::getenv("VIBE_TEST_UBERHFDL_POLL_SEC")) every_ = std::max(0.2, std::atof(t));   // ★ tests only
        run_ = true; startMono_ = feednet::mono(); okMono_ = 0; state_ = "connecting"; line_.clear(); err_.clear();
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        std::thread t;
        { std::lock_guard<std::mutex> lk(m_); if (!run_) return; run_ = false; t = std::move(th_); }
        if (t.joinable()) t.join();
    }
    std::string state() const { std::lock_guard<std::mutex> lk(m_); return state_; }
    std::string line() const { std::lock_guard<std::mutex> lk(m_); return line_; }
    std::string error() const { std::lock_guard<std::mutex> lk(m_); return err_; }
    /** Seconds since the last good answer (since the start when there has been none). */
    double secsSinceOk() const { std::lock_guard<std::mutex> lk(m_); return feednet::mono() - (okMono_ > 0 ? okMono_ : startMono_); }
    uint64_t polls() const { std::lock_guard<std::mutex> lk(m_); return polls_; }
    uint64_t rowsIn() const { std::lock_guard<std::mutex> lk(m_); return rowsIn_; }
private:
    bool running() const { std::lock_guard<std::mutex> lk(m_); return run_; }
    void loop() {
        vibeNetThread("vibe-uberhfdl");
        std::string lastFail = "\x01";   // ★ nothing said yet: the first answer (good or not) is logged
        while (running()) {
            std::string host; int port; int64_t keep; Sink sink; double every;
            { std::lock_guard<std::mutex> lk(m_); host = host_; port = port_; keep = keepMs_; sink = sink_; every = every_; }
            const double t0 = feednet::mono();
            const std::string where = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
            const auto h = feednet::httpGet(host, port, kPath, kFetchSec, kMaxBody, [this] { return !running(); });
            if (!running()) break;
            std::string why = failWords(h, where);
            Parsed p;
            if (why.empty()) {
                const int64_t nowMs = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                std::string e;
                if (!parse(h.body, nowMs, keep, p, e)) why = e;
            }
            {
                std::lock_guard<std::mutex> lk(m_);
                ++polls_;
                if (why.empty()) {
                    state_ = "connected"; okMono_ = feednet::mono(); err_.clear(); rowsIn_ += p.msgs.size();
                    line_ = "UberSDR HFDL at " + where + ": " + std::to_string(p.withIcao) + " aircraft (" + std::to_string(p.withPos) + " with positions)";
                } else { state_ = "error"; err_ = why; line_ = why; }
            }
            if (why.empty() && sink) sink(p);
            if (why != lastFail) {   // ★ one line at a change, never one per poll
                std::function<void(const std::string&)> f; { std::lock_guard<std::mutex> lk(m_); f = log_; }
                if (f) f(why.empty() ? (lastFail == "\x01" ? "reading " + where + kPath + " every " + std::to_string(int(every)) + " s" : "back: " + where)
                                     : "lost: " + why + " \xe2\x80\x94 trying again every " + std::to_string(int(every)) + " s");
                lastFail = why;
            }
            while (running() && feednet::mono() - t0 < every) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::lock_guard<std::mutex> lk(m_); state_ = "stopped";
    }
    mutable std::mutex m_;
    std::string host_; int port_ = kDefaultPort; int64_t keepMs_ = 30 * 60 * 1000; Sink sink_; double every_ = kPollSec;
    bool run_ = false; std::thread th_;
    std::function<void(const std::string&)> log_;
    std::string state_ = "stopped", line_, err_;
    double startMono_ = 0, okMono_ = 0;
    uint64_t polls_ = 0, rowsIn_ = 0;
};

}  // namespace uberhfdl
}  // namespace vibe
