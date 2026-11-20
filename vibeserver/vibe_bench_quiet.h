// vibe_bench_quiet.h — THE WHOLE MACHINE HOLDS STILL FOR A BENCHMARK (Stuart, 2026-11-20: "everything should stop for
// benchmark runs otherwise they are invalid").
//
// ★★★ WHY. On the Pi 2 (one RTL; the front door vibeserver.service + the radio's own vibeserver@00000001) the QUICK
//     benchmark was started from the setup page — so it ran in the FRONT DOOR, which owns no radio. Its "release the
//     radio" released nothing, its listener check counted nobody but the door's, and the radio process went on with its
//     idle DX scanner (a DAB band sweep, two thirds of a Pi 2 core) right through the run. The locked-capture rows read
//     78.7 % on the hottest thread and 366 drops, so every locked-range profile came out "0 users (the capture itself
//     cannot keep up)" — beside a plain WFM listener measured at ~60 % of a core in the same run.
//
// ★★ THE DESIGN (main.cpp vsBenchQuietMachine — the pure parts are here, tested by test-bench-quiet.cpp):
//   · Whichever process takes the run (the door for the quick one, a radio's own process for the wide one) is the
//     COORDINATOR. Every OTHER radio process on the machine is asked, over 127.0.0.1 on its own port (radio ports
//     behind a door answer this machine only), through the admin-gated /vibeserver/benchmark endpoint with a ticket the
//     shared admin secret signs — the same proof the setup page uses, never a new open endpoint:
//       hold=check  → {"listeners":n,"busy":bool}       (nothing changes)
//       hold=on     → quiets it (LocalSdrShim::benchQuiesce: DX scanner + background decoding stopped and held off,
//                     radio released or its capture paused) under a LEASE; refused with words if it cannot be quieted
//                     or has listeners (unless force=1)
//       hold=renew  → extends the lease (re-quiets it if the lease had run out)
//       hold=off    → takes the radio back and lets the idle work run again
//   · The lease is the safety net: a coordinator that dies mid-run (or a door restarted by systemd) leaves no radio
//     deaf for long — each radio process brings itself back when its lease runs out.
//   · Listeners are SUMMED across every radio before anything stops; one radio that cannot be quieted (does not
//     answer, refuses) stops the run before it starts, with a sentence that names it. A radio whose process is known
//     NOT to be running (its device missing — radio-status file) is skipped: it is costing the machine nothing.
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace vibe::benchquiet {

/** How long a hold lasts without a renewal, and how often the coordinator renews it. */
constexpr double kLeaseSec = 90.0;
constexpr double kRenewSec = 20.0;
inline double leaseSec() {
    if (const char* e = std::getenv("VIBESERVER_TEST_BENCH_LEASE_SEC"); e && *e) {   // ★ TEST-ONLY
        const double v = std::atof(e);
        if (v > 0) return v;
    }
    return kLeaseSec;
}
/** The renewal period for the lease in force: 20 s for the real 90, proportionally shorter under a test lease. */
inline double renewSec() { return leaseSec() * (kRenewSec / kLeaseSec); }

/** A radio process's hold, by the clock the caller passes (seconds). */
struct Lease {
    bool   on = false;
    double until = 0;
    void   hold(double now, double sec) { on = true; until = now + sec; }
    bool   expired(double now) const { return on && now >= until; }
    void   end() { on = false; until = 0; }
};

/** One number from one of OUR OWN replies ({"listeners":2,…}) — -1 when absent. Not a parser. */
inline long jsonNum(const std::string& j, const std::string& key) {
    const std::string k = "\"" + key + "\":";
    const size_t a = j.find(k);
    if (a == std::string::npos) return -1;
    return std::strtol(j.c_str() + a + k.size(), nullptr, 10);
}
inline bool jsonTrue(const std::string& j, const std::string& key) {
    return j.find("\"" + key + "\":true") != std::string::npos;
}
/** A string field of OUR OWN reply ({"error":"…"}), escapes left as they are (the words are ours). */
inline std::string jsonStr(const std::string& j, const std::string& key) {
    const std::string k = "\"" + key + "\":\"";
    const size_t a = j.find(k);
    if (a == std::string::npos) return "";
    std::string out;
    for (size_t i = a + k.size(); i < j.size(); ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) { out += j[++i]; continue; }
        if (j[i] == '"') break;
        out += j[i];
    }
    return out;
}

/** "3 listeners are connected (RTL-SDR: 2, Airspy HF+: 1) — every radio on this machine goes off the air for the
 *  measurement". Only radios WITH listeners are named. Keeps the word "listener" — the setup page offers "Run it
 *  anyway?" on exactly that. "" when nobody is listening. */
inline std::string listenersRefusal(const std::vector<std::pair<std::string, int>>& who) {
    int total = 0;
    std::string names;
    for (const auto& w : who) {
        if (w.second <= 0) continue;
        total += w.second;
        if (!names.empty()) names += ", ";
        names += w.first + ": " + std::to_string(w.second);
    }
    if (total <= 0) return "";
    return std::to_string(total) + (total == 1 ? " listener is" : " listeners are") + " connected (" + names
         + ") \xe2\x80\x94 every radio on this machine goes off the air for the measurement";
}

/** The refusal for a radio that could not be quieted: names it, says why, and why it matters. */
inline std::string cannotQuietRefusal(const std::string& label, const std::string& why) {
    return "The benchmark did not start: " + label + " " + why
         + ", and a figure measured beside a working radio would be wrong. Nothing was changed.";
}
/** The words for a radio process that did not answer at all. */
inline std::string noAnswerWhy() {
    return "did not answer on this machine (is its process running? Restart it from its tab, or pause the radio)";
}

}  // namespace vibe::benchquiet
