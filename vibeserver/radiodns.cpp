#include "radiodns.h"

#include <algorithm>
#include <cctype>
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
std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    return out;
}

/** Extract a JSON string value for `key` — enough for the DoH answer, which is flat and small. */
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
    std::snprintf(f, sizeof f, "%05d", (int)llround(freqHz / 10000.0));
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
    const std::string cmd = "curl -fsS --max-time 8 -H 'accept: application/dns-json' "
                            "'https://cloudflare-dns.com/dns-query?name=" + name + "&type=SRV' 2>/dev/null";
    const std::string js = run(cmd);
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
    const std::string cmd = "curl -fsS --max-time 8 -H 'accept: application/dns-json' "
                            "'https://cloudflare-dns.com/dns-query?name=" + fqdn + "&type=CNAME' 2>/dev/null";
    const std::string js = run(cmd);
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
        const std::string url = tag.substr(u + 5, u2 - u - 5);
        if (url.rfind("http", 0) != 0) continue;
        int w = 0;
        const size_t wp = tag.find("width=\"");
        if (wp != std::string::npos) w = atoi(tag.c_str() + wp + 7);
        if (w > bestW) { bestW = w; best = url; }
    }
    return best;
}

}  // namespace

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
        const std::string xml = run("curl -fsS --max-time 12 '" + base
                                    + "/radiodns/spi/3.1/SI.xml' 2>/dev/null");
        if (!xml.empty()) {
            // fm:<gcc>.<pi>.<freq> — the same three fields, in the order the SPI uses.
            const std::string pi = lower(piHex);
            const std::string gcc = std::string(1, pi[0]) + lower(ecc.substr(ecc.size() - 2));
            char f[16];
            std::snprintf(f, sizeof f, "%05d", (int)llround(freqHz / 10000.0));
            url = logoFromSpi(xml, "fm:" + gcc + "." + pi + "." + std::string(f));
        }
    }

    std::lock_guard<std::mutex> lk(g_mtx);
    g_cache[fqdn] = Entry{ url, now };
    return url;
}

}  // namespace vsradiodns
