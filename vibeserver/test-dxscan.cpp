// test-dxscan.cpp — THE IDLE DX SCANNER's log, rules and schedule (vibe_dxscan.h), brief §13. V12, 2026-11-04.
//
// Pure: made-up detections and a made-up clock. The FM engine (several RDS stations from one wide capture) is
// test-dxscan-fm.cpp; the server end to end (idle → scan → a listener arrives → the radio back, the hop not logged; an
// old client) is scripts/test-dx-scan.mjs.
#include <cstdio>
#include <set>
#include <string>
#include "vibe_dxscan.h"

using namespace vibe::dx;
static int pass = 0, fail = 0;
static void ok(bool c, const std::string& what) { c ? ++pass : ++fail; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); }

static FmHit fm(int khz, uint16_t pi, const char* ps, int groups, bool psc = false, float lvl = -40, float snr = 30) {
    FmHit h; h.khz = khz; h.pi = pi; h.ps = ps; h.groups = groups; h.psComplete = psc; h.levelDb = lvl; h.snrDb = snr; return h;
}
static const int64_t T0 = 1'800'000'000;   // a fixed UTC

int main() {
    std::printf("── config ──\n");
    {
        Config c; std::string e;
        ok(parseConfig("", c, e) && !c.on && c.fm && c.dab && c.waitMin == 5 && c.rasterKhz == 100 && !c.viewAdmin && !c.proof,
           "absent = the defaults: OFF, both bands, 5 min, 100 kHz, open to view, no proof clips");
        ok(parseConfig("{\"on\":true,\"bands\":\"dab\",\"waitMin\":2,\"raster\":50,\"viewAdmin\":true}", c, e) && c.on && !c.fm && c.dab
           && c.waitMin == 2 && c.rasterKhz == 50 && c.viewAdmin, "every option parsed");
        Config back; parseConfig(toJson(c), back, e);
        ok(toJson(back) == toJson(c), "round trip byte for byte: " + toJson(c));
        ok(!parseConfig("[1]", c, e) && !e.empty(), "a blob that is not an object is refused in words");
    }

    std::printf("── the FM hops ──\n");
    {
        for (double rate : { 2.4e6, 2.048e6, 3.2e6, 10e6, 0.768e6 })
            for (int r : { 100, 50 }) {
                const auto p = planFm(rate, r);
                std::set<int> seen; bool once = true, inWin = true, offDc = true;
                for (const auto& h : p) {
                    for (double f = h.loHz; f <= h.hiHz + 1; f += r * 1000.0) {
                        const int k = (int)std::lround(f / 1e3);
                        if (!seen.insert(k).second) once = false;
                        if (std::fabs(f - h.centreHz) > fmHalfUse(rate) + 1) inWin = false;
                        if (std::fabs(f - h.centreHz) < r * 500.0 - 1) offDc = false;
                    }
                }
                const int want = (int)std::lround((108.0 - 87.5) * 1000 / r) + 1;
                char b[160]; std::snprintf(b, sizeof b, "%.3f MS/s, %d kHz raster: %zu hops cover all %d channels once each, inside the window, none on DC",
                                           rate / 1e6, r, p.size(), want);
                ok((int)seen.size() == want && once && inWin && offDc, b);
            }
        ok(planFm(2.4e6, 100).size() <= 13, "an RTL at 2.4 MS/s sweeps the band in about ten hops (" + std::to_string(planFm(2.4e6, 100).size()) + ")");
        ok(planFm(0.25e6, 100).empty(), "a capture too narrow for one channel plans nothing");
    }

    std::printf("── confirmation (brief §6) ──\n");
    {
        ok(!fmConfirmed(1, false), "★ a single CRC-valid RDS group is never enough");
        ok(!fmConfirmed(2, false), "two groups are not enough either");
        ok(fmConfirmed(3, false), "the PI in 3 CRC-valid groups confirms");
        ok(fmConfirmed(1, true), "or the PS fully assembled");
        Log log(T0);
        log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "BBC R4", 1) }, T0 + 10);
        ok(log.size() == 0, "★ a visit hearing one group on a PI creates no entry");
        log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "", 3) }, T0 + 20);
        ok(log.size() == 1, "three groups do");
        ok(!dabConfirmed(0x0000, 4) && !dabConfirmed(0xC1C0, 0) && dabConfirmed(0xC1C0, 1), "DAB: the first CRC-valid FIB carrying an EId");
    }

    std::printf("── scheduler and cursor ──\n");
    {
        Config c; c.on = true;
        const auto plan = planFm(2.4e6, 100);
        const int nDab = 38;
        Log log(T0);
        // ★ interrupted repeatedly over a "day": every second visit is cut short (never advanced); the rest complete
        std::set<int> fmDone, dabDone;
        int visits = 0;
        for (int k = 0; k < 400 && (fmDone.size() < plan.size() || dabDone.size() < (size_t)nDab); ++k) {
            const Job j = nextJob(log, c, plan, nDab);
            if (k % 2 == 1) continue;   // a listener arrived mid-hop: discarded, the cursor does not move
            (j.dab ? dabDone : fmDone).insert(j.index);
            advance(log, c, j, plan, nDab);
            ++visits;
        }
        ok(fmDone.size() == plan.size() && dabDone.size() == (size_t)nDab,
           "★ interrupted half the time, the cursor still covers every FM hop and every DAB block (" + std::to_string(visits) + " completed visits)");
        ok(log.sweeps(false) >= 1 && log.sweeps(true) >= 1, "a wrap completes a sweep of each band");
        Log l2(T0);
        Job a = nextJob(l2, c, plan, nDab); advance(l2, c, a, plan, nDab);
        Job b = nextJob(l2, c, plan, nDab);
        ok(!a.dab && !b.dab && b.index == a.index + 1, "the next idle period carries on from the NEXT hop, not the band's start");
        // FM only / DAB only
        Config fmOnly = c; fmOnly.dab = false; Config dabOnly = c; dabOnly.fm = false;
        bool allFm = true, allDab = true; Log l3(T0), l4(T0);
        for (int k = 0; k < 60; ++k) { Job j = nextJob(l3, fmOnly, plan, nDab); allFm &= !j.dab; advance(l3, fmOnly, j, plan, nDab);
                                       Job d = nextJob(l4, dabOnly, plan, nDab); allDab &= d.dab; advance(l4, dabOnly, d, plan, nDab); }
        ok(allFm && allDab, "bands FM only / DAB only are honoured");
        // a changed rate resumes from the same frequency
        Log l5(T0); for (int k = 0; k < 4; ++k) { Job j = nextJob(l5, fmOnly, plan, nDab); advance(l5, fmOnly, j, plan, nDab); }
        const int at = l5.fmCursorKhz;
        const auto wide = planFm(10e6, 100);
        const Job w = nextJob(l5, fmOnly, wide, nDab);
        ok(wide[(size_t)w.index].loHz / 1e3 <= at + 0.5 && wide[(size_t)w.index].hiHz / 1e3 >= at - 0.5,
           "the cursor is a frequency: a radio whose rate changed resumes at " + std::to_string(at) + " kHz");
    }

    std::printf("── bootstrap, locals, alerts (brief §7, §13) ──\n");
    {
        Log log(T0);
        int64_t t = T0;
        int alertsDuringBoot = 0;
        // 26 hours of sweeps every 10 minutes: two strong locals (one with a SCROLLING PS), heard every visit
        const char* scroll[] = { "RADIO", "TOP 40", "NOW:", "ARTIST", "SONG", "TITLE" };
        int k = 0;
        for (; t < T0 + 26 * 3600; t += 600, ++k) {
            const auto vr = log.commitFm(98000, 99800, 98950, 100,
                                         { fm(98500, 0xC201, "BBC R4", 8), fm(99100, 0xD3C1, scroll[k % 6], 9) }, t);
            if (t < T0 + kBootstrapSec) alertsDuringBoot += vr.alerts;
        }
        const Entry* r4 = log.findKey(fmKey(0xC201, 98500));
        const Entry* sc = log.findKey(fmKey(0xD3C1, 99100));
        ok(alertsDuringBoot == 0, "★ the bootstrap's first 24 h send no alerts");
        ok(r4 && r4->cls == Cls::Local, "★ a strong local is Local after the bootstrap");
        ok(sc && sc->cls == Cls::Local && sc->psVariants.size() >= 6,
           "★ a strong local with a SCROLLING PS is still Local (judged on PI, never PS; " + std::to_string(sc ? sc->psVariants.size() : 0) + " PS variants kept for display)");
        ok(log.size() == 2, "★ the locals appear exactly ONCE (two entries after 150 visits)");
        ok(log.alertSeq() == 0, "★ and the locals never alerted");
        // a week more of locals: still no alerts
        for (; t < T0 + 9 * 86400; t += 1800) log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "BBC R4", 8) }, t);
        ok(log.alertSeq() == 0 && log.findKey(fmKey(0xC201, 98500))->cls == Cls::Local, "a week later the local is still silent and Local");
        ok(log.findKey(fmKey(0xD3C1, 99100))->cls == Cls::Dx, "a Local unheard for over 7 days drops back (unclassified → DX)");

        std::printf("── openings ──\n");
        // ★ an Es station heard for 20 min: one entry, one opening; back 2 h later: a second opening, not a second entry
        const int64_t es = t;
        int alerts0 = (int)log.alertSeq();
        for (int64_t u = es; u <= es + 20 * 60; u += 120)
            log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "BBC R4", 8), fm(98800, 0x5201, "RAI R1", 5) }, u);
        const Entry* rai = log.findKey(fmKey(0x5201, 98800));
        ok(rai && rai->cls == Cls::Dx && rai->openings.size() == 1, "★ an Es station heard for 20 min: one DX entry, one opening");
        ok((int)log.alertSeq() == alerts0 + 1, "  and one alert (new DX)");
        for (int64_t u = es + 20 * 60 + 120; u < es + 140 * 60; u += 600) log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "BBC R4", 8) }, u);
        log.commitFm(98000, 99800, 98950, 100, { fm(98800, 0x5201, "RAI R1", 4) }, es + 140 * 60);
        rai = log.findKey(fmKey(0x5201, 98800));
        size_t n52 = 0; for (const auto& kv : log.entries()) if (kv.second.pi == 0x5201) ++n52;
        ok(rai && rai->openings.size() == 2 && n52 == 1, "★ back 2 h later: a SECOND OPENING, not a second entry");
        ok((int)log.alertSeq() == alerts0 + 2, "  and an alert for the new opening");
        log.commitFm(98000, 99800, 98950, 100, { fm(98800, 0x5201, "RAI R1", 4) }, es + 160 * 60);
        ok(log.findKey(fmKey(0x5201, 98800))->openings.size() == 2, "heard again 20 min later: the same opening (under the 30 min gap)");
        // the same PI on another frequency is a separate entry (networks reuse their PI)
        log.commitFm(98000, 99800, 98950, 100, { fm(99300, 0x5201, "RAI R1", 4) }, es + 161 * 60);
        ok(log.findKey(fmKey(0x5201, 99300)) && log.findKey(fmKey(0x5201, 99300))->id != rai->id, "the same PI on a different frequency is its own entry");

        std::printf("── image / intermod suspect (TRAP) ──\n");
        // DC of this hop at 98.950: the local 98.5 images to 99.4; a station there is SUSPECT
        const uint32_t s0 = log.alertSeq();
        log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "BBC R4", 8), fm(99400, 0xC201, "BBC R4", 3) }, es + 170 * 60);
        const Entry* img = log.findKey(fmKey(0xC201, 99400));
        ok(img && img->cls == Cls::Suspect && img->suspectWhy.find("image of 98.5") != std::string::npos,
           "★ a detection on a Local's image is Suspect (" + (img ? img->suspectWhy : std::string("none")) + ")");
        ok(log.alertSeq() == s0, "  and it does not alert");
        // two locals 98.5 and 98.7 (make 98.7 local by hand-free route: a fresh log through a bootstrap)
        Log l2(T0);
        for (int64_t u = T0; u < T0 + 25 * 3600; u += 900)
            l2.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "A", 8), fm(98700, 0xC202, "B", 8) }, u);
        l2.commitFm(98000, 99800, 98950, 100, { fm(98300, 0xE123, "X", 4) }, T0 + 25 * 3600 + 60);   // 2·98.5 − 98.7 = 98.3
        const Entry* imd = l2.findKey(fmKey(0xE123, 98300));
        ok(imd && imd->cls == Cls::Suspect && imd->suspectWhy.find("intermod") != std::string::npos, "★ a detection on 2·f1 − f2 of two Locals is Suspect");
        ok(l2.alertSeq() == 0, "  silent");
    }

    std::printf("── provisional during the bootstrap, then judged ──\n");
    {
        Log log(T0);
        log.commitFm(98000, 99800, 98950, 100, { fm(99900, 0x1234, "ES", 4) }, T0 + 3600);
        ok(log.findKey(fmKey(0x1234, 99900))->cls == Cls::Provisional, "★ a new server marks everything provisional");
        ok(log.alertSeq() == 0, "  and sends no alert");
        for (int64_t u = T0 + 3600 + 600; u < T0 + 25 * 3600; u += 600) log.commitFm(98000, 99800, 98950, 100, {}, u);
        log.evaluate(T0 + 25 * 3600);
        const Entry* e = log.findKey(fmKey(0x1234, 99900));
        ok(e && e->cls == Cls::Dx, "a provisional heard once in the bootstrap is DX at its end (" + std::string(e ? clsWord(e->cls) : "-") + ")");
        log.commitFm(98000, 99800, 98950, 100, { fm(99900, 0x1234, "ES", 4) }, T0 + 26 * 3600);
        ok(log.alertSeq() == 1, "  and its next opening alerts");
    }

    std::printf("── DAB entries ──\n");
    {
        Log log(T0 - kBootstrapSec - 10);
        DabHit h; h.block = 30; h.blockName = "12B"; h.eid = 0xC1CE; h.ecc = 0xE1; h.label = "Digital One"; h.fibOk = 3; h.snrDb = 12;
        h.services = { { 0xC7D8, "Classic FM" }, { 0xC0D8, "Absolute" } };
        log.commitDab(30, &h, T0);
        DabHit t1 = h; t1.tiiMain = 4; t1.tiiSub = 7;
        DabHit t2 = h; t2.tiiMain = 9; t2.tiiSub = 2;
        log.commitDab(30, &t1, T0 + 60); log.commitDab(30, &t2, T0 + 120);
        ok(log.size() == 3, "★ EId + block without TII, and EId + block + TII main/sub: two SFN sites are two entries");
        log.commitDab(30, &t1, T0 + 180);
        ok(log.size() == 3 && log.findKey(dabKey(0xC1CE, "12B", 4, 7))->timesHeard == 2, "the same site again: the same entry");
        const Entry* e = log.findKey(dabKey(0xC1CE, "12B", -1, -1));
        ok(e && e->services.size() == 2 && e->openings.size() == 1 && e->openings[0].services.size() == 2,
           "★ the opening records the services that decoded (§12 q3)");
        DabHit none; none.block = 31; none.blockName = "12C"; none.eid = 0; none.fibOk = 0;
        log.commitDab(31, &none, T0 + 200);
        ok(log.size() == 3, "a block with no EId logs nothing");
    }

    std::printf("── bounded, prune order, promoted kept ──\n");
    {
        Log log(T0 - kBootstrapSec - 10);   // past the bootstrap: new entries are DX
        log.commitFm(87500, 89000, 88250, 100, { fm(87600, 0x0001, "KEEP", 4) }, T0);
        const int keepId = log.findKey(fmKey(0x0001, 87600))->id;
        log.setPromoted(keepId, "bm:87600000");
        for (int i = 0; i < kMaxEntries + 200; ++i)
            log.commitFm(87500, 108000, 97750, 100, { fm(88000 + (i % 200) * 100, (uint16_t)(0x2000 + i), "X", 4) }, T0 + 10 + i);
        ok((int)log.size() <= kMaxEntries, "★ the log never exceeds " + std::to_string(kMaxEntries) + " entries (" + std::to_string(log.size()) + ")");
        ok(log.find(keepId) != nullptr, "★ a PROMOTED entry is never pruned, however old");
        const int n = log.clear();
        ok(log.size() == 1 && log.find(keepId) && n > 0, "clear keeps the promoted entry");
    }

    std::printf("── persistence ──\n");
    {
        Log log(T0);
        for (int64_t u = T0; u < T0 + 25 * 3600; u += 1200) log.commitFm(98000, 99800, 98950, 100, { fm(98500, 0xC201, "BBC R4", 8) }, u);
        DabHit h; h.block = 30; h.blockName = "12B"; h.eid = 0xC1CE; h.label = "Digital One"; h.fibOk = 3; h.services = { { 1, "Svc" } };
        log.commitDab(30, &h, T0 + 25 * 3600);
        log.fmCursorKhz = 99900; log.dabCursor = 12; log.nextDab = true;
        const std::string a = log.toFileJson();
        Log back;
        ok(back.fromFileJson(a), "the file reads back");
        ok(back.toFileJson() == a, "and writes out byte for byte the same (" + std::to_string(a.size()) + " bytes)");
        ok(back.fmCursorKhz == 99900 && back.dabCursor == 12 && back.nextDab, "the cursor survives a restart");
        ok(back.findKey(fmKey(0xC201, 98500))->cls == Cls::Local, "and the classes");
        ok(!back.fromFileJson("{not json"), "an unreadable file is refused (a fresh log starts)");
        Log::Query q; q.limit = 500;
        const std::string page = log.listJson(q, T0 + 26 * 3600);
        ok(page.find("\"limit\":100") != std::string::npos, "a page is at most 100 entries");
        ok(page.find("C201") == std::string::npos && page.find("12B") != std::string::npos, "★ the default view hides Locals");
        q.showLocal = true;
        ok(log.listJson(q, T0 + 26 * 3600).find("C201") != std::string::npos, "the toggle shows them");
    }

    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
