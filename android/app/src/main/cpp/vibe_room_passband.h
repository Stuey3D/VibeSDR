// vibe_room_passband.h — THE PASSBAND IS PART OF THE ROOM'S STATE (2026-10-17).
//
// ★★★ Stuart: "on shared VFO, if a user changes the bandwidth sliders I am hearing them but they
//     aren't synchronising with mine." The shared chain applied every listener's passband — it is
//     one demodulator, so everybody heard it — and `config` never said what the passband WAS. It
//     carried the frequency, the mode, the zoom, the IF filter and the gain, all for exactly this
//     reason (the shared-dial contract: the server states its FULL state, the client adopts it),
//     and left out the one control a listener drags most.
//  ★★ THE EDGES AS SENT, NOT A WIDTH. The chain itself only runs a width (setBandwidth(hi - lo)),
//     so a width is all the shim ever kept. But what the other listeners need to draw is the pair
//     of sliders — USB's 50..2700 is not "2650 wide, centred" — so the room remembers the pair the
//     last listener chose, and the MODE it was chosen in.
//  ★★ FORGOTTEN WHEN THE CHAIN IS REBUILT, AND KEYED ON THE MODE. Every mode change rebuilds the
//     chain at that mode's default width (buildAudio — reached from the tune and mode handlers, the
//     landing, DAB, a rate change), so buildAudio forgets the edges: USB -> LSB -> USB must not
//     report the first USB edges over a chain running USB's default. The edges also carry the mode
//     they were chosen in and are only reported while it is still the mode, so a path that changes
//     the mode WITHOUT a rebuild cannot leave one mode's edges drawn on another.
//  ★ Shrunk with the chain. The width the chain actually runs can be narrower than asked (the
//    owner's bandwidth cap, the mode's ceiling); reporting the asked-for edges would show sliders
//    wider than what everybody is hearing. Both edges scale by the same factor, so a symmetric
//    passband stays symmetric and SSB stays on its side of the carrier. Never widened: a floor
//    (WFM's) is applied by the clients themselves before they send.
//  ★ CW is reported as sent: the chain keeps its own fixed CW filter whatever the client asks
//    (see setBandwidth), so "what everybody is running" is not a function of the sliders at all —
//    and the sliders the room should see are the ones that were moved.
#pragma once
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace vibepb {

class RoomPassband {
public:
  /** A listener set the passband on the shared chain. `ranHz` is the width the chain is now
   *  running (vfoBwHz after setBandwidth); `cw` says the chain ignored the width for CW. */
  void note(const std::string& mode, double lo, double hi, double ranHz, bool cw) {
    if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi) || mode.empty()) return;
    const double asked = hi - lo;
    if (!cw && ranHz > 0 && ranHz < asked - 1.0) { const double k = ranHz / asked; lo *= k; hi *= k; }
    std::lock_guard<std::mutex> lk(m_);
    mode_ = mode; lo_ = lo; hi_ = hi; known_ = true;
  }
  /** Nothing to report — a width-only request, whose edges nobody chose. */
  void forget() { std::lock_guard<std::mutex> lk(m_); known_ = false; }

  /** The config's passband fields, with a leading comma, or "" while there is nothing true to say:
   *  no listener has chosen one, or the mode has moved on since. Omitting them is what tells a
   *  client to keep the mode's default it already draws. */
  std::string configField(const std::string& modeNow) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!known_ || modeNow != mode_) return std::string();
    char b[80];
    std::snprintf(b, sizeof b, ",\"bandwidthLow\":%lld,\"bandwidthHigh\":%lld",
                  std::llround(lo_), std::llround(hi_));
    return b;
  }

private:
  mutable std::mutex m_;
  std::string mode_;
  double lo_ = 0, hi_ = 0;
  bool known_ = false;
};

}  // namespace vibepb
