// vibe_spot_report.h — V12 beta90: THIS SERVER'S DIGITAL SPOTS TO PSK REPORTER (and its WSPR spots to WSPRnet), and the
// server's own RANKING read back from the directory (docs/v12/PSK-REPORTER.md).
//
// ★★★ WRITTEN FROM THE PUBLISHED INTERFACES, OUR OWN CODE:
//   · PSK Reporter: the developer page https://pskreporter.info/pskdev.html (read 2026-11-21) — IPFIX (RFC 5101 / 7011)
//     over UDP to report.pskreporter.info:4739. The header, the template sets, the variable-length strings, the 4-byte
//     padding and the enterprise number 30351 are as that page lays them out; test-spot-report checks our encoder
//     byte for byte against the page's own complete example datagram.
//   · WSPRnet: the upload form on wsprnet.org/olddb and its published "How can I automate this?" note
//     (wsprnet.org/automate.txt): an HTTP POST of a file element "allmept" with "call" and "grid", to
//     http://wsprnet.org/meptspots.php. No account. The lines are the ALL_MEPT / ALL_WSPR text format.
//
// ★★★ THE PAGE'S RULES ARE THE DESIGN:
//   · "each callsign should be reported no more than once per five minute period. Ideally … only once per hour if it has
//     not 'changed' … A change might be a move to a different band" → one record per callsign per batch, and the same
//     callsign + band + mode not again for an hour.
//   · "no more than one [datagram] every five minutes (unless the packet becomes full)" and "Timed sends … must not be
//     synchronized to the system clock … based on when the program started" → a batch every 5 minutes from when the
//     reporter started, plus a random 0–60 s, never on the clock's minute; a full datagram splits into several.
//   · "The record format descriptors … in the first three datagrams … and then once per hour" → as written.
//   · "the same UDP source port number should be used" → one socket for the reporter's life.
//   · "informationSource … must be set to the value 1 ('automatically extracted')" → 1.
//   · "The receiver information record should appear in each datagram" → it does.
//
// ★★★ NEVER ANYTHING ABOUT LISTENERS. A record names the station HEARD (callsign, its own locator if it sent one, mode,
//     frequency, SNR, slot time) and the receiver (the owner's callsign, the server's grid, the software, the antenna).
//     Who was listening, from where, or whether anyone was, is never in a datagram — the spots come from the decoders.
//
// ★ Header-only, no dependencies beyond POSIX sockets: the Android app's server, the Linux/Mac daemon and the tests
//   compile it as it is. HTTPS (the ranking) goes through a fetcher the host sets (curl on the daemon, VibeHttp on Android).
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

namespace vibe {
namespace spotreport {

// ══ IPFIX, AS pskdev.html LAYS IT OUT ═════════════════════════════════════════════════════════════════════════════
constexpr uint32_t kPen = 30351;              ///< PSK Reporter's enterprise number ("registered to me", the page)
constexpr uint16_t kVar = 0xFFFF;             ///< variable length: a length byte, then the bytes
// The page's attribute ids (enterprise-specific unless noted).
enum : uint16_t {
    F_SENDER_CALL = 1, F_RECEIVER_CALL = 2, F_SENDER_LOC = 3, F_RECEIVER_LOC = 4, F_FREQ = 5, F_SNR = 6, F_IMD = 7,
    F_SOFTWARE = 8, F_ANTENNA = 9, F_MODE = 10, F_INFO_SOURCE = 11,
    F_FLOW_START = 150,                       ///< IANA flowStartSeconds — NOT enterprise-specific
};
struct Field { uint16_t id; uint16_t len; bool ent; };
/** A record format descriptor. `options`: the receiver's — an options template set (set id 3, one scope field). */
struct Template { uint16_t id = 0; bool options = false; std::vector<Field> fields; };

/** ★ Our template ids. "arbitrary two byte values in the range 256 to 65535 … advisable to have distinct values for
 *  different record format descriptors" — not the page's example 0x9992 / 0x9993, so a machine running another program
 *  with those never has its descriptors confused with ours. */
constexpr uint16_t kRxTemplateId = 0x5652;    // "VR"
constexpr uint16_t kTxTemplateId = 0x5653;    // "VS"

/** The receiver record: receiverCallsign, receiverLocator, decoderSoftware, antennaInformation (the page's second). */
inline Template receiverTemplate(uint16_t id = kRxTemplateId, bool antenna = true) {
    Template t; t.id = id; t.options = true;
    t.fields = { {F_RECEIVER_CALL, kVar, true}, {F_RECEIVER_LOC, kVar, true}, {F_SOFTWARE, kVar, true} };
    if (antenna) t.fields.push_back({F_ANTENNA, kVar, true});
    return t;
}
/** ★ The sender record: senderCallsign, frequency (4 bytes), sNR (1 byte), mode, informationSource (1 byte),
 *  senderLocator, flowStartSeconds. The page's fourth descriptor without iMD: the page's field definitions and layout,
 *  and the starred (optional) fields left out where we have no measurement — we never send an IMD we did not measure.
 *  (IPFIX is self-describing; the page invites a descriptor that fits the software. docs/v12/PSK-REPORTER.md.) */
inline Template senderTemplate(uint16_t id = kTxTemplateId) {
    Template t; t.id = id;
    t.fields = { {F_SENDER_CALL, kVar, true}, {F_FREQ, 4, true}, {F_SNR, 1, true}, {F_MODE, kVar, true},
                 {F_INFO_SOURCE, 1, true}, {F_SENDER_LOC, kVar, true}, {F_FLOW_START, 4, false} };
    return t;
}

struct Receiver { std::string call, locator, software, antenna; };
/** One reception record. */
struct Report {
    std::string call, locator, mode;
    uint64_t hz = 0;
    int snr = 0;
    uint32_t t = 0;                 ///< flowStartSeconds: when the transmission was heard (its slot's start)
    uint8_t infoSource = 1;         ///< 1 = automatically extracted (the page: "really must be" 1)
};

inline void put8(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)); }
inline void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
inline void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }
inline void set16(std::vector<uint8_t>& o, size_t at, uint32_t v) { o[at] = uint8_t(v >> 8); o[at + 1] = uint8_t(v); }
inline void pad4(std::vector<uint8_t>& o, size_t from) { while ((o.size() - from) % 4) o.push_back(0); }
/** "fields of byte length code followed by UTF-8 … limited to a length code of no more than 254 bytes". */
inline void putStr(std::vector<uint8_t>& o, const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), 254);
    o.push_back(uint8_t(n));
    o.insert(o.end(), s.begin(), s.begin() + (long)n);
}
inline void putUint(std::vector<uint8_t>& o, uint64_t v, uint16_t len) {
    for (int i = len - 1; i >= 0; --i) o.push_back(uint8_t(v >> (8 * i)));
}

/** A template set: "00 02" (or "00 03" with one scope field for the receiver's), its length, the template id, the field
 *  count, then each field — an enterprise field's id has the top bit set and is followed by the enterprise number. */
inline void putTemplateSet(std::vector<uint8_t>& o, const Template& t) {
    const size_t at = o.size();
    put16(o, t.options ? 3 : 2); put16(o, 0);
    put16(o, t.id); put16(o, (uint32_t)t.fields.size());
    if (t.options) put16(o, 1);
    for (const auto& f : t.fields) {
        put16(o, f.ent ? (0x8000u | f.id) : f.id); put16(o, f.len);
        if (f.ent) put32(o, kPen);
    }
    pad4(o, at);
    set16(o, at + 2, (uint32_t)(o.size() - at));
}
inline void putField(std::vector<uint8_t>& o, const Field& f, const Receiver* rx, const Report* r) {
    auto str = [&](const std::string& s) { putStr(o, s); };
    switch (f.id) {
        case F_RECEIVER_CALL: str(rx ? rx->call : ""); return;
        case F_RECEIVER_LOC:  str(rx ? rx->locator : ""); return;
        case F_SOFTWARE:      str(rx ? rx->software : ""); return;
        case F_ANTENNA:       str(rx ? rx->antenna : ""); return;
        case F_SENDER_CALL:   str(r ? r->call : ""); return;
        case F_SENDER_LOC:    str(r ? r->locator : ""); return;
        case F_MODE:          str(r ? r->mode : ""); return;
        case F_FREQ:          putUint(o, r ? r->hz : 0, f.len); return;
        case F_SNR:           putUint(o, r ? (uint64_t)(uint8_t)(int8_t)std::max(-128, std::min(127, r->snr)) : 0, f.len); return;
        case F_IMD:           putUint(o, 0, f.len); return;
        case F_INFO_SOURCE:   putUint(o, r ? r->infoSource : 1, f.len); return;
        case F_FLOW_START:    putUint(o, r ? r->t : 0, f.len); return;
        default:              if (f.len == kVar) str(""); else putUint(o, 0, f.len); return;
    }
}
inline size_t recordBytes(const Template& t, const Report& r) {
    std::vector<uint8_t> tmp; for (const auto& f : t.fields) putField(tmp, f, nullptr, &r); return tmp.size();
}

/** ONE datagram: the 16-byte header ("00 0A ll ll tt tt tt tt ss ss ss ss ii ii ii ii"), the two descriptors when
 *  `templates`, the receiver record block, then the sender records block. `seq` is the header's sequence number. */
inline std::vector<uint8_t> encodeDatagram(const Template& rxT, const Receiver& rx, const Template& txT,
                                           const std::vector<Report>& reports, uint32_t exportTime, uint32_t seq,
                                           uint32_t domain, bool templates) {
    std::vector<uint8_t> o;
    put16(o, 10); put16(o, 0); put32(o, exportTime); put32(o, seq); put32(o, domain);
    if (templates) { putTemplateSet(o, rxT); putTemplateSet(o, txT); }
    { const size_t at = o.size(); put16(o, rxT.id); put16(o, 0);
      for (const auto& f : rxT.fields) putField(o, f, &rx, nullptr);
      pad4(o, at); set16(o, at + 2, (uint32_t)(o.size() - at)); }
    if (!reports.empty()) {
        const size_t at = o.size(); put16(o, txT.id); put16(o, 0);
        for (const auto& r : reports) for (const auto& f : txT.fields) putField(o, f, nullptr, &r);
        pad4(o, at); set16(o, at + 2, (uint32_t)(o.size() - at));
    }
    set16(o, 2, (uint32_t)o.size());
    return o;
}

/** A batch → datagrams of at most `maxBytes` each (the records split across them, each carrying the receiver record).
 *  `seq` is advanced by the DATA RECORDS sent (RFC 7011: "all IPFIX Data Records sent … prior to … this Message"; the
 *  page: "the count of reports and not the count of packets"). `templatesLeft`: how many of the datagrams from here
 *  carry the descriptors (the first three ever, then one an hour — the caller decides). */
inline std::vector<std::vector<uint8_t>> buildDatagrams(const Receiver& rx, const std::vector<Report>& reports, uint32_t exportTime,
                                                        uint32_t& seq, uint32_t domain, int& templatesLeft, size_t maxBytes = 1400) {
    const Template rxT = receiverTemplate(), txT = senderTemplate();
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    do {
        const bool tpl = templatesLeft > 0;
        std::vector<Report> part;
        // the fixed part's size, then records while they fit
        size_t used = encodeDatagram(rxT, rx, txT, {}, exportTime, seq, domain, tpl).size() + 4 + 3;
        while (i < reports.size()) {
            const size_t b = recordBytes(txT, reports[i]);
            if (!part.empty() && used + b > maxBytes) break;
            part.push_back(reports[i++]); used += b;
        }
        out.push_back(encodeDatagram(rxT, rx, txT, part, exportTime, seq, domain, tpl));
        seq += (uint32_t)part.size() + 1;   // the receiver record is a data record too
        if (tpl) --templatesLeft;
    } while (i < reports.size());
    return out;
}

// ══ WHAT IS SENT, AND WHAT IS NOT ═════════════════════════════════════════════════════════════════════════════════
/** "One of the ADIF values for MODE or SUBMODE" (the page). Our decoders' names → ADIF; "" = not an ADIF mode, never
 *  sent (FT2 and JTTY have no ADIF name yet — a made-up mode would be stored as junk). CW is ADIF "CW". */
inline std::string adifMode(const std::string& m) {
    std::string u; for (char c : m) u += (char)std::toupper((unsigned char)c);
    if (u == "CW") return "CW";   // ★ V12 beta90: CW skimmer / CW decoder spots (Stuart 2026-11-21)
    if (u == "FT8" || u == "FT4" || u == "JS8" || u == "WSPR" || u == "MSK144" || u == "PSK31" || u == "PSK63"
        || u == "FST4" || u == "FST4W" || u == "JT65" || u == "JT9") return u;
    if (u.rfind("Q65", 0) == 0) return "Q65";
    if (u.rfind("FST4W", 0) == 0) return "FST4W";
    if (u.rfind("FST4", 0) == 0) return "FST4";
    return "";
}
/** A callsign as the ITU shapes them, loosely: 3–15 of A–Z 0–9 /, at least one letter and one digit, not all digits
 *  around the slash. Spots are off the air: anything else is not a callsign and is not reported. */
inline bool plausibleCall(const std::string& c) {
    if (c.size() < 3 || c.size() > 15) return false;
    bool letter = false, digit = false;
    for (char ch : c) {
        if (ch >= 'A' && ch <= 'Z') letter = true;
        else if (ch >= '0' && ch <= '9') digit = true;
        else if (ch != '/') return false;
    }
    if (c.front() == '/' || c.back() == '/' || c.find("//") != std::string::npos) return false;
    return letter && digit;
}
/** A 4- or 6-character Maidenhead locator (AA00 / AA00aa, any case), or "". */
inline std::string cleanGrid(const std::string& g) {
    if (g.size() != 4 && g.size() != 6) return "";
    auto up = [](char c) { return (char)std::toupper((unsigned char)c); };
    auto lo = [](char c) { return (char)std::tolower((unsigned char)c); };
    std::string o;
    const char a = up(g[0]), b = up(g[1]);
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || !std::isdigit((unsigned char)g[2]) || !std::isdigit((unsigned char)g[3])) return "";
    o += a; o += b; o += g[2]; o += g[3];
    if (g.size() == 6) {
        const char c = lo(g[4]), d = lo(g[5]);
        if (c < 'a' || c > 'x' || d < 'a' || d > 'x') return "";
        o += c; o += d;
    }
    if (o == "RR73") return "";
    return o;
}
/** Maidenhead to 6 characters from a position ("" off the globe). */
inline std::string gridOf(double lat, double lon) {
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return "";
    double x = std::min(359.9999, lon + 180), y = std::min(179.9999, lat + 90);
    std::string g;
    g += char('A' + int(x / 20)); g += char('A' + int(y / 10));
    g += char('0' + int(std::fmod(x, 20) / 2)); g += char('0' + int(std::fmod(y, 10)));
    g += char('a' + int(std::fmod(x, 2) * 12)); g += char('a' + int(std::fmod(y, 1) * 24));
    return g;
}
/** The amateur band name a frequency is in ("40m"), for the once-an-hour rule and the ranks; "" outside them. */
inline const char* bandOf(double hz) {
    struct B { double lo, hi; const char* n; };
    static const B b[] = {
        {135.7e3, 137.8e3, "2200m"}, {472e3, 479e3, "630m"}, {1.8e6, 2.0e6, "160m"}, {3.5e6, 4.0e6, "80m"},
        {5.25e6, 5.45e6, "60m"}, {7.0e6, 7.3e6, "40m"}, {10.1e6, 10.15e6, "30m"}, {14.0e6, 14.35e6, "20m"},
        {18.068e6, 18.168e6, "17m"}, {21.0e6, 21.45e6, "15m"}, {24.89e6, 24.99e6, "12m"}, {28.0e6, 29.7e6, "10m"},
        {50e6, 54e6, "6m"}, {70e6, 71e6, "4m"}, {144e6, 148e6, "2m"}, {420e6, 450e6, "70cm"}, {1240e6, 1300e6, "23cm"},
    };
    for (const auto& x : b) if (hz >= x.lo && hz <= x.hi) return x.n;
    return "";
}
/** The RECEIVER's name, as the owner typed it: letters (case kept), digits, '/' and '-', ≤ 20. ★★ Not a callsign check —
 *  that is plausibleCall, for the stations HEARD. PSK Reporter lists monitors by any name (its own top lists carry
 *  "WESSEX", "CMWEBSDR", "PI4NOS/LPK"), and Stuart reports as "Stuey3D-VibeSDR" beside "stuey3d" from his UberSDR
 *  (2026-11-21) — this used to upper-case it and strip the '-' silently ("STUEY3DVIBESDR"). */
constexpr size_t kMaxReceiverName = 20;
/** A HEARD station's call, before plausibleCall judges it: upper case, A–Z 0–9 / only, ≤ 15 (the strict rule stays for
 *  what came off the air — only the receiver's own name is free text, cleanCall). */
inline std::string cleanHeardCall(const std::string& s) {
    std::string o;
    for (char c : s) { const char u = (char)std::toupper((unsigned char)c); if (((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '/') && o.size() < 15) o += u; }
    return o;
}
inline std::string cleanCall(const std::string& s) {
    std::string o;
    for (char c : s) {
        const unsigned char u = (unsigned char)c;
        if ((std::isalnum(u) || c == '/' || c == '-') && o.size() < kMaxReceiverName) o += c;
    }
    return o;
}
/** Free text for a datagram field (the antenna, the software): printable ASCII, ≤ 64. */
inline std::string cleanText(const std::string& s, size_t cap = 64) {
    std::string o;
    for (char c : s) { const unsigned char u = (unsigned char)c; if (u >= 0x20 && u < 0x7F && o.size() < cap) o += c; }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    size_t a = 0; while (a < o.size() && o[a] == ' ') ++a;
    return o.substr(a);
}

/** A spot as the decoders hand it over (vibe_decoder_host.h emitSpot). */
struct Spot {
    std::string mode, call, grid;
    int snr = 0;
    double rfHz = 0;
    double slotUtc = 0;             ///< s; 0 = now
    float dt = 0, drift = 0;
    int dbm = -1000;                ///< WSPR: the power the station sent (dBm); -1000 = not WSPR / unknown
};

/** ★ One WSPR spot as a line of an ALL_MEPT / ALL_WSPR file — the format WSPRnet's upload reads:
 *  "YYMMDD HHMM sync snr dt freqMHz call grid dBm drift" (QRP Labs AN002 shows the same columns). `sync` is our fine
 *  sync score scaled to 0–10 — the column the old WSPR program wrote as its quality figure. */
inline std::string wsprLine(const Spot& s, int sync10) {
    std::time_t t = (std::time_t)(s.slotUtc > 0 ? s.slotUtc : 0);
    std::tm tm{}; gmtime_r(&t, &tm);
    char b[160];
    std::snprintf(b, sizeof b, "%02d%02d%02d %02d%02d %3d %3d %4.1f %11.6f  %s %s %d %d",
                  tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  std::max(0, std::min(10, sync10)), std::max(-99, std::min(99, s.snr)), (double)s.dt, s.rfHz / 1e6,
                  s.call.c_str(), s.grid.c_str(), s.dbm, (int)std::lround(s.drift));
    return b;
}

// ══ THE REPORTER ═══════════════════════════════════════════════════════════════════════════════════════════════════
struct Config {
    bool on = false;                ///< PSK Reporter (the owner's switch, OFF by default)
    bool wsprnet = false;           ///< ★ also WSPR spots to WSPRnet (a second switch, OFF by default; needs `on`)
    std::string call;               ///< the receiver's callsign (required — nothing is sent without one)
    std::string grid;               ///< the server's grid (from its location)
    std::string antenna;            ///< free text; the owner's, else the radio's port name / antenna
    std::string software;           ///< "VibeServer <version>"
    // ── where (tests point these at local sinks) ──
    std::string pskHost = "report.pskreporter.info";
    int pskPort = 4739;
    std::string wsprHost = "wsprnet.org";
    int wsprPort = 80;
    std::string wsprPath = "/meptspots.php";
};

constexpr int kBatchSec = 300;              ///< "no more than one every five minutes"
constexpr int kStartJitterSec = 60;         ///< + a random 0–60 s from start, then 0–30 s each batch
constexpr int kRepeatSec = 3600;            ///< the same call + band + mode again only after an hour
constexpr int kTemplateEverySec = 3600;
constexpr size_t kMaxPending = 2000;        ///< callsigns waiting for the next batch (a 5-minute batch is far smaller)
constexpr size_t kMaxRecent = 8192;         ///< the once-an-hour memory (pruned every batch)
constexpr size_t kMaxWspr = 600;            ///< WSPR lines waiting (a busy band is ~60 per two minutes)
constexpr size_t kDatagramBytes = 1400;     ///< under every path MTU we will meet

using UdpSend = std::function<bool(const std::string& host, int port, const std::vector<uint8_t>& bytes, std::string& err)>;
/** An HTTP POST; returns the status code (0 = no answer), `err` the words. */
using HttpPost = std::function<int(const std::string& host, int port, const std::string& path, const std::string& contentType,
                                   const std::string& body, const std::string& userAgent, std::string& err)>;
/** An HTTPS GET for the ranking; returns the body ("" = failed). */
using HttpGet = std::function<std::string(const std::string& url)>;

/** ★ The plain HTTP/1.1 POST WSPRnet's upload takes (http, as its note gives it) — our own, over a socket, with
 *  deadlines: 10 s to connect, 20 s for the answer. Returns the status code. */
inline int postHttp(const std::string& host, int port, const std::string& path, const std::string& contentType,
                    const std::string& body, const std::string& userAgent, std::string& err) {
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) { err = "could not look up " + host; return 0; }
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int so = 0; socklen_t sl = sizeof so;
            rc = (::poll(&p, 1, 10000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && so == 0) ? 0 : -1;
        }
        if (rc != 0) { ::close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, fl);
    }
    freeaddrinfo(res);
    if (fd < 0) { err = "could not connect to " + host; return 0; }
    timeval tv{20, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + (port != 80 ? ":" + ps : std::string()) +
                      "\r\nUser-Agent: " + userAgent + "\r\nContent-Type: " + contentType +
                      "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t off = 0;
    while (off < req.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, 0);
#endif
        if (n <= 0) { ::close(fd); err = "the upload was cut off"; return 0; }
        off += (size_t)n;
    }
    std::string head;
    char buf[1024];
    while (head.find("\r\n") == std::string::npos && head.size() < 4096) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        head.append(buf, (size_t)n);
    }
    ::close(fd);
    // "HTTP/1.1 200 OK"
    const size_t sp = head.find(' ');
    if (head.rfind("HTTP/", 0) != 0 || sp == std::string::npos) { err = "no answer from " + host; return 0; }
    return std::atoi(head.c_str() + sp + 1);
}

/** UDP to host:port from ONE socket (the same source port for every datagram — the page asks for it). */
class UdpSocket {
public:
    ~UdpSocket() { if (fd_ >= 0) ::close(fd_); }
    bool send(const std::string& host, int port, const std::vector<uint8_t>& bytes, std::string& err) {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        const std::string ps = std::to_string(port);
        if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) { err = "could not look up " + host; return false; }
        if (fd_ < 0) fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        bool ok = fd_ >= 0 && ::sendto(fd_, bytes.data(), bytes.size(), 0, res->ai_addr, res->ai_addrlen) == (ssize_t)bytes.size();
        freeaddrinfo(res);
        if (!ok) err = "the datagram could not be sent";
        return ok;
    }
private:
    int fd_ = -1;
};

class Reporter {
public:
    /** The process's one reporter (the decoders' spots land here). */
    static Reporter& shared() { static Reporter r; return r; }

    Reporter() {
        std::random_device rd;
        rng_.seed(((uint64_t)rd() << 32) ^ rd());
        domain_ = (uint32_t)rng_();
        started_ = nowS();
        nextFlush_ = started_ + kBatchSec + (long)(rng_() % (kStartJitterSec + 1));
    }
    ~Reporter() { stop(); }

    // ── test seams ──
    void setClock(std::function<long()> fn) { std::lock_guard<std::mutex> l(m_); clock_ = std::move(fn); started_ = nowS(); nextFlush_ = started_ + kBatchSec; }
    void setUdp(UdpSend fn) { std::lock_guard<std::mutex> l(m_); udp_ = std::move(fn); }
    void setPost(HttpPost fn) { std::lock_guard<std::mutex> l(m_); post_ = std::move(fn); }
    void setHttpGet(HttpGet fn) { std::lock_guard<std::mutex> l(m_); get_ = std::move(fn); }
    void setRandomSeed(uint64_t s) { std::lock_guard<std::mutex> l(m_); rng_.seed(s); }
    /** ★ Where this process writes its status line for the setup page (the runtime dir every radio process shares). */
    void setStatusDir(const std::string& d) { std::lock_guard<std::mutex> l(m_); statusDir_ = d; }
    void setRadioLabel(const std::string& s) { std::lock_guard<std::mutex> l(m_); label_ = cleanText(s, 40); }

    /** The owner's settings. ★ Off, or no callsign: nothing waits and nothing is sent. */
    void configure(Config c) {
        c.call = cleanCall(c.call);
        c.grid = cleanGrid(c.grid);
        c.antenna = cleanText(c.antenna);
        c.software = cleanText(c.software);
        if (c.software.empty()) c.software = "VibeServer";
        std::lock_guard<std::mutex> l(m_);
        const bool wasOn = active_();
        cfg_ = c;
        if (!c.wsprnet || !active_()) wspr_.clear();
        if (!active_()) { pending_.clear(); }
        if (active_() && !wasOn) { ranksDue_ = nowS() + 60 + (long)(rng_() % 240); }   // first ranking a few minutes in
        writeStatusLocked_();
    }
    Config config() const { std::lock_guard<std::mutex> l(m_); return cfg_; }
    bool active() const { std::lock_guard<std::mutex> l(m_); return active_(); }

    /** ★ A decoder heard a station (any thread; cheap — no I/O here). */
    void note(const Spot& sIn) {
        std::lock_guard<std::mutex> l(m_);
        if (!active_()) return;
        Spot s = sIn;
        s.call = cleanHeardCall(s.call);
        const std::string mode = adifMode(s.mode);
        if (mode.empty() || !plausibleCall(s.call) || !(s.rfHz > 0) || s.rfHz >= 4294967295.0) return;
        s.grid = cleanGrid(s.grid);
        if (!(s.slotUtc > 0)) s.slotUtc = (double)nowS();
        const std::string band = bandOf(s.rfHz);
        // ★ once an hour per call + band + mode (the page's "a change might be a move to a different band")
        const std::string rk = s.call + "|" + band + "|" + mode;
        auto it = recent_.find(rk);
        const bool repeat = it != recent_.end() && nowS() - it->second < kRepeatSec;
        if (!repeat) {
            auto p = pending_.find(s.call);          // ★ each station at most once per batch
            if (p == pending_.end()) {
                if (pending_.size() < kMaxPending) pending_[s.call] = Pending{s, mode, band};
            } else {
                // keep the record, take a locator if this one has one
                if (p->second.spot.grid.empty() && !s.grid.empty()) p->second.spot.grid = s.grid;
            }
        }
        // ★ WSPRnet takes every WSPR transmission (one a station per two-minute slot), when its switch is on
        if (cfg_.wsprnet && mode == "WSPR" && s.dbm > -1000) {
            const std::string wk = s.call + "|" + std::to_string((long long)s.slotUtc);
            if (wsprKeys_.insert(wk).second && wspr_.size() < kMaxWspr) wspr_.push_back(s);
        }
    }

    /** Run a batch if one is due (the thread calls it every few seconds; tests call it with a fake clock). */
    void tick() {
        std::unique_lock<std::mutex> l(m_);
        const long now = nowS();
        if (active_() && now >= nextFlush_) {
            nextFlush_ = now + kBatchSec + (long)(rng_() % 31);
            flushLocked_(l, now);
        }
        if (active_() && get_ && now >= ranksDue_) {
            ranksDue_ = now + 3600 + (long)(rng_() % 300);   // ★ hourly, as the directory refreshes them
            const std::string call = cfg_.call;
            HttpGet g = get_;
            l.unlock();
            const std::string body = g(rankUrl(call));
            l.lock();
            if (validRanks_(body)) { ranksBody_ = body; ranksAt_ = now; ranksCall_ = call; }
        }
    }
    /** Force the batch now (tests, and a switch turned off with spots waiting). */
    void flushNow() { std::unique_lock<std::mutex> l(m_); flushLocked_(l, nowS()); }

    void start() {
        std::lock_guard<std::mutex> l(tm_);
        if (th_.joinable()) return;
        stop_ = false;
        th_ = std::thread([this] {
            std::unique_lock<std::mutex> l(tm_);
            while (!stop_) {
                l.unlock(); tick(); l.lock();
                tcv_.wait_for(l, std::chrono::seconds(5), [this] { return stop_; });
            }
        });
    }
    void stop() {
        { std::lock_guard<std::mutex> l(tm_); stop_ = true; }
        tcv_.notify_all();
        if (th_.joinable()) th_.join();
    }

    /** The setup page's line: {"on","wsprnet","call","grid","sentHour","lastSent","lastError","wsprHour","wsprLast",
     *  "wsprError","pending","label"}. */
    std::string statusJson() const { std::lock_guard<std::mutex> l(m_); return statusJsonLocked_(); }

    /** ★ The ranking for the gallery: {"on":true,"call":…,"fetched":<unix s>,"ranks":<the directory's answer>} —
     *  only while reporting is on and the answer is for this callsign; else {"on":false}. */
    std::string ranksJson() const {
        std::lock_guard<std::mutex> l(m_);
        if (!active_() || ranksBody_.empty() || ranksCall_ != cfg_.call) return std::string("{\"on\":") + (active_() ? "true" : "false") + "}";
        return "{\"on\":true,\"call\":\"" + cfg_.call + "\",\"fetched\":" + std::to_string(ranksAt_) + ",\"ranks\":" + ranksBody_ + "}";
    }
    static std::string rankUrl(const std::string& call) {
        std::string u = "https://vibeserver.vibesdr.net/api/ranks?call=";
        for (char c : call) { if (c == '/') u += "%2F"; else u += c; }
        return u;
    }
    std::string userAgent() const { return cfg_.software.empty() ? std::string("VibeServer") : cfg_.software; }

    // ── tests ──
    size_t pendingCount() const { std::lock_guard<std::mutex> l(m_); return pending_.size(); }
    size_t wsprPendingCount() const { std::lock_guard<std::mutex> l(m_); return wspr_.size(); }
    long nextFlushAt() const { std::lock_guard<std::mutex> l(m_); return nextFlush_; }
    uint32_t sequence() const { std::lock_guard<std::mutex> l(m_); return seq_; }
    void forceRanksDue() { std::lock_guard<std::mutex> l(m_); ranksDue_ = 0; }

private:
    struct Pending { Spot spot; std::string mode, band; };
    bool active_() const { return cfg_.on && !cfg_.call.empty(); }
    long nowS() const { return clock_ ? clock_() : (long)std::time(nullptr); }

    void flushLocked_(std::unique_lock<std::mutex>& l, long now) {
        // ── PSK Reporter ──
        std::vector<Report> reps;
        for (auto& kv : pending_) {
            const Pending& p = kv.second;
            Report r; r.call = p.spot.call; r.locator = p.spot.grid; r.mode = p.mode; r.hz = (uint64_t)std::llround(p.spot.rfHz);
            r.snr = p.spot.snr; r.t = (uint32_t)(p.spot.slotUtc > 0 ? p.spot.slotUtc : now);
            reps.push_back(r);
            recent_[p.spot.call + "|" + p.band + "|" + p.mode] = now;
        }
        pending_.clear();
        // prune the hour's memory (and bound it)
        for (auto it = recent_.begin(); it != recent_.end();) { if (now - it->second >= kRepeatSec) it = recent_.erase(it); else ++it; }
        while (recent_.size() > kMaxRecent) recent_.erase(std::min_element(recent_.begin(), recent_.end(),
            [](const std::pair<const std::string, long>& a, const std::pair<const std::string, long>& b) { return a.second < b.second; }));
        std::vector<Spot> wl;
        wl.swap(wspr_);
        wsprKeys_.clear();
        const Config cfg = cfg_;
        if (!active_()) { writeStatusLocked_(); return; }
        std::vector<std::vector<uint8_t>> grams;
        if (!reps.empty()) {
            int tpl = 0;
            if (gramsEver_ < 3 || now - lastTemplate_ >= kTemplateEverySec) { tpl = gramsEver_ < 3 ? (int)(3 - gramsEver_) : 1; lastTemplate_ = now; }
            Receiver rx{cfg.call, cfg.grid, cfg.software, cfg.antenna};
            grams = buildDatagrams(rx, reps, (uint32_t)now, seq_, domain_, tpl, kDatagramBytes);
            gramsEver_ += grams.size();
        }
        UdpSend udp = udp_;
        HttpPost post = post_;
        l.unlock();
        // ── the network, unlocked ──
        size_t sent = 0; std::string err;
        for (const auto& g : grams) {
            std::string e;
            const bool ok = udp ? udp(cfg.pskHost, cfg.pskPort, g, e) : sock_.send(cfg.pskHost, cfg.pskPort, g, e);
            if (!ok) { err = e; break; }
            ++sent;
        }
        int wcode = -1; std::string werr; size_t wcount = 0;
        if (cfg.wsprnet && !wl.empty() && !cfg.grid.empty()) {
            std::string file;
            for (const auto& s : wl) file += wsprLine(s, 5) + "\r\n";
            const std::string bnd = "----VibeServerWsprnet" + std::to_string(now);
            std::string body;
            auto part = [&](const std::string& name, const std::string& val, const char* fname) {
                body += "--" + bnd + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
                if (fname) body += std::string("; filename=\"") + fname + "\"\r\nContent-Type: text/plain";
                body += "\r\n\r\n" + val + "\r\n";
            };
            part("call", cfg.call, nullptr);
            part("grid", cfg.grid, nullptr);
            part("allmept", file, "ALL_MEPT.TXT");
            body += "--" + bnd + "--\r\n";
            wcode = (post ? post : HttpPost(postHttp))(cfg.wsprHost, cfg.wsprPort, cfg.wsprPath,
                                                      "multipart/form-data; boundary=" + bnd, body, cfg.software, werr);
            wcount = wl.size();
        }
        l.lock();
        // ── the status ──
        size_t recs = 0;
        if (sent == grams.size()) recs = reps.size();
        else { recs = 0; }   // a partial send: count nothing we cannot be sure of
        if (!grams.empty()) {
            if (err.empty()) { hour_.push_back({now, (long)recs}); lastSent_ = now; lastError_.clear(); }
            else { lastError_ = err; lastErrorAt_ = now; }
        }
        if (wcode >= 0) {
            if (wcode == 200) { whour_.push_back({now, (long)wcount}); wLast_ = now; wError_.clear(); }
            else {
                wError_ = werr.empty() ? "WSPRnet answered " + std::to_string(wcode) : werr; wErrorAt_ = now;
                // ★ one retry next batch, never more (bounded)
                for (const auto& s : wl) if (wspr_.size() < kMaxWspr && wsprKeys_.insert(s.call + "|" + std::to_string((long long)s.slotUtc)).second) wspr_.push_back(s);
            }
        }
        while (!hour_.empty() && now - hour_.front().first >= 3600) hour_.pop_front();
        while (!whour_.empty() && now - whour_.front().first >= 3600) whour_.pop_front();
        writeStatusLocked_();
    }

    std::string statusJsonLocked_() const {
        long h = 0, w = 0;
        const long now = nowS();
        for (const auto& x : hour_) if (now - x.first < 3600) h += x.second;
        for (const auto& x : whour_) if (now - x.first < 3600) w += x.second;
        auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o; };
        std::string j = "{\"on\":"; j += active_() ? "true" : "false";
        j += ",\"wsprnet\":"; j += (active_() && cfg_.wsprnet) ? "true" : "false";
        j += ",\"call\":\"" + esc(cfg_.call) + "\",\"grid\":\"" + esc(cfg_.grid) + "\",\"label\":\"" + esc(label_) + "\"";
        j += ",\"sentHour\":" + std::to_string(h) + ",\"lastSent\":" + std::to_string(lastSent_);
        j += ",\"lastError\":\"" + esc(lastError_) + "\",\"lastErrorAt\":" + std::to_string(lastErrorAt_);
        j += ",\"wsprHour\":" + std::to_string(w) + ",\"wsprLast\":" + std::to_string(wLast_);
        j += ",\"wsprError\":\"" + esc(wError_) + "\",\"wsprErrorAt\":" + std::to_string(wErrorAt_);
        j += ",\"pending\":" + std::to_string(pending_.size()) + ",\"updated\":" + std::to_string(now) + "}";
        return j;
    }
    void writeStatusLocked_() const {
        if (statusDir_.empty()) return;
        const std::string path = statusDir_ + "/spotreport-" + std::to_string((long)::getpid()) + ".json";
        const std::string tmp = path + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "w")) {
            const std::string j = statusJsonLocked_();
            std::fwrite(j.data(), 1, j.size(), f);
            std::fclose(f);
            std::rename(tmp.c_str(), path.c_str());
        }
    }
    static bool validRanks_(const std::string& b) {
        // ★ the directory's compact answer: a JSON object, small. Anything else (an error page, a proxy's HTML) is not kept.
        if (b.size() < 2 || b.size() > 16384 || b.front() != '{' || b.back() != '}') return false;
        for (char c : b) if ((unsigned char)c < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
        return b.find("<") == std::string::npos;
    }

    mutable std::mutex m_;
    Config cfg_;
    std::function<long()> clock_;
    UdpSend udp_;
    HttpPost post_;
    HttpGet get_;
    UdpSocket sock_;
    std::mt19937_64 rng_;
    uint32_t domain_ = 0, seq_ = 0;
    size_t gramsEver_ = 0;
    long started_ = 0, nextFlush_ = 0, lastTemplate_ = 0;
    std::map<std::string, Pending> pending_;
    std::map<std::string, long> recent_;
    std::vector<Spot> wspr_;
    std::set<std::string> wsprKeys_;
    std::deque<std::pair<long, long>> hour_, whour_;
    long lastSent_ = 0, lastErrorAt_ = 0, wLast_ = 0, wErrorAt_ = 0;
    std::string lastError_, wError_;
    std::string statusDir_, label_;
    long ranksDue_ = 0, ranksAt_ = 0;
    std::string ranksBody_, ranksCall_;
    // the thread
    std::mutex tm_;
    std::condition_variable tcv_;
    bool stop_ = false;
    std::thread th_;
};

/** ★ The setup page's one line, from every radio process's status file in `dir` (written within `maxAgeSec`):
 *  {"on","wsprnet","call","sentHour","lastSent","lastError","wsprHour","wsprLast","wsprError","radios":[…each…]}. */
inline std::string aggregateStatus(const std::string& dir, long now, long maxAgeSec = 2 * 3600) {
    std::vector<std::string> each;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("spotreport-", 0) != 0 || n.size() < 17 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
            const std::string p = dir + "/" + n;
            struct stat st{};
            if (::stat(p.c_str(), &st) != 0 || st.st_size > 4096) continue;
            if (now - (long)st.st_mtime > maxAgeSec) continue;
            if (FILE* f = std::fopen(p.c_str(), "r")) {
                char buf[4097]; const size_t k = std::fread(buf, 1, 4096, f); std::fclose(f);
                std::string j(buf, k);
                if (!j.empty() && j.front() == '{' && j.back() == '}') each.push_back(j);
            }
        }
        ::closedir(d);
    }
    auto num = [](const std::string& j, const char* k) -> long {
        const std::string key = std::string("\"") + k + "\":";
        const size_t p = j.find(key); return p == std::string::npos ? 0 : std::atol(j.c_str() + p + key.size());
    };
    auto str = [](const std::string& j, const char* k) -> std::string {
        const std::string key = std::string("\"") + k + "\":\"";
        size_t p = j.find(key); if (p == std::string::npos) return "";
        p += key.size(); std::string o;
        while (p < j.size() && j[p] != '"') { if (j[p] == '\\' && p + 1 < j.size()) ++p; o += j[p++]; }
        return o;
    };
    long h = 0, w = 0, last = 0, wlast = 0, errAt = 0, werrAt = 0; bool on = false, wn = false;
    std::string call, err, werr;
    for (const auto& j : each) {
        if (j.find("\"on\":true") != std::string::npos) on = true;
        if (j.find("\"wsprnet\":true") != std::string::npos) wn = true;
        h += num(j, "sentHour"); w += num(j, "wsprHour");
        last = std::max(last, num(j, "lastSent")); wlast = std::max(wlast, num(j, "wsprLast"));
        if (call.empty()) call = str(j, "call");
        const std::string e = str(j, "lastError"); if (!e.empty() && num(j, "lastErrorAt") >= errAt) { err = e; errAt = num(j, "lastErrorAt"); }
        const std::string we = str(j, "wsprError"); if (!we.empty() && num(j, "wsprErrorAt") >= werrAt) { werr = we; werrAt = num(j, "wsprErrorAt"); }
    }
    auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o; };
    std::string o = "{\"on\":"; o += on ? "true" : "false"; o += ",\"wsprnet\":"; o += wn ? "true" : "false";
    o += ",\"call\":\"" + esc(call) + "\",\"sentHour\":" + std::to_string(h) + ",\"lastSent\":" + std::to_string(last);
    o += ",\"lastError\":\"" + esc(err) + "\",\"wsprHour\":" + std::to_string(w) + ",\"wsprLast\":" + std::to_string(wlast);
    o += ",\"wsprError\":\"" + esc(werr) + "\",\"radios\":[";
    for (size_t i = 0; i < each.size(); ++i) { if (i) o += ","; o += each[i]; }
    o += "]}";
    return o;
}

}  // namespace spotreport
}  // namespace vibe
