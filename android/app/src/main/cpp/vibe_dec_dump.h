// vibe_dec_dump.h — A DEVELOPER'S CAPTURE OF WHAT A DECODER IS FED, NOT A FEATURE (docs/v12/APRS-ON-AIR-2.md).
//
// ★★ WHY (2026-11-22): APRS packets plainly on the Pi 500's waterfall decoded nothing, and there was no way to tell a
//    deaf decoder from audio that never carried the packet. As LRPT's IQ dump (vibe_lrpt_run.h Run::Dump): with
//      VIBE_DEC_AUDIO_DUMP=<directory>          in the server's environment (OFF when unset — not even a file handle)
//      VIBE_DEC_AUDIO_DUMP_MB=<megabytes>       the cap for the whole process, every file together (default 512)
//    each decoder ATTACH on a DecoderHost writes, into that directory:
//      dec-<unix>-<ext>-<n>.wav   the exact 48 kHz mono audio the host fed its decoder (32-bit float WAV, the header
//                                 rewritten once a second so a file cut off by a restart still opens)
//      dec-<unix>-<ext>-<n>.cf32  the listener's CHANNEL IQ, when the decoder reads it (APRS AUTO / 9600, paging, VOR …):
//                                 interleaved float32 I,Q at the rate the .txt names
//      dec-<unix>-<ext>-<n>.txt   "# ext / params / session" then one line a second: t_s fed_s queued_s dropped_frames
//                                 and the server's own words for the listener's dial, mode, IF filter, squelch, gain …
//    vibeserver/tool-dec-replay.cpp feeds a .wav (and .cf32) back through a DecoderHost with the same ext and params.
// ★★ WRITTEN ON THE DECODE THREAD (vibe_decoder_host.h loop_), never the audio or DSP thread. Bounded: one cap for the
//    process (a 256 MB board's card is not filled by a forgotten variable); at the cap the files close and say so.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace vibedec {

class DecDump {
public:
    /** The directory, or "" — read ONCE per process (zero cost after: a static string compare). */
    static const std::string& dir() {
        static const std::string d = [] { const char* e = std::getenv("VIBE_DEC_AUDIO_DUMP"); return std::string(e && *e ? e : ""); }();
        return d;
    }
    static bool enabled() { return !dir().empty(); }
    static uint64_t capBytes() {
        static const uint64_t c = [] { const char* e = std::getenv("VIBE_DEC_AUDIO_DUMP_MB"); const long v = e ? std::atol(e) : 0;
                                       return uint64_t(v > 0 ? v : 512) * 1024 * 1024; }();
        return c;
    }
    /** Every file of the process together. */
    static std::atomic<uint64_t>& written() { static std::atomic<uint64_t> w{0}; return w; }

    /** One attach. `ext` the decoder ("aprs"), `params` the attach message, `session` the listener ("" = the radio's). */
    DecDump(const std::string& ext, const std::string& params, const std::string& session) {
        t0_ = nowSec();
        static std::atomic<int> seq{0};
        std::string e; for (char c : ext) e += (std::isalnum((unsigned char)c) ? c : '_');
        base_ = dir() + "/dec-" + std::to_string((long long)std::time(nullptr)) + "-" + e + "-" + std::to_string(seq.fetch_add(1));
        wav_ = std::fopen((base_ + ".wav").c_str(), "wb");
        txt_ = std::fopen((base_ + ".txt").c_str(), "w");
        if (wav_) writeHeader_();
        if (txt_) {
            std::fprintf(txt_, "# decoder audio capture (VIBE_DEC_AUDIO_DUMP): 48000 Hz mono float32 -> %s.wav\n", base_.c_str());
            std::fprintf(txt_, "# ext %s\n# session %s\n# params %s\n", ext.c_str(), session.empty() ? "(the radio's)" : session.c_str(), oneLine_(params).c_str());
            std::fprintf(txt_, "# t_s fed_audio_s queued_s dropped_frames | state\n");
            std::fflush(txt_);
        }
    }
    ~DecDump() { close_("# closed (decoder stopped)"); }
    DecDump(const DecDump&) = delete;
    DecDump& operator=(const DecDump&) = delete;

    /** The audio block the decoder is about to get (mono, 48 kHz). */
    void audio(const float* x, int n) {
        std::lock_guard<std::mutex> lk(m_);
        if (!wav_ || n <= 0) return;
        const size_t want = size_t(n) * sizeof(float);
        if (!room_(want)) return;
        const size_t w = std::fwrite(x, 1, want, wav_);
        dataBytes_ += w; written().fetch_add(w);
        fedSec_ += n / 48000.0;
    }
    /** The channel IQ block the decoder is about to get. A rate change starts a note in the .txt (the file goes on). */
    void iq(const std::complex<float>* x, int n, double rate) {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_ || n <= 0) return;
        if (!iq_) {
            iq_ = std::fopen((base_ + ".cf32").c_str(), "wb");
            if (txt_) { std::fprintf(txt_, "# channel IQ at %.3f S/s, float32 I,Q interleaved -> %s.cf32\n", rate, base_.c_str()); std::fflush(txt_); }
            iqRate_ = rate;
        } else if (std::abs(rate - iqRate_) > 0.5) {
            if (txt_) { std::fprintf(txt_, "# channel IQ rate now %.3f S/s at IQ sample %llu\n", rate, (unsigned long long)iqSamples_); std::fflush(txt_); }
            iqRate_ = rate;
        }
        if (!iq_) return;
        const size_t want = size_t(n) * sizeof(std::complex<float>);
        if (!room_(want)) return;
        const size_t w = std::fwrite(x, 1, want, iq_);
        written().fetch_add(w);
        iqSamples_ += uint64_t(n);
    }
    static double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    /** Once a second or so (the decode thread, no host lock held): the queue's figures and the server's state line.
     *  `nowSteady` = nowSec(); the line says seconds since the attach. */
    void tick(double nowSteady, double queuedSec, uint64_t droppedFrames, const std::string& state) {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return;
        if (wav_) { writeHeader_(); std::fflush(wav_); }
        if (iq_) std::fflush(iq_);
        if (txt_) { std::fprintf(txt_, "%.1f %.2f %.2f %llu | %s\n", nowSteady - t0_, fedSec_, queuedSec, (unsigned long long)droppedFrames, oneLine_(state).c_str()); std::fflush(txt_); }
    }
    double fedSec() const { return fedSec_; }
    const std::string& base() const { return base_; }

    /** ★ For the replay tool and the tests: a float32 or int16 WAV's samples (first channel) and rate. */
    static bool readWav(const std::string& path, std::vector<float>& out, int& rate) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<uint8_t> b; uint8_t tmp[65536]; size_t n;
        while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
        std::fclose(f);
        if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return false;
        auto u32 = [&](size_t p) { return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24; };
        auto u16 = [&](size_t p) { return uint16_t(b[p] | b[p + 1] << 8); };
        size_t p = 12; uint16_t fmt = 0, ch = 1, bits = 0; rate = 0; size_t at = 0, len = 0;
        while (p + 8 <= b.size()) {
            const uint32_t l = u32(p + 4);
            if (!std::memcmp(&b[p], "fmt ", 4) && p + 24 <= b.size()) { fmt = u16(p + 8); ch = u16(p + 10); rate = int(u32(p + 12)); bits = u16(p + 22); }
            if (!std::memcmp(&b[p], "data", 4)) { at = p + 8; len = std::min<size_t>(l, b.size() - at); break; }   // a cut-off file: what is there
            p += 8 + l + (l & 1);
        }
        if (!at || !rate || !ch) return false;
        out.clear();
        if (fmt == 3 && bits == 32) { const size_t fr = len / (4 * ch); out.resize(fr); for (size_t i = 0; i < fr; ++i) std::memcpy(&out[i], &b[at + i * 4 * ch], 4); }
        else if (fmt == 1 && bits == 16) { const size_t fr = len / (2 * ch); out.resize(fr); for (size_t i = 0; i < fr; ++i) out[i] = int16_t(u16(at + i * 2 * ch)) / 32768.0f; }
        else return false;
        return true;
    }

private:
    bool room_(size_t want) {
        if (written().load() + want <= capBytes()) return true;
        close_("# cap reached (VIBE_DEC_AUDIO_DUMP_MB)");
        return false;
    }
    void close_(const char* why) {
        if (closed_) return;
        closed_ = true;
        if (wav_) { writeHeader_(); std::fclose(wav_); wav_ = nullptr; }
        if (iq_) { std::fclose(iq_); iq_ = nullptr; }
        if (txt_) { std::fprintf(txt_, "%s — %.1f s of audio, %llu IQ samples\n", why, fedSec_, (unsigned long long)iqSamples_); std::fclose(txt_); txt_ = nullptr; }
    }
    /** A 32-bit float WAV header (WAVE_FORMAT_IEEE_FLOAT) for the data so far, written over the start of the file. */
    void writeHeader_() {
        const uint32_t rate = 48000, data = uint32_t(std::min<uint64_t>(dataBytes_, 0xFFFFFFF0ull));
        uint8_t h[44];
        auto p32 = [&](int at, uint32_t v) { h[at] = uint8_t(v); h[at + 1] = uint8_t(v >> 8); h[at + 2] = uint8_t(v >> 16); h[at + 3] = uint8_t(v >> 24); };
        auto p16 = [&](int at, uint16_t v) { h[at] = uint8_t(v); h[at + 1] = uint8_t(v >> 8); };
        std::memcpy(h, "RIFF", 4); p32(4, 36 + data); std::memcpy(h + 8, "WAVEfmt ", 8); p32(16, 16); p16(20, 3); p16(22, 1);
        p32(24, rate); p32(28, rate * 4); p16(32, 4); p16(34, 32); std::memcpy(h + 36, "data", 4); p32(40, data);
        const long here = std::ftell(wav_);
        std::fseek(wav_, 0, SEEK_SET); std::fwrite(h, 1, 44, wav_);
        std::fseek(wav_, here > 44 ? here : 44, SEEK_SET);
    }
    static std::string oneLine_(const std::string& s) { std::string o; for (char c : s) o += (c == '\n' || c == '\r') ? ' ' : c; return o.substr(0, 2000); }

    std::mutex m_;
    std::string base_;
    FILE* wav_ = nullptr; FILE* iq_ = nullptr; FILE* txt_ = nullptr;
    uint64_t dataBytes_ = 0, iqSamples_ = 0;
    double fedSec_ = 0, iqRate_ = 0, t0_ = 0;
    bool closed_ = false;
};

} // namespace vibedec
