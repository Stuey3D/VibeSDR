// vibe_tones_run.h — V12 the tone decoders inside a decoder host (vibe_decoder_host.h, ext "tones"): DTMF and 5-tone selective
// calling on a listener's NFM / AM audio. The receiver is decoders/tones_rx.h; this file decides WHAT MAY BE SHOWN, and to
// whom. docs/v12/TONES.md.
//
// ★★★ THE PRIVACY RULE (DECODER-SURVEY.md §2.12 and §7, DECODER-RESEARCH.md §4.2–4.3) — decided HERE, on the server, so no
//     client, old or new, is ever sent what it must not show:
//   • ON AN AMATEUR FREQUENCY (the band plan for this server's ITU region — vibe_bands.h namedBands, every band labelled
//     "amateur"; region unknown = Region 1, the narrowest 2 m / 70 cm): DTMF digits are shown — they are repeater, EchoLink
//     and AllStar control codes on traffic meant for anyone to hear — and so are 5-tone digits (amateur 5-tone is repeater
//     access and station identity, the same footing as a callsign).
//   • ANYWHERE ELSE: DTMF is "DTMF heard", NEVER the digits (on PMR and telephone patches it is a dialled number; there is no
//     owner switch for it). 5-tone is its format ("ZVEI-1 5-tone heard"); the digits are addressing (who is being called) and
//     reach ONLY the owner's own decoder sockets, and only with the owner's opt-in ("tones+digits", off by default) — the
//     host's privileged text (DecoderPeer::privileged), never the gap log, never the gallery, never another listener.
//   ★ The dial is read when the sequence ENDS (a listener who retunes mid-call is judged where they are now).
//
// Wire (on the decoder socket):
//   0x01 | u64 BE ts | u32 BE len | UTF-8      one line per sequence heard, as above
//   0x03 | u8 state                            0 listening · 1 tones sounding
//   {"type":"tones","data":{"amateur":bool,"band":"2 m amateur"|"","dtmf":bool,"five":bool,"pin":"auto"|id,"digits5":bool}}
//                                              on attach and whenever the dial moves on or off an amateur band
#pragma once
#include "decoders/tones_rx.h"
#include "vibe_bands.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace vibe {

namespace vibetones {
/** ★ Is `hz` inside an amateur allocation of `region`'s band plan? "" when not, else the band's label ("2 m amateur"). */
inline std::string amateurBand(double hz, int region) {
    if (!(hz > 0)) return {};
    for (const auto& b : vibebands::namedBands(region))
        if (hz >= b.lo && hz <= b.hi && std::string(b.label).find("amateur") != std::string::npos) return b.label;
    return {};
}
} // namespace vibetones

class TonesRun {
public:
    struct Config {
        bool dtmf = true, five = true;
        int pin = -1;               ///< 5-tone: −1 AUTO, else a tones::toneTables() index
        bool digits5 = false;       ///< the OWNER's opt-in: 5-tone digits off the amateur bands, to the owner's sockets only
    };
    struct Out {
        std::function<void(const std::string&)> text;                          ///< 0x01 to everyone
        std::function<void(const std::string&, const std::string&)> textPriv;   ///< (everyone's line, the owner's line)
        std::function<void(uint8_t)> state;                                     ///< 0x03
        std::function<void(const std::string&)> json;
    };
    TonesRun(Config c, std::function<double()> dialHz, std::function<int()> region, Out out)
        : cfg_(c), dial_(std::move(dialHz)), region_(std::move(region)), out_(std::move(out)) {
        rx_.dtmfOn = cfg_.dtmf; rx_.fiveOn = cfg_.five; rx_.five.pin = cfg_.pin;
        rx_.dtmf.onState = [this](int s) { dtmfSt_ = s; setState(); };
        rx_.five.onState = [this](int s) { fiveSt_ = s; setState(); };
        rx_.dtmf.onSequence = [this](const tones::Dtmf::Sequence& s) { onDtmf(s); };
        rx_.five.onCall = [this](const tones::FiveTone::Call& c) { onFive(c); };
        where();
        sendJson();
    }
    void feedAudio(const float* x, int n) {
        if (n <= 0) return;
        rx_.feed(x, n);
        if ((since_ += n) >= 48000) { since_ = 0; if (where()) sendJson(); }
    }
    /** For the test: the lines as they would go out. */
    const std::string& band() const { return band_; }

private:
    static std::string clock() {
        const time_t t = std::time(nullptr); struct tm u{}; gmtime_r(&t, &u);
        char b[16]; std::snprintf(b, sizeof b, "%02d:%02d:%02dz", u.tm_hour, u.tm_min, u.tm_sec);
        return b;
    }
    /** Re-reads the dial; true when amateur-ness changed. */
    bool where() {
        const std::string b = vibetones::amateurBand(dial_ ? dial_() : 0.0, region_ ? region_() : 1);
        if (b == band_ && known_) return false;
        known_ = true; band_ = b; return true;
    }
    void onDtmf(const tones::Dtmf::Sequence& s) {
        where();
        char b[200];
        if (!band_.empty())
            std::snprintf(b, sizeof b, "%s  DTMF  %s  \xc2\xb7 %zu digit%s, %.0f ms tones \xc2\xb7 %s\n", clock().c_str(), s.digits.c_str(),
                          s.digits.size(), s.digits.size() == 1 ? "" : "s", s.toneMs, band_.c_str());
        else
            std::snprintf(b, sizeof b, "%s  DTMF heard  \xc2\xb7 not an amateur frequency: a dialled number is not shown\n", clock().c_str());
        if (out_.text) out_.text(b);
    }
    void onFive(const tones::FiveTone::Call& c) {
        where();
        const tones::ToneTable& t = tones::toneTables()[c.table];
        // ★ another table that fits AND reads different digits (DZVEI under a ZVEI call) is named beside it; tables that read
        //   the same digits (ZVEI-1 / PZVEI, CCIR / EEA) are not — the digits are what matter
        std::string alt;
        for (size_t i = 0; i < c.alsoFits.size(); ++i) {
            if (c.alsoDigits[i] == c.digits) continue;
            alt += std::string(alt.empty() ? " (or " : ", ") + tones::toneTables()[c.alsoFits[i]].name + " " + c.alsoDigits[i];
        }
        if (!alt.empty()) alt += ")";
        char b[300];
        if (!band_.empty()) {
            std::snprintf(b, sizeof b, "%s  %s 5-tone  %s%s  \xc2\xb7 %.0f ms tones \xc2\xb7 %s\n", clock().c_str(), t.name, c.digits.c_str(), alt.c_str(), c.periodMs, band_.c_str());
            if (out_.text) out_.text(b);
            return;
        }
        std::snprintf(b, sizeof b, "%s  %s 5-tone heard  \xc2\xb7 %zu tones, %.0f ms\n", clock().c_str(), t.name, c.digits.size(), c.periodMs);
        const std::string pub = b;
        if (cfg_.digits5 && out_.textPriv) {
            std::snprintf(b, sizeof b, "%s  %s 5-tone  %s%s  \xc2\xb7 %.0f ms tones \xc2\xb7 shown to you only (your opt-in)\n", clock().c_str(), t.name,
                          c.digits.c_str(), alt.c_str(), c.periodMs);
            out_.textPriv(pub, b);
        } else if (out_.text) out_.text(pub);
    }
    void sendJson() {
        if (!out_.json) return;
        std::string j = "{\"type\":\"tones\",\"data\":{\"amateur\":";
        j += band_.empty() ? "false" : "true";
        j += ",\"band\":\"" + band_ + "\",\"dtmf\":" + (cfg_.dtmf ? "true" : "false") + ",\"five\":" + (cfg_.five ? "true" : "false");
        j += ",\"pin\":\"" + std::string(cfg_.pin >= 0 ? tones::toneTables()[cfg_.pin].id : "auto") + "\",\"digits5\":" + (cfg_.digits5 ? "true" : "false") + "}}";
        out_.json(j);
    }
    /** 0x03: 1 while either decoder hears tones. */
    void setState() {
        const int now = (dtmfSt_ || fiveSt_) ? 1 : 0;
        if (now != state_) { state_ = now; if (out_.state) out_.state((uint8_t)now); }
    }

    Config cfg_;
    std::function<double()> dial_;
    std::function<int()> region_;
    Out out_;
    tones::Rx rx_;
    std::string band_;
    bool known_ = false;
    int since_ = 0, state_ = 0, dtmfSt_ = 0, fiveSt_ = 0;
};

} // namespace vibe
