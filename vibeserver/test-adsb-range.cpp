// test-adsb-range.cpp — vibe_adsb_range.h, THE ADS-B RANGE OUTLINE (V12 beta94, Stuart 11-25): the 1° bins and their
// daily maxima, the rolling window (7 / 30 / 90 days / all time — a one-day tropo opening leaves the outline when its day
// leaves the window), the all-time outline and the furthest-ever record kept apart, the 650 km limit, the receiver moving,
// the owner's reset, the bounded storage and the file (round trip, damage refused), the store's per-source files — and the
// ≥ 2 consistent positions rule through vibe_adsb.h's setPos (posOk). Silent; writes only under $TMPDIR.
#include "vibe_adsb_range.h"
#include "vibe_adsb.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vibeadsb;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

// The receiver: Northampton-ish. A point `km` out on `brg` (great circle).
static const double RX_LAT = 52.29, RX_LON = -0.85;
static void dest(double km, double brg, double& lat, double& lon) {
    const double r = M_PI / 180, d = km / 6371.0088, la1 = RX_LAT * r, lo1 = RX_LON * r, b = brg * r;
    const double la2 = std::asin(std::sin(la1) * std::cos(d) + std::cos(la1) * std::sin(d) * std::cos(b));
    const double lo2 = lo1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(la1), std::cos(d) - std::sin(la1) * std::sin(la2));
    lat = la2 / r; lon = lo2 / r;
}
static bool offerAt(RangeOutline& o, double km, double brg, int64_t utc) { double la, lo; dest(km, brg, la, lo); return o.offer(la, lo, utc); }
static double kmAt(const RangeOutline& o, int days, int64_t utc, int bin) {
    std::vector<uint16_t> km; std::vector<int64_t> day; o.windowed(days, utc, km, day); return km[size_t(bin)] / 10.0;
}
static const int64_t DAY = 86400;
static const int64_t T0 = 1762128000;   // 2025-11-03 00:00 UTC

int main() {
    // ── geometry ──
    CHECK(std::fabs(RangeOutline::bearingDeg(52, 0, 53, 0) - 0) < 1e-6, "due north");
    CHECK(std::fabs(RangeOutline::bearingDeg(0, 0, 0, 1) - 90) < 1e-6, "due east");
    CHECK(std::fabs(RangeOutline::distKm(0, 0, 1, 0) - 111.2) < 0.2, "a degree of latitude ≈ 111 km");

    // ── bins and maxima ──
    {
        RangeOutline o;
        CHECK(!offerAt(o, 100, 45, T0), "no centre yet: nothing counts");
        o.setCentre(RX_LAT, RX_LON, T0);
        CHECK(offerAt(o, 100, 45.5, T0 + 10), "a first catch grows the outline");
        CHECK(std::fabs(kmAt(o, 30, T0 + 20, 45) - 100) < 0.2, "…in bin 45 (45.5°): %.1f", kmAt(o, 30, T0 + 20, 45));
        CHECK(kmAt(o, 30, T0 + 20, 44) == 0 && kmAt(o, 30, T0 + 20, 46) == 0, "…and no other bin");
        CHECK(!offerAt(o, 80, 45.5, T0 + 30), "a nearer catch on the same bearing changes nothing");
        CHECK(offerAt(o, 150, 45.2, T0 + 40) && std::fabs(kmAt(o, 30, T0 + 50, 45) - 150) < 0.2, "a further one raises it");
        CHECK(offerAt(o, 200, 359.7, T0 + 50) && std::fabs(kmAt(o, 30, T0 + 60, 359) - 200) < 0.2, "359.7° lands in bin 359");
        CHECK(!offerAt(o, 651, 10, T0 + 60), "past 650 km is a bad fix, not range");
        CHECK(offerAt(o, 649, 10, T0 + 70), "649 km counts");
        CHECK(!o.offer(RX_LAT, RX_LON, T0 + 80), "the receiver's own position is no range");
    }

    // ── THE ROLLING WINDOW: a big day expires; the all-time outline and the record keep it ──
    {
        RangeOutline o;
        o.setCentre(RX_LAT, RX_LON, T0);
        for (int d = 0; d < 40; ++d) offerAt(o, 200, 90.5, T0 + d * DAY + 3600);   // an ordinary 200 km every day
        offerAt(o, 512, 90.5, T0 + 5 * DAY + 7200);                                 // ★ the tropo day: day 5
        const int64_t day12 = T0 + 12 * DAY + 100, day36 = T0 + 36 * DAY + 100, day39 = T0 + 39 * DAY + 100;
        CHECK(std::fabs(kmAt(o, 30, day12, 90) - 512) < 0.2, "day 12, 30-day window: the tropo day is in (%.1f)", kmAt(o, 30, day12, 90));
        CHECK(std::fabs(kmAt(o, 7, day12, 90) - 200) < 0.2, "day 12, 7-day window: it has already gone (%.1f)", kmAt(o, 7, day12, 90));
        CHECK(std::fabs(kmAt(o, 30, day36, 90) - 200) < 0.2, "day 36, 30-day window: the tropo day has left it (%.1f)", kmAt(o, 30, day36, 90));
        CHECK(std::fabs(kmAt(o, 90, day39, 90) - 512) < 0.2, "day 39, 90-day window: still in");
        CHECK(std::fabs(kmAt(o, 0, day39, 90) - 512) < 0.2, "all time: in");
        std::vector<uint16_t> km; std::vector<int64_t> day;
        o.windowed(30, day36, km, day);
        CHECK(day[90] >= RangeOutline::dayOf(day36) - 29, "the window's maximum is dated inside the window");
        const std::string j = o.json(day36);
        CHECK(j.find("\"record\":{\"km\":512,\"brg\":90,") != std::string::npos, "the furthest-ever record survives the window: %s", j.substr(j.find("\"record\"")).c_str());
        CHECK(j.find("\"far\":{\"km\":200,\"brg\":90,") != std::string::npos, "the window's furthest is the ordinary day");
        CHECK(j.find("\"window\":30") != std::string::npos, "the default window is 30 days");
        // the window switch
        CHECK(o.setWindow(7) && o.json(day12).find("\"far\":{\"km\":200,") != std::string::npos, "window 7: the outline follows it");
        CHECK(o.setWindow(0) && o.json(day36).find("\"far\":{\"km\":512,") != std::string::npos, "window all-time: the tropo day is the outline");
        CHECK(!o.setWindow(14) && o.window() == 0, "a window not offered is refused");
        CHECK(o.setWindow(90) && std::fabs(kmAt(o, o.window(), T0 + 96 * DAY, 90) - 200) < 0.2, "window 90, day 96: the tropo day (day 5) is out");
        // a slot reused 90 days later is wiped first
        offerAt(o, 120, 180.5, T0 + 95 * DAY);   // day 95 → slot of day 5
        CHECK(std::fabs(kmAt(o, 90, T0 + 95 * DAY + 10, 90) - 200) < 0.2, "day 95 reuses day 5's slot — the old maxima are gone with it");
    }

    // ── bounded storage, the file, damage ──
    {
        RangeOutline o;
        o.setCentre(RX_LAT, RX_LON, T0);
        for (int d = 0; d < 400; ++d) for (int b = 0; b < 360; b += 7) offerAt(o, 50 + (d * 13 + b) % 590, b + 0.5, T0 + d * DAY + b);
        const std::string s = o.serialize();
        CHECK(s.size() < 80000, "400 busy days still serialize to %zu bytes (90 days kept)", s.size());
        CHECK(s.size() == RangeOutline().serialize().size(), "the file is the same size whatever the sky");
        RangeOutline p;
        CHECK(p.parse(s), "the file reads back");
        CHECK(p.json(T0 + 400 * DAY) == o.json(T0 + 400 * DAY), "…identically");
        std::string bad = s; bad[0] ^= 0x55;
        CHECK(!p.parse(bad), "a wrong magic is refused");
        CHECK(!p.parse(s.substr(0, s.size() - 3)), "a short file is refused");
        CHECK(!p.parse(s + "x"), "a long file is refused");
        const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vs-adsb-range-" + std::to_string(getpid());
        const std::string f = dir + ".bin";
        CHECK(o.save(f), "saved");
        RangeOutline q;
        CHECK(q.load(f) && q.json(T0 + 400 * DAY) == o.json(T0 + 400 * DAY), "loaded from disk identically");
        std::remove(f.c_str());
        CHECK(!q.load(f), "a missing file is just nothing");
    }

    // ── the receiver moving, the owner's reset ──
    {
        RangeOutline o;
        o.setCentre(RX_LAT, RX_LON, T0);
        offerAt(o, 300, 200.5, T0 + 100);
        CHECK(!o.setCentre(RX_LAT + 0.02, RX_LON, T0 + 200) && !o.empty(), "moved 2 km: the same site, the outline stays");
        CHECK(o.setCentre(RX_LAT + 0.2, RX_LON, T0 + 300) && o.empty(), "moved 22 km: a new site, a new outline");
        CHECK(o.json(T0 + 400).find("\"since\":1762128300") != std::string::npos, "…which starts then");
        o.setWindow(7);
        o.setCentre(RX_LAT, RX_LON, T0 + 500);
        offerAt(o, 300, 200.5, T0 + 600);
        o.clear(T0 + 700);
        CHECK(o.empty() && o.json(T0 + 800).find("\"record\":null") != std::string::npos, "reset: empty, the record too");
        CHECK(o.window() == 7 && o.hasCentre(), "…the owner's window and the site kept");
        CHECK(offerAt(o, 100, 10.5, T0 + 900), "and it starts again from the next aircraft");
    }

    // ── the store: per-source files, survive a restart ──
    {
        const std::string pre = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vs-adsb-store-" + std::to_string(getpid());
        {
            RangeStore st;
            st.setFiles(pre);
            st.setCentre(RX_LAT, RX_LON, T0);
            double la, lo; dest(321, 152.5, la, lo);
            CHECK(st.offer(kRangeAdsb, la, lo, T0 + 5), "the store takes an ADS-B catch");
            CHECK(!st.offer(7, la, lo, T0 + 5), "an unknown source is refused");
            CHECK(st.setWindow(-1, 90, T0 + 6) && !st.setWindow(-1, 3, T0 + 6), "the window: 90 yes, 3 no");
            st.flush(T0 + 7);
        }
        RangeStore again;
        again.setFiles(pre);
        const std::string j = again.json(kRangeAdsb, T0 + 10);
        CHECK(j.find("\"km\":[") != std::string::npos && j.find("\"far\":{\"km\":321,\"brg\":152,") != std::string::npos, "after a restart the outline is back: %s", j.substr(j.find("\"far\"")).c_str());
        CHECK(j.find("\"window\":90") != std::string::npos, "…with the owner's window");
        CHECK(again.json(-1, T0 + 10).find("{\"ok\":1,\"outlines\":{\"adsb\":{") == 0, "every source at once");
        again.clear(kRangeAdsb, T0 + 20);
        RangeStore third; third.setFiles(pre);
        CHECK(third.json(kRangeAdsb, T0 + 30).find("\"far\":null") != std::string::npos, "the reset is saved at once");
        std::remove((pre + "-adsb.bin").c_str());
    }

    // ── ≥ 2 consistent positions (vibe_adsb.h setPos → posOk), through the Tracker's own rule ──
    {
        Aircraft a;
        CHECK(a.posOk == 0, "a new aircraft has proved nothing");
        Tracker::applyFix(a, 52.5, -0.5, 100);
        CHECK(a.posOk == 0, "one fix: still nothing — the outline would not count it");
        Tracker::applyFix(a, 52.51, -0.5, 102);        // 0.6 NM in 2 s ≈ 1080 kt: flight
        CHECK(a.posOk == 1, "a second, within flying distance: it counts (posOk %u)", a.posOk);
        Tracker::applyFix(a, 54.5, -0.5, 103);          // 120 NM in 1 s: a bad decode
        CHECK(a.posOk == 0, "a jump that is not flight starts the count again");
        Tracker::applyFix(a, 54.505, -0.5, 104);
        CHECK(a.posOk == 1, "…and two consistent fixes after it count again");
    }

    printf("test-adsb-range: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
