// Host test for SpectrumEngine — behaviour that must match WaterfallView.tsx (docs/NATIVE-SPECTRUM-ENGINE.md).
//   clang++ -std=c++17 -O2 -I../vibedsp test_spectrum_engine.cpp spectrum_engine.cpp ../vibedsp/spectrum_processor.cpp
// Images and paths are faked (a heap copy of the ring / the points); the engine runs on a simulated clock.
#include "spectrum_engine.h"
#include "wf_row_target.h"
#include "wf_wide.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
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
        CHECK(rows >= 27 && rows <= 33, "default at 10 fps: %g rows in 1 s (expect 30 — Default never 20, Stuart 2026-10-29 — ±3 for where the window cuts a frame's extras)", rows);
        delete e;
    }
    // ── 2d. wf_row_target.h: the same table as scripts/test_wf_row_target.ts, from a cold 150 ms estimate ──
    {
        auto drive = [](int cap, double fps, vibewf::RowTargetState& st, double& ema, int frames) {
            double t = 0; for (int i = 0; i < frames; i++) { ema = ema * 0.8 + (1000.0 / fps) * 0.2; t = vibewf::stepRowTarget(st, cap, 1000.0 / ema); }
            return t; };
        auto settle = [&](int cap, double fps) { vibewf::RowTargetState st; double ema = 150; const double t = drive(cap, fps, st, ema, 200); return t == 0 ? fps : t; };
        struct Row { double fps, def, smo; } tbl[] = { {5, 15, 30}, {10, 30, 60}, {20, 30, 60}, {10.0 / 3, 10, 20}, {9, 30, 30} };
        for (const Row& r : tbl)
            CHECK(settle(20, r.fps) == r.def && settle(60, r.fps) == r.smo, "row target at %.2f fps: %g / %g (want %g / %g)",
                  r.fps, settle(20, r.fps), settle(60, r.fps), r.def, r.smo);
        CHECK(vibewf::rowTargetFor(20, 20) == 30 && vibewf::rowTargetFor(20, 10) == 30, "Default is never 20: 20 and 10 fps feeds run at 30");
        // ★ 2026-10-29 (Stuart, a 23 fps Kiwi: "interpolate and discard") — every choice but Native divides 60, the same
        //   table as scripts/test_wf_row_target.ts: Eco/Minimum thin a faster feed, Default/Smooth go up to the next rate.
        struct Sn { int cap; double fps, want; } sn[] = { {20, 23, 30}, {60, 23, 60}, {10, 23, 10}, {5, 23, 5}, {10, 13, 10},
            {20, 13, 30}, {20, 30, 0}, {20, 40, 60}, {60, 75, 60}, {10, 10.2, 0}, {10, 10.6, 10}, {20, 2, 0}, {20, 20.3, 30} };
        for (const Sn& r : sn)
            CHECK(vibewf::rowTargetFor(r.cap, r.fps) == r.want, "snap: cap %d at %.1f fps -> %g (want %g)", r.cap, r.fps, vibewf::rowTargetFor(r.cap, r.fps), r.want);
        {   // discard counts: 23 fps thinned to 20 keeps 2000 of 2300 frames, never two dropped in a row
            double c = 0; int rows = 0, dropped = 0, prevZero = 0, twoInRow = 0;
            for (int i = 0; i < 2300; i++) { const int k = vibewf::rowsForArrival(c, 1000.0 / 23, 20); rows += k; if (k == 0) { dropped++; if (prevZero) twoInRow++; } prevZero = k == 0; }
            CHECK(std::abs(rows - 2000) <= 1 && std::abs(dropped - 300) <= 1 && twoInRow == 0, "thin 23 -> 20: %d rows, %d dropped, %d doubled", rows, dropped, twoInRow);
            c = 0; rows = 0; int mx = 0;
            for (int i = 0; i < 2300; i++) { const int k = vibewf::rowsForArrival(c, 1000.0 / 23, 30); rows += k; mx = std::max(mx, k); }
            CHECK(std::abs(rows - 3000) <= 1 && mx == 2, "fill 23 -> 30: %d rows, max %d a frame", rows, mx);
            c = 0; CHECK(vibewf::rowsForArrival(c, 43, 0) == 1, "native: every frame one row");
        }
        CHECK(vibewf::rowTargetUnderCap(20, 20, 30) == 15, "under a 30 cap a 20 fps feed is thinned to 15");
        // ★ V12 ECO (cap 10): exactly 10 rows/s from any slower feed (over-generate and discard); 10 fps = actual rows.
        struct Eco { double fps, want; } eco[] = { {5, 10}, {10.0 / 3, 10}, {6, 10}, {7, 10}, {9, 10}, {10, 10}, {2, 10} };
        for (const Eco& r : eco)
            CHECK(std::abs(settle(10, r.fps) - r.want) < 0.05, "eco at %.2f fps: %g rows (want %g)", r.fps, settle(10, r.fps), r.want);
        // ★ V12 MINIMUM (cap 5): actual rows at 5; a slower feed (2, 3.3) is filled in to 5 — and (2026-10-29) a faster
        //   one is THINNED to 5 (it used to be shown at its own 10).
        struct Mn { double fps, want; } mn[] = { {5, 5}, {10.0 / 3, 5}, {2, 5}, {10, 5} };
        for (const Mn& r : mn)
            CHECK(std::abs(settle(5, r.fps) - r.want) < 0.05, "minimum at %.2f fps: %g rows (want %g)", r.fps, settle(5, r.fps), r.want);
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
    // ── 4b. THE ZOOM FAN (wf_zoom_fan.h): a ×2 zoom in re-maps the history — every old row's carrier moves to the column
    //        the new span puts it at (688, from 600 at 2.048 MHz), none left at its old one (the slices). Zoom back out:
    //        the old rows are their stored selves again, byte for byte. ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        void* img = nullptr; double head = 0, fanK = 1, fanC = 0;
        auto feed = [&](int frames, int n, int bin, double bw) {
            for (int f = 0; f < frames; f++) {
                auto b = frame(n, bin);
                e->pushFrame(b.data(), n, 100e6, bw, 100e6);
                run(e, 50);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) { if (img) rlImage(img); img = u.image; head = u.head; fanC += fanK * u.fanC; fanK *= u.fanK; }
                    if (u.peakPath) rlPath(u.peakPath);
                }
            }
        };
        feed(10, 1024, 600, 2.048e6);
        const double oldHead = head;
        std::vector<uint8_t> stored(((FakeImage*)img)->px);
        e->setView(100e6, 1.024e6);
        feed(24, 1024, 688, 1.024e6);
        // ★★★ A ZOOM LEAVES THE HISTORY AS DRAWN (Stuart 2026-10-28): the old rows keep the carrier at 600, the new ones ease
        //     out to 688 (the fan) and land there; the UI is handed no screen move.
        int kept = 0, moved = 0;
        for (double h = 2; h <= oldHead; h++) {
            const uint8_t* r = rowOf((FakeImage*)img, h);
            if (r[600] > 150) kept++;
            if (r[688] > 150) moved++;
        }
        CHECK(kept == (int)oldHead - 1 && moved == 0, "zoom in: %d of %d old rows kept at 600, %d moved to 688 (want as drawn)", kept, (int)oldHead - 1, moved);
        const uint8_t* nr = rowOf((FakeImage*)img, head);
        CHECK(nr[688] > 150, "zoom in: the newest row's carrier at 688 (%d)", nr[688]);
        CHECK(fanK == 1 && fanC == 0, "zoom in: the UI was handed k %g c %g, want no move", fanK, fanC);
        CHECK(((FakeImage*)img)->px.size() == (size_t)1024 * (1024 + vibewf::MOTION_ROWS), "the image carries the motion rows");
        e->setView(100e6, 2.048e6);
        feed(6, 1024, 600, 2.048e6);   // (the jitter buffer holds a couple of frames: enough to land the new span)
        int same = 0;
        for (double h = 1; h <= oldHead; h++)
            if (std::memcmp(rowOf((FakeImage*)img, h), stored.data() + ((size_t)(h - 1) % 1024) * 1024, 1024) == 0) same++;
        CHECK(same == (int)oldHead, "zoom in then out: %d of %d old rows as drawn, byte for byte", same, (int)oldHead);
        rlImage(img); delete e;
    }
    // ── 4c. V12 THE FULL-CAPTURE LAYER (wf_wide.h): zoom in AWAY from a carrier with SPWB rows arriving on the native
    //        socket, then back out — every row written meanwhile shows the carrier at its true column, from the layer.
    //        CONTROL: the same feed with no SPWB (an older server) allocates nothing and leaves the strip in black. ──
    for (int withWide = 1; withWide >= 0; withWide--) {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78;
        const double CAPC = 100e6, CAPS = 2.4e6, CAR = 99.4e6;   // the capture; the carrier 600 kHz below its centre
        e->setConfig(c); e->setView(CAPC, CAPS);
        // the SPWB the server sends: a -108 dB floor (sigma 1 dB), the carrier's cell at -40 dB, one bin wide
        std::vector<uint8_t> w(32 + 256, 0);
        w[0] = 'S'; w[1] = 'P'; w[2] = 'W'; w[3] = 'B'; w[4] = 2;
        { const uint64_t cc = (uint64_t)CAPC; for (int i = 0; i < 8; i++) w[14 + i] = (uint8_t)(cc >> (8 * i));
          const uint32_t sp = (uint32_t)CAPS; for (int i = 0; i < 4; i++) w[22 + i] = (uint8_t)(sp >> (8 * i));
          w[26] = 128; w[28] = 0; w[29] = 4;   // F 128, C 1024
          for (int i = 0; i < 128; i++) { w[32 + i] = 148; w[32 + 128 + i] = 10; }
          const double cf = (CAR - (CAPC - CAPS / 2)) / CAPS * 1024; int cell = (int)std::floor(cf), d = cell + 1, P = 0;
          while (d > 255) { w.push_back(255); w.push_back(0); w.push_back(0); d -= 255; P++; }
          w.push_back((uint8_t)d); w.push_back(216); w.push_back((uint8_t)std::min(15, (int)std::floor((cf - cell) * 16))); P++;
          w[30] = (uint8_t)P; }
        void* img = nullptr; double head = 0; int fed = 0;
        auto feed = [&](int frames, double vc, double bw) {
            e->setView(vc, bw);
            for (int f = 0; f < frames; f++, fed++) {
                const int bin = (int)std::floor((CAR - (vc - bw / 2)) / (bw / 1024));
                auto b = frame(1024, bin >= 0 && bin < 1024 ? bin : -1);
                if (withWide && bw < CAPS * 0.98 && fed % 4 == 0) e->pushWideNative(w.data(), w.size());
                e->pushFrameNative(b.data(), 1024, vc, bw, vc);
                run(e, 50);
                UiUpdate u;
                if (e->pollUi(u)) { if (u.image) { if (img) rlImage(img); img = u.image; head = u.head; } if (u.peakPath) rlPath(u.peakPath); }
            }
        };
        feed(10, CAPC, CAPS);
        const double h0 = head;
        feed(30, 100.3e6, 200e3);                  // zoomed in, 700 kHz from the carrier
        const double h1 = head;
        feed(4, CAPC, CAPS);                       // back out
        const int want = (int)std::floor((CAR - (CAPC - CAPS / 2)) / CAPS * 1024);   // 256
        // ★ The zoom's EASED rows are drawn wider than their own data (the fan). This feed's floor is index 0, so black
        //   cannot be counted as holes; instead: with SPWB the layer draws the capture around the narrow strip (rows with
        //   NO zero texel), and across the ease the narrow strip widens on screen row by row (zeros never fewer).
        (void)h1; (void)want;
        int filled = 0, mono = 1, prevZ = -1, rows = 0;
        for (double h = h0 + 1; h <= h0 + 40; h++) {
            const uint8_t* r = rowOf((FakeImage*)img, h); int z = 0; for (int x = 0; x < 1024; x++) if (r[x] == 0) z++;
            if (z == 0) { filled++; prevZ = 0; continue; }
            if (prevZ >= 0 && z < 1000) { rows++; if (z < prevZ) mono = 0; prevZ = z; }
        }
        if (getenv("WF_DEBUG")) for (double h = h0 - 2; h <= h0 + 30; h++) { const uint8_t* r = rowOf((FakeImage*)img, h); int z = 0; for (int x = 0; x < 1024; x++) if (r[x] == 0) z++; std::printf("    row %+g: %d zero\n", h - h0, z); }
        std::printf("  the zoom ease (engine, %s): %d rows filled by the layer, %d widening rows (monotone %d); off %.2f dB\n",
                    withWide ? "SPWB" : "older server", filled, rows, mono, e->wideOffForTest());
        CHECK(mono, "the zoom's eased rows do not widen steadily");
        if (withWide) {
            CHECK(e->layerForTest() != nullptr, "the layer: not allocated with SPWB arriving");
            CHECK(std::fabs(e->wideOffForTest()) < 2, "the layer's floor offset (%.2f dB) matches the detailed median", e->wideOffForTest());
        } else {
            CHECK(e->layerForTest() == nullptr, "control (older server): a layer was allocated");
        }
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
    // ── V12 THE FIXED OUTPUT CLOCK (wf_row_clock.h — Stuart 2026-10-29: "over interpolate and discard"). Every mode but
    //    NATIVE: rows at exactly the mode's rate whatever the feed does — jitter, late frames, a 9 fps dip, a 9–11 fps
    //    wander, 23 fps — each row the newest REAL frame (repeated, never blended); the supply never pauses. ──
    {
        struct Case { const char* name; int cap; double want; double (*rate)(double); double jit; double late; };
        static auto r10 = [](double) { return 10.0; };
        static auto r97 = [](double) { return 9.7; };
        static auto r23 = [](double) { return 23.0; };
        static auto rdip = [](double t) { return t >= 10000 && t < 14000 ? 9.0 : 10.0; };
        static auto rwan = [](double t) { static const double v[] = {9, 11, 9.4, 10.7, 9, 11, 10.2, 9.1}; return v[(int)(t / 2500) % 8]; };
        const Case cases[] = {
            {"Default · 10 fps ±15 ms + late", 20, 30, r10, 15, 0.03}, {"Smooth · 10 fps ±15 ms + late", 60, 60, r10, 15, 0.03},
            {"Default · 9.7 fps", 20, 30, r97, 5, 0}, {"Smooth · 9.7 fps", 60, 60, r97, 5, 0},
            {"Default · 23 fps", 20, 30, r23, 5, 0}, {"Smooth · 23 fps", 60, 60, r23, 5, 0},
            {"Default · 10 → 9 dip", 20, 30, rdip, 5, 0}, {"Smooth · 10 → 9 dip", 60, 60, rdip, 5, 0},
            {"Default · 9–11 wander ±15", 20, 30, rwan, 15, 0}, {"Smooth · 9–11 wander ±15", 60, 60, rwan, 15, 0},
            {"Eco · 23 fps (thinned)", 10, 10, r23, 5, 0},
        };
        for (const Case& cs : cases) {
            auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
            EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = cs.cap;
            e->setConfig(c); e->setView(100e6, 2.048e6);
            uint32_t seed = 12345; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
            const double T0 = gClock;
            double nominal = 0, nextAt = 0, head = 0, lastStep = 0, maxGap = 0; int rateOff = 0, polls = 0;
            std::vector<double> stepT;
            for (double t = 0; t < 30000; t += 1) {
                gClock = T0 + t;
                if (t >= nextAt) {
                    auto b = frame(1024, 300);
                    e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
                    nominal += 1000.0 / cs.rate(nominal);
                    nextAt = nominal + (rnd() * 2 - 1) * cs.jit + (rnd() < cs.late ? 40 + 40 * rnd() : 0);
                }
                e->testStep(gClock);
                UiUpdate u;
                if (e->pollUi(u)) {
                    polls++;
                    if (u.image) {
                        if (u.head > head && t >= 5000) {
                            for (double k = head; k < u.head; k++) stepT.push_back(t);
                            if (lastStep > 0) maxGap = std::max(maxGap, t - lastStep);
                            lastStep = t;
                        }
                        head = u.head; rlImage(u.image);
                    }
                    if (u.peakPath) rlPath(u.peakPath);
                    if (t >= 5000 && u.rowRate != cs.want) rateOff++;
                }
            }
            // 1 s windows after 5 s
            int wMin = 1 << 30, wMax = 0;
            for (double w = 5000; w + 1000 <= 30000; w += 1000) {
                int n = 0; for (double x : stepT) if (x >= w && x < w + 1000) n++;
                wMin = std::min(wMin, n); wMax = std::max(wMax, n);
            }
            std::printf("  clock %-28s rows/s %d–%d, longest gap %.1f ms, %d polls\n", cs.name, wMin, wMax, maxGap, polls);
            CHECK(wMin >= cs.want - 1 && wMax <= cs.want + 1, "%s: rows per 1 s window %d–%d (want %g)", cs.name, wMin, wMax, cs.want);
            CHECK(maxGap <= 1000.0 / cs.want + 1.5, "%s: the supply never pauses — longest gap between rows %g ms (a tick is %g)",
                  cs.name, maxGap, 1000.0 / cs.want);
            CHECK(rateOff == 0, "%s: poll reports the clock's rate %g every time once settled (%d off)", cs.name, cs.want, rateOff);
            delete e;
        }
    }
    // ── SAMPLE-AND-HOLD, read back from the ring: an exact 10 fps feed at Default gives EVERY real frame exactly 3 rows; a
    //    9.7 fps feed 3 or 4 (3,3,…,4 — the feed's own pattern); each row is one real frame, whole (never a blend). ──
    for (double fps : {10.0, 9.7}) {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 20;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        const double T0 = gClock; double next = 0; int fi = 0; void* img = nullptr; double head = 0;
        for (double t = 0; t < 12000; t += 1) {
            gClock = T0 + t;
            if (t >= next) { auto b = frame(1024, 100 + 40 * (fi % 20)); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6); fi++; next += 1000.0 / fps; }
            e->testStep(gClock);
            UiUpdate u;
            if (e->pollUi(u)) { if (u.image) { if (img) rlImage(img); img = u.image; head = u.head; } if (u.peakPath) rlPath(u.peakPath); }
        }
        // Walk the newest ~150 rows: runs of the same carrier position = one real frame's rows.
        std::vector<int> runs; int last = -1, run = 0, blended = 0;
        for (double h = head - 150; h <= head; h++) {
            const uint8_t* r = rowOf((FakeImage*)img, h);
            int pos = -1, hot = 0;
            for (int b = 0; b < 1024; b++) if (r[b] > 200) { if (pos < 0) pos = b; hot++; }
            if (hot > 12) blended++;                       // two carriers in one row = a blend
            if (pos == last) run++; else { if (run) runs.push_back(run); run = 1; last = pos; }
        }
        runs.erase(runs.begin()); if (!runs.empty()) runs.pop_back();   // the edges are partial
        int bad = 0, threes = 0;
        for (int n : runs) { if (n == 3) threes++; if (fps == 10.0 ? n != 3 : (n < 3 || n > 4)) bad++; }
        std::printf("  hold %.1f fps at Default: %zu frames, %d of them 3 rows, %d off-pattern, %d blended\n", fps, runs.size(), threes, bad, blended);
        CHECK(bad == 0 && blended == 0, "%.1f fps at Default: rows per real frame %s (%zu frames, %d off-pattern, %d blended)",
              fps, fps == 10.0 ? "all 3" : "3 or 4", runs.size(), bad, blended);
        if (fps == 9.7) CHECK(threes > (int)runs.size() * 0.8, "9.7 fps: mostly 3s with an occasional 4 (%d of %zu are 3)", threes, runs.size());
        if (img) rlImage(img);
        delete e;
    }
    // ── V12 NO JITTER BUFFER IN FRONT OF THE CLOCK (v12-wf-no-jb — Stuart 2026-10-29: "if the buffer isnt needed for the
    //    waterfall improve the latency there too please"): a clocked frame's row is in the ring within the clock's own
    //    delay + one tick of its ARRIVAL (through the buffer it was ~1–2 feed intervals later), and the spectrum target
    //    for it is published traceLagMs after its presentation — with its row as the UI shows it, not ahead or behind. ──
    for (int cap : {20, 60}) {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = cap;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        uint32_t seed = 4242; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
        const double T0 = gClock; double next = 0; int fi = 0;
        std::vector<double> arrAt, ringAt, specAt;
        for (double t = 0; t < 20000; t += 1) {
            gClock = T0 + t;
            if (t >= next) {
                auto b = frame(1024, 100 + 40 * (fi % 20)); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
                arrAt.push_back(t); ringAt.push_back(-1); specAt.push_back(-1); fi++;
                next = fi * 100.0 + (rnd() * 2 - 1) * 15;
            }
            e->testStep(gClock);
            UiUpdate u;
            if (e->pollUi(u)) {
                if (u.image) {   // the newest row's carrier → which frame it is (the latest one sent with that position)
                    const uint8_t* r = rowOf((FakeImage*)u.image, u.head); int pos = -1;
                    for (int b = 0; b < 1024; b++) if (r[b] > 200) { pos = b; break; }
                    for (int k = fi - 1; k >= 0 && k >= fi - 6; k--) if (std::abs(100 + 40 * (k % 20) - pos) <= 4) { if (ringAt[k] < 0) ringAt[k] = t; break; }
                    rlImage(u.image);
                }
                if (!u.specTarget.empty()) {   // the trace's peak → its frame
                    int best = 0; for (int k = 1; k < (int)u.specTarget.size(); k++) if (u.specTarget[k] > u.specTarget[best]) best = k;
                    const int bin = (int)std::lround(best * 1024.0 / u.specTarget.size());
                    for (int k = fi - 1; k >= 0 && k >= fi - 6; k--) if (std::abs(100 + 40 * (k % 20) - bin) <= 8) { if (specAt[k] < 0) specAt[k] = t; break; }
                }
                if (u.peakPath) rlPath(u.peakPath);
            }
        }
        double lat = 0, off = 0; int n = 0, no = 0, missing = 0;
        for (size_t k = 0; k + 5 < arrAt.size(); k++) {
            if (arrAt[k] < 5000) continue;
            if (ringAt[k] < 0) { missing++; continue; }
            lat += ringAt[k] - arrAt[k]; n++;
            if (specAt[k] >= 0) { off += specAt[k] - ringAt[k]; no++; }
        }
        lat /= std::max(1, n); off /= std::max(1, no);
        const double rate = cap == 20 ? 30 : 60, lag = vibewf::traceLagMs(rate);
        std::printf("  no jitter buffer, %s: arrival → ring %.1f ms mean, ring → trace %.1f ms (traceLag %.1f), %d/%d traced, %d missing\n",
                    cap == 20 ? "Default" : "Smooth", lat, off, lag, no, n, missing);
        CHECK(missing == 0, "cap %d: every frame reaches the ring (%d missing)", cap, missing);
        CHECK(lat <= 20 + 1000.0 / rate + 5, "cap %d: arrival → ring %.1f ms — the clock's delay + one tick, no jitter buffer", cap, lat);
        CHECK(no >= n * 0.9, "cap %d: the trace takes (nearly) every frame (%d of %d)", cap, no, n);
        CHECK(std::abs(off - lag) <= 1000.0 / rate / 2 + 3, "cap %d: the trace follows its row by traceLag %.1f ms (got %.1f)", cap, lag, off);
        delete e;
    }
    // ── ★★★ V12 2026-10-30 THE WATERFALL WAITS FOR THE SOUND (wf_av_sync.h; scripts/test_wf_avsync.ts proves the rule).
    //    The same 10 fps feed with no audio delay and with 300 ms of it (polled at 4 Hz, as JS does): every frame's row
    //    reaches the ring later by the hold (300 − the clock's own route), the trace still follows its row by traceLag,
    //    the rows keep coming one per tick (the hold's growth is the clock's repeats — Stuart: "simply duplicate the
    //    previous lines if needed"), and during a drag (12–14 s) nothing is held at all. ──
    for (int cap : {20, 60}) {
        struct R { double latRest = 0, latDrag = 0, latAfter = 0, off = 0; int missing = 0, gaps = 0, nT = 0; };
        auto runOne = [&](double audio) {
            auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
            EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = cap;
            e->setConfig(c); e->setView(100e6, 2.048e6);
            uint32_t seed = 777; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
            const double T0 = gClock; double next = 0; int fi = 0; double lastHeadT = -1, head = 0;
            const double tick = 1000.0 / (cap == 20 ? 30 : 60);
            std::vector<double> arrAt, ringAt, specAt;
            R r;
            for (double t = 0; t < 22000; t += 1) {
                gClock = T0 + t;
                const bool drag = t >= 12000 && t < 14000;
                if (audio >= 0 && std::fmod(t, 250) == 0) e->setAudioDelay(audio + 20 * std::fmod(t / 20, 1) - 10);
                if (t >= next) {
                    if (drag) e->setLastInteractAt(gClock);
                    auto b = frame(1024, 100 + 40 * (fi % 20)); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
                    arrAt.push_back(t); ringAt.push_back(-1); specAt.push_back(-1); fi++;
                    next = fi * 100.0 + (rnd() * 2 - 1) * 15;
                }
                e->testStep(gClock);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) {
                        if (u.head != head) {
                            const bool calm = t > 5000 && (t < 11500 || t > 16000);
                            if (calm && lastHeadT >= 0 && t - lastHeadT > tick + 2) r.gaps++;
                            lastHeadT = t; head = u.head;
                        }
                        const uint8_t* rr = rowOf((FakeImage*)u.image, u.head); int pos = -1;
                        for (int b = 0; b < 1024; b++) if (rr[b] > 200) { pos = b; break; }
                        for (int k = fi - 1; k >= 0 && k >= fi - 8; k--) if (std::abs(100 + 40 * (k % 20) - pos) <= 4) { if (ringAt[k] < 0) ringAt[k] = t; break; }
                        rlImage(u.image);
                    }
                    if (!u.specTarget.empty()) {
                        int best = 0; for (int k = 1; k < (int)u.specTarget.size(); k++) if (u.specTarget[k] > u.specTarget[best]) best = k;
                        const int bin = (int)std::lround(best * 1024.0 / u.specTarget.size());
                        for (int k = fi - 1; k >= 0 && k >= fi - 8; k--) if (std::abs(100 + 40 * (k % 20) - bin) <= 8) { if (specAt[k] < 0) specAt[k] = t; break; }
                    }
                    if (u.peakPath) rlPath(u.peakPath);
                }
            }
            int nR = 0, nD = 0, nA = 0;
            for (size_t k = 0; k + 8 < arrAt.size(); k++) {
                const double a = arrAt[k];
                if (a < 5000) continue;
                const bool rest = a < 11000, dragW = a >= 12200 && a < 14000, after = a >= 16000;
                if (!rest && !dragW && !after) continue;
                if (ringAt[k] < 0) { if (rest || after) r.missing++; continue; }
                const double l = ringAt[k] - a;
                if (rest) { r.latRest += l; nR++; } else if (dragW) { r.latDrag += l; nD++; } else { r.latAfter += l; nA++; }
                if ((rest || after) && specAt[k] >= 0) { r.off += specAt[k] - ringAt[k]; r.nT++; }
            }
            r.latRest /= std::max(1, nR); r.latDrag /= std::max(1, nD); r.latAfter /= std::max(1, nA); r.off /= std::max(1, r.nT);
            delete e;
            return r;
        };
        const R b = runOne(-1), a = runOne(300);
        const double rate = cap == 20 ? 30 : 60, lag = vibewf::traceLagMs(rate);
        const double want = 300 - vibewf::clockOwnMs(100, rate);
        std::printf("  A/V hold, %s: arrival → ring %.1f → %.1f ms at rest (hold %.0f), %.1f after a drag; during the drag %.1f → %.1f; "
                    "ring → trace %.1f (traceLag %.1f); row gaps %d → %d; missing %d\n",
                    cap == 20 ? "Default" : "Smooth", b.latRest, a.latRest, want, a.latAfter, b.latDrag, a.latDrag, a.off, lag, b.gaps, a.gaps, a.missing);
        CHECK(std::abs((a.latRest - b.latRest) - want) <= 1000.0 / rate + 6, "cap %d: held by %.0f ms at rest (got %.1f)", cap, want, a.latRest - b.latRest);
        CHECK(std::abs((a.latAfter - b.latRest) - want) <= 1000.0 / rate + 6, "cap %d: held again after the drag (%.1f)", cap, a.latAfter - b.latRest);
        CHECK(std::abs(a.latDrag - b.latDrag) <= 3, "cap %d: nothing held during a drag (%.1f vs %.1f ms)", cap, a.latDrag, b.latDrag);
        CHECK(std::abs(a.off - lag) <= 1000.0 / rate / 2 + 3, "cap %d: the trace still follows its row by traceLag %.1f (got %.1f)", cap, lag, a.off);
        CHECK(a.gaps <= b.gaps, "cap %d: no row gaps the clock did not already have (%d → %d)", cap, b.gaps, a.gaps);
        CHECK(a.missing == 0, "cap %d: every frame reaches the ring (%d missing)", cap, a.missing);
    }
    // ── ★★★ THE INTERACTION BOOST (wf_row_clock.h boostRowRate — Stuart 2026-10-29): during a tune the clock's rate steps
    //    up to the display's maximum as set (≤ 120, ≤ 12 × the feed), from frames already here, and back after the tail;
    //    NATIVE is untouched (no rate, one row a frame, through the same tune). ──
    {
        struct BC { const char* name; int cap; int panel; int dcap; double fps; double base; double boosted; };
        const BC bcs[] = {
            {"Default · 10 fps · 120 Hz panel", 20, 120, 0, 10, 30, 120}, {"Default · 10 fps · 60 Hz panel", 20, 60, 0, 10, 30, 60},
            {"Default · 5 fps · 120 Hz panel", 20, 120, 0, 5, 30, 60},     {"Smooth · 10 fps · FRAME RATE 60", 60, 120, 60, 10, 60, 60},
            {"Eco · 10 fps · 120 Hz panel", 10, 120, 0, 10, 10, 120},      {"Default · 10 fps · FRAME RATE 30", 20, 120, 30, 10, 30, 30},
            {"Native · 10 fps · 120 Hz panel", 0, 120, 0, 10, 0, 0},
        };
        for (const BC& bc : bcs) {
            auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
            EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = bc.cap; c.panelHz = bc.panel; c.displayCapHz = bc.dcap;
            c.smoothTune = true;
            e->setConfig(c); double vc = 1'000'000; e->setView(vc, 200'000);
            const double T0 = gClock; double next = 0; int fi = 0, rateBad = 0, maxRate = 0, after = -1; double head = 0, h0 = 0, h1 = 0;
            int restBad = 0;
            for (double t = 0; t < 16000; t += 1) {
                gClock = T0 + t;
                const bool tuning = t >= 8000 && t < 9500;
                if (t >= next) {
                    if (tuning) { vc -= 9'000; e->setView(vc, 200'000); e->setLastInteractAt(gClock); }
                    auto b = frame(1024, 300); e->pushFrame(b.data(), 1024, vc, 200'000, vc); fi++;
                    next = fi * 1000.0 / bc.fps;
                }
                e->testStep(gClock);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) { head = u.head; rlImage(u.image); }
                    if (u.peakPath) rlPath(u.peakPath);
                    maxRate = std::max(maxRate, (int)u.rowRate);
                    if (t >= 8300 && t < 9500 && u.rowRate != bc.boosted) rateBad++;
                    if ((t >= 5000 && t < 8000) || t >= 12500) { if (u.rowRate != bc.base) restBad++; }
                    if (t >= 9500 && after < 0 && u.rowRate == bc.base) after = (int)(t - 9500);
                }
                if (t == 13000) h0 = head;
                if (t == 15999) h1 = head;
            }
            std::printf("  boost %-34s rest %g → tune %d rows/s (max %d), back %d ms after the last step; rest rows/s %g\n",
                        bc.name, bc.base, bc.boosted > 0 ? (int)bc.boosted : 0, maxRate, after, (h1 - h0) / 3);
            CHECK(rateBad == 0 && maxRate == (int)bc.boosted, "%s: rate during the tune not %g (%d off, max %d)", bc.name, bc.boosted, rateBad, maxRate);
            CHECK(restBad == 0, "%s: the rate at rest not %g (%d off)", bc.name, bc.base, restBad);
            CHECK(maxRate <= 12 * bc.fps + 0.5, "%s: boost above 12 × the feed (%d)", bc.name, maxRate);
            if (bc.cap == 0) CHECK(std::fabs((h1 - h0) - 3 * bc.fps) <= 2, "NATIVE through a tune: %g rows in 3 s at %g fps", h1 - h0, bc.fps);
            delete e;
        }
    }
    // ── NATIVE stays as it is: one row per frame received, in order, none repeated or dropped; no clock rate ──
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 0;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        const double T0 = gClock; double next = 0; int fi = 0; void* img = nullptr; double head = 0, headAt = 0; int rateOn = 0, sent = 0;
        uint32_t seed = 777; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
        for (double t = 0; t < 15000; t += 1) {
            gClock = T0 + t;
            if (t >= next) {
                auto b = frame(1024, 100 + 40 * (fi % 20)); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6); fi++;
                if (t >= 5000) sent++;
                next = fi * 100.0 + (rnd() * 2 - 1) * 15;
            }
            if (t == 5000) headAt = head;
            e->testStep(gClock);
            UiUpdate u;
            if (e->pollUi(u)) { if (u.image) { if (img) rlImage(img); img = u.image; head = u.head; } if (u.peakPath) rlPath(u.peakPath); if (u.rowRate != 0) rateOn++; }
        }
        // Each settled frame is one row: consecutive rows differ (no repeat), carriers step 40 bins at a time (none dropped).
        int repeats = 0, skips = 0, lastPos = -1;
        for (double h = head - 80; h <= head; h++) {
            const uint8_t* r = rowOf((FakeImage*)img, h); int pos = -1;
            for (int b = 0; b < 1024; b++) if (r[b] > 200) { pos = b; break; }
            if (lastPos >= 0) { int d = ((pos - lastPos) % 800 + 800) % 800; if (d == 0) repeats++; else if (d != 40 && d != 800 - 760) skips++; }
            lastPos = pos;
        }
        std::printf("  native: %g rows for %d frames, %d repeated, %d dropped\n", head - headAt, sent, repeats, skips);
        CHECK(repeats == 0 && skips == 0, "NATIVE: one row per frame, in order (%d repeated, %d dropped in the last 80 rows)", repeats, skips);
        CHECK(std::abs((head - headAt) - sent) <= 2, "NATIVE: %g rows for %d frames since 5 s", head - headAt, sent);
        CHECK(rateOn == 0, "NATIVE: no clock rate reported (%d)", rateOn);
        if (img) rlImage(img);
        delete e;
    }
    // ── THE WHITE BAR (2026-10-29): a row the server built OUTSIDE its own capture gets no layer (wf_wide.h rowInCapture) ──
    {
        using namespace vibewf;
        WideRow w; w.centreHz = 7115000; w.spanHz = 3000000; w.cells = 64;
        w.floor.assign(32, -110.f); w.spread.assign(32, 1.f);
        w.pkCell.assign(1, 0); w.pkDb.assign(1, 0.f); w.pkShape.assign(1, 0); w.nPk = 0;
        WideState st; const double rs = 74409;
        wideArrive(st, w, 0, 8793325 - rs / 2, rs, -140);
        LayerRing L(4, 32, 64);
        const ToIdx stop(-145, -115, 0, 0), real(-114, -84, 0, 0);
        const double kOut = layerWriteRow(L, 0, &st, 8793325 - rs / 2, rs, 10, stop);
        const double kIn  = layerWriteRow(L, 1, &st, 7100000 - rs / 2, rs, 10, real);
        std::printf("  white bar: a row at 8.79 MHz on a 5.6-8.6 MHz capture -> layer k %.4f (the 40 m floor through its range = index %d); at 7.1 MHz k %.4f\n",
                    kOut, stop(-110), kIn);
        CHECK(kOut == 0, "a row outside its own capture got a layer (k %.4f) — it fills white", kOut);
        CHECK(kIn > 0, "a row inside the capture lost its layer");
    }
    // ── ★★ SCALE STAMPS (wf_stamps.h; src/utils/wfStamps.ts is the reference, scripts/test_wf_stamps.ts its proof) — the
    //    engine detects, the UI draws: a ×2 zoom in publishes ONE stamp of the OLD scale on the newest row drawn at it;
    //    a rapid run of zoom steps publishes one more (the pre-run scale), never more than 3, never crowded.
    {
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 0; c.stampViewRows = 600; c.stampMinGapRows = 45;
        e->setConfig(c); e->setView(7.1e6, 200e3);
        std::vector<vibewf::WfStamp> got; uint32_t gens = 0; void* lastImg = nullptr;
        auto feed = [&](double centre, double span, int frames) {
            for (int f = 0; f < frames; f++) {
                auto b = frame(1024, 512);
                e->setView(centre, span);
                e->pushFrame(b.data(), 1024, centre, span, centre);
                run(e, 50);
                UiUpdate u;
                if (e->pollUi(u)) {
                    if (u.image) { if (lastImg) rlImage(lastImg); lastImg = u.image; }
                    if (u.peakPath) rlPath(u.peakPath);
                    if (u.stampsNew) { got = u.stamps; gens++; }
                }
            }
        };
        feed(7.1e6, 200e3, 60);
        CHECK(got.empty(), "steady: no stamps (%zu)", got.size());
        feed(7.1e6, 100e3, 60);
        CHECK(got.size() == 1 && std::fabs(got[0].span - 200e3) < 1 && std::fabs(got[0].lo - 7.0e6) < 1,
              "a zoom in: ONE stamp of the old scale (%zu, %.0f + %.0f)", got.size(), got.empty() ? 0 : got[0].lo, got.empty() ? 0 : got[0].span);
        double span = 100e3;
        for (int k = 0; k < 8; k++) { span /= 1.2; feed(7.1e6, span, 2); }   // a rapid run: a step every 100 ms
        feed(7.1e6, span, 60);
        CHECK(got.size() == 2 && std::fabs(got[0].span - 100e3) < 1, "a rapid zoom run: ONE more, the pre-run scale (%zu, newest %.0f)",
              got.size(), got.empty() ? 0 : got[0].span);
        CHECK(got.size() < 2 || got[0].frame - got[1].frame >= 45, "never crowded (%g rows apart)", got.size() < 2 ? 0 : got[0].frame - got[1].frame);
        {   // ★★ a tap on a row drawn before the zoom reads THAT row's scale (rowGeom), the newest row the live one
            double lo = 0, span = 0;
            const bool okOld = e->rowGeom(got.back().frame, lo, span);
            CHECK(okOld && std::fabs(span - 200e3) < 1 && std::fabs(lo - 7.0e6) < 1, "rowGeom on an old row: %d %.0f + %.0f", okOld, lo, span);
            UiUpdate u; double head = 0; if (e->pollUi(u)) { head = u.head; if (u.image) rlImage(u.image); if (u.peakPath) rlPath(u.peakPath); }
            (void)head;
            CHECK(!e->rowGeom(-5, lo, span), "rowGeom of a row never written");
        }
        std::printf("  scale stamps: %zu published over %u updates\n", got.size(), gens);
        if (lastImg) rlImage(lastImg);
        delete e;
    }
    // ── 8. ★★★ THE GROWING RING (wf_ring_grow.h; Stuart, 2026-10-30: "the history only goes when it genuinely goes off
    //        screen"). Engine A runs at 1024 rows (it wraps), a zoom eases in, and JS asks for 2048 mid-ease; engine B was
    //        2048 from the start. Fed the same frames, A's images after the growth equal B's for every frame A still held —
    //        the stored/displayed rows, the motion planes (now two image rows each), the tap lookup — and A's image is the
    //        new depth with ringRows riding on it. No ringRows in the config (the 1 GB tier, an older JS) = 1024 forever. ──
    {
        auto* A = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        auto* B = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig ca; ca.width = 400; ca.wfTop = 120; ca.specH = 78; ca.targetRows = 0;
        EngineConfig cb = ca; cb.ringRows = 2048;
        A->setConfig(ca); B->setConfig(cb);
        void* ia = nullptr; void* ib = nullptr; double ha = 0, hb = 0; int rowsA = 0, rowsB = 0;
        auto step = [&](int f) {
            const double bw = f < 1290 ? 2.048e6 : 1.024e6;                 // a zoom in at 1290 (eases over the growth)
            const int bin = 300 + (f % 400);                                // a carrier walking across
            for (auto* e : { A, B }) e->setView(100e6 + (f >= 600 && f < 900 ? (f - 600) * 400.0 : 0), bw);
            auto b = frame(1024, bin);
            for (auto* e : { A, B }) e->pushFrame(b.data(), 1024, 100e6 + (f >= 600 && f < 900 ? (f - 600) * 400.0 : 0), bw, NAN);
            gClock += 50; A->testStep(gClock); B->testStep(gClock);
            UiUpdate u;
            if (A->pollUi(u)) { if (u.image) { if (ia) rlImage(ia); ia = u.image; ha = u.head; rowsA = u.ringRows; } if (u.peakPath) rlPath(u.peakPath); }
            UiUpdate v;
            if (B->pollUi(v)) { if (v.image) { if (ib) rlImage(ib); ib = v.image; hb = v.head; rowsB = v.ringRows; } if (v.peakPath) rlPath(v.peakPath); }
        };
        for (int f = 0; f < 1293; f++) step(f);
        CHECK(A->ringRowsForTest() == 1024 && rowsA == 1024, "no ringRows asked: 1024 (%d, image %d)", A->ringRowsForTest(), rowsA);
        CHECK(B->ringRowsForTest() == 2048 && rowsB == 2048, "asked 2048 from the start: 2048 (%d, image %d)", B->ringRowsForTest(), rowsB);
        CHECK(A->framesForTest() > 1100, "A has wrapped its 1024-row ring (%g rows)", A->framesForTest());
        CHECK(A->fanForTest().easeLeft > 0 && !A->fanForTest().disp.empty(), "the growth lands mid zoom-ease, a displayed ring live (%d)", A->fanForTest().easeLeft);
        const double grownAt = A->framesForTest();
        ca.ringRows = 2048; A->setConfig(ca);
        for (int f = 1293; f < 1700; f++) step(f);
        CHECK(A->ringRowsForTest() == 2048 && rowsA == 2048, "grown: 2048 rows, the image says so (%d, %d)", A->ringRowsForTest(), rowsA);
        CHECK(((FakeImage*)ia)->px.size() == (size_t)1024 * (2048 + 8), "the grown image: 2048 rows + 8 motion rows (%zu)", ((FakeImage*)ia)->px.size());
        CHECK(ha == hb && A->framesForTest() == B->framesForTest(), "both made the same rows (%g / %g)", ha, hb);
        const uint8_t* pa = A->imageBytesForTest(); const uint8_t* pb = B->imageBytesForTest();
        const double F = A->framesForTest();
        int diffRows = 0, rows = 0, geoBad = 0;
        for (double f = grownAt - 1024; f < F; f++) {
            const size_t s = (size_t)std::fmod(f, 2048.0) * 1024;
            if (std::memcmp(pa + s, pb + s, 1024) != 0) diffRows++;
            rows++;
            double la = 0, sa = 0, lb = 0, sb = 0;
            const bool ga = A->rowGeom(f, la, sa), gb = B->rowGeom(f, lb, sb);
            if (ga != gb || la != lb || sa != sb) geoBad++;
        }
        CHECK(diffRows == 0, "grown ≡ born at 2048: %d of %d displayed rows differ (every frame A held + every one since)", diffRows, rows);
        CHECK(geoBad == 0, "tap-to-tune's row geometry identical for every held frame (%d differ)", geoBad);
        CHECK(std::memcmp(pa + (size_t)2048 * 1024, pb + (size_t)2048 * 1024, (size_t)8 * 1024) == 0, "the motion planes are identical");
        const auto& ma = A->motionForTest(); const auto& mb = B->motionForTest();
        int mBad = 0;
        for (double f = grownAt - 1024; f < F; f++) { const size_t s = (size_t)std::fmod(f, 2048.0); if (ma.h[s] != mb.h[s] || ma.g[s] != mb.g[s] || !(ma.startHz[s] == mb.startHz[s] || (std::isnan(ma.startHz[s]) && std::isnan(mb.startHz[s])))) mBad++; }
        CHECK(mBad == 0 && ma.lo == mb.lo && ma.hi == mb.hi, "placement + motion per slot identical (%d differ; range %g..%g vs %g..%g)", mBad, ma.lo, ma.hi, mb.lo, mb.hi);
        int moving = 0; for (double f = ma.lo; f <= ma.hi; f++) if ((size_t)std::fmod(f, 2048.0) >= 1024 && (ma.g[(size_t)std::fmod(f, 2048.0)] != 1 || ma.h[(size_t)std::fmod(f, 2048.0)] != 0)) moving++;
        CHECK(moving > 0, "moving slots ≥ 1024 exist (the planes' second image row is exercised): %d", moving);
        // never shrinks: a smaller ask is ignored
        ca.ringRows = 1024; A->setConfig(ca); step(1700);
        CHECK(A->ringRowsForTest() == 2048, "never shrinks (%d)", A->ringRowsForTest());
        // a silly ask is bounded by the top tier
        ca.ringRows = 100000; A->setConfig(ca); step(1701);
        CHECK(A->ringRowsForTest() == SpectrumEngine::RING_MAX_ROWS, "bounded at %d (%d)", SpectrumEngine::RING_MAX_ROWS, A->ringRowsForTest());
        std::printf("  growing ring: grown at row %g, %d held rows compared\n", grownAt, rows);
        if (ia) rlImage(ia);
        if (ib) rlImage(ib);
        delete A; delete B;
    }
    // ── ★★ V12 2026-10-30 THINNED FRAMES ARE COMBINED, NOT DROPPED (wf_row_combine.h — the twin of
    //    scripts/test_wf_combine.ts). Stuart: "rather than simply dropping the excess frames can we average them". ──
    {
        const double K = vibewf::dbPerUnit(-120, -60);
        auto u = [&](double db) { return (int)std::lround(db / K); };
        vibewf::RowCombiner c; uint8_t out[8];
        std::vector<uint8_t> a(8, 100), b(8, (uint8_t)(100 + u(10)));
        vibewf::combineAdd(c, a.data(), 8, 0, 1, K); vibewf::combineAdd(c, b.data(), 8, 0, 1, K);
        const int n2 = vibewf::combineTake(c, out, 8);
        const double got = (out[0] - 100) * K, want = 10 * std::log10((1 + std::pow(10, u(10) * K / 10)) / 2);
        CHECK(n2 == 2 && std::abs(got - want) <= K && got > 7, "two frames 10 dB apart → +%.2f dB (linear mean %.2f), never +5", got, want);
        for (int f = 0; f < 5; f++) { std::vector<uint8_t> r(8, (uint8_t)(f == 2 ? 80 + u(30) : 80)); vibewf::combineAdd(c, r.data(), 8, 0, 1, K); }
        vibewf::combineTake(c, out, 8);
        const double kept = (out[0] - 80) * K;
        CHECK(kept >= u(30) * K - vibewf::COMBINE_PEAK_DB - K && kept <= u(30) * K, "a 1-frame +30 dB burst among 5 kept at +%.1f dB", kept);
        std::vector<uint8_t> one(8); for (int i = 0; i < 8; i++) one[i] = (uint8_t)(i * 31);
        vibewf::combineAdd(c, one.data(), 8, 3, 4, K);
        CHECK(vibewf::combineTake(c, out, 8) == 1 && std::memcmp(out, one.data(), 8) == 0, "a lone frame comes out byte for byte");
        std::printf("  combine: +10 dB pair → +%.2f dB, a 1-in-5 burst kept at +%.1f of +%.1f dB\n", got, kept, u(30) * K);
    }
    {   // The engine at MINIMUM (targetRows 2) on a 23 fps feed: 2 rows a second, and every frame's carrier on screen.
        auto* e = SpectrumEngine::makeForTest(mkImage, mkPath, rlImage, rlPath, clockFn);
        EngineConfig c; c.width = 400; c.wfTop = 120; c.specH = 78; c.targetRows = 2;
        e->setConfig(c); e->setView(100e6, 2.048e6);
        uint32_t seed = 99; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
        const double T0 = gClock; double next = 0; int fi = 0; double lastHead = 0;
        auto posOf = [](int k) { return 40 + 37 * (k % 25); };
        std::vector<double> arrAt; std::vector<std::pair<double, std::vector<uint8_t>>> rows;
        for (double t = 0; t < 12000; t += 1) {
            gClock = T0 + t;
            if (t >= next) {
                auto b = frame(1024, posOf(fi)); e->pushFrame(b.data(), 1024, 100e6, 2.048e6, 100e6);
                arrAt.push_back(t); fi++; next = fi * 1000.0 / 23 + (rnd() * 2 - 1) * 12;
            }
            e->testStep(gClock);
            UiUpdate up;
            if (e->pollUi(up)) {
                if (up.image) {
                    for (double h = std::max(lastHead + 1, up.head - 3); h <= up.head; h++) { const uint8_t* r = rowOf((FakeImage*)up.image, h); rows.push_back({t, std::vector<uint8_t>(r, r + 1024)}); }
                    lastHead = up.head; rlImage(up.image);
                }
                if (up.peakPath) rlPath(up.peakPath);
            }
        }
        int n = 0, seen = 0, rowsSettled = 0;
        for (auto& r : rows) if (r.first >= 4000) rowsSettled++;
        for (size_t k = 0; k < arrAt.size(); k++) {
            if (arrAt[k] < 4000 || arrAt[k] > 10500) continue;
            n++; bool vis = false;
            for (auto& r : rows) if (r.first >= arrAt[k] && r.first <= arrAt[k] + 1000) {
                for (int d = -2; d <= 2 && !vis; d++) if (r.second[posOf((int)k) + d] > 128) vis = true;
                if (vis) break;
            }
            if (vis) seen++;
        }
        const double rate = rowsSettled / 8.0;
        std::printf("  MINIMUM on 23 fps: %.2f rows/s, %d of %d frames' carriers on screen\n", rate, seen, n);
        CHECK(std::abs(rate - 2) < 0.2, "MINIMUM on a 23 fps feed: 2 rows/s (%.2f)", rate);
        CHECK(seen >= n * 0.98, "every thinned frame's carrier is combined into a row, not dropped (%d of %d)", seen, n);
        delete e;
    }
    std::printf("images made %d freed %d · paths made %d freed %d\n", gImagesMade, gImagesFreed, gPathsMade, gPathsFreed);
    CHECK(gImagesMade == gImagesFreed, "image leak: %d made, %d freed", gImagesMade, gImagesFreed);
    CHECK(gPathsMade == gPathsFreed, "path leak: %d made, %d freed", gPathsMade, gPathsFreed);
    std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
