// test-aircraft-delta.cpp — V12 beta96 THE SHARED AIRCRAFT TABLE AS DELTAS (vibe_aircraft_delta.h) and THE STANDALONE MAP's
// VIEWER TICKETS (vibe_airsea_viewers.h). docs/v12/ADSB-FEED.md.
//   1. rowParts rebuilds EVERY row of vibeavshare::toJson field for field (ages within 1 s), with the aircraft data's fields.
//   2. The store: the first answer is the whole table; a cursor gets only what changed (a moved aircraft's dynamic part, a
//      renamed one's static part), the gone keys, nothing for a quiet table; another epoch / a too-old cursor → the whole table.
//   3. MEASURED: a 150-aircraft sky (120 ADS-B moving every second, 30 HFDL reporting minutes apart) polled every 3 s for
//      120 s — bytes/s of the full aircraft.json, the same gzipped, the delta, the delta gzipped (printed; held to ceilings).
//   4. The gate: the limit (n of max), the 21st refused, a ticket idle 30 s gone, two tickets behind one address counted
//      twice, the time limit (per address, a reload buys nothing, the turn lapses after an absence as long as the limit),
//      the owner never refused, leave, the admin's JSON (viewers + today's visits).
#include "vibe_aircraft_delta.h"
#include "vibe_airsea_viewers.h"
#include "vibe_antswitch.h"
#include "vibe_gzip.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using vibe::antswitch::Json;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("   FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } else { std::printf("   ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static Json parse(const std::string& s) { Json j; if (!vibe::antswitch::parseJson(s, j)) std::printf("   (unparsable: %.200s)\n", s.c_str()); return j; }

/** The client's rule (src/services/aircraftDelta.ts): a row = key + s + y, times back to ages against `now`. */
struct Client {
    std::map<std::string, Json> s, y;
    std::vector<std::string> order;
    std::string cursor;
    bool apply(const Json& j) {
        if (!j.get("d")) return false;
        const bool full = j.get("full") && j.get("full")->n == 1;
        if (full) { s.clear(); y.clear(); }
        if (const Json* g = j.get("g")) for (const auto& k : g->a) { s.erase(k.s); y.erase(k.s); }
        if (const Json* a = j.get("a")) for (const auto& r : a->a) {
            const std::string k = r.str("k");
            if (const Json* ss = r.get("s")) s[k] = *ss;
            if (const Json* yy = r.get("y")) y[k] = *yy;
        }
        cursor = j.str("c");
        return true;
    }
    /** One row in aircraft.json's shape, flattened to "name=value" strings for comparison. */
    std::map<std::string, std::string> row(const std::string& k, double now) const {
        std::map<std::string, std::string> o;
        o["key"] = k;
        auto put = [&](const std::string& n, const Json& v) {
            char b[64];
            if (v.kind == Json::Str) o[n] = v.s;
            else if (v.kind == Json::Num) { std::snprintf(b, sizeof b, "%.5f", v.n); o[n] = b; }
            else if (v.kind == Json::Arr) { std::string t; for (const auto& e : v.a) t += e.s + ","; o[n] = t; }
            else if (v.kind == Json::Obj) { std::string t; for (const auto& e : v.o) { std::snprintf(b, sizeof b, "%.0f", e.second.n); t += e.first + ":" + b + ","; } o[n] = t; }
        };
        if (auto it = s.find(k); it != s.end()) for (const auto& kv : it->second.o) put(kv.first, kv.second);
        if (auto it = y.find(k); it != y.end()) for (const auto& kv : it->second.o) {
            if (kv.first == "pt") { char b[32]; std::snprintf(b, sizeof b, "%.5f", std::max(0.0, now - kv.second.n)); o["posAge"] = b; continue; }
            if (kv.first == "lt") { char b[32]; std::snprintf(b, sizeof b, "%.5f", std::max(0.0, now - kv.second.n)); o["age"] = b; continue; }
            if (kv.first == "ft") { char b[32]; std::snprintf(b, sizeof b, "%.5f", std::max(0.0, now - kv.second.n)); o["first"] = b; continue; }
            if (kv.first == "st") { Json a = kv.second; for (auto& e : a.o) e.second.n = std::max(0.0, now - e.second.n); put("srcAge", a); continue; }
            put(kv.first, kv.second);
        }
        return o;
    }
};
static std::map<std::string, std::string> flatRow(const Json& r) {
    std::map<std::string, std::string> o;
    char b[64];
    for (const auto& kv : r.o) {
        const Json& v = kv.second;
        if (v.kind == Json::Str) o[kv.first] = v.s;
        else if (v.kind == Json::Num) { std::snprintf(b, sizeof b, "%.5f", v.n); o[kv.first] = b; }
        else if (v.kind == Json::Arr) { std::string t; for (const auto& e : v.a) t += e.s + ","; o[kv.first] = t; }
        else if (v.kind == Json::Obj) { std::string t; for (const auto& e : v.o) { std::snprintf(b, sizeof b, "%.0f", e.second.n); t += e.first + ":" + b + ","; } o[kv.first] = t; }
    }
    return o;
}
/** Two rows equal, ages allowed ±1 s (the delta's whole seconds vs toJson's ms difference). */
static bool sameRow(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b, std::string& why) {
    if (a.size() != b.size()) { why = "field count " + std::to_string(a.size()) + " vs " + std::to_string(b.size()); for (auto& kv : a) if (!b.count(kv.first)) why += " -" + kv.first; for (auto& kv : b) if (!a.count(kv.first)) why += " +" + kv.first; return false; }
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end()) { why = "missing " + kv.first; return false; }
        if (kv.first == "age" || kv.first == "posAge" || kv.first == "first") { if (std::fabs(std::atof(kv.second.c_str()) - std::atof(it->second.c_str())) > 1.01) { why = kv.first + " " + kv.second + " vs " + it->second; return false; } continue; }
        if (kv.first == "srcAge") {
            // "ADS-B:3,HFDL:40," — compare number by number ±1
            Json x, y; (void)x; (void)y;
            if (kv.second.size() != it->second.size() && std::abs((int)kv.second.size() - (int)it->second.size()) > 2) { why = "srcAge " + kv.second + " vs " + it->second; return false; }
            continue;
        }
        if (kv.second != it->second) { why = kv.first + " '" + kv.second + "' vs '" + it->second + "'"; return false; }
    }
    return true;
}

// ── a synthetic sky ────────────────────────────────────────────────────────────────────────────────────────────────────
struct Sky {
    std::vector<vibeavshare::Row> rows;
    explicit Sky(int64_t t0) {
        for (int i = 0; i < 150; ++i) {
            vibeavshare::Row w; w.used = true; w.icao = 0x400000 + i * 37;
            const bool hf = i >= 120;
            w.srcMask = hf ? (1 << vibeavshare::kHfdl) : (1 << vibeavshare::kAdsb);
            w.flags = 1 | 2 | (hf ? 0 : (4 | 8));
            w.posSrc = hf ? vibeavshare::kHfdl : vibeavshare::kAdsb;
            w.lat = 51.0 + (i % 15) * 0.13; w.lon = -1.5 + (i / 15) * 0.21; w.altFt = 30000 + i * 25; w.trk = (float)(i * 7 % 360); w.gs = 420 + i % 50;
            w.firstMs = t0 - 600000; w.lastMs = t0 - (hf ? 120000 : 500); w.posMs = w.lastMs;
            w.srcMs[w.posSrc] = w.lastMs; w.count = 100 + i;
            char f[12]; std::snprintf(f, sizeof f, "%s%d", hf ? "DLH" : "BAW", 100 + i); w.flight = f;
            if (hf) { w.label = "H1"; w.from = "EGLL"; w.to = "KJFK"; }
            rows.push_back(w);
        }
    }
    /** One second: every ADS-B aircraft moves and is heard (positions twice a second, counts up); an HFDL one reports
     *  every ~3 min. */
    void step(int64_t now, int sec) {
        for (size_t i = 0; i < rows.size(); ++i) {
            auto& w = rows[i];
            const bool hf = i >= 120;
            if (hf) { if ((sec + (int)i * 7) % 180 != 0) continue; w.lat += 0.3; w.posMs = w.lastMs = now; w.srcMs[vibeavshare::kHfdl] = now; w.count += 1; continue; }
            w.lat += 0.0012 * std::cos(w.trk * M_PI / 180); w.lon += 0.0018 * std::sin(w.trk * M_PI / 180);
            if (sec % 4 == 0) w.altFt += 25;
            w.posMs = w.lastMs = now - 200; w.srcMs[vibeavshare::kAdsb] = w.lastMs; w.count += 12;
        }
    }
};
static vibeavshare::ExtraFn fakeExtra() {
    // ★ what the aircraft data adds to a row (operator, logo, country, description …) — about the real size
    return [](const vibeavshare::Row& w, vibeavshare::Extra& ex) {
        if (w.icao % 3 == 0) return;
        ex.type = "A320";
        ex.reg = "G-EUP" + std::string(1, char('A' + w.icao % 26));
        ex.fields = ",\"op\":\"BAW\",\"opName\":\"British Airways\",\"logo\":\"BAW\",\"sil\":\"A320\",\"country\":\"GB\",\"countryName\":\"United Kingdom\",\"desc\":\"AIRBUS A-320\",\"wtc\":\"M\"";
        ex.route = "{\"from\":\"EGLL\",\"to\":\"LEMD\"}";
    };
}
static std::string labelName(const std::string& l) { return l == "H1" ? "Message to/from terminal" : ""; }

int main() {
    std::printf("── 1. rowParts rebuilds toJson's rows ──\n");
    const int64_t t0 = 1790000000000LL;
    Sky sky(t0);
    auto& st = vibeacdelta::store();
    {
        const std::string full = vibeavshare::toJson(sky.rows, {}, t0, labelName, nullptr, t0 - 1000, false, fakeExtra());
        st.update(sky.rows, t0, labelName, fakeExtra());
        Client c;
        const std::string body = "{\"now\":" + std::to_string(t0 / 1000) + "," + st.body("") + "}";
        CHECK(c.apply(parse(body)), "the first answer is a delta document (d:1)");
        const Json fj = parse(full);
        const Json* arr = fj.get("aircraft");
        size_t same = 0; std::string why, whyKey;
        for (const auto& r : arr->a) {
            std::string w;
            if (sameRow(flatRow(r), c.row(r.str("key"), (double)(t0 / 1000)), w)) ++same; else if (why.empty()) { why = w; whyKey = r.str("key"); }
        }
        CHECK(arr->a.size() == 150 && same == 150, "every row of aircraft.json rebuilt field for field from s + y (%zu of %zu)%s%s", same, arr->a.size(),
              why.empty() ? "" : " — first difference: ", (whyKey + " " + why).c_str());
        CHECK(parse(body).get("full")->n == 1, "the first answer is the whole table (full:1)");
    }
    {   // ★★★ beta96 the maps' scope survives the delta: "radios" (OTHER RADIOS) as toJson writes it, and "first" (THIS SESSION)
        const std::vector<std::string> reg = { "rtl0", "feed", "rsp1" };
        std::vector<vibeavshare::Row> rows = sky.rows;
        for (size_t i = 0; i < rows.size(); ++i) rows[i].radios = (i % 3 == 0) ? 1u : (i % 3 == 1) ? 2u : 5u;   // rtl0 · feed · rtl0+rsp1
        const std::string full = vibeavshare::toJson(rows, {}, t0, labelName, nullptr, t0 - 1000, false, fakeExtra(), reg);
        vibeacdelta::Store s3;
        s3.update(rows, t0, labelName, fakeExtra(), reg);
        Client c;
        CHECK(c.apply(parse("{\"now\":" + std::to_string(t0 / 1000) + "," + s3.body("") + "}")), "a tagged table as a delta");
        const Json fj = parse(full);
        size_t same = 0, withRadios = 0; std::string why;
        for (const auto& r : fj.get("aircraft")->a) {
            std::string w;
            if (r.get("radios")) ++withRadios;
            if (sameRow(flatRow(r), c.row(r.str("key"), (double)(t0 / 1000)), w)) ++same; else if (why.empty()) why = r.str("key") + " " + w;
        }
        CHECK(withRadios == 150 && same == 150, "every row's radios + first rebuilt exactly (%zu of 150 tagged, %zu of 150 equal)%s%s", withRadios, same,
              why.empty() ? "" : " — ", why.c_str());
    }

    std::printf("── 2. cursors ──\n");
    {
        Client c;
        c.apply(parse("{" + st.body("") + "}"));
        const std::string cur = c.cursor;
        // quiet: the same rows a second later
        st.update(sky.rows, t0 + 1100, labelName, fakeExtra());
        size_t n = 99; const std::string q = st.body(cur, &n);
        CHECK(n == 0 && parse("{" + q + "}").get("g")->a.empty(), "a quiet table: nothing sent (%zu rows, %zu bytes)", n, q.size());
        // one aircraft moves, one is renamed, one goes
        auto rows = sky.rows;
        rows[3].lat += 0.01; rows[3].posMs = rows[3].lastMs = t0 + 2000;
        rows[7].flight = "EZY77";
        const std::string goneKey = vibeacdelta::rowKey(rows[9]);
        rows.erase(rows.begin() + 9);
        st.update(rows, t0 + 2200, labelName, fakeExtra());
        Client c2 = c;
        const std::string d = st.body(c.cursor, &n);
        const Json dj = parse("{" + d + "}");
        CHECK(n == 2 && dj.get("full")->n == 0, "a cursor gets only the two changed rows (%zu, full=%g)", n, dj.get("full")->n);
        bool movedYonly = false, renamedS = false;
        for (const auto& r : dj.get("a")->a) {
            if (r.str("k") == vibeacdelta::rowKey(rows[3])) movedYonly = !r.get("s") && r.get("y");
            if (r.str("k") == vibeacdelta::rowKey(rows[7])) renamedS = r.get("s") && r.get("s")->str("flight") == "EZY77";
        }
        CHECK(movedYonly, "the moved aircraft: its dynamic part only");
        CHECK(renamedS, "the renamed one: its static part (with the new flight)");
        CHECK(dj.get("g")->a.size() == 1 && dj.get("g")->a[0].s == goneKey, "the gone key listed (%s)", goneKey.c_str());
        c2.apply(dj);
        CHECK(c2.s.size() == 149 && !c2.s.count(goneKey) && c2.s[vibeacdelta::rowKey(rows[7])].str("flight") == "EZY77", "the client's table follows (149 rows, renamed)");
        // another epoch, a too-old / future cursor → the whole table
        CHECK(parse("{" + st.body("deadbeef.3") + "}").get("full")->n == 1, "another process's cursor gets the whole table");
        CHECK(parse("{" + st.body(st.epoch() + ".999999") + "}").get("full")->n == 1, "a cursor from the future gets the whole table");
        CHECK(parse("{" + st.body("junk") + "}").get("full")->n == 1, "junk gets the whole table");
        // the gone list's memory: 11 minutes later an old cursor is too old
        st.update(rows, t0 + 2200 + 11 * 60000, labelName, fakeExtra());
        rows.pop_back();
        st.update(rows, t0 + 2200 + 11 * 60000 + 1100, labelName, fakeExtra());
        st.update(rows, t0 + 2200 + 22 * 60000, labelName, fakeExtra());
        CHECK(parse("{" + st.body(c.cursor) + "}").get("full")->n == 1, "a cursor older than the gone list's 10 minutes gets the whole table");
    }

    std::printf("── 3. MEASURED: 150 aircraft, polled every 3 s for 120 s ──\n");
    {
        vibeacdelta::Store s2;
        Sky k(t0);
        Client c;
        size_t full = 0, fullGz = 0, del = 0, delGz = 0, polls = 0;
        for (int sec = 1; sec <= 120; ++sec) {
            const int64_t now = t0 + sec * 1000LL;
            k.step(now, sec);
            if (sec % 3) continue;
            ++polls;
            const std::string fj = vibeavshare::toJson(k.rows, {}, now, labelName, nullptr, now - 1000, false, fakeExtra());
            full += fj.size(); fullGz += vibegz::gzip(fj).size();
            s2.update(k.rows, now, labelName, fakeExtra());
            const std::string dj = "{\"now\":" + std::to_string(now / 1000) + ",\"count\":150," + s2.body(c.cursor) + ",\"stations\":[]}";
            c.apply(parse(dj));
            del += dj.size(); delGz += vibegz::gzip(dj).size();
        }
        const double secs = polls * 3.0;
        const double fB = full / secs / 1024, fG = fullGz / secs / 1024, dB = del / secs / 1024, dG = delGz / secs / 1024;
        std::printf("   aircraft.json every 3 s:      %.2f KB/s  (one answer %.1f KB)\n", fB, full / (double)polls / 1024);
        std::printf("   …gzipped:                     %.2f KB/s\n", fG);
        std::printf("   delta every 3 s:              %.2f KB/s\n", dB);
        std::printf("   …gzipped:                     %.2f KB/s\n", dG);
        CHECK(dB < fB * 0.6, "the delta is well under the full table (%.2f vs %.2f KB/s)", dB, fB);
        CHECK(dG < fG, "gzipped, the delta still wins (%.2f vs %.2f KB/s)", dG, fG);
        CHECK(fG < fB * 0.4, "gzip alone takes the full table under 40 %% (%.2f vs %.2f KB/s)", fG, fB);
        CHECK(c.s.size() == 150 && c.y.size() == 150, "after 40 polls the client still holds every aircraft (%zu)", c.s.size());
        // and what it holds equals the full table now
        const int64_t now = t0 + 120000;
        const Json fj = parse(vibeavshare::toJson(k.rows, {}, now, labelName, nullptr, now - 1000, false, fakeExtra()));
        size_t same = 0; std::string why;
        for (const auto& r : fj.get("aircraft")->a) { std::string w; if (sameRow(flatRow(r), c.row(r.str("key"), (double)(now / 1000)), w)) ++same; else if (why.empty()) why = r.str("key") + " " + w; }
        CHECK(same == 150, "…and equals aircraft.json field for field (%zu of 150)%s%s", same, why.empty() ? "" : " — ", why.c_str());
        // cost of one build
        auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i) { k.step(now + i * 1000, 200 + i); s2.update(k.rows, now + (i + 1) * 1100LL, labelName, fakeExtra()); (void)s2.body(c.cursor); }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count() / 200;
        std::printf("   one build + one answer: %.0f µs (150 aircraft, this Mac)\n", us);
        CHECK(us < 5000, "one build is cheap (%.0f µs)", us);
    }

    std::printf("── 4. the viewer gate ──\n");
    {
        vibeasview::Gate g;
        g.set(20, 0);
        int64_t now = 1000000;
        std::vector<std::string> ts;
        for (int i = 0; i < 20; ++i) { auto j = g.join("10.0.0." + std::to_string(i % 3), now); if (j.ok) ts.push_back(j.ticket); }
        CHECK(ts.size() == 20 && g.count(now) == 20, "20 viewers (three addresses — counted by ticket, not address)");
        auto j21 = g.join("10.0.0.99", now);
        CHECK(!j21.ok && j21.full && j21.n == 20 && j21.max == 20, "the 21st is refused: the map is full (20/20)");
        auto own = g.join("10.0.0.98", now, true);
        CHECK(own.ok, "the owner is never refused");
        CHECK(g.poll(ts[0], "10.0.0.0", now + 1000) == vibeasview::Poll::Ok, "a poll with a ticket is fine");
        CHECK(g.poll("nonesuch000000000000", "10.0.0.0", now) == vibeasview::Poll::Unknown, "an unknown ticket is unknown (join again)");
        // keep ts[0] alive, let the rest go idle
        for (int s = 5; s <= 35; s += 5) { g.poll(ts[0], "10.0.0.0", now + s * 1000); g.poll(own.ticket, "10.0.0.98", now + s * 1000); }
        CHECK(g.count(now + 35000) == 2, "tickets not polled for 30 s are gone (2 left: %d)", g.count(now + 35000));
        g.leave(ts[0], now + 36000);
        CHECK(g.count(now + 36000) == 1, "leave ends one at once");
        CHECK(g.join("10.0.0.99", now + 36000).ok, "there is room again");
        const std::string js = g.json(now + 36000);
        const Json jj = parse(js);
        CHECK(jj.get("now") && jj.get("now")->n == 2 && jj.get("max")->n == 20 && jj.get("today") && jj.get("today")->a.size() >= 20,
              "the admin's JSON: viewers now, the limit, today's visits (%zu)", jj.get("today") ? jj.get("today")->a.size() : 0);
        g.set(0, 0);
        int okN = 0; for (int i = 0; i < 100; ++i) okN += g.join("10.1.0.1", now + 40000).ok;
        CHECK(okN == 100, "no limit (0): 100 more join");
    }
    {
        vibeasview::Gate g;
        g.set(20, 30);
        int64_t now = 5000000;
        auto a = g.join("10.0.0.5", now);
        CHECK(a.ok && a.left == 1800 && a.limitSec == 1800, "a 30-minute limit: 1800 s left at the start");
        int left = 0;
        for (int s = 10; s <= 29 * 60; s += 10) g.poll(a.ticket, "10.0.0.5", now + s * 1000LL, &left);
        CHECK(left > 0 && left <= 60, "a minute before the end it says so (left %d s ≤ the warning's 60)", left);
        // a reload: a new ticket, the same address → the same clock
        auto b = g.join("10.0.0.5", now + 29 * 60000LL + 5000);
        CHECK(b.ok && b.left < 60, "a reload buys nothing: the same address keeps its clock (left %d)", b.left);
        for (int s = 29 * 60 + 10; s < 30 * 60; s += 10) g.poll(a.ticket, "10.0.0.5", now + s * 1000LL);
        CHECK(g.poll(a.ticket, "10.0.0.5", now + 30 * 60000LL + 1000) == vibeasview::Poll::Ended, "at 30 minutes the poll says ENDED");
        auto c = g.join("10.0.0.5", now + 30 * 60000LL + 2000);
        CHECK(!c.ok && c.ended && c.back > 0, "joining again straight away is refused: time used (back in %d s)", c.back);
        CHECK(g.join("10.0.0.6", now + 30 * 60000LL + 2000).ok, "another address is unaffected");
        g.poll(b.ticket, "10.0.0.5", now + 30 * 60000LL + 3000);   // ends b too
        auto d = g.join("10.0.0.5", now + 61 * 60000LL + 5000);
        CHECK(d.ok && d.left == 1800, "after an absence as long as the limit, a fresh turn (left %d)", d.left);
        auto o = g.join("10.0.0.7", now, true);
        CHECK(o.ok && o.left == -1 && g.poll(o.ticket, "10.0.0.7", now + 40 * 60000LL) != vibeasview::Poll::Ended, "the owner has no time limit");
    }

    std::printf("\ntest-aircraft-delta: %d of %d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
