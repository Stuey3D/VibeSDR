// test-dsc.cpp — maritime DSC (decoders/dsc_decoder.h), measured, and its plumbing (vibe_dsc_run.h through
// vibe_decoder_host.h, and the gallery's DSC log and alerts in vibe_dsc_gallery.h).
//
// ★★★ WHAT IS PINNED (ITU-R M.493; synthetic calls whose every symbol is known — tools/dsc-bench/dsc_synth.h):
//   • the 10-bit code: 128 words, each word's check bits, and nothing else decodes;
//   • the ECC, and every format's readable line — distress alert, acknowledgement, relay (all ships and individual),
//     urgency, safety, geographic area, group, individual with a VHF channel, test call and its acknowledgement, a
//     position reply, unable to comply, semi-automatic with a number, and a distress alert with an expansion message;
//   • the MID table: the countries M.585 names, coast stations, groups, SAR aircraft, SARTs;
//   • decode rate against SNR on MF/HF and VHF, in both tone senses, with a slow Rayleigh fade, and a 30 ppm clock
//     error — and NO WRONG CALL passed as clean: a call is either right or not shown (or marked UNCONFIRMED);
//   • TIME DIVERSITY: a call whose DX copies are blotted out by a burst still decodes from its RX copies, and the other
//     way round; one copy in a deep fade is outvoted by the soft sum;
//   • THE CHECK CHARACTER CORRECTS one weak symbol and does not "correct" a call into a different one;
//   • pure noise makes NO call (MF/HF and VHF, 10 minutes each);
//   • "dsc" through a DecoderHost: the text line, the JSON with its RF, repeats folded into one call; and the gallery's
//     day log and distress item (with its map pin).
// ★ Silent: nothing is played. Deterministic: fixed seeds.
#include "vibe_decoder_host.h"
#include "vibe_bgdecode.h"
#include "vibe_maritime_mid.h"
#include "../tools/dsc-bench/dsc_synth.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <thread>

using namespace vibe;
using namespace vibe::dsc;
using namespace dscsynth;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static std::vector<Call> decodeAudio(const std::vector<float>& a, bool vhf, double rate = 48000, Receiver::Stats* st = nullptr) {
    RxConfig cfg; cfg.vhf = vhf; cfg.rate = rate;
    Receiver rx(cfg);
    std::vector<Call> got;
    rx.onCall = [&](const Call& c) { got.push_back(c); };
    for (size_t i = 0; i < a.size(); i += 4096) rx.process(a.data() + i, (int)std::min<size_t>(4096, a.size() - i));
    std::vector<float> z((size_t)rate, 0.0f); rx.process(z.data(), (int)z.size());
    if (st) *st = rx.stats();
    return got;
}

struct Score { int right = 0, wrongClean = 0, unconf = 0, missed = 0, n = 0; };
static Score sweep(bool vhf, double snr, int trials, double fade, bool invert, uint64_t seed) {
    Score s;
    Rng rng(seed);
    for (int t = 0; t < trials; ++t)
        for (const auto& tc : calls()) {
            auto bits = build::transmission(tc.syms, tc.exp, vhf ? 20 : 200);
            if (invert) for (auto& b : bits) b ^= 1;
            const auto a = render(bits, vhf, snr, rng, 0.3 + rng.uni(), 0.6, fade, 1700, (rng.uni() - 0.5) * 60);
            const auto got = decodeAudio(a, vhf);
            ++s.n;
            if (got.empty()) { ++s.missed; continue; }
            const Call& c = got[0];
            if (c.symbols == tc.syms) { if (c.check == Call::Check::Failed) ++s.unconf; else ++s.right; }
            else if (c.check == Call::Check::Failed) ++s.unconf;
            else ++s.wrongClean;
        }
    return s;
}

/** A decoder socket that remembers what it was sent. */
struct Peer : DecoderPeer {
    std::mutex m; std::vector<std::vector<uint8_t>> bin; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::string decoded() {
        std::lock_guard<std::mutex> lk(m); std::string o;
        for (auto& f : bin) if (f.size() >= 13 && f[0] == 0x01) o.append((const char*)f.data() + 13, f.size() - 13);
        return o;
    }
    int count(const std::string& needle) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& t : txt) n += t.find(needle) != std::string::npos; return n; }
};
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/dsc-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::string slurp(const std::string& path) { std::ifstream in(path); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static std::string slurpTxt(const std::string& dir) {
    std::string all;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string f = e->d_name;
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".txt") == 0) all += slurp(dir + "/" + f) + "\n";
        }
        closedir(d);
    }
    return all;
}
static void rmTree(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) { const std::string f = e->d_name; if (f != "." && f != "..") ::unlink((dir + "/" + f).c_str()); }
        closedir(d);
    }
    ::rmdir(dir.c_str());
}

int main() {
    std::printf("── the code (M.493 §3)\n");
    {
        bool all = true, unique = true;
        int valid = 0;
        for (int v = 0; v < 128; ++v) { if (decodeWord(codeword(v)) != v) all = false; }
        for (int w = 0; w < 1024; ++w) if (decodeWord((uint16_t)w) >= 0) ++valid;
        unique = valid == 128;
        ok(all && unique, "128 valid words, each decodes to itself, no other word decodes");
        // symbol 0: seven 0s → check 7 = 111 sent MSB first; symbol 127: no 0s → check 000
        ok(codeword(0) == 0x380 && codeword(127) == 0x07F, "the check bits count the 0s, most significant first (0 → 0000000 111, 127 → 1111111 000)");
        ok(codeword(125) == (125 | (1 << 9)), "phasing DX 125 (one 0 → check 001)");
        std::vector<int> s{ 120, 23, 50, 0, 0, 40, 108, 23, 20, 1, 23, 40, 118, 126, 126, 126, 126, 126, 126, 126, 117 };
        int x = 0; for (int v : s) x ^= v;
        ok(eccOf(s) == x, "the ECC is the XOR of the format specifier, the message and the EOS");
    }

    std::printf("── every format's readable line (MF/HF and VHF, clean)\n");
    for (int band = 0; band < 2; ++band) {
        const bool vhf = band == 1;
        Rng rng(1);
        int good = 0, n = 0;
        for (const auto& tc : calls()) {
            ++n;
            const auto bits = build::transmission(tc.syms, tc.exp, vhf ? 20 : 200);
            const auto got = decodeAudio(render(bits, vhf, 30, rng), vhf);
            bool g = got.size() == 1 && got[0].symbols == tc.syms && got[0].check == Call::Check::Ok;
            const std::string line = got.empty() ? "" : describe(got[0]);
            for (const auto& e : tc.expect) if (line.find(e) == std::string::npos) g = false;
            if (!g) std::printf("     %s: %s\n", tc.name.c_str(), line.empty() ? "(nothing)" : line.c_str());
            good += g;
        }
        ok(good == n, std::string(vhf ? "VHF" : "MF/HF") + ": all " + std::to_string(n) + " formats read exactly, every expected phrase in the line");
    }
    {
        const auto cs = calls();
        Call c; std::string why;
        ok(parse(cs[0].syms, c) && c.level() == 3 && c.pos.known && std::fabs(c.pos.lat - 50.2) < 0.01 && std::fabs(c.pos.lon + 4.1667) < 0.01,
           "a distress alert: level 3, its position signed (50.2, -4.167)");
        std::vector<int> bad = cs[0].syms; bad[5] = 41;   // the MMSI's tenth digit 1
        ok(!parse(bad, c, &why), "an MMSI field whose tenth digit is not 0 is refused (" + why + ")");
        std::vector<int> cut(cs[5].syms.begin(), cs[5].syms.end() - 4); cut.push_back(127);
        ok(!parse(cut, c, &why), "a call cut short (one frequency field) is refused, not read as a shorter call (" + why + ")");
    }

    std::printf("── the MID table (M.585)\n");
    {
        std::string iso;
        ok(mid::countryOf("235012345", &iso) == "United Kingdom" && iso == "GB", "235 → United Kingdom (GB)");
        ok(mid::countryOf("002320004") == "United Kingdom" && describeMmsi("002320004") == "coast station 002320004 (United Kingdom)", "00 232 … → a UK coast station");
        ok(describeMmsi("023200001") == "group 023200001 (United Kingdom)", "0 232 … → a UK group");
        ok(describeMmsi("111232001") == "SAR aircraft 111232001 (United Kingdom)", "111 232 … → a UK SAR aircraft");
        ok(describeMmsi("970123456") == "AIS-SART 970123456", "970 … → an AIS-SART, no country");
        ok(mid::countryOf("217000000") == "MID 217", "a MID the table lacks is named by number, never guessed");
        size_t n = 0; const mid::Row* t = mid::table(n);
        bool sorted = true; for (size_t i = 1; i < n; ++i) sorted &= t[i - 1].mid < t[i].mid;
        ok(sorted && n > 250, "the table is in MID order (" + std::to_string(n) + " MIDs)");
        ok(mid::flag("GB") == "\xf0\x9f\x87\xac\xf0\x9f\x87\xa7", "GB → 🇬🇧");
    }

    std::printf("── decode rate against SNR (3 kHz), 6 draws × 16 formats a point\n");
    {
        struct Pt { bool vhf; double snr, fade; bool inv; int minPct; const char* what; };
        const Pt pts[] = {
            { false,  -6, 0,   false, 98, "MF/HF −6 dB" },
            { false,  -8, 0,   false, 90, "MF/HF −8 dB" },
            { false,  -8, 0,   true,  90, "MF/HF −8 dB, the other tone sense (LSB)" },
            { false,  -3, 0.5, false, 65, "MF/HF −3 dB, Rayleigh fade 0.5 Hz" },
            { true,    6, 0,   false, 98, "VHF +6 dB" },
            { true,    3, 0,   false, 88, "VHF +3 dB" },
        };
        int wrong = 0, n = 0;
        uint64_t seed = 900;
        for (const Pt& p : pts) {
            const Score s = sweep(p.vhf, p.snr, 6, p.fade, p.inv, seed++);
            const int pct = 100 * (s.right + s.unconf) / std::max(1, s.n);
            wrong += s.wrongClean; n += s.n;
            ok(pct >= p.minPct, std::string(p.what) + ": " + std::to_string(pct) + " % decoded (≥ " + std::to_string(p.minPct) + " %), "
               + std::to_string(s.wrongClean) + " wrong");
        }
        ok(wrong <= 1, "wrong calls passed as clean across all " + std::to_string(n) + " trials: " + std::to_string(wrong) + " (≤ 1)");
    }

    std::printf("── time diversity and the check character\n");
    {
        const auto cs = calls();
        const TestCall& tc = cs[0];   // the distress alert
        auto bits = build::transmission(tc.syms, tc.exp, 200);
        // ★ One stream FADED OUT (a selective fade, a gated interferer): every DX — or every RX — slot of the message at
        //   2 % amplitude in noise. The other stream alone must carry the call.
        const double spb = 480.0;                      // 48 kHz / 100 Bd
        const size_t lead = (size_t)(0.5 * 48000);
        Rng rng(6);
        auto faded = [&](int parity) {
            std::vector<float> a = render(bits, false, 6, rng, 0.5, 0.6);
            const size_t slots = (bits.size() - 200) / 10;
            for (size_t j = 12; j < slots; ++j) {
                if ((int)(j & 1) != parity) continue;
                for (size_t i = lead + (size_t)((200 + j * 10) * spb); i < lead + (size_t)((200 + j * 10 + 10) * spb) && i < a.size(); ++i) a[i] *= 0.02f;
            }
            return a;
        };
        auto g1 = decodeAudio(faded(0), false);
        ok(!g1.empty() && g1[0].symbols == tc.syms, "every DX copy faded out: the RX copies carry the call");
        auto g2 = decodeAudio(faded(1), false);
        ok(!g2.empty() && g2[0].symbols == tc.syms, "every RX copy faded out: the DX copies carry it");
        // one symbol's both copies weakened to near nothing — the check character puts it right
        auto b3 = bits;
        const size_t m = 4;                            // message index of the 4th field symbol (an MMSI pair)
        const size_t dxSlot = 12 + 2 * m, rxSlot = 17 + 2 * m;
        std::vector<float> a = render(b3, false, 25, rng, 0.5, 0.6);
        for (size_t slot : { dxSlot, rxSlot })
            for (size_t i = lead + (size_t)((200 + slot * 10) * spb); i < lead + (size_t)((200 + slot * 10 + 10) * spb) && i < a.size(); ++i) a[i] *= 0.02f;
        auto g3 = decodeAudio(a, false);
        ok(!g3.empty() && g3[0].symbols == tc.syms, std::string("one symbol faded out in BOTH copies: read anyway (") +
           (g3.empty() ? "nothing" : g3[0].check == Call::Check::Corrected ? "corrected by the check character" : "clean") + ")");
    }

    std::printf("── no calls from noise, at 48 kHz and from a 12 kHz recording rate\n");
    {
        for (int band = 0; band < 2; ++band) {
            const bool vhf = band == 1;
            RxConfig cfg; cfg.vhf = vhf;
            Receiver rx(cfg);
            int n = 0; rx.onCall = [&](const Call&) { ++n; };
            Rng rng(77 + band);
            std::vector<float> buf(48000);
            for (int s = 0; s < 600; ++s) { for (auto& v : buf) v = (float)(0.3 * rng.gauss()); rx.process(buf.data(), (int)buf.size()); }
            ok(n == 0, std::string(vhf ? "VHF" : "MF/HF") + ": 10 min of noise → " + std::to_string(n) + " calls (" + std::to_string(rx.stats().falseSyncs) + " phasing syncs rejected)");
        }
        // a recording at 12 kHz (the bench's UberSDR recordings) decodes the same
        Rng rng(8);
        const auto cs = calls();
        const auto bits = build::transmission(cs[6].syms, {}, 200);
        const auto a = render(bits, false, -3, rng, 0.5, 0.6, 0, 1700, 0, 12000);
        auto g = decodeAudio(a, false, 12000);
        ok(!g.empty() && g[0].symbols == cs[6].syms, "MF/HF at a 12 kHz input rate");
    }

    std::printf("── a real recording (Stuart's UberSDR, 8414.5 kHz, 2026-10-26 07:52 UTC — ~/VibeSDR-test-audio/dsc)\n");
    {
        const char* home = std::getenv("HOME");
        const std::string base = std::string(home ? home : "") + "/VibeSDR-test-audio/dsc/dsc_8414500_20261026T0752";
        std::ifstream f(base + ".wav", std::ios::binary);
        if (!f) std::printf("  [skip] not here (%s.wav)\n", base.c_str());
        else {
            std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // a plain 44-byte header from tools/ubersdr-rec: 12 kHz mono int16
            const uint32_t rate = *(const uint32_t*)(d.data() + 24);
            std::vector<float> a((d.size() - 44) / 2);
            for (size_t i = 0; i < a.size(); ++i) a[i] = *(const int16_t*)(d.data() + 44 + 2 * i) / 32768.0f;
            const auto got = decodeAudio(a, false, rate);
            std::string ref = slurp(base + ".decoded.txt");
            int found = 0;
            for (const auto& c : got) if (ref.find(describe(c)) != std::string::npos) ++found;
            int refN = 0; for (char ch : ref) refN += ch == '\n';
            ok(found >= refN && (int)got.size() == refN, std::to_string(got.size()) + " calls, " + std::to_string(found) + " of the "
               + std::to_string(refN) + " recorded on 2026-10-26 read word for word (test calls to Australia, the USCG, Spain …)");
        }
    }

    std::printf("── \"dsc\" through a DecoderHost\n");
    {
        DecoderHost::Env e;
        e.dialHz = [] { return 2185800.0; };
        e.rfOfAudio = [](double a) { return 2185800.0 + a; };   // USB
        DecoderHost h(e, "s1");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        ok(h.start("dsc", "{\"band\":\"hf\",\"center_frequency\":1700}") == DecoderHost::Start::Ok && h.kind() == "dsc" && h.wantsAudio(), "\"dsc\" starts on audio");
        Rng rng(11);
        const auto cs = calls();
        std::vector<float> a;
        for (int k = 0; k < 2; ++k) {                 // the distress alert, twice (a repeat), then the safety call
            const auto r1 = render(build::transmission(cs[0].syms, {}, 200), false, 6, rng, 0.5, 0.5);
            a.insert(a.end(), r1.begin(), r1.end());
        }
        const auto r2 = render(build::transmission(cs[6].syms, {}, 200), false, 6, rng, 0.5, 1.0);
        a.insert(a.end(), r2.begin(), r2.end());
        for (size_t i = 0; i < a.size(); i += 1024) {
            h.feed(a.data() + i, (int)std::min<size_t>(1024, a.size() - i), 1, NAN);
            while (h.queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<float> z(48000, 0.0f); h.feed(z.data(), (int)z.size(), 1, NAN);
        for (int w = 0; w < 300 && h.queued(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string txt = p->decoded();
        ok(txt.find("2187.5 kHz  DISTRESS \xc2\xb7 MMSI 235012345 (United Kingdom)") != std::string::npos, "the text line names the DSC frequency (dial + 1700 Hz) and the call");
        ok(p->count("\"type\":\"dsc_call\"") == 3, "three dsc_call frames: the alert, its repeat, the safety call");
        ok(p->count("\"repeat\":2") == 1 && p->count("\"id\":1,") == 2, "the repeat is the same call (id 1), counted twice");
        ok(p->count("\"rf\":2187500") == 3 && p->count("\"lat\":50.2") == 2, "each carries its RF; the distress alert (and its repeat) its position");
        int lines = 0; for (size_t i = 0; (i = txt.find('\n', i)) != std::string::npos; ++i) ++lines;
        ok(lines == 2, "the repeat adds no second text line (" + std::to_string(lines) + " lines)");
        h.shutdown();
    }

    std::printf("── the gallery: the day's log and a distress item\n");
    {
        gallery::Store st; const std::string dir = tmpDir("gal"); st.setDir(dir); st.setLimitBytes(10u << 20);
        gallery::Meta m; m.kind = "dsc"; m.origin = "background"; m.freqHz = 2187500; m.mode = "usb"; m.decoder = "dsc"; m.profile = "Profile 1";
        {
            auto tap = bg::makeGalleryTap(st, m);
            ok(tap != nullptr, "kind \"dsc\" has a gallery tap");
            const auto cs = calls();
            Call c0, c6, c5;
            parse(cs[0].syms, c0); parse(cs[6].syms, c6); parse(cs[5].syms, c5);
            tap->dscCall(c6, describe(c6), "", false);
            tap->dscCall(c0, describe(c0), "", false);
            tap->dscCall(c0, describe(c0), "", true);   // a repeat: nothing new
            tap->dscCall(c5, describe(c5), "", false);
            tap->finish(false);
        }
        const std::string all = slurpTxt(dir);
        const std::string idx = slurp(dir + "/index.jsonl");
        ok(all.find("SAFETY \xc2\xb7 all ships") != std::string::npos && all.find("  2187.5 kHz  DISTRESS") != std::string::npos,
           "the day's log holds each call's line, with its time and frequency");
        int dl = 0; for (size_t i = 0; (i = all.find("MMSI 235012345", i)) != std::string::npos; ++i) ++dl;
        ok(dl == 2, "the distress call once in the log and once in its own item (the repeat is not written)");
        ok(idx.find("\"lv\":3") != std::string::npos && idx.find("\"plat\":50.2") != std::string::npos && idx.find("\"plon\":-4.16") != std::string::npos,
           "the distress item is marked level 3 and pinned at its position");
        ok(idx.find("3 calls \xc2\xb7 1 distress \xc2\xb7 1 urgency") != std::string::npos, "the log's title counts the day (3 calls · 1 distress · 1 urgency)");
        ok(idx.find("\"dn\":\"DSC 2187.5 ") != std::string::npos, "the log downloads as \"DSC 2187.5 <date>.txt\"");
        ok(bg::galleryKindFor("dsc", "{}") == "dsc", "galleryKindFor(\"dsc\") files it as DSC");
        rmTree(dir);
    }

    std::printf("── background decoding's table\n");
    {
        const bg::DecoderDef* d = bg::decoderDef("dsc");
        const bg::DecoderDef* v = bg::decoderDef("dsc-vhf");
        ok(d && v && bg::decoderFitsDemod("dsc", "usb") && bg::decoderFitsDemod("dsc", "lsb") && !bg::decoderFitsDemod("dsc", "nfm")
           && bg::decoderFitsDemod("dsc-vhf", "nfm") && !bg::decoderFitsDemod("dsc-vhf", "usb"), "DSC on USB/LSB, DSC VHF on NFM only");
        bg::Vfo f; f.hz = 2185800; f.mode = "usb"; f.dec = "dsc";
        ok(std::fabs(bg::signalHz(f) - 2187500) < 1, "a USB VFO at 2185.8 kHz listens for 2187.5 kHz");
        ok(bg::attachMsg(f).find("\"band\":\"hf\"") != std::string::npos, "its attach message asks for MF/HF");
        f.mode = "nfm"; f.dec = "dsc-vhf"; f.hz = 156525000;
        ok(bg::attachMsg(f) == "{\"band\":\"vhf\"}", "channel 70's asks for VHF");
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
