// ★★★ THE VibeAGC RECORD (vibe_agc_record.h, 2026-10-20): bounded, in memory, flap counters that count flaps.
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/tagcrec test-agc-record.cpp
#include "vibe_agc_record.h"
#include <cstdio>
#include <string>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}
static size_t lines(const std::string& s) { size_t n = 0; for (char c : s) if (c == '\n') ++n; return n; }

int main() {
    using namespace vibeagc;
    printf("\nBounded\n");
    {
        AgcRecord r;
        ok(r.bytesReserved() == 0, "nothing allocated until the first record (a quiet receiver pays nothing)");
        for (int i = 0; i < 5000; ++i) r.forget(1000.0 + i, "test");
        ok(r.size() == kAgcRecordCap, "5000 records in: the ring holds exactly 2000");
        ok(r.bytesReserved() <= 420 * 1024, "and never more than ~400 KB");
        printf("        sizeof(AgcRec) = %zu bytes, %zu KB reserved\n", sizeof(AgcRec), r.bytesReserved() / 1024);
        const std::string j = r.jsonl(7000.0);
        ok(lines(j) == kAgcRecordCap + 1, "the dump: a header line + one line per record");
        ok(j.find("\"t\":4000.000") != std::string::npos && j.find("\"t\":3999.000") == std::string::npos,
           "the oldest 3000 were overwritten (t = 1000..3999 gone, 4000 kept)");
        ok(j.find("\"t\":5999.000") > j.find("\"t\":4000.000"), "oldest first, newest last");
    }
    printf("\nWhat goes in\n");
    {
        AgcRecord r;
        r.tune(10.0, 14.200e6, 14.203e6, 1, true, "usb");
        r.gain(11.0, GainSrc::Loop, 280, 297, "headroom (carrierless)");
        r.forget(12.0, "say \"hi\"\\");
        AgcSnap s; s.gainDb = 29.7f; s.peakDbfs = -7.5f; s.steps = 11; vibeagc::agcCopyWhy(s.profile, sizeof s.profile, "narrow");
        r.snap(12.5, s);
        const std::string j = r.jsonl(13.0);
        ok(j.find("\"k\":\"tune\",\"fromHz\":14200000,\"toHz\":14203000,\"dHz\":3000,\"forget\":\"channel\",\"tunerMoved\":true,\"mode\":\"usb\"") != std::string::npos,
           "a tune: distance, what it forgot, whether the tuner moved, the mode");
        ok(j.find("\"k\":\"gain\",\"src\":\"vibeagc\",\"from\":28.0,\"to\":29.7,\"why\":\"headroom (carrierless)\"") != std::string::npos,
           "a gain write: who, from, to, why");
        ok(j.find("say \\\"hi\\\"\\\\") != std::string::npos, "a cause is JSON-escaped");
        ok(j.find("\"k\":\"snap\",\"gain\":29.7,\"steps\":11,\"peak\":-7.5,\"peakHeld\":null") != std::string::npos,
           "a snapshot: unknowns are null, not invented numbers");
    }
    printf("\nSnapshots only around events\n");
    {
        AgcRecord r;
        ok(!r.wantSnap(100.0), "never moved: no snapshots");
        r.gain(100.0, GainSrc::Loop, 280, 297, "x");
        ok(r.wantSnap(110.0) && r.wantSnap(130.0), "within 30 s of an event: yes");
        ok(!r.wantSnap(130.5), "30 s after the last event: stops");
    }
    printf("\nFlap counters\n");
    {
        AgcRecord r;
        // Converging: four moves down, none back.
        r.gain(1.0, GainSrc::Loop, 400, 380, "a"); r.gain(3.0, GainSrc::Loop, 380, 360, "a");
        r.gain(5.0, GainSrc::Loop, 360, 340, "a"); r.gain(7.0, GainSrc::Loop, 340, 320, "a");
        ok(r.moves(8.0) == 4 && r.reversals(8.0) == 0, "converging: 4 moves, 0 reversals");
        // Flapping: up, down, up, down.
        r.gain(10.0, GainSrc::Loop, 320, 340, "b"); r.gain(14.0, GainSrc::Loop, 340, 320, "b");
        r.gain(18.0, GainSrc::Loop, 320, 340, "b"); r.gain(22.0, GainSrc::Loop, 340, 320, "b");
        ok(r.moves(23.0) == 8 && r.reversals(23.0) == 4, "then up-down-up-down: 8 moves, 4 reversals");
        ok(r.moves(75.0) == 2 && r.reversals(75.0) == 2, "at t=75 only the last minute counts (the moves at 18 and 22 s)");
        r.gain(30.0, GainSrc::Owner, 320, 496, "owner");
        r.gain(31.0, GainSrc::BandSeed, 496, 200, "band");
        ok(r.moves(32.0) == 8, "the owner's slider and a band seed are recorded but are not the loop flapping");
    }
    printf("\nagc record: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
