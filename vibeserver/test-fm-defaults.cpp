// test-fm-defaults.cpp — the owner's Broadcast FM defaults and the REMEMBER / RETURN rules
// (vibe_fm_defaults.h). Pure logic: no radio, no sockets, nothing played.
//
// ★ The cases Stuart's request turns on: a fresh server, owner-set defaults, REMEMBER and RETURN on a
//   shared dial (one pipeline) and on a locked range (a channel per listener), the joiner who must not
//   move an occupied dial, and the reload that must not lose a listener's own choice.

#include "vibe_fm_defaults.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void ok(bool cond, const char* what) {
    std::printf("   %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

int main() {
    using vibefm::Defaults; using vibefm::Memory; using vibefm::State;

    std::printf("fresh server (no fm* keys in the config)\n");
    {
        Defaults d;                                   // what an old config reads as
        vibefm::fromJson("{\"label\":\"x\"}", d);
        ok(d.deemphUs == -1, "de-emphasis is BY REGION");
        ok(d.stereo && d.ims && d.ceq && d.autobw && d.nb, "ST, IMS, CEQ, AUTO BW, NB stay ON (today's defaults)");
        ok(!d.nr, "NR is OFF by default (Saber, 2026-11-03)");
        ok(!d.remember, "the policy is RETURN to the owner's defaults");
        ok(std::fabs(vibefm::resolve(d, 1).deemphTau - 50e-6) < 1e-12, "Region 1 -> 50 us");
        ok(std::fabs(vibefm::resolve(d, 2).deemphTau - 75e-6) < 1e-12, "Region 2 -> 75 us");
        ok(std::fabs(vibefm::resolve(d, 3).deemphTau - 50e-6) < 1e-12, "Region 3 -> 50 us");
    }

    std::printf("the config keys round-trip, and junk is refused\n");
    {
        Defaults d; d.deemphUs = 75; d.nr = true; d.ceq = false; d.autobw = false; d.remember = true;
        Defaults back; vibefm::fromJson("{" + vibefm::toJson(d) + "}", back);
        ok(back == d, "toJson -> fromJson gives the same defaults");
        Defaults j; vibefm::fromJson("{\"fmDeemph\":33,\"fmCeq\": false , \"fmNb\":0}", j);
        ok(j.deemphUs == -1, "a de-emphasis that is not 0/50/75 falls back to BY REGION");
        ok(!j.ceq && !j.nb, "spaces and 0/1 are read");
        Defaults off; vibefm::fromJson("{\"fmDeemph\":0}", off);
        ok(vibefm::resolve(off, 2).deemphTau == 0.0, "OFF is OFF in every region");
    }

    Defaults own; own.nr = false; own.ceq = true; own.deemphUs = 50;   // the owner's house sound
    State ceqOff = vibefm::resolve(own, 1); ceqOff.ceq = false;         // what a listener changes to

    std::printf("RETURN, shared dial (one pipeline)\n");
    {
        Memory m; m.configure(own, 1);
        State out;
        ok(m.sharedArrival("A", 0, out) && out == m.owner(), "the first listener gets the owner's defaults");
        m.note("A", ceqOff);
        ok(!m.sharedArrival("B", 1, out), "a JOINER on an occupied dial changes nothing (never under someone)");
        ok(m.sharedArrival("B", 0, out) && out == m.owner(), "the next listener on an EMPTY dial gets the defaults back");
        ok(m.sharedArrival("A", 0, out) && out == ceqOff, "A reloading (same session) gets A's own choice, not the defaults");
    }

    std::printf("REMEMBER, shared dial\n");
    {
        Defaults r = own; r.remember = true;
        Memory m; m.configure(r, 1);
        State out;
        ok(!m.sharedArrival("A", 0, out), "an arrival never resets the dial — it carries what was last set");
        m.note("A", ceqOff);
        ok(!m.sharedArrival("B", 0, out), "the next listener inherits CEQ OFF as the last listener left it");
    }

    std::printf("RETURN, locked range (a channel per listener)\n");
    {
        Memory m; m.configure(own, 1);
        ok(m.channelStart("A") == m.owner(), "a new session's channel starts at the owner's defaults");
        m.note("A", ceqOff);
        ok(m.channelStart("B") == m.owner(), "B's new channel does NOT inherit A's CEQ OFF");
        ok(m.channelStart("A") == ceqOff, "A's rebuilt channel (a reload) keeps A's own choice");
    }

    std::printf("REMEMBER, locked range\n");
    {
        Defaults r = own; r.remember = true;
        Memory m; m.configure(r, 1);
        ok(m.channelStart("A") == m.owner(), "nothing set yet: the owner's defaults");
        m.note("A", ceqOff);
        ok(m.channelStart("B") == ceqOff, "B starts from the LAST values anyone set");
    }

    std::printf("a restart (configure) forgets the listeners\n");
    {
        Defaults r = own; r.remember = true;
        Memory m; m.configure(r, 1);
        m.note("A", ceqOff);
        m.configure(r, 1);
        ok(m.channelStart("B") == m.owner(), "after a start the owner's defaults are the starting point");
        State out; ok(!m.known("A", out), "and no session's memory survives it");
    }

    std::printf("the session memory is bounded\n");
    {
        Memory m; m.configure(own, 1);
        for (int i = 0; i < 200; ++i) m.note("s" + std::to_string(i), ceqOff);
        State out;
        ok(!m.known("s0", out) && m.known("s199", out), "only the most recent sessions are kept (no unbounded map)");
        m.note("", ceqOff);   // an older client with no session id
        ok(!m.known("", out), "no session id = no per-session memory");
    }

    std::printf(failures ? "\nFAILED (%d)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
