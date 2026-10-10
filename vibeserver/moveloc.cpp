// moveloc.cpp — see moveloc.h. The nearest town (or region) as the server moves; privacy first.
#include "moveloc.h"
#include "proc.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vsmoveloc {
namespace {

std::mutex  g_mtx;
std::string g_dir = "/var/lib/vibeserver";

// ── tiny JSON readers: the two answers we read are flat and small, and their keys are unique ──
std::string strAfter(const std::string& s, const std::string& key, size_t from = 0) {
    const std::string k = "\"" + key + "\"";
    size_t i = s.find(k, from);
    if (i == std::string::npos) return {};
    size_t c = s.find(':', i + k.size());
    if (c == std::string::npos) return {};
    size_t p = c + 1;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    if (p >= s.size()) return {};
    if (s[p] == '"') {
        std::string out;
        for (size_t q = p + 1; q < s.size(); q++) {
            if (s[q] == '\\' && q + 1 < s.size()) { out += s[++q]; continue; }
            if (s[q] == '"') return out;
            out += s[q];
        }
        return {};
    }
    size_t e = p;
    while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']' && s[e] != ' ' && s[e] != '\n') e++;
    return s.substr(p, e - p);
}
bool numAfter(const std::string& s, const std::string& key, double& v, size_t from = 0) {
    const std::string t = strAfter(s, key, from);
    if (t.empty()) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && end != t.c_str() && std::isfinite(v);
}
std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}

/** A cheap fingerprint of the machine's IPv4 addresses — it changes when the network does. */
std::string netSignature() {
    std::set<std::string> a;
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0) return {};
    for (auto* p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        char b[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, b, sizeof b);
        a.insert(std::string(p->ifa_name) + "=" + b);
    }
    ::freeifaddrs(ifs);
    std::string s;
    for (const auto& x : a) s += x + ";";
    return s;
}

std::string toJson(const Place& p) {
    char b[96];
    std::snprintf(b, sizeof b, "\"lat\":%.5f,\"lon\":%.5f", p.lat, p.lon);
    return "{\"label\":\"" + jesc(p.label) + "\",\"region\":" + (p.region ? "true" : "false") + "," + b
         + ",\"grid\":\"" + jesc(p.grid) + "\",\"iso\":\"" + jesc(p.iso) + "\"}\n";
}

/** ★ Written ONLY when the place changes, and atomically (temp + fsync + rename): on a pocket box this
 *  is the SD card, and the power can go at any moment. */
bool writeAtomic(const std::string& path, const std::string& body) {
    const std::string tmp = path + ".tmp";
    ::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < body.size()) {
        const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += (size_t)n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}

Place parseFile(const std::string& s) {
    Place p;
    p.label = strAfter(s, "label");
    p.region = strAfter(s, "region") == "true";
    p.grid = strAfter(s, "grid");
    p.iso = strAfter(s, "iso");
    p.valid = !p.label.empty() && numAfter(s, "lat", p.lat) && numAfter(s, "lon", p.lon)
              && p.lat >= -90 && p.lat <= 90 && p.lon >= -180 && p.lon <= 180;
    return p;
}

const char* kUserAgent = "VibeServer/11 (https://vibesdr.net; location as a server moves, nearest town only)";

std::string curl(std::vector<std::string> extra, const std::string& url) {
    std::vector<std::string> argv{"curl", "-fsS", "--max-time", "15", "--proto", "=https", "-A", kUserAgent};
    for (auto& e : extra) argv.push_back(e);
    argv.push_back(url);
    std::string out;
    vibeproc::run(argv, &out);
    return out;
}

/** One round: scan list → BeaconDB → Nominatim. False when anything along the way had no answer. */
bool lookUp(Place& out) {
    std::string scan;
    vibeproc::run({"nmcli", "-t", "-f", "BSSID,SIGNAL", "dev", "wifi", "list", "--rescan", "no"}, &scan);
    int n = 0;
    const std::string body = beaconBody(scan, n);
    // ★ Two or more, as the MLS API has always asked: a single access point locates nothing reliably.
    if (n < 2) return false;
    const std::string fix = curl({"-H", "content-type: application/json", "--data-binary", body},
                                 "https://api.beacondb.net/v1/geolocate");
    double lat = 0, lon = 0;
    const size_t loc = fix.find("\"location\"");
    if (loc == std::string::npos || !numAfter(fix, "lat", lat, loc) || !numAfter(fix, "lng", lon, loc)) return false;
    char q[160];
    std::snprintf(q, sizeof q, "&lat=%.5f&lon=%.5f", lat, lon);
    // ★★ VILLAGE LEVEL first (zoom 13 — a hamlet or a farm is never an answer), then, failing a town, the
    //    COUNTY level (zoom 8) for a general region. See placeFromNominatim for the rule.
    const std::vector<std::string> en{"-H", "accept-language: en"};
    Place p = placeFromNominatim(curl(en,
        std::string("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=13&addressdetails=1&extratags=1") + q));
    if (!p.valid)
        p = regionFromNominatim(curl(en,
            std::string("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=8&addressdetails=1") + q));
    // ★ The fix itself goes out of scope here and is never stored or logged.
    if (!p.valid) return false;
    out = p;
    return true;
}

} // namespace

void setDir(const std::string& dir) { std::lock_guard<std::mutex> lk(g_mtx); if (!dir.empty()) g_dir = dir; }
std::string stateFile() { std::lock_guard<std::mutex> lk(g_mtx); return g_dir + "/moveloc.json"; }

std::string gridOf(double lat, double lon, int chars) {
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return {};
    double x = lon + 180.0, y = lat + 90.0;
    if (x >= 360) x = 359.999999;
    if (y >= 180) y = 179.999999;
    std::string g;
    g += (char)('A' + (int)(x / 20)); g += (char)('A' + (int)(y / 10));
    x = std::fmod(x, 20); y = std::fmod(y, 10);
    g += (char)('0' + (int)(x / 2)); g += (char)('0' + (int)(y / 1));
    if (chars >= 6) {
        x = std::fmod(x, 2); y = std::fmod(y, 1);
        g += (char)('a' + (int)(x * 12)); g += (char)('a' + (int)(y * 24));
    }
    return g;
}

std::string beaconBody(const std::string& t, int& count) {
    count = 0;
    std::string aps;
    size_t s = 0;
    while (s < t.size()) {
        size_t e = t.find('\n', s);
        if (e == std::string::npos) e = t.size();
        const std::string line = t.substr(s, e - s);
        s = e + 1;
        // "AA\:BB\:CC\:DD\:EE\:FF:72" — the BSSID's colons are escaped, the field separator is not.
        std::string mac, sig; bool inMac = true;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '\\' && i + 1 < line.size()) { (inMac ? mac : sig) += line[++i]; continue; }
            if (line[i] == ':' && inMac) { inMac = false; continue; }
            (inMac ? mac : sig) += line[i];
        }
        if (mac.size() != 17 || sig.empty()) continue;
        bool ok = true;
        for (size_t i = 0; i < 17; i++)
            if (i % 3 == 2 ? mac[i] != ':' : !std::isxdigit((unsigned char)mac[i])) ok = false;
        if (!ok) continue;
        const int pct = std::atoi(sig.c_str());
        if (pct <= 0 || pct > 100) continue;
        // ★ NetworkManager reports 0–100 %; the API wants dBm. NM's own mapping is roughly linear,
        //   -100 dBm at 0 % to -50 dBm at 100 %.
        const int dbm = pct / 2 - 100;
        if (count >= 20) break;      // the strongest few are plenty, and nmcli lists strongest first
        if (!aps.empty()) aps += ",";
        aps += "{\"macAddress\":\"" + mac + "\",\"signalStrength\":" + std::to_string(dbm) + "}";
        count++;
    }
    return "{\"considerIp\":false,\"wifiAccessPoints\":[" + aps + "]}";
}

namespace {
std::string cleanName(const std::string& in) {
    // ★ A place name goes into JSON built by hand downstream: no quotes, backslashes or control bytes.
    std::string out;
    for (unsigned char c : in) if (c >= 0x20 && c != '"' && c != '\\') out += (char)c;
    return out;
}
bool centre(const std::string& js, Place& p) {
    double lat = 0, lon = 0;
    // ★ The RESULT's own lat/lon — the centre of the place it returned, never the fix.
    if (!numAfter(js, "lat", lat) || !numAfter(js, "lon", lon)) return false;
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return false;
    p.lat = lat; p.lon = lon;
    return true;
}
std::string isoOf(const std::string& js, size_t a) {
    std::string iso = strAfter(js, "country_code", a);
    for (auto& c : iso) c = (char)std::toupper((unsigned char)c);
    return iso;
}
} // namespace

Place placeFromNominatim(const std::string& js) {
    // ★★★ THE TOWN RULE (Stuart, 2026-10-11, measured on real look-ups the same night). Asked at VILLAGE
    //     level (zoom 13). A town or city named in the address — the spot is IN it — is the answer (Corby
    //     centre comes back as the suburb "Corby Village" with town=Corby). A VILLAGE only when OSM records
    //     1,000+ people: almost no UK village has a population tag (Moulton, Holcot), and a small village
    //     beside a cabin would give the cabin away, so an unknown size is NOT large. Anything else — not
    //     valid here, and the caller asks for the REGION instead.
    Place p;
    if (js.empty() || js.find("\"error\"") != std::string::npos) return p;
    const size_t a = js.find("\"address\"");
    if (a == std::string::npos || !centre(js, p)) return p;
    std::string town;
    for (const char* k : {"city", "town"}) { town = strAfter(js, k, a); if (!town.empty()) break; }
    const std::string type = strAfter(js, "addresstype");
    if (town.empty() && type == "village") {
        double pop = 0;
        const size_t x = js.find("\"extratags\"");
        if (x != std::string::npos && numAfter(js, "population", pop, x) && pop >= 1000) town = strAfter(js, "name");
    }
    p.label = cleanName(town);
    if (p.label.empty()) return Place{};
    p.region = false;
    p.iso = isoOf(js, a);
    p.grid = gridOf(p.lat, p.lon, 6);
    p.valid = true;
    return p;
}

Place regionFromNominatim(const std::string& js) {
    // ★ No town: a GENERAL REGION — the county, else the state — and its centre, with only a 4-character
    //   square. Highland, Natrona County, Northern Territory; the outback and the Sahara end up here.
    Place p;
    if (js.empty() || js.find("\"error\"") != std::string::npos) return p;
    const size_t a = js.find("\"address\"");
    if (!centre(js, p)) return p;
    std::string r;
    if (a != std::string::npos)
        for (const char* k : {"county", "state_district", "state", "region", "country"}) { r = strAfter(js, k, a); if (!r.empty()) break; }
    if (r.empty()) r = strAfter(js, "name");
    p.label = cleanName(r);
    if (p.label.empty()) return Place{};
    p.region = true;
    p.iso = a != std::string::npos ? isoOf(js, a) : std::string();
    p.grid = gridOf(p.lat, p.lon, 4);
    p.valid = true;
    return p;
}

Place current(bool& changed) {
    static std::mutex m;
    static struct { long long ino = -1, size = -1, mtime = -1; } seen;
    static Place last;
    std::lock_guard<std::mutex> lk(m);
    changed = false;
    struct stat st{};
    if (::stat(stateFile().c_str(), &st) != 0) {
        if (last.valid) { last = Place{}; changed = true; }
        seen = {};
        return last;
    }
    if ((long long)st.st_ino == seen.ino && (long long)st.st_size == seen.size && (long long)st.st_mtime == seen.mtime)
        return last;
    seen.ino = (long long)st.st_ino; seen.size = (long long)st.st_size; seen.mtime = (long long)st.st_mtime;
    std::string body;
    if (FILE* f = std::fopen(stateFile().c_str(), "r")) {
        char b[2048]; size_t n = std::fread(b, 1, sizeof b, f); body.assign(b, n); std::fclose(f);
    }
    const Place p = parseFile(body);
    if (p.valid != last.valid || p.label != last.label || p.region != last.region) changed = true;
    last = p;
    return last;
}

void start(std::function<bool()> enabled) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([enabled] {
        using clock = std::chrono::steady_clock;
        std::string lastSig;
        auto next = clock::now();          // ★ first look straight away once ticked
        Place have; { bool c; have = current(c); }
        for (;;) {
            const bool on = enabled && enabled();
            if (!on) {
                // ★ Tick off ⇒ the file goes, and every process falls back to the HOME location.
                if (::access(stateFile().c_str(), F_OK) == 0) ::unlink(stateFile().c_str());
                have = Place{};
                lastSig.clear();
                next = clock::now();
            } else {
                const std::string sig = netSignature();
                if (sig != lastSig) { lastSig = sig; next = clock::now() + std::chrono::seconds(10); }
                if (clock::now() >= next && !sig.empty()) {
                    Place p;
                    if (lookUp(p)) {
                        if (!have.valid || p.label != have.label || p.region != have.region) {
                            if (writeAtomic(stateFile(), toJson(p)))
                                std::printf("VibeServer: location while moving — %s%s\n",
                                            p.region ? "region " : "", p.label.c_str());
                            have = p;
                        }
                        next = clock::now() + std::chrono::minutes(15);
                    } else {
                        next = clock::now() + std::chrono::minutes(2);   // no Wi-Fi list or no answer yet
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(20));
        }
    }).detach();
}

} // namespace vsmoveloc
