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
#include "vibe_decoder_reach.h"
#include "vibe_ism_bands.h"

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
        CHECK(modes().size() == 7, "seven modes (six on Aviation & Maritime — AIS + DSC since beta96 — ISM on Amateur)");
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

    std::printf("── §5b the pass scheduler holds kept VDL2 (vibe_sat_pass_sched.h In.hold)\n");
    {
        using namespace vibe::satsched;
        auto meteor = [](double hz) { Region r; r.lo = hz - 81.2e3; r.hi = hz + 81.2e3; return r; };
        In in; in.meteorOn = true; in.orbcommOn = true; in.orbcommUp = true; in.passUp = true; in.passName = "METEOR-M2 4 LRPT";
        in.passRegion = meteor(137.9e6); in.usableHz = 2.4e6 * vibe::kBandFlatFrac;
        in.hold.push_back(vdl2Region()); in.holdName = "VDL2";
        Out o = decide(in);
        CHECK(o.runPass && o.runOrbcomm && o.both && o.placeCentre, "2.4 MS/s: pass + VDL2 + ORBCOMM together (%s)", o.state.c_str());
        const double half = in.usableHz / 2;
        CHECK(o.centreHz - half <= vdl2Region().lo && o.centreHz + half >= in.passRegion.hi, "…the placed centre holds VDL2 and the pass");
        CHECK(o.state.find("VDL2") != std::string::npos, "…the admin line names VDL2");
        // ORBCOMM switched off: the pass + VDL2, still placed
        In i2 = in; i2.orbcommOn = false;
        o = decide(i2);
        CHECK(o.runPass && !o.runOrbcomm && o.placeCentre, "no ORBCOMM: pass + VDL2 placed (%s)", o.state.c_str());
        // a narrow capture (1.4 MS/s usable ~1.1 MHz): pass + VDL2 fit (1.37 MHz?) — no: ORBCOMM gives way first, the pass runs
        In i3 = in; i3.usableHz = 1.2e6;
        o = decide(i3);
        CHECK(o.runPass && !o.runOrbcomm && !o.orbcommWhyNot.empty(), "narrow: the pass runs, ORBCOMM paused (%s)", o.state.c_str());
        // no hold: beta94 exactly
        In i4 = in; i4.hold.clear();
        o = decide(i4);
        CHECK(o.runPass && o.runOrbcomm && o.both && o.state.find("VDL2") == std::string::npos, "no hold: beta94's both");
    }

    std::printf("── §7 ISM under the AMATEUR map (Stuart 11-26: 868 ISM for MeshCore repeaters, all day)\n");
    {
        CHECK(toCsv(parseCsv("ism:868")) == "ism:868", "the band rides in the token");
        CHECK(parseCsv("ism").empty() && parseCsv("ism:").empty() && parseCsv("ism:86 8x!").empty(), "ISM without a (clean) band is nothing");
        CHECK(toCsv(parseCsv("ism:868,ism:433")) == "ism:868", "one band: the first");
        Entry e;
        CHECK(entryFor({"ism:868"}, e) && e.kind == Ism && e.band == "868" && e.map == kMapAmateur
              && e.msg == "{\"type\":\"ism\",\"on\":1,\"band\":\"868\"}", "ISM 868 → ISM's own key on its own band: %s", e.msg.c_str());
        CHECK(e.name == "ISM 868 MHz" && e.bench == "dec_ism" && e.kindWord() == "ism", "named, priced by dec_ism");
        Stored a; a.landingMap = "amateur"; a.landingModes = "ism:868"; a.mapKeep = true; migrate(a);
        CHECK(a.landingMap == "amateur" && a.landingModes == "ism:868" && a.mapKeep && !a.landingAdsb, "the amateur map keeps ISM 868");
        Stored b; b.landingMap = "avmar"; b.landingModes = "ism:868"; b.mapKeep = true; migrate(b);
        CHECK(b.landingMap.empty() && !b.mapKeep, "ISM is not an Aviation & Maritime mode");
        Stored c; c.landingMap = "amateur"; c.landingModes = "vdl2"; migrate(c);
        CHECK(c.landingMap.empty(), "VDL2 is not an Amateur mode");
        std::vector<Avail> av = { {"ism"} };
        auto g = gate(av, false, "", 2.4e6, {"ism:868"}, kMapAmateur);
        CHECK(g.size() == 1 && g[0].id == "ism" && g[0].on && g[0].tok == "ism:868", "the amateur map draws ISM only, ticked as ism:868");
        CHECK(gate(rtlAvail(), false, "", 2.4e6, {"vdl2"}).size() == 6, "the aviation & maritime map still lists its six (AIS + DSC undrawn where hw is false)");
        CHECK(toCsv(normalise({"ism:433"}, av, false, kMapAmateur)) == "ism:433", "normalise keeps the band");
        std::vector<Avail> blk = { {"ism", true, "the server owner has switched ISM off on this receiver"} };
        CHECK(!gate(blk, false, "", 2.4e6, {"ism:868"}, kMapAmateur)[0].on, "ISM blocked: not on, greyed");
    }

    std::printf("── §8 the SINGLE-MODE RADIO (Stuart 11-26)\n");
    {
        const auto& D = vibereach::listeningDemods(); const auto& W = vibereach::wholeRadioModes();
        CHECK(singleOk("") && singleOk("adsb") && singleOk("vdl2") && singleOk("ism:868") && singleOk("dab") && singleOk("ndb")
              && singleOk("sondes") && singleOk("wfm") && singleOk("sat") && !singleOk("ism") && !singleOk("dsc") && !singleOk("cwu"), "the tokens");
        CHECK(singleBlockOf("vdl2") == "avdata" && singleBlockOf("acars") == "avdata" && singleBlockOf("vor") == "vormap"
              && singleBlockOf("ism:868") == "ism" && singleBlockOf("adsb") == "adsb" && singleBlockOf("dab") == "dab", "the switch each stands on");
        CHECK(singleKeepable("vdl2") && singleKeepable("ism:868") && !singleKeepable("dab") && !singleKeepable("ndb") && !singleKeepable("wfm") && !singleKeepable("sat"),
              "kept running where keep-running applies");
        // ADS-B single: every demod, every other whole mode, satellites off; ADS-B never; options untouched
        bool all = true;
        for (const char* m : D) all = all && singleBlocks("adsb", m, D, W);
        for (const char* m : W) all = all && (std::string(m) == "adsb" ? !singleBlocks("adsb", m, D, W) : singleBlocks("adsb", m, D, W));
        CHECK(all && singleBlocks("adsb", "sat", D, W) && !singleBlocks("adsb", "commb", D, W) && !singleBlocks("adsb", "avdata+text", D, W),
              "ADS-B single: everything else off (SATELLITE too), ADS-B on, options untouched");
        CHECK(!singleBlocks("vdl2", "avdata", D, W) && singleBlocks("vdl2", "adsb", D, W), "VDL2 single stands on the datalink switch");
        CHECK(!singleBlocks("", "wfm", D, W) && !singleBlocks("", "adsb", D, W), "General: nothing added");
        // ★ the DAB-only B210 (56 MS/s locked range, per-listener DAB): with single "dab" every one of dabOnlyNow's others is off
        static const char* kOthers[] = { "wfm", "nfm", "am", "sam", "sau", "sal", "ecssu", "ecssl", "amst", "usb", "lsb", "cwu", "cwl",
                                         "drm", "freedv", "m17", "dvsig", "adsb", "ism", "ndb", "ais", "avdata", "sondes", "vormap" };
        bool dabOnly = !singleBlocks("dab", "dab", D, W);
        for (const char* m : kOthers) dabOnly = dabOnly && singleBlocks("dab", m, D, W);
        CHECK(dabOnly, "★ DAB single = dabOnlyNow's shape (the B210 locked range's per-listener DAB path)");
        // the only-mode rule reads it straight off: onlyModeLeft on the enforced list
        auto blk = [](const std::string& single) { return [single](const char* m) {
            return singleBlocks(single, m, vibereach::listeningDemods(), vibereach::wholeRadioModes()); }; };
        CHECK(vibereach::onlyModeLeft(blk("adsb")) == "adsb" && vibereach::onlyModeLeft(blk("vdl2")) == "avdata"
              && vibereach::onlyModeLeft(blk("ism:868")) == "ism" && vibereach::onlyModeLeft(blk("")) == "", "★ onlyModeLeft returns the single mode — independent of the owner's ticks");
        CHECK(vibereach::onlyModeLeft(blk("wfm")) == "", "WFM only: a demodulator, no whole-mode exit lock (the others are simply off)");
        // satellites: the demodulators stay (the passes tune), every whole mode off, sat on
        CHECK(!singleBlocks("sat", "nfm", D, W) && !singleBlocks("sat", "sat", D, W) && singleBlocks("sat", "adsb", D, W) && singleBlocks("sat", "dab", D, W),
              "SATELLITES single: whole modes off, the passes' demodulators and SATELLITE on (listeners held by the server's gate)");
    }

    std::printf("── §9 beta96 THE MARITIME COMBINATION (AIS + DSC channel 70 — one choice)\n");
    {
        Entry e;
        CHECK(entryFor({"aisdsc"}, e) && e.kind == Ais && e.band == "ais+dsc" && e.msg == "{\"type\":\"ais\",\"on\":1,\"band\":\"ais+dsc\"}"
              && e.bench == "dec_aisdsc" && e.name == "AIS + DSC", "AIS + DSC → AIS's key with the combination's band: %s", e.msg.c_str());
        CHECK(entryFor({"ais"}, e) && e.msg == "{\"type\":\"ais\",\"on\":1}", "AIS alone unchanged");
        CHECK(toCsv(parseCsv("aisdsc")) == "aisdsc" && toCsv(parseCsv("ais,aisdsc")) == "ais,aisdsc", "the CSV reads it");
        CHECK(!entryFor({"ais", "aisdsc"}, e), "AIS ticked AND AIS + DSC: not one mode (the gate never allows it)");
        Stored s; s.landingMap = kMapAvMar; s.landingModes = "ais,aisdsc"; migrate(s);
        CHECK(s.landingModes == "ais", "a bad pair keeps its first (%s)", s.landingModes.c_str());
        // the gate: an RTL (the server says hw false — not drawn at all); an RSP at 8 (drawn, one at a time with AIS)
        std::vector<Avail> rtl = rtlAvail(); rtl.push_back({ "aisdsc", false });
        auto g = gate(rtl, false, "", 2.4e6, {});
        CHECK(find(g, "aisdsc") && !find(g, "aisdsc")->drawn, "RTL 2.4: AIS + DSC not drawn (no dead row)");
        CHECK(find(g, "ais")->drawn && find(g, "ais")->enabled, "…AIS as before");
        std::vector<Avail> rsp = rtlAvail(); rsp.push_back({ "aisdsc" });
        g = gate(rsp, true, "", 8e6, {});
        CHECK(find(g, "aisdsc")->drawn && find(g, "aisdsc")->enabled && find(g, "aisdsc")->group == "maritime", "RSP 8: drawn under MARITIME");
        g = gate(rsp, true, "", 8e6, {"aisdsc"});
        CHECK(find(g, "aisdsc")->on && !find(g, "ais")->enabled && find(g, "ais")->why.find("AIS + DSC is ticked") != std::string::npos,
              "ticked: AIS greyed — %s", find(g, "ais")->why.c_str());
        CHECK(normalise({"aisdsc"}, rsp, true) == std::vector<std::string>{"aisdsc"} && normalise({"aisdsc"}, rtl, false).empty(),
              "saved where it runs; dropped where the server says it cannot");
        rsp.back().why = "the server owner has switched DSC off on this receiver";
        g = gate(rsp, true, "", 8e6, {});
        CHECK(find(g, "aisdsc")->drawn && !find(g, "aisdsc")->enabled && find(g, "aisdsc")->why.find("DSC off") != std::string::npos, "DSC switched off: greyed with the reason");
        // single mode
        CHECK(singleOk("aisdsc") && singleBlockOf("aisdsc") == "ais" && singleKeepable("aisdsc") && singleMap("aisdsc") == kMapAvMar,
              "single mode AIS + DSC: a token, stands on the AIS switch, kept running on Aviation & Maritime");
        const std::vector<const char*> D = { "wfm", "nfm", "am", "usb", "lsb", "drm" }, W = { "dab", "adsb", "ism", "ndb", "ais", "avdata", "sondes", "vormap" };
        CHECK(!singleBlocks("aisdsc", "ais", D, W) && singleBlocks("aisdsc", "adsb", D, W) && singleBlocks("aisdsc", "nfm", D, W),
              "…AIS stays on, every other mode off");
    }


    std::printf("── §10 b96-bgsplit THE TWO CHOICES — background decoding / where new listeners land (Stuart 11-26)\n");
    {
        // every old shape → the two groups, identical behaviour
        { Stored s; s.landingAdsb = true; s.adsbKeepRunning = true; applyRead(s, true, false);
          CHECK(s.bgMode == "adsb" && s.landingModes == "adsb" && s.landingMap == kMapAvMar, "beta95 ADS-B + keep → background ADS-B + land on the map in ADS-B"); }
        { Stored s; s.landingAdsb = true; applyRead(s, true, false);
          CHECK(s.bgMode.empty() && s.landingModes == "adsb", "beta95 ADS-B landing only → background manual + land in ADS-B"); }
        { Stored s; applyRead(s, false, false);
          CHECK(s.bgMode.empty() && s.landingMap.empty() && !s.mapKeep, "nothing set → background manual + the plain landing (never default on)"); }
        { Stored s; s.landingMap = kMapAvMar; s.landingModes = "vdl2"; s.mapKeep = true; applyRead(s, false, true);
          CHECK(s.bgMode == "vdl2" && s.landingModes == "vdl2" && s.mapKeep, "b96-keeprun map + VDL2 + keep → background VDL2 + land on the map in VDL2 (mapKeep mirrors)"); }
        { Stored s; s.landingMap = kMapAvMar; s.landingModes = "vdl2"; s.mapKeep = false; applyRead(s, false, true);
          CHECK(s.bgMode.empty() && s.landingModes == "vdl2" && !s.mapKeep, "b96-keeprun map + VDL2, no keep → background manual + land in VDL2"); }
        { Stored s; s.landingMap = kMapAmateur; s.landingModes = "ism:868"; s.mapKeep = true; applyRead(s, false, true);
          CHECK(s.bgMode == "ism:868" && s.landingModes == "ism:868", "b96-keeprun Amateur ISM 868 kept → background ISM 868 + land on the Amateur map"); }
        { Stored s; s.landingMap = kMapAvMar; s.landingModes = "vdl2,acars"; s.mapKeep = true; applyRead(s, false, true);
          CHECK(s.bgMode == "vdl2,acars", "the combination kept → background the combination: %s", s.bgMode.c_str()); }
        { Stored s; s.landingMap = kMapAvMar; s.landingModes = "aisdsc"; s.mapKeep = true; applyRead(s, false, true);
          CHECK(s.bgMode == "aisdsc" && s.mapKeep, "AIS + DSC kept → background AIS + DSC"); }
        // the split itself (Listen-Amateur's case)
        { Stored s; s.bgMode = "ais"; applyRead(s, false, true, true);
          CHECK(s.bgMode == "ais" && s.landingMap.empty() && !s.mapKeep && !s.adsbKeepRunning && !s.landingAdsb,
                "background AIS + the plain landing (FM): no map landing, the mirrors OFF (an older build idles, never lands in AIS)"); }
        { Stored s; s.bgMode = "ais"; s.landingMap = kMapAvMar; s.landingModes = "adsb"; applyRead(s, false, true, true);
          CHECK(s.bgMode == "ais" && s.landingAdsb && !s.adsbKeepRunning && !s.mapKeep, "background AIS + land on the map in ADS-B: ADS-B landing mirrored, no keep mirror"); }
        { Stored s; s.bgMode = "adsb"; applyRead(s, false, true, true);
          CHECK(s.bgMode == "adsb" && !s.landingAdsb && !s.adsbKeepRunning, "background ADS-B + the plain landing: the beta95 pair stays off (keep needed the landing)"); }
        { Stored s; s.bgMode = "ism:915"; applyRead(s, false, false, true);
          CHECK(s.bgMode == "ism:915", "background ISM on any band of the plan (915)"); }
        { Stored s; s.bgMode = "ais,adsb"; applyRead(s, false, false, true);
          CHECK(s.bgMode == "adsb", "two modes no single mode decodes: the first in table order: %s", s.bgMode.c_str()); }
        { Stored s; s.bgMode = "bogus"; applyRead(s, false, false, true); CHECK(s.bgMode.empty(), "an unknown word → manual"); }
        // an older writer over a split config
        { Stored s; s.bgMode = "ais"; s.bgSet = true; s.landingAdsb = false; s.adsbKeepRunning = false; applyRead(s, true, false);
          CHECK(s.bgMode == "ais", "a beta95 page's 'ADS-B off' never clears background AIS it cannot know about"); }
        { Stored s; s.bgMode = "adsb"; s.bgSet = true; s.landingMap = kMapAvMar; s.landingModes = "adsb"; s.landingAdsb = false; s.adsbKeepRunning = false;
          applyRead(s, true, false);
          CHECK(s.bgMode.empty() && s.landingMap.empty(), "a beta95 page switching its ADS-B landing + keep off clears the ADS-B background it implied"); }
        { Stored s; s.bgMode = "ais"; s.bgSet = true; applyRead(s, false, false);
          CHECK(s.bgMode == "ais", "a read with no landing words (a partial update) leaves the background as it is"); }
        // idempotent through the writer's mirrors
        { Stored s; s.bgMode = "vdl2"; s.landingMap = kMapAvMar; s.landingModes = "vdl2"; applyRead(s, false, true, true);
          Stored again = s; again.bgSet = false; applyRead(again, false, true);   // ★ read back WITHOUT bgMode (an older beta96 reader)
          CHECK(again.bgMode == "vdl2" && again.mapKeep, "the mirror carries 'background VDL2 + landing VDL2' to a b96-keeprun reader"); }
        // the entry the background runs as
        { Stored s; s.bgMode = "aisdsc"; s.bgSet = true; migrate(s); Entry e; CHECK(bgEntry(s, e) && e.kind == Ais && e.band == "ais+dsc", "background AIS + DSC opens through AIS's door with the combination's band"); }
        { Stored s; migrate(s); Entry e; CHECK(!bgEntry(s, e), "manual: no entry"); }
        // the gate over every map ("" = the background card)
        { const auto o = gate({ {"adsb"}, {"vdl2"}, {"acars"}, {"vor"}, {"ais"}, {"ism"} }, false, "", 2.4e6, {"ais"}, "");
          CHECK(find(o, "ism") && find(o, "ism")->drawn && find(o, "adsb") && !find(o, "adsb")->enabled && find(o, "ais")->on,
                "the background card draws every map's modes, one at a time at 2.4 MS/s"); }
    }

    std::printf("── §11 ISM IS WORLDWIDE (Stuart 11-26): the region's own bands first, from the plan\n");
    {
        using namespace vibeism;
        auto has = [](Region r, const char* id) { for (const char* x : regionBands(r)) if (std::string(x) == id) return true; return false; };
        const Region us = regionAt(40.7, -74.0), uk = regionAt(51.5, -0.1);
        CHECK(us == Region::NorthAmerica && has(us, "915") && has(us, "315"), "a US server: 915 and 315 are its own");
        CHECK(uk == Region::Other && has(uk, "868") && has(uk, "433"), "a UK server: 868 and 433 are its own");
        CHECK(!has(uk, "907") && !has(us, "868"), "and not each other's mesh bands");
        for (Region r : { Region::Other, Region::NorthAmerica, Region::SouthAmerica, Region::Oceania, Region::Asia, Region::India, Region::China })
            for (const char* id : regionBands(r)) CHECK(bandById(id) != nullptr, "region list names a band of the plan: %s", id);
        const Band* l = landingBand(40.7, -74.0, [](const Band&) { return true; });
        CHECK(l && std::string(l->id) == "907", "the ISM mode's landing is unchanged (US → 907)");
        CHECK(tokenOk("ism:315") && tokenOk("ism:927") && tokenOk("ism:479"), "any band id is a background / landing token");
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
