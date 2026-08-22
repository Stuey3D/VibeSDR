#include "radiodns.h"

#include <algorithm>
#include <array>
#include <cctype>
// ★★★ <cmath> FOR llround, AND ITS ABSENCE ONLY BROKE THE PI. libc++ (the Mac) pulls it in
//     transitively through <string>, libstdc++ (bookworm/GCC) does not — so this file built
//     cleanly here and failed the publish with "'llround' was not declared in this scope".
//     That failed build is WHY the demo Pi answered 404 for /vibeserver/stationlogo: the
//     endpoint was never in a package. A Mac-only compile is not evidence that the release
//     builds (2026-08-11).
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace vsradiodns {
namespace {

std::mutex g_mtx;
std::string g_dir = "/var/lib/vibeserver";

struct Entry { std::string url; time_t at = 0; };
std::map<std::string, Entry> g_cache;
// ★ A day for a hit, an hour for a miss. Logos change rarely; a station JOINING RadioDNS is worth
//   noticing sooner than that, but not at the cost of a lookup every time somebody tunes past.
constexpr time_t kHitTtl  = 24 * 3600;
constexpr time_t kMissTtl = 3600;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/** ★ curl, exactly as eibi/geoip/asndb do it — the daemon has no TLS stack of its own and this is
 *  a rare fetch. Quoted destinations and URLs; see the space-in-the-path note in asndb.cpp. */
FetchFn g_fetch;

std::string runCurl(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    return out;
}

/** Extract a JSON string value for `key` — enough for the DoH answer, which is flat and small. */
/** ★★ ONE PLACE THAT FETCHES. It was three curl command lines with their own timeouts and
 *  quoting; now the URL and the Accept header are the only things a caller supplies, which is what
 *  makes the transport swappable at all. */
std::string httpGet(const std::string& url, const std::string& accept) {
    if (g_fetch) return g_fetch(url, accept);
    // ★ Quoted, because these URLs carry & and ? — see the note above.
    std::string cmd = "curl -fsS --max-time 12 ";
    if (!accept.empty()) cmd += "-H 'accept: " + accept + "' ";
    cmd += "'" + url + "' 2>/dev/null";
    return runCurl(cmd);
}

std::string jsonStr(const std::string& s, const std::string& key) {
    const std::string k = "\"" + key + "\"";
    size_t i = s.find(k);
    while (i != std::string::npos) {
        size_t c = s.find(':', i + k.size());
        if (c == std::string::npos) return {};
        size_t q1 = s.find('"', c);
        if (q1 == std::string::npos) return {};
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return s.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

/**
 * The RadioDNS FM FQDN.
 *
 * ★★ THE GCC IS NOT THE ECC. It is the PI's COUNTRY NIBBLE followed by the ECC — "c" from PI
 *    C202 plus ECC E1 gives "ce1". Using the ECC alone resolves nothing, and it is the kind of
 *    mistake that looks like "RadioDNS has no entry for this station" rather than like a bug.
 * ★ Frequency is MHz x 100, five digits, zero padded: 88.1 MHz -> "08810". (Units of 10 kHz — my
 *   first cut divided by 100 kHz and produced "00881", which resolves nothing; the comment beside
 *   it already had the right answer, which is the useful kind of disagreement to notice.)
 */
std::string fqdnFor(const std::string& piHex, const std::string& ecc, double freqHz) {
    if (piHex.size() != 4 || ecc.size() < 2 || freqHz <= 0) return {};
    const std::string pi = lower(piHex);
    const std::string gcc = std::string(1, pi[0]) + lower(ecc.substr(ecc.size() - 2));
    char f[16];
    std::snprintf(f, sizeof f, "%05d", (int)std::llround(freqHz / 10000.0));
    return std::string(f) + "." + pi + "." + gcc + ".fm.radiodns.org";
}

/** Resolve an SRV record over DoH. Returns "host:port", or empty.
 *  ★★★ THE CNAME TARGET IS NOT A WEB SERVER — it is a naming anchor, and this step is what I
 *      missed first time round: 08810.c202.ce1.fm.radiodns.org resolves to
 *      radiodns.api.bbci.co.uk, which HAS NO ADDRESS RECORD AT ALL ("could not resolve host").
 *      The service itself is found with an SRV lookup under that name — _radiospi._tcp — which
 *      gives the real host AND the port, and the port is what says whether to speak http or
 *      https. The BBC answers 443 (https); the spec's older _radioepg._tcp answers 80. */
std::string resolveSrv(const std::string& name) {
    const std::string js = httpGet(
        "https://cloudflare-dns.com/dns-query?name=" + name + "&type=SRV", "application/dns-json");
    const size_t a = js.find("\"Answer\"");
    if (a == std::string::npos) return {};
    // "data":"<prio> <weight> <port> <target>"
    const std::string rec = jsonStr(js.substr(a), "data");
    int prio = 0, weight = 0, port = 0;
    char host[256] = {0};
    if (std::sscanf(rec.c_str(), "%d %d %d %255s", &prio, &weight, &port, host) != 4) return {};
    std::string h(host);
    if (!h.empty() && h.back() == '.') h.pop_back();
    if (h.empty() || port <= 0) return {};
    return h + ":" + std::to_string(port);
}

/** Resolve a CNAME over DNS-over-HTTPS. ★ A page cannot do this and neither can plain curl, but
 *  the JSON DoH API is an ordinary HTTPS GET — which is why this is a server-side feature. */
std::string resolveCname(const std::string& fqdn) {
    const std::string js = httpGet(
        "https://cloudflare-dns.com/dns-query?name=" + fqdn + "&type=CNAME", "application/dns-json");
    if (js.empty()) return {};
    // The answer's "data" is the target, with a trailing dot.
    const size_t a = js.find("\"Answer\"");
    if (a == std::string::npos) return {};
    std::string host = jsonStr(js.substr(a), "data");
    if (!host.empty() && host.back() == '.') host.pop_back();
    return host;
}

/** Pull the logo out of an SPI document for OUR bearer.
 *  ★★ The document lists every service the broadcaster runs, so taking the first <multimedia> in
 *     the file would hand back a SIBLING STATION'S logo — Radio 1's artwork on Radio 4. The bearer
 *     id is what says which service is ours, so the search is scoped to the <service> block that
 *     contains it. */
std::string logoFromSpi(const std::string& xml, const std::string& bearerId) {
    const size_t b = xml.find(bearerId);
    if (b == std::string::npos) return {};
    // Walk back to this service's opening tag, forward to its close.
    const size_t open = xml.rfind("<service", b);
    size_t close = xml.find("</service>", b);
    if (open == std::string::npos) return {};
    if (close == std::string::npos) close = xml.size();
    const std::string block = xml.substr(open, close - open);

    // ★ Prefer a bigger logo: these are offered at several sizes and the bar can scale down far
    //   more gracefully than it can scale up.
    std::string best; int bestW = -1;
    size_t i = 0;
    while ((i = block.find("<multimedia", i)) != std::string::npos) {
        const size_t end = block.find('>', i);
        if (end == std::string::npos) break;
        const std::string tag = block.substr(i, end - i);
        i = end;
        const size_t u = tag.find("url=\"");
        if (u == std::string::npos) continue;
        const size_t u2 = tag.find('"', u + 5);
        if (u2 == std::string::npos) continue;
        std::string url = tag.substr(u + 5, u2 - u - 5);
        if (url.rfind("http", 0) != 0) continue;
        // ★★★ https OR THE BROWSER WILL NOT LOAD IT AT ALL. SPI documents still carry plain http
        //     urls — Global's Classic FM entry does — and a page served over https (the demo, and
        //     any tunnelled server) BLOCKS mixed content before a request is made. The failure is
        //     silent and looks exactly like "this station has no logo", which is the case we just
        //     went to some trouble to fix. Both schemes answered 200 for every host tested, so
        //     the upgrade costs nothing; a host that genuinely cannot do TLS simply fails to load
        //     and the client falls back, as it does for a station with no SPI entry at all.
        if (url.rfind("http://", 0) == 0) url = "https://" + url.substr(7);
        int w = 0;
        const size_t wp = tag.find("width=\"");
        if (wp != std::string::npos) w = atoi(tag.c_str() + wp + 7);
        if (w > bestW) { bestW = w; best = url; }
    }
    return best;
}

}  // namespace

/** The ECC country table: row = ECC, column = the PI's country nibble 1..F.
 *  ★ Mirrored from src/services/rdsCountry.ts and kept in the SAME order and shape so the two can
 *    be diffed by eye — a divergence shows up as a station whose logo works in the app and not in
 *    the browser, which is miserable to chase from either end alone.
 *  ★ A function rather than two copies: eccForIso() and eccCandidates() both need it, and a second
 *    transcription of 22 rows of country codes is a typo waiting to happen. */
const std::map<std::string, std::array<const char*, 15>>& kEccTable() {
    static const std::map<std::string, std::array<const char*, 15>> kTable = {
        {"A1", {{"",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "CA", "CA", "CA", "CA", "GL"}}},
        {"A2", {{"AI", "AG", "",   "FK", "BB", "BZ", "KY", "CR", "CU", "AR", "BR", "",   "",   "GP", "BS"}}},
        {"A3", {{"BO", "CO", "JM", "MQ", "",   "PY", "NI", "",   "PA", "DM", "DO", "CL", "GD", "TB", "GY"}}},
        {"A4", {{"GT", "HN", "AW", "",   "MS", "TT", "PE", "SR", "UY", "KN", "LC", "SN", "HT", "VE", "VG"}}},
        {"A5", {{"",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "MX", "VC", "MX", "MX", "MX"}}},
        {"A6", {{"",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "PM"}}},
        {"D0", {{"CM", "CF", "DJ", "MG", "ML", "AO", "GQ", "GA", "GN", "ZA", "BF", "CG", "TG", "BJ", "MW"}}},
        {"D1", {{"NA", "LR", "GH", "MR", "ST", "CV", "SN", "GM", "BI", "SH", "BW", "KM", "TZ", "ET", "NG"}}},
        {"D2", {{"SL", "ZW", "MZ", "UG", "SZ", "KE", "SO", "NE", "TD", "GW", "CD", "CI", "",   "ZM", "ER"}}},
        {"D3", {{"",   "",   "EH", "",   "RW", "LS", "",   "SC", "",   "MU", "",   "SD", "",   "",   ""}}},
        {"D4", {{"",   "",   "",   "",   "",   "",   "",   "",   "",   "SS", "",   "",   "",   "",   ""}}},
        {"E0", {{"DE", "DZ", "AD", "IL", "IT", "BE", "RU", "PS", "AL", "AT", "HU", "MT", "DE", "",   "EG"}}},
        {"E1", {{"GR", "CY", "SM", "CH", "JO", "FI", "LU", "BG", "DK", "GI", "IQ", "GB", "LY", "RO", "FR"}}},
        {"E2", {{"MA", "CZ", "PL", "VA", "SK", "SY", "TN", "",   "LI", "IS", "MC", "LT", "RS", "ES", "NO"}}},
        {"E3", {{"ME", "IE", "TR", "",   "TJ", "",   "",   "NL", "LV", "LB", "AZ", "HR", "KZ", "SE", "BY"}}},
        {"E4", {{"MD", "EE", "MK", "",   "",   "UA", "XK", "PT", "SI", "AM", "UZ", "GE", "",   "TM", "BA"}}},
        {"E5", {{"",   "",   "KG", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   ""}}},
        {"F0", {{"AU", "AU", "AU", "AU", "AU", "AU", "AU", "AU", "SA", "AF", "MM", "CN", "KP", "BH", "MY"}}},
        {"F1", {{"KI", "BT", "BD", "PK", "FJ", "OM", "NR", "IR", "NZ", "SB", "BN", "LK", "TW", "KR", "HK"}}},
        {"F2", {{"KW", "QA", "KH", "WS", "IN", "MO", "VN", "PH", "JP", "SG", "MV", "ID", "AE", "NP", "VU"}}},
        {"F3", {{"LA", "TH", "TO", "",   "",   "",   "",   "CN", "PG", "",   "YE", "",   "",   "FM", "MN"}}},
        {"F4", {{"",   "",   "",   "",   "",   "",   "",   "",   "CN", "",   "MH", "",   "",   "",   ""}}},
    };
    return kTable;
}

std::string eccForIso(const std::string& iso, const std::string& piHex);

/** Every ECC that could possibly go with this PI's country nibble, best guess first.
 *
 * ★★★ BECAUSE DERIVING FROM THE RECEIVER'S COUNTRY DOES NOT WORK WHEN THERE IS NO COUNTRY. The
 *     demo server has none configured, so eccForIso() returned nothing and the whole feature
 *     silently did nothing at all: "RadioDNS is not showing up any icons anymore" (Stuart,
 *     2026-08-14). A lookup that depends on a setting most owners will never fill in is a lookup
 *     that mostly will not happen.
 * ★★★ SO ASK RADIODNS INSTEAD OF ASKING THE CONFIG. A wrong GCC does not resolve — that is the
 *     whole point of a DNS-backed identity — so the candidates can simply be TRIED. The nibble
 *     narrows it to about ten, each answer is cached for a day (misses for an hour), and it costs
 *     nothing at all for the many stations that do transmit an ECC.
 * ★★ It also fixes the case the config approach could never handle: a station from ACROSS A
 *    BORDER, whose country is not the receiver's. The old derivation would refuse those by design.
 * ★ The receiver's own country still goes FIRST when it is known, so the common case is one
 *   lookup rather than ten.
 */
std::vector<std::string> eccCandidates(const std::string& piHex, const std::string& preferIso) {
    std::vector<std::string> out;
    const std::string first = eccForIso(preferIso, piHex);
    if (!first.empty()) out.push_back(first);
    if (piHex.empty()) return out;
    const char c = (char)std::toupper((unsigned char)piHex[0]);
    int nibble = -1;
    if (c >= '0' && c <= '9')      nibble = c - '0';
    else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
    if (nibble < 1 || nibble > 15) return out;
    for (const auto& [ecc, row] : kEccTable()) {
        if (!row[nibble - 1] || !*row[nibble - 1]) continue;   // no country at this nibble
        if (std::find(out.begin(), out.end(), ecc) == out.end()) out.push_back(ecc);
    }
    return out;
}

std::string eccForIso(const std::string& iso, const std::string& piHex) {
    if (iso.size() != 2 || piHex.empty()) return {};
    // ★ The ECC country table, mirrored from src/services/rdsCountry.ts. Row = ECC, column = the
    //   PI's country nibble 1..F. Kept in the SAME order and shape as the TypeScript so the two
    //   can be diffed by eye; a divergence here shows up as a station whose logo works in the app
    //   and not in the browser, which is a miserable thing to chase from either end alone.
    const auto& kTable = kEccTable();

    const char c = (char)std::toupper((unsigned char)piHex[0]);
    int nibble = -1;
    if (c >= '0' && c <= '9')      nibble = c - '0';
    else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
    if (nibble < 1 || nibble > 15) return {};

    std::string want = iso;
    for (auto& ch : want) ch = (char)std::toupper((unsigned char)ch);

    std::string found;
    for (const auto& [ecc, row] : kTable) {
        if (want != row[nibble - 1]) continue;
        if (!found.empty()) return {};      // two rows claim it — say nothing rather than pick
        found = ecc;
    }
    return found;
}

void setFetcher(FetchFn fn) {
    // ★ No lock: set once at startup, before any lookup can run. Same contract as setDir.
    g_fetch = std::move(fn);
}

void setDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!dir.empty()) g_dir = dir;
}

std::string logoFor(const std::string& piHex, const std::string& ecc, double freqHz) {
    const std::string fqdn = fqdnFor(piHex, ecc, freqHz);
    if (fqdn.empty()) return {};

    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_cache.find(fqdn);
        if (it != g_cache.end()) {
            const time_t ttl = it->second.url.empty() ? kMissTtl : kHitTtl;
            if (now - it->second.at < ttl) return it->second.url;
        }
    }

    std::string url;
    const std::string anchor = resolveCname(fqdn);
    // ★ _radiospi first (the modern name for this service), _radioepg as the older fallback —
    //   plenty of broadcasters still publish only the latter.
    std::string hostPort;
    if (!anchor.empty()) {
        hostPort = resolveSrv("_radiospi._tcp." + anchor);
        if (hostPort.empty()) hostPort = resolveSrv("_radioepg._tcp." + anchor);
    }
    if (!hostPort.empty()) {
        // ★ The PORT decides the scheme: 443 is https, anything else plain http. Hardcoding
        //   either one fails half the broadcasters.
        const bool tls = hostPort.size() > 4 && hostPort.substr(hostPort.size() - 4) == ":443";
        const std::string base = tls ? "https://" + hostPort.substr(0, hostPort.size() - 4)
                                     : "http://" + hostPort;
        const std::string xml = httpGet(base + "/radiodns/spi/3.1/SI.xml", "");
        if (!xml.empty()) {
            // fm:<gcc>.<pi>.<freq> — the same three fields, in the order the SPI uses.
            const std::string pi = lower(piHex);
            const std::string gcc = std::string(1, pi[0]) + lower(ecc.substr(ecc.size() - 2));
            char f[16];
            std::snprintf(f, sizeof f, "%05d", (int)std::llround(freqHz / 10000.0));
            url = logoFromSpi(xml, "fm:" + gcc + "." + pi + "." + std::string(f));
        }
    }

    std::lock_guard<std::mutex> lk(g_mtx);
    g_cache[fqdn] = Entry{ url, now };
    return url;
}

/**
 * The logo for an FM service, deriving the ECC when the station does not transmit one.
 *
 * ★★★ THIS IS THE ENTRY POINT THE SERVER SHOULD USE. Deriving from the receiver's configured
 *     COUNTRY was the first attempt and it failed in the field for the most ordinary reason
 *     possible: the demo server has no country configured, so nothing could be derived and the
 *     feature silently did nothing at all. A lookup that depends on a setting most owners will
 *     never fill in is a lookup that mostly will not happen.
 * ★★ RadioDNS is the authority, so ASK IT. A wrong GCC does not resolve; the nibble narrows the
 *    field to about ten; every answer, hit or miss, is cached. The receiver's country still goes
 *    first when it is known, so the common case is one lookup rather than ten — and a station from
 *    across a BORDER now works too, which the country-derived version refused by design.
 */
std::string logoForAuto(const std::string& piHex, const std::string& ecc, double freqHz,
                        const std::string& preferIso) {
    const bool haveEcc = ecc.size() >= 2 && ecc != "00" && ecc != "0";
    if (haveEcc) return logoFor(piHex, ecc, freqHz);
    for (const auto& cand : eccCandidates(piHex, preferIso)) {
        const std::string url = logoFor(piHex, cand, freqHz);
        if (!url.empty()) return url;
    }
    return {};
}


void clearCache() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_cache.clear();
}
}  // namespace vsradiodns
