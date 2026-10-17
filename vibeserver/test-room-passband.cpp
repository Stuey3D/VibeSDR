// ★★★ THE ROOM'S PASSBAND IN CONFIG (vibe_room_passband.h, 2026-10-17). Stuart: "on shared VFO, if a
//     user changes the bandwidth sliders I am hearing them but they aren't synchronising with mine."
//     The shared chain applied the passband and `config` never carried it, so nobody's sliders could
//     follow. This pins what config now says: the edges as chosen, in the mode they were chosen in,
//     shrunk with the chain, and NOTHING when there is nothing true to say.
#include "vibe_room_passband.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}

int main() {
  vibepb::RoomPassband pb;
  ok(pb.configField("usb").empty(), "nobody has chosen a passband: config says nothing (clients keep the mode default)");

  pb.note("usb", 100, 3000, 2900, false);
  ok(pb.configField("usb") == ",\"bandwidthLow\":100,\"bandwidthHigh\":3000",
     "SSB edges reported as chosen — the pair, not a width centred on the carrier");
  ok(pb.configField("lsb").empty(), "the mode moved on: the old mode's edges are not reported");
  ok(pb.configField("usb") == ",\"bandwidthLow\":100,\"bandwidthHigh\":3000",
     "…and asking about another mode does not throw them away");

  pb.note("am", -6000, 6000, 6000, false);
  ok(pb.configField("am") == ",\"bandwidthLow\":-3000,\"bandwidthHigh\":3000",
     "the chain ran narrower than asked (owner's cap): both edges shrink, symmetric stays symmetric");
  ok(pb.configField("usb").empty(), "a newer choice in another mode replaces the old one");

  pb.note("usb", 50, 2700, 1325, false);
  ok(pb.configField("usb") == ",\"bandwidthLow\":25,\"bandwidthHigh\":1350",
     "SSB shrunk by the same factor stays on its side of the carrier");

  pb.note("cwu", -200, 200, 1200, true);
  ok(pb.configField("cwu") == ",\"bandwidthLow\":-200,\"bandwidthHigh\":200",
     "CW reported as the sliders were set — the chain's fixed CW filter is not a slider position");

  pb.note("wfm", -100000, 100000, 0, false);
  ok(pb.configField("wfm") == ",\"bandwidthLow\":-100000,\"bandwidthHigh\":100000",
     "an unknown running width (0) leaves the edges as chosen");

  pb.note("wfm", 5000, 5000, 0, false);
  ok(pb.configField("wfm") == ",\"bandwidthLow\":-100000,\"bandwidthHigh\":100000",
     "an empty passband (hi <= lo) is refused, not recorded");
  pb.note("", -1, 1, 0, false);
  ok(pb.configField("wfm") == ",\"bandwidthLow\":-100000,\"bandwidthHigh\":100000",
     "a passband with no mode is refused, not recorded");

  pb.forget();
  ok(pb.configField("wfm").empty(), "a width-only request (edges nobody chose): config says nothing");

  std::printf(fails ? "\033[31m%d FAILED\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
  return fails ? 1 : 0;
}
