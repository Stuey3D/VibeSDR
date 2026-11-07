// adv_rds_test.cpp — the handheld Advanced RDS port's checks (src/adv_rds_core.h). Silent: no audio, no GL.
//   1. the golden replay of the app's lrMeter.ts / mpxPower.ts (gen_ards_golden.ts writes it), when given a file;
//   2. scenario checks of the panel's readings against AdvRdsPanel.tsx's rules, worked by hand from the TS.
// Run: handheld-ui/test/run_ards_test.sh
#include "../src/adv_rds_core.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace hh;
using namespace hh::ardr;

static int fails = 0, checks = 0;
static void eq(const std::string& what, const std::string& got, const std::string& want) {
    checks++;
    if (got != want) { fails++; std::printf("FAIL %s\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(), want.c_str()); }
}
static const char* toneName(Tone t) { return t == T_GOOD ? "ok" : t == T_WARN ? "warn" : t == T_BAD ? "bad" : t == T_MUTED ? "none" : "value"; }
static std::string jsStr(double v) { return jsNum(v); }

static void golden(const char* path) {
    std::ifstream in(path);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string p;
        while (std::getline(ss, p, '|')) f.push_back(p);
        if (!line.empty() && line.back() == '|') f.push_back("");
        if (f.empty()) continue;
        n++;
        if (f[0] == "L") {
            const bool have = f[1] == "1";
            const double l = std::stod(f[2]), r = std::stod(f[3]);
            const Lr o = lrDisplay(have, l, r, f[4] == "1");
            std::string got = o.show ? "1" : "0";
            if (o.show) got += "|" + jsStr(o.l.pct) + "|" + o.l.text + "|" + toneName(o.l.tone) + "|" + jsStr(o.r.pct) + "|" + o.r.text + "|" + toneName(o.r.tone);
            std::string want;
            for (size_t i = 5; i < f.size(); i++) want += (i > 5 ? "|" : "") + f[i];
            eq("lrDisplay " + line, got, want);
        } else if (f[0] == "P") {
            const double db = f[1] == "NaN" ? NAN : std::stod(f[1]), secs = std::stod(f[2]);
            const auto o = mpxPowerParts(db, secs);
            eq("mpxPowerParts " + line, o.first + "|" + o.second, f[3] + "|" + (f.size() > 4 ? f[4] : ""));
        }
    }
    std::printf("golden: %d lines from the app's TS\n", n);
}

static std::string rowText(const Core& k, const char* l) { const Row* r = k.row(l); return r ? r->text : "<no row>"; }
static std::string rowTone(const Core& k, const char* l) { const Row* r = k.row(l); return r ? toneName(r->tone) : "<no row>"; }

int main(int argc, char** argv) {
    if (argc > 1) golden(argv[1]);

    // ── toFixed ──
    eq("fx 2.5", fx(2.5, 0), "3"); eq("fx 0.25", fx(0.25, 1), "0.3"); eq("fx 1.005", fx(1.005, 2), "1.00");
    eq("fx -0.4", fx(-0.4, 0), "-0"); eq("fx 67.5", fx(67.5, 0), "68"); eq("fx 6.85", fx(6.85, 1), "6.8");

    // ── a healthy stereo station ──
    const Json rds = Json::parse(R"({"type":"rds","pi":49665,"ecc":225,"ber":0,"ps":"BBC R2  "})");
    const char* good = R"({"type":"rdsx","pty":10,"tp":1,"ta":0,"ms":1,"di":1,"ct":754,"ctoff":2,"gtot":500,"afseen":4,
        "rtpTitle":"Song","rtpArtist":"Band","longPs":"","ptyn":"","lang":9,"pinDay":12,"pinHour":7,"pinMin":5,
        "phase":3.0,"phaseSigned":-2.5,"phaseDrift":0.2,"phaseCoh":0.95,"pilotDev":6.8,"pilotLock":true,
        "rdsDev":3.2,"rdsDevPeak":4.1,"rdsDevRaw":3.6,"ber":0,"mpxSnr":34,"multipath":0.046,"multipathOk":1,"snrOk":1,
        "hiCutLmr":15000,"hiCutAud":15000,"ifGain":0,"ifCand":0,"ifBw":0,"imsBlend":0,"imsWhy":3,"ceqOn":0,"ceqAfter":0,
        "ceqWhy":3,"nbRate":0,"afAll":[[88100,1],[90300,0],[88100,0]],"grp":[200,0,0,0,150,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,148],
        "eon":[{"pi":"C202","ps":"BBC R3  ","af":90300,"ta":1}],"oda":[{"aid":"4BD7","grp":22}],"af":[88100],
        "xy":[100,2,-100,-3,98,1,-99,0,101,-2,-100,1,99,3,-101,-1,100,0,-98,2],"mpx":[-10,-20,-30],
        "eyeW":4,"eyeH":2,"eyeDev":30.2,"eyeAmp":[6.8,20.1,3.2],"eyeP":".D","eyeS":"ABCDEFGH","eyeR":".A.F",
        "mpxDev":74.0,"mpxAvg":38.2,"mpxHold":74.4,"mpxNoise":2.0,"mpxPow":-0.4,"mpxPowS":44,"lDev":70.0,"rDev":76.0})";
    Core k;
    k.update(Json::parse(good), rds, 1, "GB", 100.0);
    eq("Type", rowText(k, "Type"), "Pop Music (10)");
    eq("Flags", rowText(k, "Flags"), "TP \xC2\xB7 Music");
    eq("Errors", rowText(k, "Errors"), "0%");
    eq("Pilot dev", rowText(k, "Pilot dev"), "6.8 kHz \xC2\xB7 nominal \xC2\xB7 locked"); eq("Pilot tone", rowTone(k, "Pilot dev"), "ok");
    eq("RDS dev", rowText(k, "RDS dev"), "avg 3.2 \xC2\xB7 peak 4.1 \xC2\xB7 raw 3.6 kHz \xC2\xB7 nominal");
    eq("MPX S/N", rowText(k, "MPX S/N"), "34 dB \xC2\xB7 clean \xC2\xB7 no treatment"); eq("MPX tone", rowTone(k, "MPX S/N"), "ok");
    eq("Rate first", rowText(k, "Rate"), "500 total");
    eq("Phase", rowText(k, "RDS/pilot"), "\xE2\x88\x92" "2\xC2\xB0 \xC2\xB7 in phase \xC2\xB7 95% steady");   // Math.round(-2.5) = -2
    eq("Multipath", rowText(k, "Multipath"), "4.6% \xC2\xB7 slight \xC2\xB7 IMS standing by \xC2\xB7 nothing to suppress");
    eq("CEQ", rowText(k, "CEQ"), "standing by \xC2\xB7 nothing to correct"); eq("CEQ tone", rowTone(k, "CEQ"), "ok");
    eq("Blanker", rowText(k, "Blanker"), "nothing to blank");
    eq("IF", rowText(k, "IF narrow"), "\xE2\x80\x94");
    eq("Now", rowText(k, "Now playing"), "Band \xE2\x80\x94 Song");
    eq("Lang", rowText(k, "Language"), "English");
    eq("PIN", rowText(k, "PIN"), "day 12 07:05");
    eq("ODA", rowText(k, "ODA"), "RT+ in 11A");
    eq("EON", rowText(k, "Other networks"), "BBC R3 90.3 [TA]");
    eq("DI", rowText(k, "DI"), "Stereo");
    eq("Clock", rowText(k, "Clock"), "12:34 UTC+1");
    eq("Country", rowText(k, "Country"), "GB \xC2\xB7 ECC E1");
    eq("PI detail", rowText(k, "PI detail"), "49665 \xC2\xB7 National \xC2\xB7 ref 1 \xC2\xB7 cc 12");
    eq("AF score", rowText(k, "AF score"), "1/4 \xC2\xB7 25%");
    eq("AF", rowText(k, "AF MHz"), "88.1*  90.3   (* confirmed)");
    eq("Groups", rowText(k, "Group share"), "of 500 groups: 0A 40%  2A 30%  10A 30%  4A 0%");
    eq("dev peak", k.dev.peak, "74 kHz"); eq("dev avg", k.dev.avg, "38 kHz");
    eq("dev verdict", k.dev.verdict, "nominal \xC2\xB7 2 kHz noise removed"); eq("dev tone", toneName(k.dev.tone), "ok");
    eq("eye", k.eyeV.first, "strong stereo");
    eq("lr", k.lr.l.text + " " + toneName(k.lr.l.tone) + " / " + k.lr.r.text + " " + toneName(k.lr.r.tone), "70 kHz ok / 76 kHz warn");
    eq("pow", k.powValue + " | " + k.powSettling, "\xE2\x88\x92" "0.4 dBr | settling 16 s");
    eq("const", k.constV.first.substr(0, 8), "clean \xC2\xB7");
    eq("bands", std::string(toneName(k.bands[0].tone)) + toneName(k.bands[1].tone) + toneName(k.bands[2].tone), "okokok");
    {
        std::vector<uint8_t> cells;
        checks++; if (!decodeEye(".D", 4, cells) || cells != std::vector<uint8_t>{0, 0, 0, 0}) { fails++; std::printf("FAIL eye run\n"); }
        checks++; if (!decodeEye(".A.F", 7, cells) || cells[0] != 0 || cells[6] != 0) { fails++; std::printf("FAIL eye runs\n"); }
        checks++; if (!decodeEye("ABCDEFGH", 8, cells) || cells[7] != 7) { fails++; std::printf("FAIL eye literal\n"); }
        checks++; if (decodeEye("AB\"", 3, cells)) { fails++; std::printf("FAIL eye bad char accepted\n"); }
        checks++; if (decodeEye("AB", 4, cells)) { fails++; std::printf("FAIL eye short accepted\n"); }
    }
    const uint64_t g1 = k.eyeGen;

    // ── the rate: successive deltas over ≥ 1 s, smoothed ──
    k.update(Json::parse(std::string(good).replace(std::string(good).find("\"gtot\":500"), 10, "\"gtot\":511")), rds, 1, "GB", 101.0);
    eq("Rate", rowText(k, "Rate"), "11.0/s of 11.4 \xC2\xB7 511 total");

    // ── a frame WITHOUT the eye grids keeps the last ones (eyeEvery) ──
    k.update(Json::parse(R"({"type":"rdsx","gtot":520,"pilotDev":6.8,"pilotLock":true,"mpxSnr":34,"snrOk":1,"multipathOk":0,"eyeW":4,"eyeH":2})"), rds, 1, "GB", 101.5);
    eq("eye kept", k.eye[1], "ABCDEFGH");
    checks++; if (k.eyeGen != g1 + 1) { fails++; std::printf("FAIL eyeGen moved on a frame with no grids (%llu vs %llu)\n", (unsigned long long)k.eyeGen, (unsigned long long)g1); }
    // ★ multipath HELD (dimmed) once multipathOk drops, never "too noisy" over a good reading
    eq("mp held", rowText(k, "Multipath"), "4.6% \xC2\xB7 slight \xC2\xB7 held");   // (imsWhy absent = 1 running: no clause)
    eq("mp held tone", rowTone(k, "Multipath"), "none");
    // ★ RDS dev held 4 s through a dropout, then an honest zero
    eq("rds hold", rowText(k, "RDS dev"), "avg 3.2 \xC2\xB7 peak 4.1 \xC2\xB7 raw 3.6 kHz \xC2\xB7 nominal");
    k.update(Json::parse(R"({"type":"rdsx","gtot":530,"mpxSnr":34,"snrOk":1})"), rds, 1, "GB", 106.0);
    eq("rds hold over", rowText(k, "RDS dev"), "0.0 kHz \xC2\xB7 none");

    // ── a weak, unlocked station: the gate refuses, the words say why ──
    {
        Core w;
        w.update(Json::parse(R"({"type":"rdsx","pilotDev":5.2,"pilotLock":false,"rdsDev":2.0,"ber":60,"mpxSnr":6,"snrOk":0,
            "mpxDev":40,"mpxHold":45,"phase":40,"phaseCoh":0.2,"gtot":30,"ct":-1,"grp":[10,0,0,0,0,0,0,0,0]})"), rds, 0, "", 10.0);
        eq("weak pilot", w.row("Pilot dev")->text, "5.2 kHz \xC2\xB7 low \xC2\xB7 NOT locked"); eq("weak pilot tone", rowTone(w, "Pilot dev"), "warn");
        eq("weak rds", w.row("RDS dev")->text, "not measurable at this error rate");
        eq("weak snr", w.row("MPX S/N")->text, "no pilot to measure");
        eq("weak mp", w.row("Multipath")->text, "no pilot to measure");
        eq("weak phase", w.row("RDS/pilot")->text, "unstable \xE2\x80\x94 not measurable");
        eq("weak dev", w.dev.verdict, "low S/N, unreliable"); eq("weak dev tone", toneName(w.dev.tone), "none");
        eq("weak eye", w.eyeV.first, "pilot present but not locking \xE2\x80\x94 forms and collapses");
        eq("weak clock", w.row("Clock")->text, "waiting\xE2\x80\xA6 (1/min)");
        eq("weak country", w.row("Country")->text, "ECC E1 \xC2\xB7 unmatched");
        checks++; if (w.lr.show) { fails++; std::printf("FAIL lr shown with nothing ever measured\n"); }
        eq("weak const", w.constV.first, "RDS no lock");
    }
    // ── bypassed NR, treatment acting, IF narrowed, CEQ helping, overmodulation ──
    {
        Core t;
        t.update(Json::parse(R"({"type":"rdsx","mpxSnr":22,"snrOk":1,"hiCutLmr":8400,"hiCutAud":11200,"multipath":0.12,"multipathOk":1,
            "imsBlend":6500,"ceqOn":1,"ceqAfter":0.05,"ifBw":134000,"ifCand":200000,"ifGain":-3.9,"nbRate":0.008,"mpxDev":90,"mpxHold":94,
            "phase":45,"phaseCoh":0.9,"phaseDrift":0,"ber":2})"), rds, 1, "", 0);
        eq("acting", t.row("MPX S/N")->text, "22 dB \xC2\xB7 blend 8.4k \xC2\xB7 cut 11.2k"); eq("acting tone", rowTone(t, "MPX S/N"), "warn");
        eq("ims", t.row("Multipath")->text, "12.0% \xC2\xB7 moderate \xC2\xB7 IMS blending L\xE2\x88\x92R to 6.5k");
        eq("ceq", t.row("CEQ")->text, "before 12.0% \xC2\xB7 after 5.0%"); eq("ceq tone", rowTone(t, "CEQ"), "ok");
        eq("if", t.row("IF narrow")->text, "134k narrow \xC2\xB7 wide would cost 3.9 dB");
        eq("nb", t.row("Blanker")->text, "0.80% blanked"); eq("nb tone", rowTone(t, "Blanker"), "warn");
        eq("over", t.dev.verdict, "OVERMODULATED"); eq("over tone", toneName(t.dev.tone), "bad");
        eq("phase fault", t.row("RDS/pilot")->text, "45\xC2\xB0 \xC2\xB7 FAULT \xC2\xB7 90% steady");
        t.update(Json::parse(R"({"type":"rdsx","mpxSnr":22,"snrOk":1})"), rds, 0, "", 1);
        eq("bypassed", t.row("MPX S/N")->text, "22 dB \xC2\xB7 bypassed"); eq("bypassed tone", rowTone(t, "MPX S/N"), "value");
        eq("phase none", t.row("RDS/pilot")->text, "\xE2\x80\x94");
        eq("drift", (t.update(Json::parse(R"({"type":"rdsx","phase":10,"phaseCoh":0.9,"phaseDrift":3.4})"), rds, 1, "", 2), t.row("RDS/pilot")->text),
           "rotating 3\xC2\xB0/s \xE2\x80\x94 encoder not locked to pilot");
    }
    std::printf("adv rds: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
