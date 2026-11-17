// test-dab-own.cpp — V12 DAB OWN STATION (docs/v12/DAB-OWN-STATION.md): each listener's own station on one multiplex.
//
//   1. THE RULE (vibe_dab_own.h): N from the benchmark, on/off against the user limit, the owner's switch, the cap, and
//      the setup page's sentence (the page's copy of it is held to these strings by scripts/test-setup-dab-own.mjs).
//   2. THE ROSTER: rules A–E — the primary's service joins it, a playing station is joined, a lone listener retunes the
//      radio's own (never opens a station: the old path exactly), otherwise a station opens; leaving frees it; a block
//      change sends everyone to the primary with their wish pending; a reconnect inside the grace period resumes.
//   3. STATION MODE ON A REAL MULTIPLEX (the bench clip: twelve services, each its own tones): the radio's DabService plays
//      one service while three stations, fed only by its MSC tap, play three others — and EACH STATION'S AUDIO IS ITS OWN
//      SERVICE (the dominant tone set of its PCM), the radio's own decode reads exactly as with no station attached, a block
//      change closes every station, and closing one stops its thread.
// Needs vibeserver/bench-clip/dab-bench-clean.vbu8 (make-clip.sh) or VIBE_DAB_CLIP= for part 3; parts 1–2 always run.
#include "vibe_dab_own.h"
#include "vibe_dab_service.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++fails; } else { std::printf("  ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

static long field(const std::string& j, const char* k) {
    const std::string key = std::string("\"") + k + "\":";
    const size_t p = j.find(key);
    return p == std::string::npos ? -1 : std::atol(j.c_str() + p + key.size());
}

/** ★ IS THIS THE SAME PROGRAMME? Decoding is deterministic, so a station's audio and the same service decoded ALONE by the
 *  radio's own path are the same samples, give or take where each started. Both are folded to mono and averaged down to
 *  4.8 kHz; one second from the middle of `a` is slid across `ref` and the best normalised correlation is returned
 *  (1.0 = the same audio; another service, or noise, sits near 0). No dependence on the clip's recipe. */
static std::vector<float> fold(const std::vector<float>& lr) {
    std::vector<float> m;
    for (size_t i = 0; i + 20 <= lr.size(); i += 20) { float s = 0; for (int k = 0; k < 20; ++k) s += lr[i + k]; m.push_back(s / 20); }
    return m;
}
static double sameAudio(const std::vector<float>& a, const std::vector<float>& ref) {
    const size_t w = 4800;
    if (a.size() < 3 * w || ref.size() < 2 * w) return 0;
    const size_t at = a.size() / 2 - w / 2;
    double ea = 0; for (size_t i = 0; i < w; ++i) ea += double(a[at + i]) * a[at + i];
    double best = 0;
    for (size_t lag = 0; lag + w <= ref.size(); ++lag) {
        double x = 0, er = 0;
        for (size_t i = 0; i < w; ++i) { x += double(a[at + i]) * ref[lag + i]; er += double(ref[lag + i]) * ref[lag + i]; }
        if (ea > 0 && er > 0) best = std::max(best, x / std::sqrt(ea * er));
    }
    return best;
}

/** ★ AND WHAT IT SOUNDS LIKE: the power spectrum 120–900 Hz at 2 Hz steps (Goertzel) on the 4.8 kHz fold, normalised. DAB+
 *  through FAAD2 is not sample-exact between two decodes that started on different super frames (concealment, priming),
 *  so an AAC station is also named by the reference whose spectrum it matches best. */
static std::vector<double> fingerprint(const std::vector<float>& m) {
    std::vector<double> v;
    for (int f = 120; f < 900; f += 2) {
        const double w = 2 * M_PI * f / 4800.0, c = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (float x : m) { const double s0 = x + c * s1 - s2; s2 = s1; s1 = s0; }
        v.push_back(std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)));
    }
    double mu = 0; for (double x : v) mu += x; mu /= double(v.size());
    double e = 0; for (double& x : v) { x -= mu; e += x * x; }
    e = std::sqrt(e); if (e > 0) for (double& x : v) x /= e;
    return v;
}
static double corr(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0; for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) s += a[i] * b[i]; return s;
}

static void rules() {
    using namespace vibedab::own;
    std::printf("── 1. the rule ──\n");
    CHECK(nFromBench(18.0, 95.0, 4) == 1 + int((280 - 95 - 70) / 18.0), "Pi 2 class: 4 cores, front end 95 %%, 18 %% a station -> N = %d", nFromBench(18.0, 95.0, 4));
    CHECK(nFromBench(18.0, 95.0, 1) == 1, "one core -> 1 (no own stations)");
    CHECK(nFromBench(0.5, 12.0, 12) == 16, "a fast box is capped at 16");
    CHECK(nFromBench(-1, 50, 4) == 0, "no row -> 0 (not measured)");
    CHECK(effectiveOn(-1, 6, 5) && effectiveOn(-1, 5, 5) && !effectiveOn(-1, 4, 5), "rule: N >= limit on, N < limit off");
    CHECK(!effectiveOn(-1, 0, 1), "not measured -> off under the rule");
    CHECK(effectiveOn(1, 2, 10) && !effectiveOn(0, 16, 2), "the owner's switch always wins (never hide)");
    CHECK(stationCap(-1, 6, 5) == 5 && stationCap(-1, 4, 5) == 0, "cap = N - 1 when on, 0 when off");
    CHECK(stationCap(1, 2, 10) == 9, "forced on above N: a station per listener (limit - 1)");
    CHECK(setupText(-1, 3, 5) == "Off: this box can give up to 3 listeners their own DAB station. Lower the listener limit to 3 to turn it on.",
          "setup text, off by the rule: '%s'", setupText(-1, 3, 5).c_str());
    CHECK(setupText(-1, 8, 5) == "On: this box can give up to 8 listeners their own DAB station, and the listener limit is 5.",
          "setup text, on by the rule");
    CHECK(setupText(-1, 0, 5).rfind("Not measured yet", 0) == 0, "setup text, not measured");
    CHECK(setupText(1, 3, 5).rfind("On by your choice", 0) == 0 && setupText(0, 8, 5).rfind("Off by your choice", 0) == 0, "setup text, the owner's choice");

    std::printf("── 2. the roster ──\n");
    Roster r;
    const uint32_t P = 0xC001;
    r.arrive("a", 0); r.arrive("b", 0); r.arrive("c", 0);
    Action x = r.pick("a", 0xC002, P, 2);
    CHECK(x.kind == Action::OpenStation && r.stationOf("a") == 0xC002, "A picks another service while B and C hear the primary: a station opens");
    x = r.pick("b", 0xC002, P, 2);
    CHECK(x.kind == Action::JoinStation && r.stationOf("b") == 0xC002 && r.stations().size() == 1, "B picks the same: joins it (one decode)");
    x = r.pick("c", 0xC003, P, 2);
    CHECK(x.kind == Action::RetunePrimary, "C alone on the primary picks a third: the primary retunes (no new station)");
    const auto pl = r.playing(0xC003, { "a", "b", "c" });
    CHECK(pl.size() == 2 && pl[0].first == 0xC002 && pl[0].second == 2 && pl[1].first == 0xC003 && pl[1].second == 1,
          "playing: C002 x2, C003 x1");
    x = r.pick("a", 0xC003, 0xC003, 2);
    CHECK(x.kind == Action::JoinPrimary && x.closeSid == 0 && r.stationOf("a") == 0, "A joins the primary's service; B still holds C002");
    x = r.pick("b", 0xC004, 0xC003, 2);
    CHECK(x.kind == Action::OpenStation && x.closeSid == 0xC002, "B, last on C002, moves on: C002 closes, C004 opens");
    CHECK(r.leave("b", 10) == 0xC004, "B leaves: its station empties");
    CHECK(r.arrive("b", 50) == 0xC004, "B back within the grace period: resumes C004");
    r.leave("b", 60);
    CHECK(r.arrive("b", 500) == 0, "B back after the grace period: the primary");
    // the cap (E)
    Roster q; q.arrive("a", 0); q.arrive("b", 0); q.arrive("c", 0);
    q.pick("a", 0xC002, P, 1);
    x = q.pick("b", 0xC003, P, 1);
    CHECK(x.kind == Action::SwitchAll, "no station left to open: the pick switches the primary, as before the feature");
    // single listener: never a station
    Roster one; one.arrive("solo", 0);
    CHECK(one.pick("solo", 0xC005, P, 15).kind == Action::RetunePrimary && one.stations().empty(), "one listener: the radio's own station follows them");
    // block change
    Roster bc; bc.arrive("a", 0); bc.arrive("b", 0);
    bc.pick("a", 0xC002, P, 3);
    bc.blockChanged();
    CHECK(bc.stationOf("a") == 0 && bc.pending().count("a") && bc.pending().at("a") == 0xC002, "block change: everyone on the primary, A's wish pending");
}

struct Clip { double rate = 0, centre = 0; uint32_t sid = 0; std::vector<float> iq; };

static int stationMode() {
    std::printf("── 3. station mode on the bench multiplex ──\n");
    const char* env = std::getenv("VIBE_DAB_CLIP");
    const std::string path = env && *env ? env : "vibeserver/bench-clip/dab-bench-clean.vbu8";
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { std::printf("  no bench clip (%s) — part 3 NOT RUN\n", path.c_str()); return 3; }
    Clip c; char magic[8] = {0};
    bool ok = std::fread(magic, 1, 8, fp) == 8 && std::memcmp(magic, "VIBEBU81", 8) == 0
           && std::fread(&c.rate, 8, 1, fp) == 1 && std::fread(&c.centre, 8, 1, fp) == 1 && std::fread(&c.sid, 4, 1, fp) == 1;
    std::vector<uint8_t> u; { uint8_t b[65536]; size_t n; while (ok && (n = std::fread(b, 1, sizeof b, fp)) > 0) u.insert(u.end(), b, b + n); }
    std::fclose(fp);
    if (!ok) { std::printf("  unreadable clip\n"); return 1; }
    c.iq.resize(u.size());
    for (size_t i = 0; i < u.size(); ++i) c.iq[i] = (float(u[i]) - 127.5f) / 127.5f;
    int blk = -1;
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i) if (std::fabs(double(vibedab::kBandIII[i].centreHz) - c.centre) < 1000.0) blk = i;

    using FP = std::vector<float>;
    auto run = [&](uint32_t primary, bool withStations, std::string& primaryJson, std::map<uint32_t, FP>& heard, std::map<uint32_t, std::string>& sj) {
        vibedab::DabService svc;
        svc.setRfRate(c.rate); svc.setRfCentre(c.centre); svc.setChannel(blk);
        svc.setService(primary);
        const uint32_t want[] = { 0xC002, 0xC006, 0xC0FF };
        std::vector<std::shared_ptr<vibedab::DabService>> st;
        if (withStations) for (uint32_t s : want) st.push_back(svc.openStation(s));
        std::map<vibedab::DabService*, std::vector<float>> pcm;
        std::vector<float> buf(48000 * 2);
        const size_t step = size_t(c.rate / 20) * 2;
        size_t pos = 0;
        for (double fed = 0; fed < 16.0; fed += double(step / 2) / c.rate) {
            const size_t n = std::min(step, c.iq.size() - pos);
            svc.feed(c.iq.data() + pos, n / 2);
            pos = (pos + n) % c.iq.size();
            for (int w = 0; w < 4000; ++w) {
                bool busy = svc.backlogForTest() > 0;
                for (auto& s : st) if (s->stationBacklogForTest() > 0) busy = true;
                if (!busy) break;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            auto take = [&](vibedab::DabService& s) {
                const size_t got = s.takePcm(buf.data(), buf.size() / 2);
                if (fed > 4.0) pcm[&s].insert(pcm[&s].end(), buf.begin(), buf.begin() + long(got * 2));
            };
            take(svc);
            for (auto& s : st) take(*s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        primaryJson = svc.json();
        heard[primary] = fold(pcm[&svc]);
        for (size_t k = 0; k < st.size(); ++k) { heard[want[k]] = fold(pcm[st[k].get()]); sj[want[k]] = st[k]->json(); }
        if (withStations) {
            CHECK(svc.stationCount() == 3, "three stations open");
            for (auto& s : st) CHECK(s->stationDropped() == 0, "station %04X dropped no frame under back-pressure", unsigned(s->service()));
            const uint32_t g0 = svc.stationGeneration();
            const uint32_t f0 = st[0]->stationFrames();
            svc.closeStation(st[0]);
            svc.feed(c.iq.data(), size_t(c.rate / 2));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(svc.stationCount() == 2 && st[0]->stationFrames() == f0, "a closed station gets no more frames");
            svc.setChannel(blk == 0 ? 1 : 0);
            CHECK(svc.stationCount() == 0 && svc.stationGeneration() == g0 + 1, "a block change closes every station (generation %u)", svc.stationGeneration());
        }
        st.clear();   // ★ the last references: each joins its own thread
        svc.stopWorker();
    };
    std::string pj0, pj1, pjr; std::map<uint32_t, FP> ref, h1; std::map<uint32_t, std::string> sj0, sj1;
    run(0xC001, false, pj0, ref, sj0);
    for (uint32_t s : { 0xC002u, 0xC006u, 0xC0FFu }) run(s, false, pjr, ref, sj0);   // ★ the references: each service ALONE
    run(0xC001, true, pj1, h1, sj1);
    // ★ Named by the same samples (r > 0.95) or, for DAB+ through FAAD2, by the spectrum (r > 0.8) — the best reference either way.
    auto named = [&](const FP& f, double& best) {
        uint32_t who = 0, whoS = 0; double bestS = -2; best = -2;
        for (const auto& kv : ref) {
            const double r = sameAudio(f, kv.second); if (r > best) { best = r; who = kv.first; }
            const double q = corr(fingerprint(f), fingerprint(kv.second)); if (q > bestS) { bestS = q; whoS = kv.first; }
        }
        if (best > 0.95) return who;
        best = bestS;
        return bestS > 0.8 ? whoS : 0u;
    };
    std::printf("  primary alone:  sfOk %ld/%ld rsLost %ld auBad %ld\n", field(pj0, "sfOk"), field(pj0, "sfTried"), field(pj0, "rsLost"), field(pj0, "auBad"));
    std::printf("  with stations:  sfOk %ld/%ld rsLost %ld auBad %ld\n", field(pj1, "sfOk"), field(pj1, "sfTried"), field(pj1, "rsLost"), field(pj1, "auBad"));
    CHECK(field(pj0, "sfOk") > 50, "the primary decodes");
    CHECK(field(pj0, "sfOk") == field(pj1, "sfOk") && field(pj0, "sfTried") == field(pj1, "sfTried") && field(pj0, "rsLost") == field(pj1, "rsLost")
          && field(pj0, "auBad") == field(pj1, "auBad"), "★ the radio's own decode is IDENTICAL with three stations attached");
    double r = 0;
    CHECK(named(h1[0xC001], r) == 0xC001, "the primary's audio is its own service (C001, r %.3f)", r);
    for (uint32_t s : { 0xC002u, 0xC006u, 0xC0FFu }) {
        const std::string& j = sj1[s];
        const uint32_t who = named(h1[s], r);
        std::printf("  station %04X: sounds like %04X (r %.3f; vs the primary %.3f), sid %ld, sfOk %ld/%ld, mp2 %ld/%ld\n", s, who, r,
                    sameAudio(h1[s], ref[0xC001]), field(j, "sid"), field(j, "sfOk"), field(j, "sfTried"), field(j, "mp2Out"), field(j, "mp2In"));
        CHECK(who == s, "station %04X's audio is ITS service, not the primary's", s);
        CHECK(field(j, "sid") == long(s) && j.find("\"label\":\"VibeSDR Bench\"") != std::string::npos && j.find("\"locked\":true") != std::string::npos,
              "station %04X's report: its own sid, the multiplex's label and lock", s);
    }
    return 0;
}

int main() {
    rules();
    const int rc = stationMode();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    if (fails) return 1;
    return rc == 3 ? 3 : 0;
}
