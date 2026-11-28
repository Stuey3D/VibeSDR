// test-bg-budget.cpp — the box's CPU budget (vibe_bg_budget.h): a simulated 4-core box with 4 radio processes over budget.
//
// The model is the Pi 500 of 2026-11-28: an RSP1B all-band skimmer (four Band VFOs, RTTY weather, WEFAX stations, three
// DSC watches), a V4 on 20 m and a V4L on 17 m (one Band VFO each), and a V4L with ISM 868 and the Meteor passes (no
// background profile). Each radio's CPU need falls with every step the budget takes; when the box's need passes its four
// cores the front ends starve first (the RSP's, the biggest, drops IQ) — what the journal showed.
//   ✓ shedding converges: total need under the cores, every front end within its allowance
//   ✓ the owner's order: the LOWEST row sheds first; inside the Band row DEPTH before extra modes before whole bands
//   ✓ a satellite pass: its radio is never a victim, its decoder's drops stop, the others shed for it
//   ✓ calm: steps come back, the TOP of the list first
//   ✓ every process makes the same decision (four Policy instances, one ledger)
//   ✓ the order: default on a fresh box, parse/repair, persisted (the live file), the ledger's files round-trip
#include "vibe_bg_budget.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace vibe::bg::budget;

static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "/tmp/vibe-test-bgbudget-%s-%d", tag, (int)::getpid());
    ::mkdir(t, 0755);
    return t;
}

// ── the simulated radio ───────────────────────────────────────────────────────────────────────────────────────────
struct SimRadio {
    std::string id, name;
    double fixed = 0;          ///< its own DSP, the radio library, ISM: never the budget's
    double front = 0;          ///< its background channeliser's need (cores)
    int bandVfos = 0, textVfos = 0, picVfos = 0, safetyVfos = 0;
    double bandDecoder = 0.12; ///< one Band VFO's decoders at full depth (cores)
    double textCost = 0.04, picCost = 0.06, safetyCost = 0.01;
    bool pass = false;         ///< a satellite pass on it
    std::array<int, kCatN> level{};   ///< budget steps taken per row
    std::vector<int> stepLog;  ///< (cat*100 + kind) per step, for the order checks: kind 0 depth, 1 chips, 2 pause
    int maxLevel(int c) const {
        if (c == kBand) return bandVfos ? 5 + 2 * bandVfos : 0;
        if (c == kText) return textVfos;
        if (c == kPictures) return picVfos;
        return 0;
    }
    int vfos(int c) const { return c == kBand ? bandVfos : c == kText ? textVfos : c == kPictures ? picVfos : 0; }
    double catCost(int c) const {
        if (c == kBand) {
            if (!bandVfos) return 0;
            const int L = level[kBand];
            const int depth = std::min(L, 5), chips = std::min(std::max(0, L - 5), bandVfos), paused = std::max(0, L - 5 - bandVfos);
            const int live = bandVfos - paused;
            const double perDecoder = bandDecoder * (1.0 - 0.12 * depth);
            // ★ a chips step takes ~30 % of a band's decoders (CW, RTTY, wide PSK)
            return live * perDecoder - std::min(chips, live) * perDecoder * 0.30;
        }
        if (c == kText) return (textVfos - level[kText]) * textCost;
        if (c == kPictures) return (picVfos - level[kPictures]) * picCost;
        return 0;
    }
    double frontNeed() const {
        if (!bandVfos && !textVfos && !picVfos) return 0;
        // ★ a paused Band VFO no longer needs its inverse FFTs: ~6 % of the front end each; with NOTHING left decoding the
        //   runner skips its forward FFT altogether (BgRunner::loop)
        const int paused = std::max(0, level[kBand] - 5 - bandVfos);
        if (paused >= bandVfos && level[kText] >= textVfos && level[kPictures] >= picVfos && safetyVfos == 0) return 0;
        return front * (1.0 - 0.06 * paused);
    }
    double need() const {
        double n = fixed + frontNeed() + safetyVfos * safetyCost;
        for (int c = 0; c < kCatN; ++c) n += catCost(c);
        return n;
    }
    void shed(int c) {
        const int L = level[(size_t)c]++;
        stepLog.push_back(c * 100 + (c == kBand ? (L < 5 ? 0 : L < 5 + bandVfos ? 1 : 2) : 2));
    }
    void restore(int c) { if (level[(size_t)c] > 0) --level[(size_t)c]; }
};

struct SimBox {
    int cores = 4;
    std::vector<SimRadio> r;
    double needAll() const { double n = 0; for (auto& x : r) n += x.need(); return n; }
    /** The ledger as the radios would write it this tick: over 4 cores, the shortfall falls on the front ends (nice 16,
     *  but still under every audio/DSP thread and the ISM decoders at 10-12) — the biggest drops most. */
    std::vector<Entry> ledger(double& busy) const {
        const double n = needAll();
        busy = std::min(100.0, 100.0 * n / cores);
        const double over = std::max(0.0, n - cores);
        double fronts = 0; for (auto& x : r) fronts += x.frontNeed();
        std::vector<Entry> v;
        for (auto& x : r) {
            Entry e; e.id = x.id; e.name = x.name;
            e.cls = x.pass ? kPass : kIdle;
            if (x.pass) e.passName = "Meteor-M2-4 LRPT";
            // ★ a pass's decoder (nice 10) drops only when the box is saturated
            e.urgentDrop = x.pass && n >= cores * 0.99;
            const double share = fronts > 0 ? x.frontNeed() / fronts : 0;
            const double lost = x.frontNeed() > 0 ? std::min(1.0, over * share / x.frontNeed()) : 0;
            e.frontDropPerMin = 60.0 * lost;
            double bg = x.frontNeed();
            for (int c = 0; c < kCatN; ++c) {
                CatState& s = e.cat[(size_t)c];
                s.vfos = x.vfos(c); s.level = x.level[(size_t)c]; s.maxLevel = x.maxLevel(c); s.costPct = 100.0 * x.catCost(c);
                s.nextKind = c == kBand ? (s.level < 5 ? 0 : s.level < 5 + x.bandVfos ? 1 : 2) : 2;
                bg += x.catCost(c);
            }
            e.bgCores = bg;
            v.push_back(e);
        }
        return v;
    }
};

static SimBox pi500() {
    SimBox b;
    SimRadio rsp; rsp.id = "a-rsp"; rsp.name = "RSP1B"; rsp.fixed = 1.25; rsp.front = 0.95;
    rsp.bandVfos = 4; rsp.textVfos = 1; rsp.picVfos = 1; rsp.safetyVfos = 3;
    SimRadio v4; v4.id = "b-v4"; v4.name = "RTL-SDR V4"; v4.fixed = 0.12; v4.front = 0.28; v4.bandVfos = 1; v4.bandDecoder = 0.30;
    SimRadio v4l; v4l.id = "c-v4l"; v4l.name = "V4L 17 m"; v4l.fixed = 0.12; v4l.front = 0.28; v4l.bandVfos = 1; v4l.bandDecoder = 0.22;
    SimRadio ism; ism.id = "d-ism"; ism.name = "V4L ISM"; ism.fixed = 0.75;
    b.r = { rsp, v4, v4l, ism };
    return b;
}

/** Run `secs` of 2 s health ticks: every radio's Policy decides from the same ledger; only the named radio acts. */
struct Run {
    SimBox box;
    Order order;
    std::vector<Policy> pol;
    int64_t now = 1800000000000LL, lastStep = -1;
    int sheds = 0, restores = 0, disagreements = 0;
    bool bandPausedEarly = false, modesEarly = false;   // ★ a band paused while another band anywhere still had depth or modes
    std::vector<std::pair<std::string, int>> steps;   // (radio, cat) in order, sheds only
    explicit Run(SimBox b, Order o = defaultOrder()) : box(std::move(b)), order(o), pol(box.r.size()) {}
    void ticks(int secs) {
        for (int t = 0; t < secs; t += 2) {
            now += 2000;
            double busy = 0;
            const auto led = box.ledger(busy);
            Box bx; bx.nowMs = now; bx.cores = box.cores; bx.busyPct = busy; bx.lastStepMs = lastStep;
            std::vector<Decision> ds;
            for (size_t i = 0; i < box.r.size(); ++i) ds.push_back(pol[i].tick(bx, led, order));
            for (size_t i = 1; i < ds.size(); ++i) if (ds[i].act != ds[0].act || ds[i].id != ds[0].id || ds[i].cat != ds[0].cat) ++disagreements;
            const Decision& d = ds[0];
            if (d.act == Act::None) continue;
            for (auto& x : box.r) {
                if (x.id != d.id) continue;
                if (d.act == Act::Shed) {
                    if (d.cat == kBand) {
                        const int L = x.level[kBand];
                        const bool pausing = L >= 5 + x.bandVfos, chips = L >= 5 && !pausing;
                        for (auto& y : box.r) if (y.bandVfos) {
                            if (pausing && y.level[kBand] < 5 + y.bandVfos) bandPausedEarly = true;
                            if (chips && y.level[kBand] < 5) modesEarly = true;
                        }
                    }
                    x.shed(d.cat); ++sheds; steps.push_back({ x.id, d.cat });
                }
                else { x.restore(d.cat); ++restores; }
                lastStep = now;
            }
        }
    }
    double drops() const { double b; double m = 0; for (auto& e : box.ledger(b)) m = std::max(m, e.frontDropPerMin); return m; }
    bool urgent() const { double b; for (auto& e : box.ledger(b)) if (e.urgentDrop) return true; return false; }
    SimRadio& radio(const std::string& id) { for (auto& x : box.r) if (x.id == id) return x; return box.r[0]; }
};

static void overBudget() {
    std::printf("── a 4-core box with 4 radios over budget: shedding converges, the lowest row first, depth before bands ──\n");
    Run run(pi500());
    double busy = 0; (void)run.box.ledger(busy);
    ok(run.box.needAll() > 4.2 && run.drops() > 10, "the start: the box needs " + std::to_string(run.box.needAll()) + " cores of 4, the RSP drops "
       + std::to_string(run.drops()) + " s of IQ a minute (the Pi 500 dropped 30-43)");
    run.ticks(15 * 60);
    ok(run.box.needAll() <= 4.0 && run.drops() <= kFrontDropSecPerMin,
       "★★★ after 15 min: need " + std::to_string(run.box.needAll()) + " cores (≤ 4), worst front end " + std::to_string(run.drops()) + " s/min (≤ 1.5)");
    ok(run.disagreements == 0, "★ every radio process made the same decision every tick (" + std::to_string(run.disagreements) + " disagreements)");
    // the order: pictures (lowest) first, then text, then the Band row
    int firstBand = -1, lastPic = -1, lastText = -1;
    for (size_t i = 0; i < run.steps.size(); ++i) {
        if (run.steps[i].second == kPictures) lastPic = (int)i;
        if (run.steps[i].second == kText) lastText = (int)i;
        if (run.steps[i].second == kBand && firstBand < 0) firstBand = (int)i;
    }
    ok(lastPic >= 0 && lastText > lastPic && (firstBand < 0 || firstBand > lastText),
       "★★ the owner's order: pictures paused first (step " + std::to_string(lastPic + 1) + "), then text (" + std::to_string(lastText + 1)
       + "), the all-band skimmers last (from step " + std::to_string(firstBand + 1) + ")");
    // inside the Band row, on every radio: depth (kind 0) before chips (1) before pauses (2)
    bool depthFirst = true; int bandSteps = 0;
    for (auto& x : run.box.r) {
        int last = 0;
        for (int s : x.stepLog) if (s / 100 == kBand) { ++bandSteps; const int k = s % 100; if (k < last) depthFirst = false; last = k; }
    }
    ok(bandSteps > 0 && depthFirst, "★★★ inside the Band row DEPTH before extra modes before whole bands (" + std::to_string(bandSteps) + " band steps)");
    ok(!run.modesEarly && !run.bandPausedEarly, "★★★ ACROSS the radios too: depth at the minimum on every band before any band lost a mode, "
       "and modes off everywhere before any band paused");
    int bandsPaused = 0;
    for (auto& x : run.box.r) bandsPaused += std::max(0, x.level[kBand] - 5 - x.bandVfos);
    std::printf("  (this box: %d of 6 bands paused to fit — the honest answer for three skimmers + ISM on 4 cores)\n", bandsPaused);
    ok(run.radio("d-ism").level[kBand] == 0 && run.radio("d-ism").stepLog.empty(), "the ISM radio (no background profile) is never touched");
    // state words
    double b2; const auto led = run.box.ledger(b2);
    ok(stateWords(kPictures, led) == "1 of 1 paused" && stateWords(kBand, led).rfind("depth", 0) == 0,
       "the card's words: pictures \"" + stateWords(kPictures, led) + "\", band \"" + stateWords(kBand, led) + "\"");
    ok(stateWords(kSkim, led) == "not running on this box", "a row nothing on the box uses: \"" + stateWords(kSkim, led) + "\"");
}

static void ownerOrder() {
    std::printf("── the owner's order decides: band skimmers at the BOTTOM shed before pictures ──\n");
    Run run(pi500(), parseOrder("pictures,text,skim,band"));
    run.ticks(4 * 60);
    ok(!run.steps.empty() && run.steps.front().second == kBand, "★ band skimmers at the bottom of the list: the first step is theirs");
    ok(run.radio("a-rsp").level[kPictures] == 0, "...and the pictures at the top keep running (" + std::to_string(run.radio("a-rsp").level[kPictures]) + " steps)");
}

static void satPass() {
    std::printf("── a satellite pass: its radio is never shed, its decoder's drops stop, the others give way ──\n");
    SimBox b = pi500();
    for (auto& x : b.r) if (x.id == "d-ism") { x.pass = true; x.fixed = 1.10; }   // ★ LRPT (~0.35 core) on top of ISM's own share
    Run run(b);
    ok(run.urgent(), "the pass starts on a full box: its decoder drops input");
    run.ticks(60);
    int t = 60;
    while (run.urgent() && t < 15 * 60) { run.ticks(2); t += 2; }
    ok(!run.urgent() && t <= 240, "★★★ the pass's decoder whole again after " + std::to_string(t) + " s (a 15 min pass; an urgent step every 4 s)");
    run.ticks(10 * 60);
    bool passHit = false; for (auto& s : run.steps) if (s.first == "d-ism") passHit = true;
    ok(!passHit && !run.urgent(), "★ the pass's radio was never a victim, and no drops for the rest of the pass");
    double busy = 0; (void)run.box.ledger(busy);
    ok(busy < kBusyHighPass + 0.01 || run.drops() <= kFrontDropSecPerMin, "headroom kept for the pass: the box at " + std::to_string((int)busy) + " %");
    // the pass ends: calm, then everything comes back, top first
    for (auto& x : run.box.r) if (x.pass) { x.pass = false; x.fixed = 0.75; }
    { double bz = 0; (void)run.box.ledger(bz); std::printf("  (after the pass the box is %.0f %% busy)\n", bz); }
    const int before = run.restores;
    run.ticks(4 * 60);
    ok(run.restores > before, "after the pass and 3 calm minutes, steps come back (" + std::to_string(run.restores - before) + ")");
}

static void restoreTopFirst() {
    std::printf("── calm: given back the TOP of the list first ──\n");
    SimBox b = pi500();
    for (auto& x : b.r) if (x.id == "a-rsp") { x.level[kBand] = 3; x.level[kPictures] = 1; x.level[kText] = 1; }
    for (auto& x : b.r) { x.fixed *= 0.3; if (x.id != "a-rsp") { x.bandVfos = 0; x.front = 0; } }   // ★ the load has gone (Stuart stopped the 20 m / 17 m skimmers)
    Run run(b);
    run.ticks(3 * 60 + 2);
    ok(run.restores == 1 && run.radio("a-rsp").level[kBand] == 2, "★ the first step back is the Band row's (the top), after 3 calm minutes");
    run.ticks(60 * 10);
    std::printf("  (levels now: band %d, text %d, pictures %d; restores %d)\n", run.radio("a-rsp").level[kBand], run.radio("a-rsp").level[kText],
                run.radio("a-rsp").level[kPictures], run.restores);
    ok(run.radio("a-rsp").level[kBand] == 0 && run.radio("a-rsp").level[kPictures] == 0 && run.radio("a-rsp").level[kText] == 0,
       "...and in time everything, the pictures (the bottom) last");
}

static void orderFiles() {
    std::printf("── the order: the default on a fresh box, repaired when partial, persisted in the live file ──\n");
    ok(defaultOrder().str() == "band,skim,text,pictures", "a fresh box: " + defaultOrder().str());
    ok(parseOrder("").str() == "band,skim,text,pictures", "nothing saved = the default");
    ok(parseOrder("pictures, band").str() == "pictures,band,skim,text", "a partial list: the missing rows appended in the default order");
    ok(parseOrder("text,bogus,text,band").str() == "text,band,skim,pictures", "unknown ids ignored, repeats once");
    ok(parseOrder("pictures").rankOf(kPictures) == 0 && parseOrder("pictures").rankOf(kBand) == 1, "rankOf follows the list");
    const std::string dir = tmpDir("order");
    ok(Board::readOrder(dir).empty(), "no live file yet");
    ok(Board::writeOrder(dir, parseOrder("text,pictures,band,skim")) && parseOrder(Board::readOrder(dir)).str() == "text,pictures,band,skim",
       "★ saved and read back: " + Board::readOrder(dir));
    // the ledger's files
    Board a, b; a.open(dir, "1001"); b.open(dir, "1002");
    Entry e; e.name = "RSP1B"; e.cls = kPass; e.passName = "Meteor-M2-4 LRPT"; e.frontDropPerMin = 33.6; e.bgCores = 1.3;
    e.cat[kBand] = { 4, 3, 13, 48.5 };
    a.publish(e);
    Entry f; f.name = "V4\ttab"; b.publish(f);
    auto all = Board::all(dir);
    ok(all.size() == 2 && all[0].id == "1001" && all[0].cls == kPass && all[0].passName == "Meteor-M2-4 LRPT" && std::fabs(all[0].frontDropPerMin - 33.6) < 0.01
       && all[0].cat[kBand].level == 3 && all[0].cat[kBand].maxLevel == 13 && all[1].name == "V4 tab", "★ the ledger round-trips (and a tab in a name cannot break it)");
    b.close();
    ok(Board::all(dir).size() == 1, "a radio that stops is gone from the ledger");
    Board::noteStep(dir, 123456789);
    ok(Board::lastStep(dir) == 123456789, "the box's last-step stamp");
    Box pw; pw.curGHz = 1.5; pw.maxGHz = 2.4; pw.clockFrac = 1.5 / 2.4;
    ok(powerWords(pw) == "this machine is power-limited or hot: running at 1.5 GHz of 2.4", "power-limited, in words: " + powerWords(pw));
    Box full; full.curGHz = 2.2; full.maxGHz = 2.2;
    ok(powerWords(full).empty(), "at full speed: nothing said");
    // ★★ live 11-28: drops with the CPU to spare do not shed anything
    {
        Policy pol; Entry e; e.id = "r"; e.name = "RSP1B"; e.frontDropPerMin = 9.3;
        e.cat[kPictures] = { 1, 0, 1, 6.0, 2 }; e.cat[kText] = { 1, 0, 1, 4.0, 2 };
        Box bx; bx.nowMs = 1000000; bx.cores = 4; bx.busyPct = 35.0;
        const Decision d = pol.tick(bx, { e }, defaultOrder());
        ok(d.act == Act::None, "★★ 9.3 s/min of front-end drops with the box 35 % busy: nothing shed (shedding cannot cure it)");
        bx.busyPct = 92.0; bx.nowMs += 2000;
        ok(pol.tick(bx, { e }, defaultOrder()).act == Act::Shed, "...the same drops with the box 92 % busy: shed");
        Entry p = e; p.id = "s"; p.cls = kPass; p.passName = "Meteor-M2-4 LRPT"; p.urgentDrop = true; p.frontDropPerMin = 0;
        Policy pol2; Box lo = bx; lo.busyPct = 30.0;
        ok(pol2.tick(lo, { e, p }, defaultOrder()).act == Act::Shed, "a pass's decoder dropping is urgent at any load");
    }
}

int main() {
    overBudget();
    ownerOrder();
    satPass();
    restoreTopFirst();
    orderFiles();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
