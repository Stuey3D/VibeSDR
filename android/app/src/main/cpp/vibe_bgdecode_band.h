// vibe_bgdecode_band.h — where a background Band digital-spots VFO's slice goes (V12, 2026-10-27).
//
// ★ vibe_bgdecode_config.h is read by the daemon's save path, which must not pull in the decoders; the span of a band
//   (digi::spanOf) needs the registry of modes (decoders/digi_modes.cpp). This installs the config's hook from the
//   registry. Call installBandSpan() before validate() wherever the decoders are linked (vibeserver/main.cpp's save,
//   BgRunner::start, the tests). Idempotent and cheap.
#pragma once
#include "vibe_bgdecode_config.h"
#include "decoders/digi_bands.h"

#include <mutex>

namespace vibe {
namespace bg {

inline void installBandSpan() {
    // ★ once, whichever thread gets here first (the save path's HTTP thread, the housekeeping thread's BgRunner::start)
    static std::once_flag once;
    std::call_once(once, [] {
        bandSpanHook() = [](double hz, BandSpan& o) {
            const digi::Band* b = digi::bandAt(hz);
            if (!b) return false;
            const digi::Span s = digi::spanOf(*b, digi::modes());
            if (!s.ok) return false;
            o.loHz = s.loHz; o.hiHz = s.hiHz; o.centreHz = s.centreHz; o.name = b->name;
            return true;
        };
    });
}

} // namespace bg
} // namespace vibe
