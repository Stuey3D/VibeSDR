// test-fm-defaults.cpp — the owner's Broadcast FM defaults and the REMEMBER / RETURN rules
// (vibe_fm_defaults.h). Pure logic: no radio, no sockets, nothing played.
//
// ★ The cases Stuart's request turns on: a fresh server, owner-set defaults, REMEMBER and RETURN on a
//   shared dial (one pipeline) and on a locked range (a channel per listener), the joiner who must not
//   move an occupied dial, and the reload that must not lose a listener's own choice.

#include "vibe_fm_defaults.h"
#include "vibeserver_config.h"

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

    std::printf("the config carries them per radio (both writers, both readers, the worker's copy)\n");
    {
        vsconfig::ServerConfig c;
        vsconfig::RadioConfig r; r.serial = "00000001"; r.driver = "rtl";
        r.fm.nr = true; r.fm.ceq = false; r.fm.deemphUs = 75; r.fm.remember = true;
        c.radios.push_back(r);
        vsconfig::RadioConfig r2; r2.serial = "00000002"; r2.driver = "rtl";   // untouched: the defaults
        c.radios.push_back(r2);
        const std::string j = vsconfig::toJson(c);
        vsconfig::ServerConfig back; std::string err;
        ok(vsconfig::fromJson(j, back, err) && back.radios.size() == 2, "the saved server config loads");
        ok(back.radios.size() == 2 && back.radios[0].fm == r.fm, "radio 1 keeps its owner's choices");
        ok(back.radios.size() == 2 && back.radios[1].fm == vibefm::Defaults{}, "radio 2 keeps the defaults");
        ok(j.find("\"fmCeq\"") != std::string::npos, "the keys are in the file, so a hand-editor can find them");
        // ★ and the worker (one radio's process) sees them — effectiveFor builds its Config
        if (back.radios.size() == 2) {
            const vsconfig::Config w = vsconfig::effectiveFor(back, back.radios[0]);
            ok(w.fm == r.fm, "effectiveFor hands them to the radio's process");
        }
        // ★ a patch that does not mention them leaves them alone
        vsconfig::ServerConfig p = back;
        ok(vsconfig::fromJson("{\"name\":\"Attic\"}", p, err) && p.radios.size() == 2 && p.radios[0].fm == r.fm,
           "a patch about something else does not reset them");
    }

    std::printf("THE HANDHELD: the owner's change becomes the owner's default (adoptAsDefaults)\n");
    {
        Defaults cur;                                   // a fresh card: by region, NR off, RETURN
        State s = vibefm::resolve(cur, 1); s.nr = true; // the user turns NR on in the audio menu
        const Defaults d = vibefm::adoptAsDefaults(cur, s, 1);
        ok(d.nr, "NR ON is kept as the radio's default");
        ok(d.deemphUs == -1, "a de-emphasis that is still the region's own stays BY REGION");
        ok(!d.remember, "the policy (RETURN) is left as the owner set it");
        ok(d.stereo && d.ims && d.ceq && d.autobw && d.nb, "the rest of the row is unchanged");
        State t = s; t.deemphTau = 75e-6; t.ceq = false; t.autobw = false; t.stereo = false; t.ims = false; t.nb = false;
        const Defaults e = vibefm::adoptAsDefaults(d, t, 1);
        ok(e.deemphUs == 75 && !e.ceq && !e.autobw && !e.stereo && !e.ims && !e.nb && e.nr, "all seven are carried");
        State z = t; z.deemphTau = 0;
        ok(vibefm::adoptAsDefaults(e, z, 1).deemphUs == 0, "de-emphasis OFF is kept as 0");
        State back = t; back.deemphTau = 50e-6;
        ok(vibefm::adoptAsDefaults(e, back, 1).deemphUs == 50, "an explicit 50 us after 75 is 50, not by region");
        // ★ and through the rules: RETURN now returns to what the owner chose last
        Memory m; m.configure(cur, 1);
        m.note("ses-1", s);
        m.adoptDefaults(vibefm::adoptAsDefaults(m.defaults(), s, 1));
        State out;
        ok(m.sharedArrival("ses-2", 0, out) && out.nr, "a new arrival (RETURN) starts with NR ON");
        ok(m.known("ses-1", out), "adopting forgets nobody (no configure)");
        Defaults round; vibefm::fromJson("{" + vibefm::toJson(m.defaults()) + "}", round);
        ok(round == m.defaults(), "what is written to the engine config reads back the same");
    }

    std::printf(failures ? "\nFAILED (%d)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
