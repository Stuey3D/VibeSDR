// ★★★ THE ICOM RECEIVE-ONLY PROMISE, PROVED WITHOUT A RADIO (plan §3.2).
//
// civ::mayTransmit() is the last function before every byte VibeServer writes to an Icom. This
// test is its second, independent reader: the allowed shapes are written out AGAIN here, by hand,
// in a different form (a list of rows, not a switch), and every cmd/sub byte pair is tried at 0-6
// data bytes with a spread of values. The two must agree on every one of them — about 3.6 million
// frames — and the transmit forms the plan names are asserted refused by name.
//
// ★ If this test fails, NOTHING SHIPS (plan §8 risk 1). A new allowed command is added to BOTH
//   civ_codec.h and the table below, deliberately, never to one.
#include "civ_codec.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) return;
    failures++;
    if (failures < 40) std::printf("   FAIL %s %s\n", what, extra.c_str());
}
static void okv(bool cond, const char* what) { ok(cond, what); std::printf("   %s %s\n", cond ? "ok  " : "FAIL", what); }

static civ::Policy policy7300() {
    civ::Policy p;
    p.radio = 0x94;
    p.ranges[0] = {30000, 74800000}; p.nRanges = 1;
    const uint8_t m[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x08};
    std::memcpy(p.modes, m, sizeof m); p.nModes = (int)sizeof m;
    p.att[0] = 0x00; p.att[1] = 0x20; p.nAtt = 2;
    p.preampMax = 2; p.dataModeMax = 1;
    const uint32_t s[] = {2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000};
    std::memcpy(p.spansHalfHz, s, sizeof s); p.nSpans = 8;
    p.menuItems[0][0] = 0x00; p.menuItems[0][1] = 0x89; p.nMenuItems = 1;
    return p;
}

static std::vector<uint8_t> frame(uint8_t to, uint8_t from, uint8_t c, const std::vector<uint8_t>& d) {
    std::vector<uint8_t> f = {0xFE, 0xFE, to, from, c};
    f.insert(f.end(), d.begin(), d.end());
    f.push_back(0xFD);
    return f;
}
static bool allowed(const std::vector<uint8_t>& f, const civ::Policy& p) {
    return civ::mayTransmit(f.data(), f.size(), p) == civ::Verdict::Ok;
}

// ── The independent table ──────────────────────────────────────────────────────────────────
// A row: cmd, sub (-1 = none), number of value bytes AFTER cmd+sub(+selector), selector byte
// required, and a value predicate. Reads are rows with 0 value bytes.
using Pred = bool (*)(const uint8_t* v, const civ::Policy& p);
struct Row { int cmd, sub; int vals; bool sel; Pred ok; const char* name; };

static int bcd(uint8_t b) { int h = b >> 4, l = b & 15; return (h > 9 || l > 9) ? -1 : h * 10 + l; }
static bool any(const uint8_t*, const civ::Policy&) { return true; }
static bool onoff(const uint8_t* v, const civ::Policy&) { return v[0] <= 1; }
static bool lvl(const uint8_t* v, const civ::Policy&) { int a = bcd(v[0]), b = bcd(v[1]); return a >= 0 && b >= 0 && a * 100 + b <= 255; }
static bool freqIn(const uint8_t* v, const civ::Policy& p) {
    long long hz = 0, m = 1;
    for (int i = 0; i < 5; i++) { int d = bcd(v[i]); if (d < 0) return false; hz += d * m; m *= 100; }
    for (int i = 0; i < p.nRanges; i++) if ((unsigned long long)hz >= p.ranges[i].lo && (unsigned long long)hz <= p.ranges[i].hi) return true;
    return false;
}
static bool spanIn(const uint8_t* v, const civ::Policy& p) {
    long long hz = 0, m = 1;
    for (int i = 0; i < 5; i++) { int d = bcd(v[i]); if (d < 0) return false; hz += d * m; m *= 100; }
    for (int i = 0; i < p.nSpans; i++) if ((long long)p.spansHalfHz[i] == hz) return true;
    return false;
}
static bool modeFil(const uint8_t* v, const civ::Policy& p) {
    bool m = false; for (int i = 0; i < p.nModes; i++) m |= p.modes[i] == v[0];
    return m && v[1] >= 1 && v[1] <= 3;
}
static bool attOk(const uint8_t* v, const civ::Policy& p) { for (int i = 0; i < p.nAtt; i++) if (p.att[i] == v[0]) return true; return false; }
static bool preOk(const uint8_t* v, const civ::Policy& p) { return v[0] <= p.preampMax; }
static bool agcOk(const uint8_t* v, const civ::Policy&) { return v[0] >= 1 && v[0] <= 3; }
static bool dataOk(const uint8_t* v, const civ::Policy& p) { return v[0] <= p.dataModeMax && v[1] <= 3; }
static bool widthOk(const uint8_t* v, const civ::Policy&) { int a = bcd(v[0]); return a >= 0 && a <= 49; }
static bool menuOk(const uint8_t* v, const civ::Policy& p) {
    for (int i = 0; i < p.nMenuItems; i++) if (p.menuItems[i][0] == v[0] && p.menuItems[i][1] == v[1]) return true;
    return false;
}
static bool sc4(const uint8_t* v, const civ::Policy&) { return v[0] <= 3; }
static bool sc3(const uint8_t* v, const civ::Policy&) { return v[0] <= 2; }
static bool refOk(const uint8_t* v, const civ::Policy&) { int a = bcd(v[0]), b = bcd(v[1]); return a >= 0 && b >= 0 && a * 100 + b <= 2000 && v[2] <= 1; }

static const Row kRows[] = {
    {0x03, -1, 0, false, any, "03 read freq"},
    {0x04, -1, 0, false, any, "04 read mode"},
    {0x05, -1, 5, false, freqIn, "05 set freq"},
    {0x06, -1, 2, false, modeFil, "06 set mode"},
    {0x11, -1, 0, false, any, "11 read att"},
    {0x11, -1, 1, false, attOk, "11 set att"},
    {0x15, 0x02, 0, false, any, "15 02 S-meter"},
    {0x19, 0x00, 0, false, any, "19 00 ID"},
    {0x1C, 0x00, 0, false, any, "1C 00 TX state READ"},
    {0x14, 0x02, 0, false, any, "14 02 read"}, {0x14, 0x02, 2, false, lvl, "14 02 RF gain"},
    {0x14, 0x03, 0, false, any, "14 03 read"}, {0x14, 0x03, 2, false, lvl, "14 03 squelch"},
    {0x16, 0x02, 0, false, any, "16 02 read"}, {0x16, 0x02, 1, false, preOk, "16 02 preamp"},
    {0x16, 0x12, 0, false, any, "16 12 read"}, {0x16, 0x12, 1, false, agcOk, "16 12 AGC"},
    {0x16, 0x22, 0, false, any, "16 22 read"}, {0x16, 0x22, 1, false, onoff, "16 22 NB"},
    {0x16, 0x40, 0, false, any, "16 40 read"}, {0x16, 0x40, 1, false, onoff, "16 40 NR"},
    {0x16, 0x41, 0, false, any, "16 41 read"}, {0x16, 0x41, 1, false, onoff, "16 41 ANF"},
    {0x16, 0x46, 0, false, any, "16 46 VOX READ"},
    {0x1A, 0x03, 0, false, any, "1A 03 read"}, {0x1A, 0x03, 1, false, widthOk, "1A 03 width"},
    {0x1A, 0x05, 2, false, menuOk, "1A 05 preflight READ"},
    {0x1A, 0x06, 0, false, any, "1A 06 read"}, {0x1A, 0x06, 2, false, dataOk, "1A 06 data mode"},
    {0x27, 0x10, 0, false, any, "27 10 read"}, {0x27, 0x10, 1, false, onoff, "27 10 scope"},
    {0x27, 0x11, 0, false, any, "27 11 read"}, {0x27, 0x11, 1, false, onoff, "27 11 data out"},
    {0x27, 0x14, 0, true, any, "27 14 read"},  {0x27, 0x14, 1, true, sc4, "27 14 mode"},
    {0x27, 0x15, 0, true, any, "27 15 read"},  {0x27, 0x15, 5, true, spanIn, "27 15 span"},
    {0x27, 0x17, 1, true, onoff, "27 17 hold"},
    {0x27, 0x19, 0, true, any, "27 19 read"},  {0x27, 0x19, 3, true, refOk, "27 19 REF"},
    {0x27, 0x1A, 0, true, any, "27 1A read"},  {0x27, 0x1A, 1, true, sc3, "27 1A speed"},
};

/** The test's own answer for a frame to the right radio, from the right controller. */
static bool expected(uint8_t c, const std::vector<uint8_t>& d, const civ::Policy& p) {
    for (const Row& r : kRows) {
        if (r.cmd != c) continue;
        size_t at = 0;
        if (r.sub >= 0) { if (d.empty() || d[0] != r.sub) continue; at = 1; }
        if (r.sel) { if (d.size() <= at || d[at] != 0x00) continue; at++; }
        if (d.size() != at + (size_t)r.vals) continue;
        if (r.ok(d.data() + at, p)) return true;
    }
    return false;
}

int main() {
    const civ::Policy p = policy7300();
    std::printf("── Icom CI-V allow-list (plan §3.2) ──\n");

    // ── 1. Exhaustive: every cmd/sub pair, 0-6 data bytes, a spread of values ────────────────
    static const uint8_t kFill[] = {0x00, 0x01, 0x02, 0x03, 0x20, 0x55, 0x99, 0x0A, 0xA0, 0xE0, 0x7F};
    long long tried = 0, agreed = 0, passed = 0;
    std::vector<uint8_t> d;
    for (int c = 0; c < 256; c++) {
        if (c == 0xFE || c == 0xFD) continue;   // cannot be a command byte (framing)
        // no data
        d.clear();
        { auto f = frame(0x94, 0xE0, (uint8_t)c, d);
          const bool a = allowed(f, p), e = expected((uint8_t)c, d, p);
          tried++; agreed += a == e; passed += a;
          ok(a == e, "no-data agreement", "cmd " + std::to_string(c)); }
        for (int s = 0; s < 256; s++) {
            if (s == 0xFE || s == 0xFD) continue;
            for (int len = 1; len <= 6; len++) {
                for (uint8_t fill : kFill) {
                    // Two layouts: every byte after the sub = fill; and a selector 00 then fill.
                    for (int lay = 0; lay < 2; lay++) {
                        d.assign((size_t)len, fill);
                        d[0] = (uint8_t)s;
                        if (lay == 1 && len >= 2) d[1] = 0x00;
                        else if (lay == 1) continue;
                        auto f = frame(0x94, 0xE0, (uint8_t)c, d);
                        const bool a = allowed(f, p), e = expected((uint8_t)c, d, p);
                        tried++; agreed += a == e; passed += a;
                        ok(a == e, "agreement", "cmd " + std::to_string(c) + " sub " + std::to_string(s)
                                                + " len " + std::to_string(len) + " fill " + std::to_string(fill)
                                                + (a ? " (codec ALLOWS)" : " (codec refuses)"));
                    }
                }
            }
        }
    }
    std::printf("   %s exhaustive: %lld frames, %lld agree, %lld allowed\n",
                agreed == tried ? "ok  " : "FAIL", tried, agreed, passed);

    // ── 2. The transmit frames the plan names, refused BY NAME ───────────────────────────────
    struct Bad { std::vector<uint8_t> data; uint8_t c; const char* what; };
    const Bad bad[] = {
        {{0x00, 0x01}, 0x1C, "1C 00 01 — TRANSMIT"},
        {{0x00, 0x00}, 0x1C, "1C 00 00 — a TX write, even 'receive'"},
        {{0x01, 0x02}, 0x1C, "1C 01 02 — antenna tuner start"},
        {{0x01, 0x01}, 0x1C, "1C 01 01 — tuner on"},
        {{0x41, 0x42}, 0x17, "17 … — send CW"},
        {{}, 0x17, "17 bare"},
        {{0x00, 0x01}, 0x28, "28 00 01 — TX voice memory"},
        {{0x00}, 0x18, "18 00 — power off"},
        {{0x01}, 0x18, "18 01 — power on"},
        {{0x00, 0x89, 0x01}, 0x1A, "1A 05 00 89 01 — a menu WRITE (USB SEND)"},
        {{0x05, 0x00, 0x89, 0x01}, 0x1A, "1A 05 00 89 01 (with sub)"},
        {{0x05, 0x00, 0x01}, 0x1A, "1A 05 00 01 — read of an item NOT in the preflight"},
        {{0x01}, 0x0F, "0F 01 — split on"},
        {{0x01}, 0x0D, "0D — offset"},
        {{}, 0x09, "09 — memory write"},
        {{}, 0x0B, "0B — memory clear"},
        {{0x0A, 0x02, 0x55}, 0x14, "14 0A — RF POWER"},
        {{0x0B, 0x01, 0x00}, 0x14, "14 0B — mic gain"},
        {{0x46, 0x01}, 0x16, "16 46 01 — VOX ON"},
        {{0x46, 0x00}, 0x16, "16 46 00 — a VOX write of any value"},
        {{0x47, 0x01}, 0x16, "16 47 01 — break-in"},
        {{0x00, 0x01}, 0x1A, "1A 00 — memory contents"},
        {{0x01, 0x01}, 0x1A, "1A 01 — band stack"},
        {{0x02, 0x01}, 0x1A, "1A 02 — keyer memory"},
        {{}, 0x1E, "1E — band edges"},
        {{0x01}, 0x0E, "0E 01 — scan"},
        {{0x00, 0x00, 0x01, 0x01}, 0x27, "27 00 — waveform is the RADIO's to send"},
        {{0x01, 0x00}, 0x06, "06 01 00 — filter 0"},
        {{0x06, 0x01}, 0x06, "06 06 01 — WFM, not a 7300 mode"},
        {{0x30}, 0x11, "11 30 — ATT value not offered"},
        {{0x02, 0x03}, 0x16, "16 02 03 — preamp 3"},
        {{0x02, 0x03, 0x00}, 0x14, "14 02 0300 — RF gain past 255"},
        {{0x15, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00}, 0x27, "27 15 — a span not on the ladder"},
    };
    for (const Bad& b : bad) okv(!allowed(frame(0x94, 0xE0, b.c, b.data), p), b.what);

    // ── 3. Shape: address, controller, framing ───────────────────────────────────────────────
    okv(!allowed(frame(0x98, 0xE0, 0x03, {}), p), "a read to the WRONG radio address");
    okv(!allowed(frame(0x00, 0xE0, 0x03, {}), p), "a read to the broadcast address");
    okv(!allowed(frame(0x94, 0xE1, 0x03, {}), p), "from a controller address that is not ours");
    { std::vector<uint8_t> f = {0xFE, 0xFE, 0xFE, 0x94, 0xE0, 0x03, 0xFD};
      okv(!allowed(f, p), "a wake run (FE FE FE …)"); }
    { std::vector<uint8_t> f = {0xFE, 0xFE, 0x94, 0xE0, 0x03};
      okv(!allowed(f, p), "no FD"); }
    { auto f = frame(0x94, 0xE0, 0x05, {0x00, 0x40, 0x07, 0xFD, 0x00});
      okv(!allowed(f, p), "an FD inside the data"); }

    // ── 4. What MUST pass ────────────────────────────────────────────────────────────────────
    okv(civ::mayTransmit(civ::build::setFreq(0x94, 14074000), p) == civ::Verdict::Ok, "05 14.074 MHz");
    okv(civ::mayTransmit(civ::build::setFreq(0x94, 30000), p) == civ::Verdict::Ok, "05 at the bottom edge");
    okv(civ::mayTransmit(civ::build::setFreq(0x94, 74800000), p) == civ::Verdict::Ok, "05 at the top edge");
    okv(civ::mayTransmit(civ::build::setFreq(0x94, 74800001), p) == civ::Verdict::OutOfRange, "05 one Hz past the top: refused");
    okv(civ::mayTransmit(civ::build::setFreq(0x94, 144300000), p) == civ::Verdict::OutOfRange, "05 at 2 m on a 7300: refused");
    okv(civ::mayTransmit(civ::build::setMode(0x94, civ::mode::USB, 1), p) == civ::Verdict::Ok, "06 USB FIL1");
    okv(civ::mayTransmit(civ::build::setDataMode(0x94, 1, 1), p) == civ::Verdict::Ok, "1A 06 01 01 USB-D");
    okv(civ::mayTransmit(civ::build::readTx(0x94), p) == civ::Verdict::Ok, "1C 00 — the TX READ");
    okv(civ::mayTransmit(civ::build::readFunc(0x94, 0x46), p) == civ::Verdict::Ok, "16 46 — the VOX READ");
    { const uint8_t it[2] = {0x00, 0x89};
      okv(civ::mayTransmit(civ::build::readMenu(0x94, it), p) == civ::Verdict::Ok, "1A 05 00 89 — preflight read"); }
    okv(civ::mayTransmit(civ::build::scopeOn(0x94, true), p) == civ::Verdict::Ok, "27 10 01");
    okv(civ::mayTransmit(civ::build::scopeData(0x94, false), p) == civ::Verdict::Ok, "27 11 00");
    okv(civ::mayTransmit(civ::build::scopeSpan(0x94, 50000), p) == civ::Verdict::Ok, "27 15 00 ±50 kHz");
    okv(civ::mayTransmit(civ::build::scopeMode(0x94, 1), p) == civ::Verdict::Ok, "27 14 00 01 fixed");
    okv(civ::mayTransmit(civ::build::setLevel(0x94, 0x02, 255), p) == civ::Verdict::Ok, "14 02 0255");
    okv(civ::mayTransmit(civ::build::readScope(0x94, 0x15), p) == civ::Verdict::Ok, "27 15 00 — span read");
    okv(civ::mayTransmit(civ::build::readScope(0x94, 0x10), p) == civ::Verdict::Ok, "27 10 — scope read");

    // ── 5. BCD and the parser ────────────────────────────────────────────────────────────────
    { uint8_t b[5]; civ::freqToBcd(14074000, b);
      okv(b[0] == 0x00 && b[1] == 0x40 && b[2] == 0x07 && b[3] == 0x14 && b[4] == 0x00, "14.074 MHz → 00 40 07 14 00");
      okv(civ::bcdToFreq(b, 5) == 14074000, "and back");
      b[2] = 0x0A; okv(civ::bcdToFreq(b, 5) == -1, "a non-digit nibble is not a frequency"); }
    { uint8_t b[2]; civ::intToBcdBig(255, b, 2);
      okv(b[0] == 0x02 && b[1] == 0x55 && civ::bcdBigToInt(b, 2) == 255, "255 → 02 55 → 255"); }
    {
        civ::Parser ps;
        int n = 0; uint8_t lastCmd = 0; int lastLen = -1; uint8_t lastFrom = 0;
        auto cb = [&](const civ::Frame& f) { n++; lastCmd = f.cmd; lastLen = f.len; lastFrom = f.from; };
        const uint8_t s1[] = {0x00, 0xFE, 0xFE, 0xE0, 0x94, 0x03, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD};
        ps.feed(s1, sizeof s1, cb);
        okv(n == 1 && lastCmd == 0x03 && lastLen == 5 && lastFrom == 0x94, "parser: a freq reply after junk");
        const uint8_t s2[] = {0xFE, 0xFE, 0xFE, 0xFE, 0xE0, 0x94, 0xFB, 0xFD};
        ps.feed(s2, sizeof s2, cb);
        okv(n == 2 && lastCmd == 0xFB, "parser: a long preamble run, then OK");
        const uint8_t s3[] = {0xFE, 0xFE, 0xE0, 0x94, 0x27, 0x00, 0xFC, 0xFE, 0xFE, 0xE0, 0x94, 0xFA, 0xFD};
        ps.feed(s3, sizeof s3, cb);
        okv(n == 3 && lastCmd == 0xFA && ps.framesDropped >= 1, "parser: a jammer abandons the frame, the next parses");
        // split across reads
        const uint8_t a[] = {0xFE, 0xFE, 0xE0}, b2[] = {0x94, 0x04, 0x01}, c2[] = {0x02, 0xFD};
        ps.feed(a, 3, cb); ps.feed(b2, 3, cb); ps.feed(c2, 2, cb);
        okv(n == 4 && lastCmd == 0x04 && lastLen == 2, "parser: a frame split over three reads");
        std::vector<uint8_t> big = {0xFE, 0xFE, 0xE0, 0x94, 0x27};
        big.insert(big.end(), civ::kMaxData + 10, 0x11); big.push_back(0xFD);
        ps.feed(big.data(), big.size(), cb);
        okv(n == 4, "parser: an over-long frame is dropped whole");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
