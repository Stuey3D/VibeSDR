// test-dab-stereo.cpp — the stereo light in DAB is the SERVICE's own (vibe_dab_stereo.h).
//
// ★★★ Stuart, 2026-10-01: "the stereo icon from WFM also is stuck when in DAB mode even when on Mono
//     stations." DAB has no pilot; the light now reads the playing service's audio headers. This holds
//     the table down — every Layer II mode through the REAL header parser, and every DAB+ channel
//     configuration.
#include "vibe_dab_stereo.h"
#include "vibe_dab_mp2.h"
#include <cstdio>
#include <cstdint>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    // ── DAB (Layer II): the header's mode field, read by the parser the decoder uses ──
    // FF FD = MPEG-1 Layer II, no CRC; 0x94 = 128 kbit/s, 48 kHz, no padding; the top two bits of the
    // fourth byte are the mode.
    struct { int mode; bool stereo; const char* name; } mp2[] = {
        { 0, true,  "stereo" }, { 1, true, "joint stereo" },
        { 2, false, "dual channel (two mono programmes)" }, { 3, false, "single channel (mono)" },
    };
    for (const auto& c : mp2) {
        const uint8_t hdr[4] = { 0xFF, 0xFD, 0x94, uint8_t(c.mode << 6) };
        const Mp2Info f = mp2Header(hdr, sizeof hdr);
        char m[96];
        snprintf(m, sizeof m, "Layer II header parses (mode %d)", c.mode);
        CHECK(f.valid && f.mode == c.mode, m);
        snprintf(m, sizeof m, "Layer II %s -> %s", c.name, c.stereo ? "stereo" : "NOT stereo");
        CHECK(dabMp2ModeIsStereo(f.mode) == c.stereo, m);
    }
    // MPEG-2 LSF (24 kHz, the other half of DAB's Layer II): the same field, the same answer.
    {
        const uint8_t lsfMono[4] = { 0xFF, 0xF5, 0x84, 0xC0 };   // MPEG-2, Layer II, mono
        const Mp2Info f = mp2Header(lsfMono, sizeof lsfMono);
        CHECK(f.valid && f.lsf && !dabMp2ModeIsStereo(f.mode), "24 kHz LSF mono -> NOT stereo");
        const uint8_t lsfJs[4] = { 0xFF, 0xF5, 0x84, 0x40 };     // MPEG-2, Layer II, joint stereo
        const Mp2Info g = mp2Header(lsfJs, sizeof lsfJs);
        CHECK(g.valid && g.lsf && dabMp2ModeIsStereo(g.mode), "24 kHz LSF joint stereo -> stereo");
    }

    // ── DAB+ (HE-AAC): aac_channel_mode × ps_flag ──
    CHECK(!dabAacIsStereo(false, false), "DAB+ mono core, no PS -> mono");
    CHECK( dabAacIsStereo(true,  false), "DAB+ stereo core -> stereo");
    CHECK( dabAacIsStereo(false, true),  "DAB+ HE-AAC v2 parametric stereo (mono core) -> stereo");
    CHECK( dabAacIsStereo(true,  true),  "DAB+ stereo core + PS flag -> stereo");

    printf(fails ? "FAIL  dab stereo: %d failed\n" : "ok  dab stereo: every mode\n", fails);
    return fails ? 1 : 0;
}
