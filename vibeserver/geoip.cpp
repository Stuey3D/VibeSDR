// IP -> COUNTRY, from the Regional Internet Registries' own published statistics.
//
// ★★★ WHY THIS SOURCE, AND NOT A GEOLOCATION API.
// The obvious way to put a flag beside a listener is to POST their address to a free
// geolocation service. That is precisely the thing this project refused to do for VPN detection
// (see vibe_admin.h): it hands every visitor's address to a third party, on every connection, for
// a decoration. A receiver that quietly reports its listeners to someone else is not one Stuart
// would want to run, and it is not one anybody should have to trust.
//
// ★★ AND NOT MaxMind GeoLite2 EITHER — which is what UberSDR uses, and is a perfectly good
// database. It needs a signed-up licence key, a EULA, and a redistribution agreement, which for
// an `apt install` appliance means every owner registering an account with an American company
// before they can see a flag. The RIR delegated-extended files have no account, no key and no
// licence to accept: they are the registries' own record of which block was allocated to which
// country, published daily, and free to use.
//
// ★ ACCURACY IS "WHICH COUNTRY WAS THIS BLOCK ALLOCATED TO", nothing finer. That is exactly the
//   granularity a flag implies and no more — there are no cities here, no coordinates, and no
//   pretence of knowing where a person is. A block allocated to a multinational ISP shows that
//   ISP's registered country, which is occasionally wrong and always honest about what it is.
//
// Format (delegated-extended), one record per line:
//   registry|cc|type|start|value|date|status|opaque-id
//   arin|US|ipv4|3.0.0.0|16777216|20100104|allocated|...     value = COUNT OF ADDRESSES
//   ripencc|GB|ipv6|2001:600::|32|19990819|allocated|...     value = PREFIX LENGTH
//
// Same shape as eibi.cpp: curl to a temp file, parse, atomically replace, cache under
// /var/lib/vibeserver, refresh at most daily.
#include "geoip.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace geoip {
namespace {

struct V4Range { uint32_t lo, hi; char cc[2]; };
struct V6Range { uint64_t lo, hi; char cc[2]; };

std::mutex            g_mtx;
std::vector<V4Range>  g_v4;
std::vector<V6Range>  g_v6;
std::string           g_dir = "/var/lib/vibeserver";
long long             g_updated = 0;    // epoch of the cached data

const char* kSources[] = {
    "https://ftp.ripe.net/pub/stats/ripencc/delegated-ripencc-extended-latest",
    "https://ftp.arin.net/pub/stats/arin/delegated-arin-extended-latest",
    "https://ftp.apnic.net/stats/apnic/delegated-apnic-extended-latest",
    "https://ftp.afrinic.net/pub/stats/afrinic/delegated-afrinic-extended-latest",
    "https://ftp.lacnic.net/pub/stats/lacnic/delegated-lacnic-extended-latest",
};

std::string cachePath() { return g_dir + "/geoip.bin"; }
std::string stampPath() { return g_dir + "/geoip.date"; }

bool parseV4(const char* s, uint32_t& out) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

/** Top 64 bits of an IPv6 address. Country allocations are /12../48, all well inside the top
 *  half, so the low 64 bits cannot change the answer and carrying them would double the table. */
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

void sortAndMerge() {
    std::sort(g_v4.begin(), g_v4.end(), [](const V4Range& a, const V4Range& b) { return a.lo < b.lo; });
    std::sort(g_v6.begin(), g_v6.end(), [](const V6Range& a, const V6Range& b) { return a.lo < b.lo; });
}

/** Parse one delegated-extended file into the tables. */
void parseFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        // registry|cc|type|start|value|date|status|...
        char* fld[8] = {nullptr};
        int n = 0;
        char* p = line;
        fld[n++] = p;
        while (*p && n < 8) { if (*p == '|') { *p = 0; fld[n++] = p + 1; } p++; }
        if (n < 7) continue;
        const char* cc    = fld[1];
        const char* type  = fld[2];
        const char* start = fld[3];
        const char* value = fld[4];
        const char* status= fld[6];
        // ★ "allocated" and "assigned" are the two that mean a real holder in a real country.
        //   "available" and "reserved" are unallocated space and must not produce a flag.
        if (strncmp(status, "allocated", 9) != 0 && strncmp(status, "assigned", 8) != 0) continue;
        if (!cc[0] || cc[0] == '|' || strlen(cc) < 2) continue;
        if (strcmp(type, "ipv4") == 0) {
            uint32_t lo;
            if (!parseV4(start, lo)) continue;
            const unsigned long long count = strtoull(value, nullptr, 10);
            if (!count) continue;
            V4Range r;
            r.lo = lo;
            r.hi = (uint32_t)std::min<unsigned long long>((unsigned long long)lo + count - 1, 0xFFFFFFFFull);
            r.cc[0] = cc[0]; r.cc[1] = cc[1];
            g_v4.push_back(r);
        } else if (strcmp(type, "ipv6") == 0) {
            uint64_t lo;
            if (!parseV6Hi(start, lo)) continue;
            const int bits = atoi(value);
            if (bits <= 0 || bits > 64) {
                // A prefix longer than /64 cannot be distinguished in the top 64 bits; treat it
                // as a single /64, which is the finest this table represents.
                if (bits > 64) { V6Range r; r.lo = lo; r.hi = lo; r.cc[0]=cc[0]; r.cc[1]=cc[1]; g_v6.push_back(r); }
                continue;
            }
            V6Range r;
            r.lo = lo;
            r.hi = bits == 64 ? lo : (lo | ((uint64_t)~0ull >> bits));
            r.cc[0] = cc[0]; r.cc[1] = cc[1];
            g_v6.push_back(r);
        }
    }
    fclose(f);
}

void saveCache() {
    const std::string tmp = cachePath() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint32_t magic = 0x50494F47;   // "GOIP"
    const uint32_t n4 = (uint32_t)g_v4.size(), n6 = (uint32_t)g_v6.size();
    fwrite(&magic, 4, 1, f); fwrite(&n4, 4, 1, f); fwrite(&n6, 4, 1, f);
    if (n4) fwrite(g_v4.data(), sizeof(V4Range), n4, f);
    if (n6) fwrite(g_v6.data(), sizeof(V6Range), n6, f);
    fclose(f);
    rename(tmp.c_str(), cachePath().c_str());
    if (FILE* s = fopen(stampPath().c_str(), "w")) {
        fprintf(s, "%lld\n", (long long)time(nullptr));
        fclose(s);
    }
}

bool loadCache() {
    FILE* f = fopen(cachePath().c_str(), "rb");
    if (!f) return false;
    uint32_t magic = 0, n4 = 0, n6 = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x50494F47) { fclose(f); return false; }
    if (fread(&n4, 4, 1, f) != 1 || fread(&n6, 4, 1, f) != 1) { fclose(f); return false; }
    // Sanity: a corrupt length must not make us allocate gigabytes.
    if (n4 > 5000000 || n6 > 5000000) { fclose(f); return false; }
    g_v4.resize(n4); g_v6.resize(n6);
    bool ok = true;
    if (n4 && fread(g_v4.data(), sizeof(V4Range), n4, f) != n4) ok = false;
    if (n6 && fread(g_v6.data(), sizeof(V6Range), n6, f) != n6) ok = false;
    fclose(f);
    if (!ok) { g_v4.clear(); g_v6.clear(); return false; }
    if (FILE* s = fopen(stampPath().c_str(), "r")) {
        char b[64] = {0};
        if (fgets(b, sizeof b, s)) g_updated = atoll(b);
        fclose(s);
    }
    return true;
}

}  // namespace

void setDir(const std::string& dir) { std::lock_guard<std::mutex> lk(g_mtx); g_dir = dir; }

bool load() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_v4.clear(); g_v6.clear();
    return loadCache() && !g_v4.empty();
}

int count() { std::lock_guard<std::mutex> lk(g_mtx); return (int)(g_v4.size() + g_v6.size()); }
long long updated() { std::lock_guard<std::mutex> lk(g_mtx); return g_updated; }

bool refresh(std::string& err) {
    std::vector<std::string> got;
    for (const char* url : kSources) {
        const std::string tmp = g_dir + "/geoip.src.tmp";
        // ★ curl, for the same reason eibi.cpp uses it: the daemon has no TLS stack of its own,
        //   and this is a once-a-day fetch. -f so an HTTP error is a failure, not a saved error page.
        const std::string cmd = "curl -fsSL --max-time 120 -o " + tmp + " '" + url + "' 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) continue;    // one registry down must not lose the rest
        got.push_back(tmp);
        std::lock_guard<std::mutex> lk(g_mtx);
        parseFile(tmp);
        remove(tmp.c_str());
    }
    if (got.empty()) { err = "could not download any registry file"; return false; }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_v4.empty()) { err = "downloaded, but no usable records"; return false; }
    sortAndMerge();
    g_updated = (long long)time(nullptr);
    saveCache();
    return true;
}

bool stale(int maxAgeDays) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_v4.empty()) return true;
    if (g_updated <= 0) return true;
    return (time(nullptr) - g_updated) > (long long)maxAgeDays * 86400;
}

std::string lookup(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (ip.find(':') != std::string::npos) {
        uint64_t v;
        if (g_v6.empty() || !parseV6Hi(ip.c_str(), v)) return {};
        // Last range whose lo <= v.
        auto it = std::upper_bound(g_v6.begin(), g_v6.end(), v,
                                   [](uint64_t val, const V6Range& r) { return val < r.lo; });
        if (it == g_v6.begin()) return {};
        --it;
        if (v > it->hi) return {};
        return std::string(it->cc, 2);
    }
    uint32_t v;
    if (g_v4.empty() || !parseV4(ip.c_str(), v)) return {};
    auto it = std::upper_bound(g_v4.begin(), g_v4.end(), v,
                               [](uint32_t val, const V4Range& r) { return val < r.lo; });
    if (it == g_v4.begin()) return {};
    --it;
    if (v > it->hi) return {};
    return std::string(it->cc, 2);
}

}  // namespace geoip
