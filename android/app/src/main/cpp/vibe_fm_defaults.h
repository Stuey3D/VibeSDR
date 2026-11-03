// vibe_fm_defaults.h — the OWNER's Broadcast FM defaults, and when a listener gets them.
//
// ★★★ WHAT THIS IS (Stuart, 2026-11-03): "can the Broadcast FM options be able to be set by the owner in
//     the setup and have an option so that the server either remembers what the last user set or
//     defaults back to the server set defaults". The BROADCAST FM row of the audio menu — de-emphasis,
//     ST, NR, IMS, CEQ, AUTO BW, NB — had defaults written into the code and nothing the owner could
//     change, and on a shared dial whatever the last listener left was what the next one got, with no
//     way for the owner to say otherwise.
//
// ★★★ THE TWO POLICIES, AND EXACTLY WHEN "RETURN" HAPPENS
//   REMEMBER  the radio keeps what the last listener set. A new listener starts from it — on a shared
//             dial because the dial IS that state, on a locked range because their new channel is
//             seeded from the last values anyone set. Until the server restarts: a start (a reboot, a
//             settings save, an update) begins from the owner's defaults, exactly as the landing does.
//   RETURN    a NEW listener starts from the owner's defaults:
//               · shared dial / single-user radio — when a session arrives and NOBODY ELSE is on;
//               · locked range (each listener their own channel) — every new session's channel.
//             NEVER mid-session under someone: a joiner on an occupied shared dial takes the dial as
//             it is (the shared-dial contract: the server sends the full state, the client has no
//             vote), and nobody's channel is ever changed but by its own listener.
//   ★ A RETURNING session (the same tab reloading — its session id survives) gets its OWN last
//     settings back in both modes. A reload is not a new listener, and since the browser no longer
//     re-asserts its saved values on connect (that was the CEQ-off fault — see main.ts
//     pushSettingsToServer), this is what keeps a listener's choice across F5.
//
// ★★ Listeners still change every one of these for themselves exactly as before; this only decides
//    where they START. Header-only and free of the shim so the rules are unit-tested on their own
//    (vibeserver/test-fm-defaults.cpp).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vibefm {

/** What a pipeline is running. De-emphasis in SECONDS, like the wire. */
struct State {
    double deemphTau = 50e-6;
    bool stereo = true;
    bool nr     = false;   // the Broadcast FM NR (weak-signal processing: high-blend + high-cut) — `wsp` on the wire
    bool ims    = true;
    bool ceq    = true;
    bool autobw = true;    // WFM's TEF-style AUTO BW (shared pipeline only — not offered per listener)
    bool nb     = true;
    bool operator==(const State& o) const {
        return std::fabs(deemphTau - o.deemphTau) < 1e-9 && stereo == o.stereo && nr == o.nr
            && ims == o.ims && ceq == o.ceq && autobw == o.autobw && nb == o.nb;
    }
    bool operator!=(const State& o) const { return !(*this == o); }
};

/** The owner's choice, as stored in the radio's config (flat keys: fmDeemph, fmStereo, …).
 *  ★ The defaults ARE today's behaviour except NR, which is OFF (Saber, 2026-11-03: "wouldn't it be a
 *    better idea to have the NR be off until someone enables it?"): it is a moving filter a strong
 *    station has no use for, and the listener who needs it is the one who will reach for it.
 *  ★ deemphUs -1 = BY REGION: 75 µs in ITU Region 2 (the Americas), 50 µs everywhere else. */
struct Defaults {
    int  deemphUs = -1;
    bool stereo = true, nr = false, ims = true, ceq = true, autobw = true, nb = true;
    bool remember = false;   // false = RETURN to these defaults (see the note at the top)
    bool operator==(const Defaults& o) const {
        return deemphUs == o.deemphUs && stereo == o.stereo && nr == o.nr && ims == o.ims
            && ceq == o.ceq && autobw == o.autobw && nb == o.nb && remember == o.remember;
    }
};

/** ★ 0, 50 and 75 are the only values; anything else is BY REGION. */
inline int cleanDeemphUs(int us) { return (us == 0 || us == 50 || us == 75) ? us : -1; }

/** ITU Region 2 (the Americas) broadcasts 75 µs; Regions 1 and 3 broadcast 50 µs. */
inline double regionTau(int ituRegion) { return ituRegion == 2 ? 75e-6 : 50e-6; }

inline State resolve(const Defaults& d, int ituRegion) {
    State s;
    const int us = cleanDeemphUs(d.deemphUs);
    s.deemphTau = us < 0 ? regionTau(ituRegion) : us * 1e-6;
    s.stereo = d.stereo; s.nr = d.nr; s.ims = d.ims; s.ceq = d.ceq; s.autobw = d.autobw; s.nb = d.nb;
    return s;
}

// ── The config's flat keys — ONE reader and ONE writer, used by every copy site ─────────────────────
/** Find `"key":` in a flat JSON object and return the text after the colon (nullptr = absent). */
inline const char* rawValue(const std::string& j, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t at = j.find(pat);
    while (at != std::string::npos) {
        size_t c = at + pat.size();
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) ++c;
        if (c < j.size() && j[c] == ':') {
            ++c;
            while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) ++c;
            return j.c_str() + c;
        }
        at = j.find(pat, at + 1);
    }
    return nullptr;
}
inline void readBool(const std::string& j, const char* key, bool& dst) {
    const char* v = rawValue(j, key);
    if (!v) return;
    if (!std::strncmp(v, "true", 4) || *v == '1') dst = true;
    else if (!std::strncmp(v, "false", 5) || *v == '0') dst = false;
}
inline void readInt(const std::string& j, const char* key, int& dst) {
    const char* v = rawValue(j, key);
    if (!v) return;
    char* end = nullptr;
    const double n = std::strtod(v, &end);
    if (end != v && std::isfinite(n)) dst = (int)std::lround(n);
}

/** Read the fm* keys that are present; absent keys keep `d`'s value (an old config reads as today). */
inline void fromJson(const std::string& j, Defaults& d) {
    readInt(j, "fmDeemph", d.deemphUs); d.deemphUs = cleanDeemphUs(d.deemphUs);
    readBool(j, "fmStereo", d.stereo);  readBool(j, "fmNr", d.nr);
    readBool(j, "fmIms", d.ims);        readBool(j, "fmCeq", d.ceq);
    readBool(j, "fmAutoBw", d.autobw);  readBool(j, "fmNb", d.nb);
    readBool(j, "fmRemember", d.remember);
}
/** The same keys, as a fragment to splice into an object: `"fmDeemph":-1,"fmStereo":true,…` (no braces). */
inline std::string toJson(const Defaults& d) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    char buf[256];
    std::snprintf(buf, sizeof buf,
        "\"fmDeemph\":%d,\"fmStereo\":%s,\"fmNr\":%s,\"fmIms\":%s,\"fmCeq\":%s,\"fmAutoBw\":%s,\"fmNb\":%s,\"fmRemember\":%s",
        cleanDeemphUs(d.deemphUs), b(d.stereo), b(d.nr), b(d.ims), b(d.ceq), b(d.autobw), b(d.nb), b(d.remember));
    return buf;
}

// ── The rules ─────────────────────────────────────────────────────────────────────────────────────
/** ★ Not thread-safe on its own: the shim holds it under a mutex. */
class Memory {
public:
    static constexpr size_t kSessions = 64;   // ★ bounded — the 256 MB floor (no unbounded maps)

    void configure(const Defaults& d, int ituRegion) {
        d_ = d; region_ = ituRegion;
        haveLast_ = false;      // ★ a (re)configure is a start: begin from the owner's word
        ses_.clear();
    }
    const Defaults& defaults() const { return d_; }
    State owner() const { return resolve(d_, region_); }
    bool remember() const { return d_.remember; }

    /** A listener changed something; `s` is what their pipeline now runs. Empty session = an older
     *  client with no id: it still counts as "the last listener" but has no memory of its own. */
    void note(const std::string& session, const State& s) {
        last_ = s; haveLast_ = true;
        if (session.empty()) return;
        for (size_t i = 0; i < ses_.size(); ++i)
            if (ses_[i].first == session) { ses_.erase(ses_.begin() + (long)i); break; }
        ses_.emplace_back(session, s);
        if (ses_.size() > kSessions) ses_.erase(ses_.begin());
    }
    bool known(const std::string& session, State& out) const {
        if (session.empty()) return false;
        for (const auto& kv : ses_) if (kv.first == session) { out = kv.second; return true; }
        return false;
    }

    /** ★★ ONE SHARED PIPELINE (shared dial, or a one-listener radio): a session arrives.
     *  @param othersOn listeners already here, NOT counting this session's own sockets.
     *  @return true and `out` = what the pipeline should now run; false = leave the dial alone. */
    bool sharedArrival(const std::string& session, int othersOn, State& out) const {
        if (othersOn > 0) return false;   // never under someone — the joiner adopts the dial
        if (d_.remember) return false;    // the dial IS the last listener's settings
        if (known(session, out)) return true;   // a reload: their own, not the owner's
        out = owner();
        return true;
    }
    /** ★★ A LOCKED RANGE: a new listener's own channel is being built — what it starts with. */
    State channelStart(const std::string& session) const {
        State s;
        if (known(session, s)) return s;
        if (d_.remember && haveLast_) return last_;
        return owner();
    }

private:
    Defaults d_;
    int region_ = 1;
    State last_;
    bool haveLast_ = false;
    std::vector<std::pair<std::string, State>> ses_;   // oldest first
};

}  // namespace vibefm
