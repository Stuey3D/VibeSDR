// vibe_paging_run.h — the PAGING receiver inside a decoder host (vibe_decoder_host.h, ext "paging"). V12, 2026-10-31.
//
// ★★★ ANDROID LOCAL HARDWARE ONLY (Stuart's spec, docs/v12/DECODERS-WAVE2.md "PAGING"): the phone's own plugged-in radio,
//     heard by that phone's own app — never a server, never a LAN or tunnel client, nothing to the gallery. Compiled only
//     where VIBE_PAGING is defined (the Android native library); the shim refuses the extension unless the engine is
//     bound to loopback (not serving), the source is a USB radio with IQ, and the socket is loopback (vsPagingAllowed).
//
// ★★ FED THE CHANNEL IQ, NOT THE AUDIO. Pagers are direct 2-FSK NRZ: the listener's audio chain is wrong for them (the NFM
//    Voice filter's 300 Hz high-pass takes the low end of a 512 bit/s stream, de-emphasis rounds 2400 bit/s, the squelch
//    and the 48 kHz resampler add nothing). So, as the CTCSS reader does (tone_id.h), this runs its OWN discriminator on
//    the pipeline's channel IQ — post-NCO, the dial at DC, cut to the passband, at the channel rate (≈ 31 kHz for a
//    12.5 kHz NFM filter; the pipeline never runs a channel below 2.5 × its band). Whatever the listener picked for
//    Voice/Raw or de-emphasis, the pager sees the same flat discriminator.
//    ★ The discriminator is arg(x·x*₋₁) four lanes at a time with q65v's vatan2 (NEON / SSE2 / scalar, identical bits:
//      sign-bit selects, no float compares — so GCC armhf's compare lowering cannot bite). It is the only loop here
//      that vectorises; the engine's per-rate loops are per-sample feedback (see paging_decoder.h).
//
// Wire (the decoder socket, beside RTTY's frames):
//   0x01 | u64 BE ts | u32 BE len | UTF-8   one line per page: "[POCSAG 1200 · 1234567 · F3] HELLO" — the rate that
//                                           carried it, the 21-bit address, the function bits; "(tone only)" for an
//                                           address with no message, "(cut short)" when the lock went mid-message.
//   0x03 | u8 state                         0 hunting · 1 locked (a batch lock holds at some rate)
//   {"type":"paging_status","data":{...}}   mode (the id asked for), detected ("POCSAG 1200" or ""), pages, rate (the
//                                           channel rate it is fed), short (the rate is below 24 kHz: 2400 bit/s
//                                           loses ≈ 4 dB there) — on change only.
// ★ Paging content is the user's own reception on their own phone: it goes to that one socket (and the host's in-memory
//   gap log for a held session), never to the gallery, a file, or the log.
#pragma once
#include "decoders/paging_decoder.h"
#include "decoders/digimodes/vmath4.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class PagingRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;   ///< 0x01
        std::function<void(uint8_t)> state;             ///< 0x03
        std::function<void(const std::string&)> json;   ///< a JSON text frame
    };
    explicit PagingRun(PagingDecoder::Mode m, Out out) : mode_(m), out_(std::move(out)) {}

    /** The listener's channel IQ at `rate` Hz (the decoder thread). A new rate (a filter change rebuilt the pipeline)
     *  builds the engine anew. */
    void feedIq(const std::complex<float>* x, int n, double rate) {
        if (n <= 0 || !(rate > 0)) return;
        if (!dec_ || rate != rate_) {
            rate_ = rate;
            dec_.reset(new PagingDecoder(rate, mode_));
            dec_->onPage = [this](const PagingDecoder::Page& p) { onPage(p); };
            last_ = {1.f, 0.f};
            dirty_ = true;
        }
        disc_.resize((size_t)n);
        discriminate(x, n, last_, disc_.data());
        dec_->feed(disc_.data(), n);
        const std::string d = dec_->detected();
        if (d != detected_) {
            detected_ = d; dirty_ = true;
            const uint8_t s = d.empty() ? 0 : 1;
            if (s != state_) { state_ = s; if (out_.state) out_.state(s); }
        }
        if (dirty_) { dirty_ = false; sendStatus(); }
    }
    const std::string& detected() const { return detected_; }
    unsigned long pages() const { return pages_; }

    /** p = x[i]·conj(x[i−1]); out = arg(p) in Hz (· rate/2π — any scale suits the engine, Hz reads in a debugger).
     *  Four lanes; the tail through the same lanes, padded, so every sample's arithmetic is identical on every path. */
    static void discriminate(const std::complex<float>* x, int n, std::complex<float>& last, float* out) {
        using namespace q65v;
        const float* xf = reinterpret_cast<const float*>(x);
        int i = 0;
        {   // the first sample against the last block's last
            const std::complex<float> p = x[0] * std::conj(last);
            float yr[4] = {p.real(), 0, 0, 0}, yi[4] = {p.imag(), 0, 0, 0}, o[4];
            st(o, vatan2(ld(yi), ld(yr)));
            out[0] = o[0];
            i = 1;
        }
        for (; i + 4 <= n; i += 4) {
            V ar, ai, br, bi;
            ld2(xf + 2 * i, ar, ai);
            ld2(xf + 2 * (i - 1), br, bi);
            const V pr = add(mul(ar, br), mul(ai, bi));
            const V pi = sub(mul(ai, br), mul(ar, bi));
            st(out + i, vatan2(pi, pr));
        }
        for (; i < n; i += 4) {
            float yr[4] = {0, 0, 0, 0}, yi[4] = {0, 0, 0, 0}, o[4];
            for (int q = 0; q < 4 && i + q < n; ++q) {
                const std::complex<float> a = x[i + q], b = x[i + q - 1];
                yr[q] = a.real() * b.real() + a.imag() * b.imag();   // ★ the lanes' own order: no std::complex multiply
                yi[q] = a.imag() * b.real() - a.real() * b.imag();
            }
            st(o, vatan2(ld(yi), ld(yr)));
            for (int q = 0; q < 4 && i + q < n; ++q) out[i + q] = o[q];
        }
        last = x[n - 1];
    }
    /** The page as one line of text (the 0x01 frame). Public for the test. */
    static std::string line(const PagingDecoder::Page& p) {
        char head[96];
        std::snprintf(head, sizeof head, "[%s %d \xc2\xb7 %07u \xc2\xb7 F%d] ", p.proto.c_str(), p.baud, (unsigned)p.address, p.function);
        std::string s = head;
        if (p.kind == "tone") s += "(tone only)";
        else s += p.text;
        if (p.truncated) s += " (cut short)";
        return s + "\n";
    }

private:
    void onPage(const PagingDecoder::Page& p) {
        ++pages_; dirty_ = true;
        if (out_.text) out_.text(line(p));
    }
    void sendStatus() {
        if (!out_.json) return;
        char b[256];
        std::snprintf(b, sizeof b,
            "{\"type\":\"paging_status\",\"data\":{\"mode\":\"%s\",\"detected\":\"%s\",\"pages\":%lu,\"rate\":%.0f,\"short\":%s}}",
            PagingDecoder::idOf(mode_), detected_.c_str(), pages_, rate_, rate_ < 24000.0 ? "true" : "false");
        out_.json(b);
    }

    PagingDecoder::Mode mode_;
    Out out_;
    std::unique_ptr<PagingDecoder> dec_;
    double rate_ = 0;
    std::complex<float> last_{1.f, 0.f};
    std::vector<float> disc_;
    std::string detected_;
    uint8_t state_ = 0;
    unsigned long pages_ = 0;
    bool dirty_ = true;
};

/** ★ The vibeserver.json fields that offer it — `"paging":true` and the manual choices as data (PagingDecoder::modes).
 *  Only where the shim says it may run (vsPagingAllowed); absent everywhere else, so no client draws a PAGE key there. */
inline std::string pagingCapsJson() {
    std::string j = ",\"paging\":true,\"pagingModes\":[";
    bool first = true;
    for (const auto& m : PagingDecoder::modes()) {
        if (!first) j += ',';
        first = false;
        j += std::string("{\"id\":\"") + m.id + "\",\"label\":\"" + m.label + "\"}";
    }
    return j + "]";
}

} // namespace vibe
