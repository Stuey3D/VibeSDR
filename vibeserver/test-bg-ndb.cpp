// test-bg-ndb.cpp — BACKGROUND DECODING: THE NDB SKIMMER VFO (V12, 2026-10-27).
//
// Stuart (2026-10-27): the NDB skimmer is a per-VFO decoder in a background profile, running BESIDE the other VFO
// decoders on the same capture — his Lenovo RSP1A at 3 MS/s, RF centre 1200 kHz, NAVTEX 489.5 + 517.5, the 160 m digital
// modes, and the NDB band whole. It was a whole-radio profile kind; he looked in the VFO decoder list and it wasn't there.
//
//   · the config: "NDB skimmer (summary)" is listed; what it covers at a centre and a rate (ndbSegment); the save's
//     refusals in words (a capture that misses the band, a radio that reaches no LF, two in one profile); Stuart's
//     Lenovo profile valid and round-tripped; an older whole-radio "ndb":true profile converted on load;
//   · end to end without a server: synthetic LF IQ (WOD on 352.0 kHz, CAM on 333.0 kHz, NAVTEX's 518 kHz neighbour
//     empty) at 1.024 MS/s centred 700 kHz — the band off-centre, as on the Lenovo — through BgRunner's channeliser and
//     the NDB slice → the listener's NDB service (fed-slice mode) → the gallery's NDB cards; a NAVTEX VFO beside it
//     running at the same time; both decoder slots taken and given back;
//   · one slot only: NAVTEX (VFO 1) runs, the NDB skimmer waits for a slot — and a listener's own NDB mode pauses it, with
//     the reason, its slot back;
//   · the radio moved off the band (a listener's dial at 7 MHz): the skimmer leaves the window, its slot back.
// ★ Silent: nothing is played.
#include "vibe_bgdecode_ndb.h"
#include "ndb_synth.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/bgndb-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static size_t count(const std::string& s, const std::string& what) {
    size_t n = 0; for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n; return n;
}

static void config() {
    std::printf("── the config: \"NDB skimmer (summary)\" ──\n");
    bg::installBandSpan();
    const bg::DecoderDef* d = bg::decoderDef("ndb-skim");
    ok(d && std::string(d->label) == "NDB skimmer (summary)", "the decoder is listed");
    ok(bg::decoderFitsDemod("ndb-skim", "usb") && !bg::decoderFitsDemod("ndb-skim", "lsb"), "on a USB row only (none runs — it is fed IQ)");
    double lo = 0, hi = 0;
    ok(bg::ndbSegment(1200000, 3000000, lo, hi) && lo == 190000 && hi == 535000, "1200 kHz at 3 MS/s covers the whole band, 190–535 kHz");
    ok(bg::ndbSegment(700000, 1024000, lo, hi) && lo > 240000 && lo < 242000 && hi == 535000,
       "700 kHz at 1.024 MS/s covers " + std::to_string((long long)lo) + "–" + std::to_string((long long)hi) + " Hz (the window's guard)");
    ok(!bg::ndbSegment(7000000, 2400000, lo, hi), "7 MHz at 2.4 MS/s: none of it");
    ok(!bg::ndbSegment(600000, 250000, lo, hi) && hi - lo < 100000, "600 kHz at 250 kS/s: under 100 kHz of it — not worth a slot");
    ok(bg::ndbSegment(362750, 3000000, lo, hi, 150000, 300000) && lo == 190000 && hi == 300000, "clipped to what the radio reaches");

    // ★★ Stuart's Lenovo profile: NAVTEX ×2, the 160 m Band spots, the NDB skimmer — at 1200 kHz, 3 MS/s
    const char* lenovo = R"({"on":true,"waitMin":5,"profiles":[{"centre":1200000,"from":"00:00","to":"00:00","vfos":[)"
                         R"({"hz":489500,"mode":"usb","dec":"navtex"},{"hz":517500,"mode":"usb","dec":"navtex"},)"
                         R"({"hz":1840000,"mode":"usb","dec":"spots-band"},{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]})";
    bg::Config c; std::string err;
    ok(bg::parse(lenovo, c, err) && c.profiles.size() == 1 && c.profiles[0].vfos.size() == 4 && bg::isNdbSkim(c.profiles[0].vfos[3]),
       "the Lenovo profile parses (" + err + ")");
    ok(bg::validate(c, 3000000, err, 0, 0, true, true), "…and is valid at 3 MS/s on a radio that reaches LF (" + err + ")");
    bg::Config c2; std::string e2;
    const std::string j1 = bg::toJson(c);
    ok(bg::parse(j1, c2, e2) && bg::toJson(c2) == j1 && j1.find("\"dec\":\"ndb-skim\"") != std::string::npos && j1.find("\"ndb\":true") == std::string::npos,
       "…and round-trips exactly (NAVTEX + Band + NDB): " + j1);
    std::string e3;
    ok(!bg::validate(c, 3000000, e3, 0, 0, true, false) && e3.find("VFO 4") != std::string::npos && e3.find("NDB band") != std::string::npos
       && e3.find("direct sampling") != std::string::npos, "refused, in words, on a radio that reaches no LF: " + e3);
    bg::Config far; std::string e4;
    bg::parse(R"({"on":true,"profiles":[{"centre":7000000,"vfos":[{"hz":7038600,"mode":"usb","dec":"ft8"},{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]})", far, e4);
    ok(!bg::validate(far, 2400000, e4, 0, 0, true, true) && e4.find("VFO 2") != std::string::npos && e4.find("100 kHz of the NDB band") != std::string::npos
       && e4.find("none of the band") != std::string::npos, "a capture that misses the band is refused, in words: " + e4);
    bg::Config thin; std::string e5;
    bg::parse(R"({"on":true,"profiles":[{"centre":600000,"vfos":[{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]})", thin, e5);
    ok(!bg::validate(thin, 250000, e5, 0, 0, true, true) && e5.find("only ") != std::string::npos, "…and one that holds too little of it: " + e5);
    bg::Config two; std::string e6;
    bg::parse(R"({"on":true,"profiles":[{"centre":362750,"vfos":[{"hz":0,"mode":"usb","dec":"ndb-skim"},{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]})", two, e6);
    ok(!bg::validate(two, 768000, e6, 0, 0, true, true) && e6.find("already has an NDB skimmer") != std::string::npos, "two in one profile: refused — " + e6);
    bg::Config lk; std::string e7;
    bg::parse(R"({"on":true,"profiles":[{"centre":0,"vfos":[{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]})", lk, e7);
    ok(bg::validate(lk, 1000000, e7, 0, 400000, true, true), "a radio LOCKED over the band may run it — nothing moves (" + e7 + ")");
    std::string e8;
    ok(!bg::validate(lk, 1000000, e8, 0, 7100000, true, true) && e8.find("lock must cover the band") != std::string::npos, "…one locked elsewhere may not: " + e8);
    // ★ the old whole-radio kind, converted on load
    bg::Config old; std::string e9;
    ok(bg::parse(R"({"on":true,"profiles":[{"centre":0,"from":"18:00","to":"06:00","vfos":[],"ndb":true}]})", old, e9)
       && old.profiles[0].vfos.size() == 1 && bg::isNdbSkim(old.profiles[0].vfos[0]) && old.profiles[0].centreHz == bg::kNdbLegacyCentreHz
       && old.profiles[0].fromMin == 18 * 60 && bg::validate(old, 768000, e9, 0, 0, true, true),
       "an older \"ndb\":true profile loads as a VFO profile with the NDB skimmer, its hours kept (" + e9 + ")");
}

/** A synthetic capture of `secs` at `fs` centred `centre`: NDBs at their RF, noise. Generated a chunk at a time (a minute
 *  at 1 MS/s is half a gigabyte whole); the keying is computed at 2 kHz and held. */
struct Lf {
    struct B { ndbsynth::Beacon b; double rf = 0, A = 0, ph = 0, tph = 0; std::vector<float> key; };
    std::vector<B> bs; double fs = 0, centre = 0; size_t at = 0; ndbsynth::Rng rng{7};
    Lf(double fs_, double centre_, double secs) : fs(fs_), centre(centre_) {
        auto add = [&](const char* id, double rf, double snr, double period) {
            B x; x.b.ident = id; x.b.snrDb = snr; x.b.toneHz = 1020; x.b.periodSec = period; x.rf = rf;
            x.A = std::sqrt(std::pow(10.0, snr / 10.0) * 2500.0 / fs);
            x.key = ndbsynth::keying(x.b, 2000, secs);
            bs.push_back(std::move(x));
        };
        add("WOD", 352000, 15, 10);
        add("CAM", 333000, 12, 9);
    }
    void next(std::vector<cf32>& out, size_t n) {
        out.assign(n, cf32(0, 0));
        const double sg = std::sqrt(0.5);
        for (size_t i = 0; i < n; ++i) out[i] = cf32((float)(sg * rng.gauss()), (float)(sg * rng.gauss()));
        for (auto& x : bs) {
            const double dph = 2 * M_PI * (x.rf - centre) / fs, dt = 2 * M_PI * x.b.toneHz / fs;
            for (size_t i = 0; i < n; ++i) {
                const size_t ki = (size_t)((double)(at + i) * 2000.0 / fs);
                const float k = ki < x.key.size() ? x.key[ki] : 0.0f;
                const double a = x.A * (1.0 + x.b.depth * k * std::cos(x.tph));
                out[i] += cf32((float)(a * std::cos(x.ph)), (float)(a * std::sin(x.ph)));
                x.ph += dph; if (x.ph > M_PI) x.ph -= 2 * M_PI; else if (x.ph < -M_PI) x.ph += 2 * M_PI;
                x.tph += dt; if (x.tph > 2 * M_PI) x.tph -= 2 * M_PI;
            }
        }
        at += n;
    }
};

static void endToEnd() {
    std::printf("\n── end to end: NDB beside NAVTEX on one capture ──\n");
    const std::string dir = tmpDir("e2e");
    gallery::Store s; s.setDir(dir); s.setLimitBytes(100ull << 20);
    FileSlots slots("", [] { return 4; });
    const double fs = 1024000, centre = 700000, secs = 90;
    std::atomic<int64_t> clk{1793000000};
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 1; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 32768; p.slots = &slots; p.store = &s;
    p.lat = 52.29; p.lon = -0.85;
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{517500, "usb", "navtex"}, bg::Vfo{0, "usb", "ndb-skim"} };
    std::vector<std::string> logs;
    p.log = [&](const std::string& t) { logs.push_back(t); };
    auto make = bg::makeNdbFeedFactory(&s, [&clk] { return clk.load(); });
    std::shared_ptr<bg::NdbFeed> feed;
    bg::NdbStart seen;
    p.ndbMake = [&](const bg::NdbStart& st) { seen = st; feed = make(st); return feed; };
    r.start(p);
    ok(r.decodersRunning() == 2 && slots.inUse() == 2, "NAVTEX and the NDB skimmer both running, a decoder slot each");
    ok(feed && seen.loHz > 240000 && seen.loHz < 242000 && seen.hiHz == 535000 && seen.profileIdx == 1,
       "the skimmer was given the part of the band the capture holds: " + std::to_string((long long)seen.loHz) + "–" + std::to_string((long long)seen.hiHz) + " Hz");
    auto* svc = feed ? &static_cast<bg::ServiceNdbFeed*>(feed.get())->service() : nullptr;
    Lf gen(fs, centre, secs);
    std::vector<cf32> chunk;
    const size_t step = 51200;   // 50 ms
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o < (size_t)(secs * fs); o += step) {
        gen.next(chunk, step);
        r.feedIq(chunk.data(), (int)chunk.size(), (double)clk.load());
        clk.store(1793000000 + (int64_t)((o + step) / fs));
        // ★ paced by the queues themselves, never a guess: the runner's (0.5 s cap) and the skimmer's (2 s)
        for (int w = 0; w < 4000 && (r.queuedForTest() > (size_t)(fs * 0.25) || (svc && svc->fedQueuedSec() > 1.0)); ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int w = 0; w < 4000 && (r.queuedForTest() > 0 || (svc && svc->fedQueuedSec() > 0)); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  (%.0f s of 1.024 MS/s capture in %.1f s)\n", secs, wall);
    const std::string st = r.statusJson();
    ok(st.find("\"dec\":\"ndb-skim\"") != std::string::npos && st.find("\"ndbHi\":535000") != std::string::npos, "the admin line names the part of the band it covers");
    ok(st.find("beacons identified") != std::string::npos, "…and how it is doing: " + (svc ? svc->brief() : std::string("?")));
    const std::string rep = svc ? svc->report() : std::string();
    ok(rep.find("\"ident\":\"WOD\"") != std::string::npos && rep.find("\"ident\":\"CAM\"") != std::string::npos,
       "WOD (352.0) and CAM (333.0) identified off the runner's slice");
    ok(rep.find("\"name\":\"Woodley\"") != std::string::npos, "…matched to the published list (WOD = Woodley)");
    ok(r.droppedSamples() == 0 && feed->droppedBlocks() == 0, "nothing dropped (runner " + std::to_string((unsigned long long)r.droppedSamples())
       + " samples, skimmer " + std::to_string((unsigned long long)feed->droppedBlocks()) + " slices)");
    r.stop();
    ok(slots.inUse() == 0, "stopped — both slots back");
    const std::string list = s.listJson("ndb", 50);
    ok(count(list, "\"title\":\"NDB last 10 min") == 1, "the gallery's standing NDB last-10-minutes card");
    ok(list.find("WOD") != std::string::npos || list.find("2 beacons") != std::string::npos, "…naming what was heard");
    ok(list.find("\"o\":\"background\"") != std::string::npos && list.find("\"p\":\"Profile 2\"") != std::string::npos, "…as background's, Profile 2");
    ok(count(list, "\"title\":\"NDB ") >= 3, "and the day's log and the hour so far (" + std::to_string(count(list, "\"title\":\"NDB ")) + " NDB items)");
    std::string cmd = "rm -rf '" + dir + "'"; if (std::system(cmd.c_str()) != 0) std::printf("  (could not remove %s)\n", dir.c_str());
}

static void slotsAndMoves() {
    std::printf("\n── one slot, a listener's NDB mode, the radio moved ──\n");
    FileSlots slots("", [] { return 1; });
    std::atomic<bool> listenerNdb{false};
    int made = 0;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = 3000000; p.centreHz = 1200000; p.fftSize = 65536; p.slots = &slots;
    p.profile.centreHz = 1200000;
    p.profile.vfos = { bg::Vfo{517500, "usb", "navtex"}, bg::Vfo{0, "usb", "ndb-skim"} };
    p.log = [](const std::string&) {};
    auto make = bg::makeNdbFeedFactory(nullptr);
    p.ndbMake = [&](const bg::NdbStart& st) { ++made; return make(st); };
    p.ndbWhyNot = [&] { return listenerNdb.load() ? std::string("a listener is running NDB mode") : std::string(); };
    r.start(p);
    std::string st = r.statusJson();
    ok(r.decodersRunning() == 1 && slots.inUse() == 1 && made == 0 && st.find("waiting for a decoder slot") != std::string::npos,
       "one slot: NAVTEX (VFO 1) runs, the NDB skimmer waits for a slot");
    r.stop();
    FileSlots two("", [] { return 2; });
    p.slots = &two;
    r.start(p);
    ok(r.decodersRunning() == 2 && made == 1, "two slots: both run");
    listenerNdb.store(true);
    r.slotTickForTest();
    st = r.statusJson();
    ok(r.decodersRunning() == 1 && two.inUse() == 1 && st.find("a listener is running NDB mode") != std::string::npos,
       "a listener's own NDB mode: the skimmer pauses, says why, its slot back");
    r.slotTickForTest();
    ok(r.decodersRunning() == 1, "…and stays paused while it runs");
    listenerNdb.store(false);
    r.slotTickForTest();
    ok(r.decodersRunning() == 2 && made == 2, "…and resumes when it ends");
    r.retarget(7000000, 0, true);
    st = r.statusJson();
    ok(r.decodersRunning() == 0 && two.inUse() == 0 && count(st, "\"state\":\"out\"") == 2, "the radio moved to 7 MHz: both VFOs out of the window, the slots back");
    r.retarget(1200000, 0, false);
    r.slotTickForTest(); r.slotTickForTest();
    ok(r.decodersRunning() >= 1, "back on 1200 kHz: they come back in (" + std::to_string(r.decodersRunning()) + " running)");
    r.stop();
    // ★ a host without the NDB decoder: shown, never run
    bg::BgRunner r2;
    bg::BgRunner::Params q = p; q.ndbMake = nullptr; q.slots = &two;
    r2.start(q);
    ok(r2.decodersRunning() == 1 && r2.statusJson().find("not available on this server") != std::string::npos, "no NDB decoder on this host: said, never run");
    r2.stop();
}

int main() {
    config();
    endToEnd();
    slotsAndMoves();
    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
