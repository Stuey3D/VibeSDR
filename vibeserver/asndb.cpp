// IP -> ASN, from iptoasn.com's public-domain BGP-derived table.
//
// ★★★ WHY A SECOND DATASET AND NOT THE RIR FILES.
// I said the registry files already carried what an ASN ban needs. They do not, and it is worth
// writing down so nobody re-derives the disappointment: the delegated-extended format's last
// field is an OPAQUE-ID — a per-holder UUID, scoped to one registry, with no organisation name —
// and its `asn` records say which AS NUMBERS were allocated to whom, not which AS announces a
// given prefix. That mapping only exists in BGP.
//
// ★★ THE SOURCE IS iptoasn.com: a daily dump derived from the global routing table, published
// into the PUBLIC DOMAIN (PDDL 1.0). Same three properties that decided the country data — no
// account, no licence to accept, and NOTHING IS ASKED AT RUNTIME, so no visitor's address is ever
// sent anywhere. The alternative everyone reaches for (Team Cymru's whois/DNS service) is a
// third-party query per lookup, which is the shape this project keeps refusing.
//
// Format, tab-separated, ranges already expanded — no prefix arithmetic needed:
//   1.0.0.0    1.0.0.255                13335  US    CLOUDFLARENET
//   1.0.1.0    1.0.3.255                0      None  Not routed
//   2606:4700::  2606:4700:1:ffff:...   13335  US    CLOUDFLARENET
//
// ★ Rows with AS 0 ("Not routed") are DROPPED. They are a third of the file and mean "no network
//   announces this", which is exactly the case lookup() must report as unknown — keeping them
//   would let an owner ban AS0 and catch every unrouted address at once.
#include "asndb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace asndb {
namespace {

struct V4Row { uint32_t lo, hi, asn, name; };   // name = index into g_names
struct V6Row { uint64_t lo, hi; uint32_t asn, name; };

std::mutex               g_mtx;
std::vector<V4Row>       g_v4;
std::vector<V6Row>       g_v6;
std::vector<std::string> g_names;
std::string              g_dir = "/var/lib/vibeserver";
long long                g_updated = 0;

const char* kUrl = "https://iptoasn.com/data/ip2asn-combined.tsv.gz";

std::string cachePath() { return g_dir + "/asn.bin"; }

bool parseV4(const char* s, uint32_t& out) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

/** Top 64 bits only — see geoip.cpp for the same reasoning: an AS announcement is never finer
 *  than a /64, so the low half cannot change the answer and would double the table. */
bool parseV6Hi(const char* s, uint64_t& out) {
    uint16_t g[8] = {0};
    int gi = 0, dbl = -1;
    const char* p = s;
    if (p[0] == ':' && p[1] == ':') { dbl = 0; p += 2; }
    while (*p && gi < 8) {
        if (*p == ':') { dbl = gi; p++; continue; }
        if (!isxdigit((unsigned char)*p)) break;
        unsigned v = 0; int n = 0;
        while (*p && isxdigit((unsigned char)*p) && n < 4) {
            v = v * 16 + (unsigned)(*p <= '9' ? *p - '0' : (tolower(*p) - 'a' + 10));
            p++; n++;
        }
        g[gi++] = (uint16_t)v;
        if (*p == ':') p++;
    }
    if (dbl >= 0 && gi < 8) {
        const int move = gi - dbl;
        for (int k = 0; k < move; k++) g[7 - k] = g[gi - 1 - k];
        for (int k = dbl; k < 8 - move; k++) g[k] = 0;
    } else if (dbl < 0 && gi != 8) {
        return false;
    }
    out = 0;
    for (int k = 0; k < 4; k++) out = (out << 16) | g[k];
    return true;
}

void saveCache() {
    const std::string tmp = cachePath() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint32_t magic = 0x314E5341;   // "ASN1"
    const uint32_t n4 = (uint32_t)g_v4.size(), n6 = (uint32_t)g_v6.size(),
                   nn = (uint32_t)g_names.size();
    fwrite(&magic, 4, 1, f); fwrite(&n4, 4, 1, f); fwrite(&n6, 4, 1, f); fwrite(&nn, 4, 1, f);
    if (n4) fwrite(g_v4.data(), sizeof(V4Row), n4, f);
    if (n6) fwrite(g_v6.data(), sizeof(V6Row), n6, f);
    for (const auto& s : g_names) {
        const uint16_t len = (uint16_t)std::min<size_t>(s.size(), 65535);
        fwrite(&len, 2, 1, f);
        if (len) fwrite(s.data(), 1, len, f);
    }
    fclose(f);
    rename(tmp.c_str(), cachePath().c_str());
}

bool loadCache() {
    FILE* f = fopen(cachePath().c_str(), "rb");
    if (!f) return false;
    uint32_t magic = 0, n4 = 0, n6 = 0, nn = 0;
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == 0x314E5341
           && fread(&n4, 4, 1, f) == 1 && fread(&n6, 4, 1, f) == 1 && fread(&nn, 4, 1, f) == 1;
    if (ok && (n4 > 8000000 || n6 > 8000000 || nn > 8000000)) ok = false;
    if (ok) {
        g_v4.resize(n4); g_v6.resize(n6); g_names.clear(); g_names.reserve(nn);
        if (n4 && fread(g_v4.data(), sizeof(V4Row), n4, f) != n4) ok = false;
        if (ok && n6 && fread(g_v6.data(), sizeof(V6Row), n6, f) != n6) ok = false;
        for (uint32_t i = 0; ok && i < nn; i++) {
            uint16_t len = 0;
            if (fread(&len, 2, 1, f) != 1) { ok = false; break; }
            std::string s(len, '\0');
            if (len && fread(&s[0], 1, len, f) != len) { ok = false; break; }
            g_names.push_back(std::move(s));
        }
    }
    fclose(f);
    if (!ok) { g_v4.clear(); g_v6.clear(); g_names.clear(); return false; }
    // ★ The build time is the cache file's mtime — one fewer thing to keep in step, and it
    //   cannot disagree with the file it describes.
    return !g_v4.empty();
}

}  // namespace

void setDir(const std::string& dir) { std::lock_guard<std::mutex> lk(g_mtx); g_dir = dir; }
int count() { std::lock_guard<std::mutex> lk(g_mtx); return (int)(g_v4.size() + g_v6.size()); }
long long updated() { std::lock_guard<std::mutex> lk(g_mtx); return g_updated; }

bool load() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!loadCache()) return false;
    // Build time = the cache file's modification time.
    struct stat st;
    if (::stat(cachePath().c_str(), &st) == 0) g_updated = (long long)st.st_mtime;
    return true;
}

bool stale(int maxAgeDays) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_v4.empty()) return true;
    if (g_updated <= 0) return true;
    return (time(nullptr) - g_updated) > (long long)maxAgeDays * 86400;
}

bool refresh(std::string& err) {
    const std::string tsv = g_dir + "/asn.tsv.tmp";
    // ★ curl piped through gunzip: the daemon has no zlib linked and this is a once-a-week fetch.
    //   Same reasoning as eibi.cpp shelling out to curl rather than growing an HTTP stack.
    const std::string cmd = "curl -fsSL --max-time 180 '" + std::string(kUrl)
                          + "' | gunzip -c > " + tsv + " 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) { err = "download failed"; remove(tsv.c_str()); return false; }

    FILE* f = fopen(tsv.c_str(), "r");
    if (!f) { err = "could not read the download"; return false; }

    std::vector<V4Row>       v4;
    std::vector<V6Row>       v6;
    std::vector<std::string> names;
    std::map<std::string, uint32_t> nameIdx;   // dedup: ~700k rows share ~100k names
    auto intern = [&](const char* s) -> uint32_t {
        auto it = nameIdx.find(s);
        if (it != nameIdx.end()) return it->second;
        const uint32_t i = (uint32_t)names.size();
        names.push_back(s);
        nameIdx.emplace(s, i);
        return i;
    };

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char* fld[5] = {nullptr};
        int n = 0;
        char* p = line;
        fld[n++] = p;
        while (*p && n < 5) { if (*p == '\t') { *p = 0; fld[n++] = p + 1; } p++; }
        if (n < 5) continue;
        // strip the trailing newline from the description
        char* d = fld[4];
        size_t dl = strlen(d);
        while (dl && (d[dl - 1] == '\n' || d[dl - 1] == '\r')) d[--dl] = 0;
        const uint32_t asn = (uint32_t)strtoul(fld[2], nullptr, 10);
        if (!asn) continue;                       // "Not routed" — see the header note
        const uint32_t ni = intern(d);
        if (strchr(fld[0], ':')) {
            uint64_t lo, hi;
            if (!parseV6Hi(fld[0], lo) || !parseV6Hi(fld[1], hi)) continue;
            v6.push_back({lo, hi, asn, ni});
        } else {
            uint32_t lo, hi;
            if (!parseV4(fld[0], lo) || !parseV4(fld[1], hi)) continue;
            v4.push_back({lo, hi, asn, ni});
        }
    }
    fclose(f);
    remove(tsv.c_str());

    if (v4.empty()) { err = "downloaded, but no usable rows"; return false; }
    std::sort(v4.begin(), v4.end(), [](const V4Row& a, const V4Row& b) { return a.lo < b.lo; });
    std::sort(v6.begin(), v6.end(), [](const V6Row& a, const V6Row& b) { return a.lo < b.lo; });

    std::lock_guard<std::mutex> lk(g_mtx);
    g_v4.swap(v4); g_v6.swap(v6); g_names.swap(names);
    g_updated = (long long)time(nullptr);
    saveCache();
    return true;
}

bool lookup(const std::string& ip, uint32_t& asn, std::string& name) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (ip.find(':') != std::string::npos) {
        uint64_t v;
        if (g_v6.empty() || !parseV6Hi(ip.c_str(), v)) return false;
        auto it = std::upper_bound(g_v6.begin(), g_v6.end(), v,
                                   [](uint64_t val, const V6Row& r) { return val < r.lo; });
        if (it == g_v6.begin()) return false;
        --it;
        if (v > it->hi) return false;
        asn = it->asn;
        name = it->name < g_names.size() ? g_names[it->name] : std::string();
        return true;
    }
    uint32_t v;
    if (g_v4.empty() || !parseV4(ip.c_str(), v)) return false;
    auto it = std::upper_bound(g_v4.begin(), g_v4.end(), v,
                               [](uint32_t val, const V4Row& r) { return val < r.lo; });
    if (it == g_v4.begin()) return false;
    --it;
    if (v > it->hi) return false;
    asn = it->asn;
    name = it->name < g_names.size() ? g_names[it->name] : std::string();
    return true;
}

}  // namespace asndb
