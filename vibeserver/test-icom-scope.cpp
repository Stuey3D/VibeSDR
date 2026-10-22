// ★★ THE ICOM SCOPE AS OUR WATERFALL (icom_scope.h): divisions → whole sweeps → SPEC rows over the
//    radio's real edges. Synthetic frames first (exact bytes we choose), then every capture under
//    test/fixtures/icom/*.civhex — the format tools/icom/civ-capture.py writes, so Saber's real 7300
//    frames drop straight in and are replayed here on every ctest.
//
//  .civhex: one frame per line, "<ms> <dir> <hex bytes…>", dir '<' = radio→us, '>' = us→radio;
//  '#' starts a comment. ★ The '>' lines are checked against the allow-list too: a capture is a
//  record of what WE sent, and it must pass the same rule as the server.
#include "civ_codec.h"
#include "icom_models.h"
#include "icom_scope.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
static void ok(bool c, const std::string& what) {
    checks++; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); if (!c) failures++;
}

/** The data of a 27 00 frame (everything after cmd 27): 00 sel seq total … */
static std::vector<uint8_t> div(int seq, int total, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> d = {0x00, 0x00, civ::toBcdByte(seq), civ::toBcdByte(total)};
    d.insert(d.end(), body.begin(), body.end());
    return d;
}
static std::vector<uint8_t> header(uint8_t mode, uint64_t a, uint64_t b) {
    std::vector<uint8_t> h = {mode};
    uint8_t x[5]; civ::freqToBcd(a, x); h.insert(h.end(), x, x + 5);
    civ::freqToBcd(b, x); h.insert(h.end(), x, x + 5);
    h.push_back(0x00);
    return h;
}
/** A 475-point USB sweep in 11 divisions, amplitude from `f(i)`. */
template <class F> static std::vector<std::vector<uint8_t>> usbSweep(uint8_t mode, uint64_t a, uint64_t b, F f) {
    std::vector<std::vector<uint8_t>> out;
    out.push_back(div(1, 11, header(mode, a, b)));
    int at = 0;
    for (int d = 2; d <= 11; d++) {
        std::vector<uint8_t> body;
        for (int k = 0; k < (d == 11 ? 25 : 50); k++) body.push_back((uint8_t)f(at++));
        out.push_back(div(d, 11, body));
    }
    return out;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void replayFixture(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { ok(false, "open " + path); return; }
    const icom::Model* m = icom::modelByAddr(0x94);
    civ::Policy p; p.radio = 0x94;
    p.ranges[0] = {m->ranges[0].lo, m->ranges[0].hi}; p.nRanges = 1;
    for (int i = 0; i < m->nModes; i++) if (!civ::inList(p.modes, p.nModes, m->modeBytes[i])) p.modes[p.nModes++] = m->modeBytes[i];
    for (int i = 0; i < m->nSpans; i++) p.spansHalfHz[p.nSpans++] = m->spansHalfHz[i];
    for (int i = 0; i < m->nMenu; i++) { p.menuItems[i][0] = m->menu[i].item[0]; p.menuItems[i][1] = m->menu[i].item[1]; p.nMenuItems++; }
    civ::Parser ps; icom::ScopeAssembler as;
    int sweeps = 0, sane = 0, sentBad = 0, sent = 0;
    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* s = line;
        std::strtod(s, &s);
        while (*s == ' ') s++;
        const char dir = *s++;
        std::vector<uint8_t> b;
        for (;;) {
            while (*s == ' ') s++;
            const int h = hexval(s[0]), l = s[0] ? hexval(s[1]) : -1;
            if (h < 0 || l < 0) break;
            b.push_back((uint8_t)(h * 16 + l)); s += 2;
        }
        if (dir == '>') { sent++; if (civ::mayTransmit(b.data(), b.size(), p) != civ::Verdict::Ok) sentBad++; continue; }
        ps.feed(b.data(), b.size(), [&](const civ::Frame& fr) {
            if (fr.cmd != 0x27 || fr.len < 1 || fr.data[0] != 0x00) return;
            if (as.feed(fr.data, fr.len)) {
                sweeps++;
                const icom::Sweep& w = as.out();
                if (w.hiHz > w.loHz && w.n >= 400 && w.n <= icom::kMaxPoints) sane++;
            }
        });
    }
    std::fclose(f);
    const std::string name = path.substr(path.rfind('/') + 1);
    ok(sweeps > 0 && sane == sweeps, name + ": " + std::to_string(sweeps) + " whole sweeps, all with real edges and a full row");
    ok(sentBad == 0, name + ": every frame WE sent (" + std::to_string(sent) + ") passes the allow-list");
}

int main() {
    std::printf("── Icom scope assembler ──\n");
    {
        icom::ScopeAssembler a;
        int done = 0;
        for (auto& d : usbSweep(0, 14074000, 25000, [](int i) { return i == 237 ? 150 : 30; }))
            done += a.feed(d.data(), (int)d.size());
        const icom::Sweep& s = a.out();
        ok(done == 1 && s.n == 475, "11 USB divisions → one sweep of 475 points");
        ok(s.loHz == 14049000 && s.hiHz == 14099000, "centre mode: A = centre, B = half-span → 14.049-14.099 MHz");
        ok(s.amp[237] == 150 && s.amp[0] == 30, "amplitudes in order");
        std::vector<uint8_t> row(1024);
        icom::sweepToWire(s, 1024, row.data(), 160, 80.0);
        // ★ FFT order: bin 0 is the CENTRE — where the 150 carrier is
        ok(row[0] == icom::ampToWire(150, 160, 80.0), "wire row: the centre carrier is bin 0 (FFT order, DC first)");
        ok(row[512] == icom::ampToWire(30, 160, 80.0), "the band edge is bin n/2");
        ok(icom::ampToWire(160, 160, 80.0) == 256 - 130 + 80 - 0 || icom::ampToWire(160, 160, 80.0) == 206, "full scale lands at -50 dB on the wire (byte 206)");
        ok(icom::ampToWire(0, 160, 80.0) == 126, "zero lands at the -130 dB floor (byte 126)");
        // ★ downsampling keeps a ONE-POINT carrier (peak-hold), upsampling never invents a value
        std::vector<uint8_t> small(64);
        icom::ScopeAssembler a2; int d2 = 0;
        for (auto& d : usbSweep(0, 7074000, 25000, [](int i) { return i == 100 ? 120 : 20; })) d2 += a2.feed(d.data(), (int)d.size());
        icom::sweepToWire(a2.out(), 64, small.data(), 160, 80.0);
        bool seen = false; for (uint8_t v : small) seen |= v == icom::ampToWire(120, 160, 80.0);
        ok(d2 == 1 && seen, "64 bins from 475 points: the one-point carrier survives (peak-hold)");
        std::vector<uint8_t> big(4096);
        icom::sweepToWire(a2.out(), 4096, big.data(), 160, 80.0);
        int vals = 0; for (uint8_t v : big) vals += (v == icom::ampToWire(120, 160, 80.0));
        ok(vals >= 7 && vals <= 10, "4096 bins from 475: sample-and-hold, the carrier is ~8.6 bins wide, no interpolated slope (" + std::to_string(vals) + ")");
    }
    {
        icom::ScopeAssembler a; int done = 0;
        auto sw = usbSweep(1, 14000000, 14350000, [](int) { return 40; });
        for (auto& d : sw) done += a.feed(d.data(), (int)d.size());
        ok(done == 1 && a.out().loHz == 14000000 && a.out().hiHz == 14350000, "fixed mode: A and B are the edges");
    }
    {
        icom::ScopeAssembler a; int done = 0;
        auto sw = usbSweep(0, 14074000, 25000, [](int) { return 40; });
        sw.erase(sw.begin() + 5);                    // a division lost on the wire
        for (auto& d : sw) done += a.feed(d.data(), (int)d.size());
        ok(done == 0 && a.dropped >= 1, "a missing division drops the sweep — never a row with a hole in it");
        for (auto& d : usbSweep(0, 14074000, 25000, [](int) { return 40; })) done += a.feed(d.data(), (int)d.size());
        ok(done == 1, "and the next whole sweep is taken");
    }
    {
        // ★ LAN: the whole sweep in ONE division (01/01) — header then 689 points (a 7610)
        std::vector<uint8_t> body = header(0, 7100000, 50000);
        for (int i = 0; i < 689; i++) body.push_back(50);
        icom::ScopeAssembler a;
        auto d = div(1, 1, body);
        ok(a.feed(d.data(), (int)d.size()) && a.out().n == 689, "LAN: one division carries the whole 689-point sweep");
    }
    {
        icom::ScopeAssembler a;
        std::vector<uint8_t> bad = div(1, 11, header(0, 14074000, 25000));
        bad[6] = 0xA5;                               // a non-BCD frequency byte
        ok(!a.feed(bad.data(), (int)bad.size()), "a corrupt header is not a sweep");
    }
#ifdef VIBE_ICOM_FIXTURES
    std::printf("── fixtures (%s) ──\n", VIBE_ICOM_FIXTURES);
    if (DIR* d = opendir(VIBE_ICOM_FIXTURES)) {
        int n = 0;
        while (dirent* e = readdir(d)) {
            const std::string nm = e->d_name;
            if (nm.size() > 7 && nm.substr(nm.size() - 7) == ".civhex") { replayFixture(std::string(VIBE_ICOM_FIXTURES) + "/" + nm); n++; }
        }
        closedir(d);
        ok(n > 0, std::to_string(n) + " fixture file(s) replayed");
    } else ok(false, "fixture directory missing");
#endif
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
