// test-navtex-quality.cpp — beta96 (Stuart 11-26): a NAVTEX message's DECODE QUALITY and SNR TIMELINE as the server's
// gallery keeps them (vibe_gallery.h NavtexAsm + navtexQualityJson / navtexQualityLine / navtexSnrLine; vibe_bgdecode.h's
// tap reads ops 0x07 and 0x08). DISPLAY ONLY — nothing here touches the decoder.
//  · slices tied to the text: each slice's character END in the cleaned text; a mid-message fade = the right lines red;
//  · redPct (characters decoded under kSnrRed, 9 dB since the shared scale) and minSnr; an all-green message has 0 % weak;
//  · the slice cap (≤ 120, pairs merged, the slice length doubled);
//  · a fade's lock loss: its slices cover the gap marker line;
//  · the tap end to end: 0x08 slices + text + the 21-byte 0x07 → an index line with nqc/nqf/nql/nqd, snr, tl, tc, and the
//    file's "Decode quality:" / "SNR" lines; the old 13-byte 0x07 → no split, the old title, nothing invented;
//  · an OLD record (no fields) lists exactly as before.
#include "vibe_bgdecode.h"
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "/tmp/vibe-navq-%s-%d", tag, (int)getpid());
    std::string cmd = std::string("rm -rf ") + t + " && mkdir -p " + t; if (std::system(cmd.c_str()) != 0) {}
    return t;
}
static std::string ints(const std::vector<int>& v) { std::string o; for (int x : v) o += std::to_string(x) + " "; return o; }

int main() {
    std::printf("── 1. slices tied to the text, a fade in the middle ──\n");
    {
        gallery::NavtexAsm a; std::vector<gallery::NavtexMsg> got;
        a.onMessage = [&](const gallery::NavtexMsg& m) { got.push_back(m); };
        int64_t t = 1000000;
        a.slice(-30, -50);                                  // between messages: dropped (nothing to tie it to)
        a.push("ZCZC GA01\r\n", t += 1000);
        a.slice(-30, -50);                                  // 20 dB — the header's second
        a.push("LINE ONE GREEN\r\n", t += 1000); a.slice(-30, -50);
        a.push("LINE TWO RED\r\n", t += 1000);   a.slice(-30, -32);   // 2 dB
        a.push("LINE THREE RED\r\n", t += 1000); a.slice(-30, -33);   // 3 dB
        a.push("LINE FOUR GREEN\r\n", t += 1000); a.slice(-30, -45);  // 15 dB
        a.push("NNNN\r\n", t += 1000);
        ok(got.size() == 1 && got[0].haveSnr, "one message, measured");
        if (!got.empty()) {
            const auto& m = got[0];
            ok(m.tl.size() == 5 && m.tl[0] == 20 && m.tl[2] == 2 && m.tl[3] == 3 && m.tl[4] == 15, "slices (dB): " + ints(m.tl));
            const size_t two = m.text.find("LINE TWO"), three = m.text.find("LINE THREE"), four = m.text.find("LINE FOUR");
            ok(m.tc.size() == 5 && (size_t)m.tc[1] == two && (size_t)m.tc[2] == three && (size_t)m.tc[3] == four && (size_t)m.tc[4] == m.text.size(),
               "each slice ENDS where the next line starts in the cleaned text: " + ints(m.tc));
            // the characters decoded under kSnrRed (9 dB): LINE TWO RED + LINE THREE RED (newlines not counted)
            int red = 0, all = 0; for (char c : m.text) if (!std::isspace((unsigned char)c)) all++;
            for (char c : std::string("LINE TWO RED") + "LINE THREE RED") if (!std::isspace((unsigned char)c)) red++;
            ok(m.redPct == (int)std::lround(100.0 * red / all), "redPct " + std::to_string(m.redPct) + " % (want " + std::to_string((int)std::lround(100.0 * red / all)) + ")");
            ok(m.minSnr == 2, "minSnr " + std::to_string(m.minSnr) + " dB");
            // mean powers, not mean dB: noise (2×1e-5 + 1e-3.2 + 1e-3.3 + 10^-4.5) / 5 → the red seconds dominate
            const double nz = 10 * std::log10((2 * 1e-5 + std::pow(10, -3.2) + std::pow(10, -3.3) + std::pow(10, -4.5)) / 5);
            ok(std::fabs(m.sigDbfs + 30) < 0.01 && std::fabs(m.snr - (-30 - nz)) < 0.01, "mean SNR from mean POWERS: " + std::to_string(m.snr) + " dB (signal -30 dBFS)");
            const std::string j = gallery::navtexQualityJson(m);
            ok(j.find("\"red\":") != std::string::npos && j.find("\"tl\":[20,") != std::string::npos && j.find("\"tc\":[") != std::string::npos
               && j.find("\"nqc\"") == std::string::npos, "index members: the SNR ones, no split (no 0x07 seen): " + j.substr(0, 90));
            ok(gallery::NavtexAsm::fileText(m, "").find("SNR 6 dB avg \xc2\xb7 lowest 2 dB \xc2\xb7 47 % weak") != std::string::npos, "the file says: " + gallery::navtexSnrLine(m));
        }
        got.clear();
        a.push("ZCZC GA02\r\n", t += 1000); a.slice(-30, -45);
        a.push("ALL GREEN\r\n", t += 1000); a.slice(-30, -44);
        a.push("NNNN\r\n", t += 1000);
        ok(got.size() == 1 && got[0].redPct == 0 && got[0].minSnr == 14, "an all-green message: 0 % weak, lowest 14 dB");
    }
    std::printf("── 2. the cap ──\n");
    {
        gallery::NavtexAsm a; std::vector<gallery::NavtexMsg> got;
        a.onMessage = [&](const gallery::NavtexMsg& m) { got.push_back(m); };
        int64_t t = 2000000;
        a.push("ZCZC GA03\r\n", t);
        for (int i = 0; i < 300; i++) { a.push("WORD" + std::to_string(i) + " ", t += 1000); a.slice(-30, i >= 140 && i < 160 ? -31 : -45); }
        a.push("\r\nNNNN\r\n", t += 1000);
        ok(got.size() == 1 && got[0].tl.size() <= gallery::kMaxSlices && got[0].tl.size() >= 60, "300 s → " + std::to_string(got.empty() ? 0 : got[0].tl.size()) + " slices (≤ 120)");
        ok(got.size() == 1 && got[0].sliceSec == 4, "...each " + std::to_string(got.empty() ? 0 : got[0].sliceSec) + " s long (merged twice: 300 s / 4 = 75)");
        ok(got.size() == 1 && got[0].tl.size() == 75, "...75 of them, all the same length (" + std::to_string(got.empty() ? 0 : got[0].tl.size()) + ")");
        bool mono = true; for (size_t i = 1; !got.empty() && i < got[0].tc.size(); i++) mono = mono && got[0].tc[i] >= got[0].tc[i - 1];
        ok(mono, "...character ends still in order");
        int red = 0; for (int v : got.empty() ? std::vector<int>() : got[0].tl) red += v < gallery::kSnrRed;
        ok(red == 5, "...the 20 s fade still reads red after merging (" + std::to_string(red) + " slices of 4 s)");
    }
    std::printf("── 3. a fade that cost the lock: its slices cover the gap marker ──\n");
    {
        gallery::NavtexAsm a; std::vector<gallery::NavtexMsg> got;
        a.onMessage = [&](const gallery::NavtexMsg& m) { got.push_back(m); };
        int64_t t = 3000000;
        a.lockState(3, t);
        a.push("ZCZC VA28\r\nDOVER STRAIT\r\n", t += 1000); a.slice(-30, -45);
        a.lockState(1, t += 100); a.push("XQ_", t += 500); a.slice(-30, -28); a.slice(-30, -29);
        a.lockState(3, t += 8000);
        a.push("THAMES\r\nNNNN\r\n", t += 1000); a.slice(-30, -45);
        ok(got.size() == 1 && got[0].text.find("signal lost") != std::string::npos, "one message, with its gap marker");
        if (!got.empty()) {
            const auto& m = got[0];
            const size_t g0 = m.text.find("[\xe2\x80\xa6"), g1 = m.text.find('\n', g0);
            bool covered = false;
            for (size_t i = 0; i < m.tl.size(); i++) if (m.tl[i] < 0 && (size_t)m.tc[i] >= g1 - 1 && (i == 0 || (size_t)m.tc[i - 1] <= g0 + 1)) covered = true;
            ok(covered, "the fade's slices (−2, −1 dB) end at the marker line: tl " + ints(m.tl) + "tc " + ints(m.tc));
        }
    }
    std::printf("── 4. the gallery tap, end to end ──\n");
    {
        gallery::Store s; s.setDir(tmpDir("tap"));
        gallery::Meta m; m.kind = "navtex"; m.origin = "background"; m.freqHz = 517500; m.mode = "usb"; m.decoder = "navtex";
        auto be32 = [](uint32_t v, std::vector<uint8_t>& o) { o.push_back(v >> 24); o.push_back(v >> 16); o.push_back(v >> 8); o.push_back(v); };
        auto slice = [](int sig10, int noise10) { return std::vector<uint8_t>{ 0x08, (uint8_t)((uint16_t)sig10 >> 8), (uint8_t)sig10, (uint8_t)((uint16_t)noise10 >> 8), (uint8_t)noise10 }; };
        {
            auto tap = bg::makeGalleryTap(s, m);
            tap->text("ZCZC GA42\r\n"); auto f = slice(-300, -450); tap->frame(f.data(), f.size());
            tap->text("GALE WARNING DOVER\r\n"); f = slice(-300, -320); tap->frame(f.data(), f.size());
            tap->text("NNNN\r\n");
            std::vector<uint8_t> c{ 0x07 }; be32(40, c); be32(2, c); be32(1, c); be32(38, c); be32(1, c);   // clean repaired lost agreed disagreed
            tap->frame(c.data(), c.size());
            tap->finish(false);
        }
        const std::string l = s.listJson("navtex", 10);
        ok(l.find("\"nqc\":38,\"nqf\":4,\"nql\":1,\"nqd\":1") != std::string::npos, "index: 38 clean, 4 FEC (40 + 2 − 38), 1 lost, 1 disagreed");
        ok(l.find("\"snr\":") != std::string::npos && l.find("\"tl\":[15,2]") != std::string::npos && l.find("\"red\":") != std::string::npos, "index: snr, the timeline [15,2], red");
        std::string name; { const size_t p = l.find("\"f\":\""); if (p != std::string::npos) name = l.substr(p + 5, l.find('"', p + 5) - p - 5); }
        std::string path, ct, body;
        if (s.resolve(name, path, ct)) { FILE* fp = std::fopen(path.c_str(), "r"); char b[4096]; size_t n = fp ? std::fread(b, 1, sizeof b, fp) : 0; if (fp) std::fclose(fp); body.assign(b, n); }
        ok(body.find("Decode quality: 88.4 % clean \xc2\xb7 9.3 % FEC \xc2\xb7 2.3 % lost (43 characters; copies disagreed 2.3 %)") != std::string::npos,
           "the file: " + body.substr(body.find("Decode"), 100));
        ok(body.find("\nSNR ") != std::string::npos, "...and its SNR line");
        // ★ an older server: the 13-byte 0x07, no slices → the old figures only
        gallery::Store s2; s2.setDir(tmpDir("old"));
        {
            auto tap = bg::makeGalleryTap(s2, m);
            tap->text("ZCZC GA43\r\nGALE\r\nNNNN\r\n");
            std::vector<uint8_t> c{ 0x07 }; be32(10, c); be32(2, c); be32(0, c);
            tap->frame(c.data(), c.size()); tap->finish(false);
        }
        const std::string l2 = s2.listJson("navtex", 10);
        ok(l2.find("2 repaired") != std::string::npos && l2.find("\"nqc\"") == std::string::npos && l2.find("\"snr\"") == std::string::npos && l2.find("\"tl\"") == std::string::npos,
           "an older server's 13-byte count: the old title, no split, no SNR — nothing invented");
    }

    if (fails) { std::printf("  %d FAILED\n", fails); return 1; }
    std::printf("  all passed\n");
    return 0;
}
