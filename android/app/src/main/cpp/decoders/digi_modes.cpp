// decoders/digi_modes.cpp — THE MODE REGISTRY (decoders/digi_mode.h). The list of digital-spots modes this build has.
//
// ★★ ADDING A MODE: drop decoders/digimodes/<name>.h defining `inline ModeDef vibeDigiMode_<name>()` (and its engine's
//    .cpp files in decoders/digimodes/ — globbed by all three builds: vibeserver/CMakeLists.txt, the Android
//    CMakeLists.txt and modules/vibe-local-sdr/build_ios.sh). The seven below that are being built now are picked up by
//    __has_include, so for them the header IS the change. Any other mode adds one ADD() line here (JTTY and PSK did,
//    2026-10-27).
// ★ Order matters only for display (the list's mode filter, the map legend): the order modes register is the order shown.
#include "digi_mode.h"
#include "digimodes/ft8.h"

#if __has_include("digimodes/wspr.h")
#include "digimodes/wspr.h"
#define VIBE_DIGI_HAVE_WSPR 1
#endif
#if __has_include("digimodes/js8.h")
#include "digimodes/js8.h"
#define VIBE_DIGI_HAVE_JS8 1
#endif
#if __has_include("digimodes/ft2.h")
#include "digimodes/ft2.h"
#define VIBE_DIGI_HAVE_FT2 1
#endif
#if __has_include("digimodes/jtty.h")
#include "digimodes/jtty.h"
#define VIBE_DIGI_HAVE_JTTY 1
#endif
#if __has_include("digimodes/psk.h")
#include "digimodes/psk.h"
#define VIBE_DIGI_HAVE_PSK 1
#endif
#if __has_include("digimodes/q65.h")
#include "digimodes/q65.h"
#define VIBE_DIGI_HAVE_Q65 1
#endif
#if __has_include("digimodes/fst4.h")
#include "digimodes/fst4.h"
#define VIBE_DIGI_HAVE_FST4 1
#endif
#if __has_include("digimodes/fst4w.h")
#include "digimodes/fst4w.h"
#define VIBE_DIGI_HAVE_FST4W 1
#endif
#if __has_include("digimodes/msk144.h")
#include "digimodes/msk144.h"
#define VIBE_DIGI_HAVE_MSK144 1
#endif

#if __has_include("digimodes/cw.h")
#include "digimodes/cw.h"
#define VIBE_DIGI_HAVE_CW 1
#endif
#if __has_include("digimodes/rtty.h")
#include "digimodes/rtty.h"
#define VIBE_DIGI_HAVE_RTTY 1
#endif
#if __has_include("digimodes/pskw.h")
#include "digimodes/pskw.h"
#define VIBE_DIGI_HAVE_PSKW 1
#endif

#include <cctype>
#include <cstdlib>

namespace vibe {
namespace digi {

const std::vector<ModeDef>& modes() {
    static const std::vector<ModeDef> all = [] {
        std::vector<ModeDef> v;
        auto ADD = [&](ModeDef m) { if (!m.name.empty() && m.make) v.push_back(std::move(m)); };
        ADD(vibeDigiMode_ft8());
        ADD(vibeDigiMode_ft4());
#ifdef VIBE_DIGI_HAVE_FT2
        ADD(vibeDigiMode_ft2());
#endif
#ifdef VIBE_DIGI_HAVE_JS8
        ADD(vibeDigiMode_js8());
#endif
#ifdef VIBE_DIGI_HAVE_WSPR
        ADD(vibeDigiMode_wspr());
#endif
#ifdef VIBE_DIGI_HAVE_JTTY
        ADD(vibeDigiMode_jtty());       // ★ V12 2026-10-27: slot-less, every JTTY signal on its dial at once
#endif
#ifdef VIBE_DIGI_HAVE_PSK
        ADD(vibeDigiMode_psk());        // ★ V12 2026-10-27: PSK31/63 spots (callsigns heard identifying)
#endif
#ifdef VIBE_DIGI_HAVE_FST4W
        ADD(vibeDigiMode_fst4w());
#endif
#ifdef VIBE_DIGI_HAVE_FST4
        ADD(vibeDigiMode_fst4());
#endif
#ifdef VIBE_DIGI_HAVE_Q65
        ADD(vibeDigiMode_q65());
#endif
#ifdef VIBE_DIGI_HAVE_MSK144
        ADD(vibeDigiMode_msk144());
#endif
        return v;
    }();
    return all;
}

/* ★★ V12 beta91 — THE IQ MODES (the band decode's CW, RTTY and wide-PSK chips: windows, not dials — Band only;
 *  docs/v12/BAND-SKIM-CW-RTTY.md), a registry of their own. modes() is the AUDIO registry every other reader walks (the
 *  benchmark's rows, Single, the Band budget's units, the spans' sizes); these join a Band run and the server's lists only
 *  where they are wired (DecoderHost::startSpots, bandExtraSlices, the chips, spotModes / spotBands). Last: shed and
 *  paused first, shown last. */
const std::vector<ModeDef>& iqModes() {
    static const std::vector<ModeDef> v = [] {
        std::vector<ModeDef> o;
#ifdef VIBE_DIGI_HAVE_CW
        o.push_back(vibeDigiMode_cw());
#endif
#ifdef VIBE_DIGI_HAVE_RTTY
        o.push_back(vibeDigiMode_rtty());
#endif
#ifdef VIBE_DIGI_HAVE_PSKW
        o.push_back(vibeDigiMode_pskw());
#endif
        return o;
    }();
    return v;
}

const std::vector<ModeDef>& allModes() {
    static const std::vector<ModeDef> v = [] {
        std::vector<ModeDef> o = modes();
        for (const ModeDef& m : iqModes()) o.push_back(m);
        return o;
    }();
    return v;
}

const ModeDef* modeByName(const std::string& name) {
    auto eq = [&](const std::string& n) {
        if (n.size() != name.size()) return false;
        for (size_t i = 0; i < name.size(); ++i) if (std::toupper((unsigned char)n[i]) != std::toupper((unsigned char)name[i])) return false;
        return true;
    };
    // ★ the audio registry's own entries first (callers compare pointers into modes()), then the IQ modes'
    for (const ModeDef& m : modes()) if (eq(m.name)) return &m;
    for (const ModeDef& m : iqModes()) if (eq(m.name)) return &m;
    return nullptr;
}

} // namespace digi
} // namespace vibe
