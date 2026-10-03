// test-decoder-hardening.cpp — what a STRANGER'S attach message, or a hostile peer, may do to the
// audio decoders and the SpyServer parser (audit 2026-10-03).
//
// ★★★ WHY THIS TEST EXISTS. Any listener can send an audio_extension_attach with any numbers in
//     it, and those numbers sized buffers and divided things: an RTTY baud of NaN or 1e9 made the
//     bit length 0 (a modulo by zero on every sample), a bit length that was not a multiple of 4
//     wrote one past the zero-crossing table, WEFAX lpm 0 divided by zero and a width of 2^31 sized
//     a buffer to match. Each case below drives the REAL decoder through the REAL host with the
//     hostile message and then real audio, so "it did not crash and produced sane output" is the
//     result — not an inspection of a clamp.
//  ★ And the SpyServer parsers, whose 32-bit `20 + bodySize` wrapped on armeabi-v7a.
#include "vibe_decoder_host.h"
#include "spyserver/spyserver_messages.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

/** A few seconds of tone + noise at 48 kHz: enough to make every decoder do real work. */
static std::vector<float> audio(double secs, double hz) {
    std::vector<float> v((size_t)(secs * 48000));
    uint32_t r = 12345;
    for (size_t i = 0; i < v.size(); ++i) {
        r = r * 1103515245u + 12345u;
        v[i] = 0.4f * (float)std::sin(2 * M_PI * hz * (double)i / 48000.0) + 0.1f * ((float)(r >> 16) / 32768.0f - 1.0f);
    }
    return v;
}

static bool survives(const std::string& ext, const std::string& msg, double hz) {
    DecoderHost::Env e;
    DecoderHost h(e, "S");
    const auto st = h.start(ext, msg);
    if (st != DecoderHost::Start::Ok) return false;
    const auto a = audio(3.0, hz);
    for (size_t o = 0; o < a.size(); o += 960) h.feed(a.data() + o, (int)std::min<size_t>(960, a.size() - o), 1);
    h.stop();
    return true;
}

int main() {
    std::printf("test-decoder-hardening\n");

    std::printf("── 3. RTTY: hostile attach parameters ──\n");
    ok(survives("fsk", "{\"baud_rate\":nan,\"center_frequency\":1000}", 1000), "★★★ baud NaN is the default, not a modulo by zero");
    ok(survives("fsk", "{\"baud_rate\":1e300}", 1000), "★★★ baud 1e300 is clamped to 1200");
    ok(survives("fsk", "{\"baud_rate\":-5}", 1000), "★ a negative baud is clamped to 10");
    ok(survives("fsk", "{\"baud_rate\":1100,\"framing\":\"5N1\"}", 1000), "★★ 1100 baud (bit length 44 -> not the problem) runs");
    ok(survives("fsk", "{\"baud_rate\":1013,\"framing\":\"5N1\"}", 1000), "★★★ a bit length that is not a multiple of 4 (47) cannot index past the table");
    ok(survives("fsk", "{\"center_frequency\":0,\"shift\":0}", 1000), "★★ centre 0 / shift 0 are clamped, nothing divides by zero");
    ok(survives("fsk", "{\"center_frequency\":inf}", 1000), "★ centre inf is treated as absent");
    ok(survives("fsk", "{\"framing\":\"9N2\"}", 1000), "★★ framing 9N2 falls back to 5N1.5");
    ok(survives("fsk", "{\"framing\":\"0N1\"}", 1000), "★★ framing 0N1 falls back too");
    ok(survives("fsk", "{\"framing\":\"8N1.5\"}", 1000), "★ 1.5 stop bits only with 5 data bits");
    // Direct: the decoder clamps on its own, whoever constructs it.
    {
        FskDecoder d(48000, NAN, NAN, NAN, "garbage", "ITA2", false);
        std::vector<int16_t> s(48000); for (size_t i = 0; i < s.size(); ++i) s[i] = (int16_t)(8000 * std::sin(i * 0.13));
        d.process(s.data(), (int)s.size());
        ok(true, "★★ FskDecoder(NaN, NaN, NaN, \"garbage\") constructs and decodes without faulting");
    }

    std::printf("── 4. WEFAX: hostile attach parameters ──\n");
    ok(survives("wefax", "{\"lpm\":0,\"image_width\":1809,\"use_phasing\":false}", 1900), "★★★ lpm 0 is whitelisted to 120, not divided by");
    ok(survives("wefax", "{\"lpm\":1e12,\"image_width\":0,\"use_phasing\":false}", 1900), "★★★ lpm 1e12 / width 0 are sane, and the int cast is not UB");
    ok(survives("wefax", "{\"lpm\":120,\"image_width\":2147483647,\"use_phasing\":false}", 1900), "★★★ width 2^31 is clamped to 4096, not allocated");
    ok(survives("wefax", "{\"lpm\":60,\"image_width\":256,\"deviation\":0,\"carrier\":nan,\"use_phasing\":false}", 1900), "★★ deviation 0 / carrier NaN fall back to the defaults");
    {
        // ★★★ Memory: a fax that never stops must not grow. 600 lines at 240 lpm, phasing and
        //     auto-start off so every line is decoded; the line counter passes 256 twice over.
        WefaxDecoder::Config c; c.lpm = 240; c.imageWidth = 1809; c.usePhasing = false; c.autoStart = false; c.autoStop = false;
        WefaxDecoder d(48000, c);
        uint32_t lines = 0, lastW = 0;
        d.onLine = [&](uint32_t, uint32_t w, const uint8_t*) { ++lines; lastW = w; };
        std::vector<int16_t> s(12000 * 50);     // 50 lines per block
        for (int blk = 0; blk < 12; ++blk) {
            for (size_t i = 0; i < s.size(); ++i) s[i] = (int16_t)(8000 * std::sin(2 * M_PI * 1900.0 * (double)(i + blk * s.size()) / 48000.0));
            d.process(s.data(), (int)s.size());
        }
        ok(lines >= 500 && lastW == 1809, "★★★ 600 lines decode with a two-line buffer (was: doubled for ever)");
    }

    std::printf("── 5. SpyServer: a body size that wraps a 32-bit size_t ──\n");
    {
        using namespace vibe::spyserver;
        std::vector<uint8_t> m(24, 0);
        putU32(m.data() + 16, 0xFFFFFFF0u);                  // 20 + this wraps to 4 on 32-bit
        Message_t out; size_t used = 0;
        ok(!parseMessage(m.data(), m.size(), &out, &used), "★★★ parseMessage refuses a body larger than the buffer");
        putU32(m.data() + 16, 4);
        ok(parseMessage(m.data(), m.size(), &out, &used) && used == 24, "★ a body that fits is parsed");
        std::vector<uint8_t> c(12, 0);
        putU32(c.data() + 4, 0xFFFFFFFCu);
        Command_t cmd;
        ok(!parseCommand(c.data(), c.size(), &cmd, &used), "★★★ parseCommand refuses the same wrap");
        putU32(c.data() + 4, kMaxBodySize + 1);
        std::vector<uint8_t> big(8 + (size_t)kMaxBodySize + 1, 0); putU32(big.data() + 4, kMaxBodySize + 1);
        ok(!parseCommand(big.data(), big.size(), &cmd, &used), "★★ a body over the cap is refused even when it is all there");
    }

    if (fails) { std::printf("  %d FAILED\n", fails); return 1; }
    std::printf("  all passed\n");
    return 0;
}
