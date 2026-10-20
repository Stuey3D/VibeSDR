// Host test for SpectrumEngine — behaviour that must match WaterfallView.tsx (docs/NATIVE-SPECTRUM-ENGINE.md).
//   clang++ -std=c++17 -O2 -I../vibedsp test_spectrum_engine.cpp spectrum_engine.cpp ../vibedsp/spectrum_processor.cpp
// Images and paths are faked (a heap copy of the ring / the points); the engine runs on a simulated clock.
#include "spectrum_engine.h"
#include "wf_row_target.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vibespec;

static double gClock = 1'700'000'000'000.0;
static double clockFn() { return gClock; }
static int gImagesMade = 0, gImagesFreed = 0, gPathsMade = 0, gPathsFreed = 0;
struct FakeImage { std::vector<uint8_t> px; };
static void* mkImage(const uint8_t* px, int w, int h, size_t rb) {
    gImagesMade++; auto* f = new FakeImage; f->px.assign(px, px + (size_t)h * rb); (void)w; return f;
}
static void* mkPath(const float* xy, int n, bool) { gPathsMade++; return new std::vector<float>(xy, xy + 2 * n); }
static void rlImage(void* p) { gImagesFreed++; delete (FakeImage*)p; }
static void rlPath(void* p) { gPathsFreed++; delete (std::vector<float>*)p; }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// Advance simulated time in 1 ms steps, running the engine at every step (timers fire on time).
static void run(SpectrumEngine* e, double ms) { for (double t = 0; t < ms; t += 1) { gClock += 1; e->testStep(gClock); } }

static std::vector<float> frame(int n, int carrierBin, float lvl = -40) {
    std::vector<float> b(n);
    for (int i = 0; i < n; i++) b[i] = -110 + (float)((i * 7919) % 13) * 0.3f;
    if (carrierBin >= 0) for (int k = -3; k <= 3; k++) if (carrierBin + k >= 0 && carrierBin + k < n) b[carrierBin + k] = lvl;
    return b;
}

// Latest image's ring row for head h (row index (h-1) % 1024).
static const uint8_t* rowOf(const FakeImage* img, double head) { return img->px.data() + ((size_t)(head - 1) % 1024) * 1024; }

int main() {
    // ── 1. Steady 20 fps feed, sharp scroll (no extras) ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 0;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        int polls = 0, imgs = 0; double lastHead = 0, headAt20 = 0; size_t specLen = 0; void* lastImg = nullptr;
        for (int f = 0; f < 40; f++) {
            auto b = frame(1024, 512);
            e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
            run(e, 50);
            UiUpdate u;
            if (e->pollUi(u)) {
                polls++;
                if (u.image) { imgs++; lastHead = u.head; if (lastImg) rlImage(lastImg); lastImg = u.image; }
                if (!u.specTarget.empty()) specLen = u.specTarget.size();
                if (u.peakPath) rlPath(u.peakPath);
            }
            if (f == 19) headAt20 = lastHead;
        }
        // ★ The TS jitter buffer starts at a 150 ms arrival estimate: at 20 fps it drains slowly at first, the
        //   queue tops out at 3 and the OLDEST frame is dropped until the estimate converges — faithfully copied.
        CHECK(lastHead >= 36 && lastHead <= 40, "head %g (a couple of early drops allowed)", lastHead);
        CHECK(lastHead - headAt20 == 20, "settled: every frame becomes a row (%g of 20)", lastHead - headAt20);
        CHECK(polls >= 30 && imgs >= 30, "polls %d images %d", polls, imgs);
        CHECK(specLen == 200, "spectrum target = ceil(width/2) points, got %zu", specLen);
        // The carrier lands at bin 512 of the newest row (no shift: true centre = view centre).
        const uint8_t* r = rowOf((FakeImage*)lastImg, lastHead);
        CHECK(r[512] > 200 && r[100] < 60, "carrier row: r[512]=%d r[100]=%d", r[512], r[100]);
        rlImage(lastImg);
        delete e;
    }
    // ── 2. Repeat rows: Smooth (cap 60) at a 10 fps feed → 60 rows/s: 5 extras per frame at ~16.7 ms ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 60;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        double head = 0, headAt30 = 0; std::vector<double> headTimes;
        for (int f = 0; f < 40; f++) {
            if (f == 30) headAt30 = head;
            auto b = frame(1024, 300);
            e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
            for (int t = 0; t < 100; t++) {
                gClock += 1; e->testStep(gClock);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) { if (u.head != head && f >= 35) headTimes.push_back(gClock); head = u.head; rlImage(u.image); }
                    if (u.peakPath) rlPath(u.peakPath);
                }
            }
        }
        // Settled (avgWfGap ≈ 100 ms → dur 100, target 60 → carry 6/frame → 5 extras): 6 rows per frame, frames 30-39.
        CHECK(head - headAt30 >= 57 && head - headAt30 <= 63, "rows with extras: %g in 1 s once settled (expect ~60)", head - headAt30);
        if (headTimes.size() >= 8) {
            const double gap = (headTimes.back() - headTimes.front()) / (double)(headTimes.size() - 1);
            CHECK(gap >= 13 && gap <= 21, "mean row spacing %g ms (dur/(extras+1) ≈ 16.7, dur = the measured gap)", gap);
        }
        delete e;
    }
    // ── 2b. SMOOTH from a 5 fps feed: 30 rows/s (6 x 5, Stuart 10-20) — not 60 (x12) ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 60;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        double head = 0, headAt10 = 0;
        for (int f = 0; f < 20; f++) {
            if (f == 10) headAt10 = head;
            auto b = frame(1024, 300);
            e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
            for (int t = 0; t < 200; t++) {
                gClock += 1; e->testStep(gClock);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) { head = u.head; rlImage(u.image); }
                    if (u.peakPath) rlPath(u.peakPath);
                }
            }
        }
        // Frames 10-19, settled (avgWfGap ≈ 200 ms → dur 200, target 30 → carry 6/frame → 5 extras): ~60 rows = 30/s.
        const double rows = head - headAt10;
        CHECK(rows >= 57 && rows <= 63, "smooth at 5 fps: %g rows in 2 s (expect ~60 = 30 rows/s)", rows);
        delete e;
    }
    // ── 2c. DEFAULT 20 rows/s from a 10 fps feed: 2 rows a frame, every frame ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 20;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        double head = 0, headAt10 = 0;
        for (int f = 0; f < 20; f++) {
            if (f == 10) headAt10 = head;
            auto b = frame(1024, 300);
            e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
            for (int t = 0; t < 100; t++) {
                gClock += 1; e->testStep(gClock);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) { head = u.head; rlImage(u.image); }
                    if (u.peakPath) rlPath(u.peakPath);
                }
            }
        }
        const double rows = head - headAt10;
        CHECK(rows >= 18 && rows <= 22, "default at 10 fps: %g rows in 1 s (expect 20, ±2 for where the window cuts a frame's extras)", rows);
        delete e;
    }
    // ── 2d. wf_row_target.h: the same table as scripts/test_wf_row_target.ts, from a cold 150 ms estimate ──
    {
        auto drive = [](int cap, double fps, vibewf::RowTargetState& st, double& ema, int frames) {
            double t = 0; for (int i = 0; i < frames; i++) { ema = ema * 0.8 + (1000.0 / fps) * 0.2; t = vibewf::stepRowTarget(st, cap, 1000.0 / ema); }
            return t; };
        auto settle = [&](int cap, double fps) { vibewf::RowTargetState st; double ema = 150; const double t = drive(cap, fps, st, ema, 200); return t == 0 ? fps : t; };
        struct Row { double fps, def, smo; } tbl[] = { {5, 15, 30}, {10, 20, 60}, {20, 20, 60}, {10.0 / 3, 10, 20}, {9, 20, 30} };
        for (const Row& r : tbl)
            CHECK(settle(20, r.fps) == r.def && settle(60, r.fps) == r.smo, "row target at %.2f fps: %g / %g (want %g / %g)",
                  r.fps, settle(20, r.fps), settle(60, r.fps), r.def, r.smo);
        CHECK(vibewf::rowTargetFor(20, 20) == 0, "20 fps Default is actual rows");
        // A governor step 10 → 5: Smooth changes once, 60 → 30.
        vibewf::RowTargetState st; double ema = 150; double t = drive(60, 10, st, ema, 200); int changes = 0;
        for (int i = 0; i < 400; i++) { ema = ema * 0.8 + 200 * 0.2; const double n = vibewf::stepRowTarget(st, 60, 1000.0 / ema); if (n != t) changes++; t = n; }
        CHECK(changes == 1 && t == 30, "governor 10 -> 5: Smooth changes %d time(s), ends %g (want once, 30)", changes, t);
    }
    // ── 3. In-flight alignment: the row's true centre is 2 bins above the view → shifted right by 2 ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78;
        e->setConfig(c);
        const double bw = 2.048e6, hzBin = bw / 1024;
        e->setView(100e6, bw);
        void* img = nullptr; double head = 0;
        for (int f = 0; f < 10; f++) {
            auto b = frame(1024, 500);
            e->pushFrame(b.data(), 1024, 100e6, bw, 100e6 + 2 * hzBin);
            run(e, 50);
            UiUpdate u;
            if (e->pollUi(u)) { if (u.image) { if (img) rlImage(img); img = u.image; head = u.head; } if (u.peakPath) rlPath(u.peakPath); }
        }
        const uint8_t* r = rowOf((FakeImage*)img, head);
        CHECK(r[502] > 200 && r[0] == 0 && r[1] == 0, "shift +2: r[502]=%d r[0..1]=%d,%d", r[502], r[0], r[1]);
        rlImage(img); delete e;
    }
    // ── 4. 2048 bins → max-pooled to 1024: a one-bin carrier at 1001 lands at 500 ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        void* img = nullptr; double head = 0;
        for (int f = 0; f < 10; f++) {
            auto b = frame(2048, -1);
            for (int k = 1000; k <= 1003; k++) b[k] = -40;
            e->pushFrame(b.data(), 2048, 100e6, 2.048e6, 100e6);
            run(e, 50);
            UiUpdate u;
            if (e->pollUi(u)) { if (u.image) { if (img) rlImage(img); img = u.image; head = u.head; } if (u.peakPath) rlPath(u.peakPath); }
        }
        const uint8_t* r = rowOf((FakeImage*)img, head);
        CHECK(r[500] > 150 && r[501] > 150, "max-pool: r[500]=%d r[501]=%d", r[500], r[501]);
        rlImage(img); delete e;
    }
    // ── 5. Covered: rows go into the ring, no images; uncover publishes ONE ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        auto drainUi = [&](int& images, double& head) {
            UiUpdate u;
            if (e->pollUi(u)) { if (u.image) { images++; head = u.head; rlImage(u.image); } if (u.peakPath) rlPath(u.peakPath); }
        };
        int images = 0; double head = 0;
        for (int f = 0; f < 5; f++) { auto b = frame(1024, 200); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6); run(e, 50); drainUi(images, head); }
        e->setCovered(true); run(e, 1); drainUi(images, head);
        const int before = images;
        for (int f = 0; f < 5; f++) { auto b = frame(1024, 200); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6); run(e, 50); drainUi(images, head); }
        CHECK(images == before, "no images while covered (%d → %d)", before, images);
        const double headBefore = head;
        e->setCovered(false); run(e, 2); drainUi(images, head);
        CHECK(images == before + 1 && head >= headBefore + 4, "uncover: ONE image carrying the covered rows (images +%d, head %g → %g)",
              images - before, headBefore, head);
        delete e;
    }
    // ── 6. Background stops the repeat rows at once (the JS timer did not) ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 40;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        double head = 0;
        auto drain = [&]() { UiUpdate u; if (e->pollUi(u)) { if (u.image) { head = u.head; rlImage(u.image); } if (u.peakPath) rlPath(u.peakPath); } };
        for (int f = 0; f < 10; f++) { auto b = frame(1024, 200); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6); run(e, 100); drain(); }
        auto b = frame(1024, 200); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
        run(e, 2); drain();
        const double h0 = head;
        e->setBackground(true); run(e, 300); drain();
        CHECK(head == h0, "no rows after background (head %g → %g)", h0, head);
        delete e;
    }
    std::printf("images made %d freed %d · paths made %d freed %d\n", gImagesMade, gImagesFreed, gPathsMade, gPathsFreed);
    CHECK(gImagesMade == gImagesFreed, "image leak: %d made, %d freed", gImagesMade, gImagesFreed);
    CHECK(gPathsMade == gPathsFreed, "path leak: %d made, %d freed", gPathsMade, gPathsFreed);
    std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
