// test-keep-run — THE LANDING MAP AND ITS KEEP-RUNNING MODES (vibe_keep_run.h, beta96; docs/v12/KEEP-RUNNING.md).
//
// §1 the table and the CSV: known ids in table order, the combination's own word, the entry each choice opens through.
// §2 MIGRATION: beta95's landingAdsb / adsbKeepRunning become the map + "adsb" + keep unchanged; the old two are mirrored back
//    for an older build; a bad set keeps its first mode; idempotent.
// §3 THE RATE GATE at 2.048 / 2.4 / 2.56 / 8 MS/s — one mode at a time at an ordinary rate (the rest greyed WITH the reason),
//    ACARS + VDL2 together only where the capture holds the wide band; a mode the radio cannot do is not drawn; a blocked one
//    is greyed with the server's words.
// §4 MULTI-RADIO: two radios of one server, each gated by ITS rate (the Pi 500 shape: V4L on ADS-B, V4L on VDL2, RSP1B on AIS).
// §5 THE SHARED 137 MHz CAPTURE: kept VDL2 + ORBCOMM + a Meteor (137.9 / 137.1) or NOAA pass fit one 2.4 / 2.56 MS/s capture
//    with the DC clear of VDL2 and the pass; an HF+'s 912 kS/s does not; the IF half-width covers 136.69–138.0.
// §6 the cost words from the benchmark (set the default, never hide).
// Silent. Run: VIBE_TESTS="test-keep-run" scripts/run-tests.sh
#include "vibe_band_slice.h"
#include "vibe_keep_run.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibekeep;

static const Offer* find(const std::vector<Offer>& v, const char* id) { for (auto& o : v) if (o.id == id) return &o; return nullptr; }

/** An RTL-type radio: everything reachable; the combination fits only at `rate` ≥ its minimum (or a boost). */
static std::vector<Avail> rtlAvail() { return { {"adsb"}, {"vdl2"}, {"acars"}, {"vor"}, {"ais"} }; }

int main() {
    std::printf("── §1 table, CSV, entries\n");
    {
        CHECK(modes().size() == 5, "five modes");
        CHECK(toCsv(parseCsv(" VDL2 , acars,bogus,vdl2")) == "vdl2,acars", "table order, known, no repeats: %s", toCsv(parseCsv(" VDL2 , acars,bogus,vdl2")).c_str());
        CHECK(toCsv(parseCsv("acars+vdl2")) == "vdl2,acars", "the combination's word reads as both");
        // ★ table order is adsb, vdl2, acars, vor, ais — so isCombo wants {vdl2, acars}? No: acars comes AFTER vdl2 in the table.
        const std::vector<std::string> both = parseCsv("acars,vdl2");
        Entry e;
        CHECK(entryFor({"vdl2"}, e) && e.kind == Avdata && e.band == "vdl2" && e.msg == "{\"type\":\"avdata\",\"on\":1,\"band\":\"vdl2\"}", "VDL2 → the datalink's vdl2 band: %s", e.msg.c_str());
        CHECK(entryFor({"adsb"}, e) && e.kind == Adsb && e.msg == "{\"type\":\"adsb\",\"on\":1}", "ADS-B → its own key");
        CHECK(entryFor({"ais"}, e) && e.kind == Ais && e.msg == "{\"type\":\"ais\",\"on\":1}", "AIS → its own key");
        CHECK(entryFor({"vor"}, e) && e.kind == Vormap && e.msg == "{\"type\":\"vormap\",\"on\":1}", "VOR → the VOR map");
        CHECK(entryFor({"acars"}, e) && e.band == "acars", "ACARS → the acars band");
        CHECK(entryFor(both, e) && e.kind == Avdata && e.band == "acars+vdl2" && e.bench == "dec_avboth", "ACARS + VDL2 → the wide band (%s)", toCsv(both).c_str());
        CHECK(!entryFor({"adsb", "vdl2"}, e), "ADS-B + VDL2: no single mode decodes both");
        CHECK(!entryFor({}, e), "nothing ticked: no entry");
        CHECK(!modeById("dsc"), "✗ DSC is not a keep-running mode (a VFO decoder — no dead control)");
        // ★ the centre and every channel come from the datalink's own table — nothing typed by the owner
        const vibeavi::Band* b = vibeavi::bandById("vdl2");
        CHECK(b && std::fabs(b->centreHz - 136.8625e6) < 1 && b->chans.size() == 12 && std::fabs(b->chans.front() - 136.700e6) < 1
              && std::fabs(b->chans.back() - 136.975e6) < 1, "VDL2 = 136.8625 MHz, 12 channels 136.700–136.975");
        CHECK(comboMinRateHz() > 6.0e6 && comboMinRateHz() < 6.2e6, "ACARS + VDL2 needs ~6.1 MS/s (%.3f)", comboMinRateHz() / 1e6);
    }

    std::printf("── §2 migration\n");
    {
        Stored s; s.landingAdsb = true; s.adsbKeepRunning = true;
        migrate(s);
        CHECK(s.landingMap == kMapAvMar && s.landingModes == "adsb" && s.mapKeep, "beta95 ADS-B + keep → map, adsb, keep");
        CHECK(s.landingAdsb && s.adsbKeepRunning, "and the old two still say so (an older build reads the same)");
        Stored t; t.landingAdsb = true; t.adsbKeepRunning = false;
        migrate(t);
        CHECK(t.landingMap == kMapAvMar && t.landingModes == "adsb" && !t.mapKeep && t.landingAdsb && !t.adsbKeepRunning, "landing only stays landing only");
        Stored n; migrate(n);
        CHECK(n.landingMap.empty() && n.landingModes.empty() && !n.mapKeep && !n.landingAdsb && !n.adsbKeepRunning, "★ never default on");
        Stored k; k.adsbKeepRunning = true; migrate(k);
        CHECK(!k.adsbKeepRunning && k.landingMap.empty(), "keep without the landing is nothing (beta95's rule)");
        Stored v; v.landingMap = "avmar"; v.landingModes = "vdl2"; v.mapKeep = true; v.landingAdsb = true; v.adsbKeepRunning = true;
        migrate(v);
        CHECK(v.landingModes == "vdl2" && v.mapKeep && !v.landingAdsb && !v.adsbKeepRunning, "VDL2 kept: the old ADS-B two are OFF (an older build idles the radio, it never lands in ADS-B)");
        Stored bad; bad.landingMap = "avmar"; bad.landingModes = "ais,adsb"; migrate(bad);
        CHECK(bad.landingModes == "adsb", "two modes no single mode decodes: the first (table order) stays: %s", bad.landingModes.c_str());
        Stored combo; combo.landingMap = "avmar"; combo.landingModes = "vdl2,acars"; combo.mapKeep = true; migrate(combo);
        CHECK(combo.landingModes == "vdl2,acars", "the combination is stored as itself");
        Stored empty; empty.landingMap = "avmar"; empty.landingModes = ""; empty.mapKeep = true; migrate(empty);
        CHECK(empty.landingMap.empty() && !empty.mapKeep, "a map with no mode is no landing");
        Stored odd; odd.landingMap = "shipping"; odd.landingModes = "ais"; migrate(odd);
        CHECK(odd.landingMap.empty(), "an unknown map name is no landing");
        Stored again = v; migrate(again);
        CHECK(again.landingModes == v.landingModes && again.mapKeep == v.mapKeep && again.landingAdsb == v.landingAdsb, "idempotent");
    }

    std::printf("── §3 the rate gate\n");
    {
        // 2.048 MS/s: ADS-B cannot run (the server's own words); VDL2 can; nothing ticked
        std::vector<Avail> a = rtlAvail();
        a[0].why = "ADS-B needs a capture of 2.4 MS/s";
        auto g = gate(a, false, "", 2.048e6, {});
        CHECK(!find(g, "adsb")->enabled && find(g, "adsb")->why.find("2.4 MS/s") != std::string::npos && find(g, "adsb")->drawn, "2.048: ADS-B greyed, the server's reason");
        CHECK(find(g, "vdl2")->enabled && find(g, "acars")->enabled && find(g, "ais")->enabled, "2.048: the rest may be ticked");
        // 2.4 MS/s, VDL2 ticked: everything else greyed with the rate and the reason; ACARS says what it would need
        g = gate(rtlAvail(), false, "", 2.4e6, {"vdl2"});
        CHECK(find(g, "vdl2")->on && find(g, "vdl2")->enabled, "VDL2 ticked");
        CHECK(!find(g, "adsb")->enabled && find(g, "adsb")->why == "2.4 MS/s: one mode at a time on this radio \xe2\x80\x94 VDL2 is ticked", "2.4: ADS-B greyed: %s", find(g, "adsb")->why.c_str());
        CHECK(!find(g, "acars")->enabled && find(g, "acars")->why.find("6.1 MS/s") != std::string::npos, "2.4: ACARS (the partner) says the combination needs 6.1 MS/s: %s", find(g, "acars")->why.c_str());
        // 2.56 MS/s — the same
        g = gate(rtlAvail(), false, "", 2.56e6, {"ais"});
        CHECK(!find(g, "vdl2")->enabled && find(g, "vdl2")->why.rfind("2.56 MS/s: one mode at a time", 0) == 0, "2.56: one at a time: %s", find(g, "vdl2")->why.c_str());
        // 8 MS/s — ACARS + VDL2 together
        g = gate(rtlAvail(), true, "", 8e6, {"vdl2"});
        CHECK(find(g, "acars")->enabled && !find(g, "adsb")->enabled, "8: VDL2 ticked leaves ACARS open, the rest greyed");
        g = gate(rtlAvail(), true, "", 8e6, {"acars", "vdl2"});
        CHECK(find(g, "acars")->on && find(g, "vdl2")->on, "8: both ticked");
        CHECK(!find(g, "ais")->enabled && find(g, "ais")->why == "ACARS + VDL2 already take this radio's capture", "8: the third greyed: %s", find(g, "ais")->why.c_str());
        // the combination asked for at 2.4 (a stale save): only the first stays
        g = gate(rtlAvail(), false, "", 2.4e6, {"acars", "vdl2"});
        CHECK(find(g, "vdl2")->on && !find(g, "acars")->on, "2.4: a stored pair keeps its first (VDL2)");
        CHECK(toCsv(normalise({"acars", "vdl2"}, rtlAvail(), false)) == "vdl2", "normalise at 2.4: vdl2");
        CHECK(toCsv(normalise({"acars", "vdl2"}, rtlAvail(), true)) == "vdl2,acars", "normalise at 8: both");
        CHECK(toCsv(normalise({"adsb"}, a, false)).empty(), "normalise: a mode that cannot run is not stored as running");
        // the server's own combination words win
        g = gate(rtlAvail(), false, "ACARS + VDL2 needs a capture of 6.1 MS/s or more \xe2\x80\x94 this receiver runs at 2.400 MS/s", 2.4e6, {"acars"});
        CHECK(find(g, "vdl2")->why.find("this receiver runs at 2.400") != std::string::npos, "the server's words for the partner");
        // an HF+: no ADS-B row at all (it can never reach 1090), the rest as ever
        std::vector<Avail> hf = rtlAvail(); hf[0].hw = false;
        g = gate(hf, false, "", 0.912e6, {});
        CHECK(!find(g, "adsb")->drawn && find(g, "vdl2")->drawn && find(g, "vdl2")->enabled, "HF+: ADS-B not drawn (AGENTS.md), VDL2 offered");
        // blocked by the owner
        std::vector<Avail> bl = rtlAvail(); bl[1].why = "the server owner has switched the aircraft datalink off on this receiver";
        g = gate(bl, false, "", 2.4e6, {"vdl2"});
        CHECK(!find(g, "vdl2")->on && !find(g, "vdl2")->enabled && find(g, "vdl2")->why.find("switched") != std::string::npos, "blocked: never on, greyed with why");
        CHECK(find(g, "adsb")->enabled, "…and the rest free again");
        CHECK(rateWords(2.048e6) == "2.048 MS/s" && rateWords(2.4e6) == "2.4 MS/s" && rateWords(2.56e6) == "2.56 MS/s" && rateWords(8e6) == "8 MS/s", "rate words");
    }

    std::printf("── §4 multi-radio\n");
    {
        // ★ the Pi 500 shape: each radio gated by ITS rate and its own reach, its own stored choice
        struct Radio { const char* name; double rate; bool comboOk; std::vector<Avail> av; const char* stored; };
        std::vector<Avail> rsp = rtlAvail();
        std::vector<Radio> rs = {
            { "V4L 00000001", 2.4e6, false, rtlAvail(), "adsb" },
            { "V4L 00000004", 2.56e6, false, rtlAvail(), "vdl2" },
            { "RSP1B", 8e6, true, rsp, "acars,vdl2" },
        };
        CHECK(toCsv(normalise(parseCsv(rs[0].stored), rs[0].av, rs[0].comboOk)) == "adsb", "radio 1 ADS-B");
        CHECK(toCsv(normalise(parseCsv(rs[1].stored), rs[1].av, rs[1].comboOk)) == "vdl2", "radio 2 VDL2");
        CHECK(toCsv(normalise(parseCsv(rs[2].stored), rs[2].av, rs[2].comboOk)) == "vdl2,acars", "radio 3 (8 MS/s) ACARS + VDL2");
        auto g1 = gate(rs[1].av, rs[1].comboOk, "", rs[1].rate, parseCsv(rs[1].stored));
        auto g2 = gate(rs[2].av, rs[2].comboOk, "", rs[2].rate, {"vdl2"});
        CHECK(!find(g1, "acars")->enabled && find(g2, "acars")->enabled, "the same choice greys ACARS on the 2.56 radio and not on the 8 MS/s one");
    }

    std::printf("── §5 the shared 137 MHz capture\n");
    {
        const vibe::satsched::Region vr = vdl2Region();
        CHECK(std::fabs(vr.lo - 136.6875e6) < 1 && std::fabs(vr.hi - 136.9875e6) < 1 && !vr.dcOk, "VDL2 region 136.6875–136.9875, no DC inside");
        auto meteor = [](double hz) { vibe::satsched::Region r; r.lo = hz - 75e3 - 6.2e3; r.hi = hz + 75e3 + 6.2e3; return r; };
        for (double rate : { 2.4e6, 2.56e6 }) {
            const double usable = rate * vibe::kBandFlatFrac;
            for (double mhz : { 137.9e6, 137.1e6 }) {
                const vibe::satsched::Region m = meteor(mhz);
                const Share s = shareFit(usable, true, &m);
                CHECK(s.ok, "%.2f MS/s: VDL2 + ORBCOMM + Meteor %.1f fit one capture (%s)", rate / 1e6, mhz / 1e6, s.why.c_str());
                const double half = usable / 2;
                CHECK(s.centreHz - half <= vr.lo && s.centreHz + half >= m.hi && s.centreHz + half >= vibe::satsched::kOrbHiHz, "…the capture holds all three");
                CHECK(!(s.centreHz > vr.lo - 10e3 && s.centreHz < vr.hi + 10e3), "…the DC clear of every VDL2 channel (%.4f)", s.centreHz / 1e6);
                CHECK(!(s.centreHz > m.lo - 10e3 && s.centreHz < m.hi + 10e3), "…and of the pass's signal");
                const double ih = ifHalfFor(s.centreHz, s.lo, s.hi);
                CHECK(s.centreHz - ih <= 136.69e6 && s.centreHz + ih >= std::max(m.hi, 137.81e6), "…the IF filter opens over 136.69–%.2f MHz (half %.0f kHz)", s.hi / 1e6, ih / 1e3);
            }
        }
        // between passes: VDL2 + ORBCOMM alone
        const Share b = shareFit(2.4e6 * vibe::kBandFlatFrac, true, nullptr);
        CHECK(b.ok && b.centreHz > 136.9875e6 + 10e3, "VDL2 + ORBCOMM between passes: centre %.4f MHz, DC above the VDL2 block", b.centreHz / 1e6);
        const Share alone = shareFit(2.4e6 * vibe::kBandFlatFrac, false, nullptr);
        CHECK(alone.ok, "VDL2 alone always fits");
        CHECK(ifHalfFor(136.8625e6, vr.lo, vr.hi) == 250000.0, "VDL2 alone: the IF floor (no narrower than 500 kHz)");
        // an HF+ at 912 kS/s: VDL2 + ORBCOMM do not fit — the pass pauses VDL2 instead (the shim's fallback)
        const vibe::satsched::Region m = meteor(137.9e6);
        const Share hf = shareFit(0.912e6 * 0.8, true, &m);
        CHECK(!hf.ok && !hf.why.empty(), "HF+: no shared capture (%s)", hf.why.c_str());
        // 2.048 MS/s still holds VDL2 + Meteor 137.9 (1.29 MHz) on its flat part
        const Share r2 = shareFit(2.048e6 * vibe::kBandFlatFrac, false, &m);
        CHECK(r2.ok, "2.048 MS/s: VDL2 + Meteor 137.9 fit (%s)", r2.why.c_str());
    }

    std::printf("── §6 cost words\n");
    {
        bool heavy = false;
        CHECK(costWords(-1).empty(), "not measured: nothing said");
        CHECK(costWords(9.4, &heavy) == "about 9 % of one core (setup benchmark)" && !heavy, "%s", costWords(9.4).c_str());
        CHECK(costWords(0.3) == "under 1 % of one core (setup benchmark)", "under 1 %%");
        costWords(72, &heavy);
        CHECK(heavy, "over half a core: the page warns (it never hides)");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
