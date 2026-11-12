// test-dab-station-choice.cpp — WHAT PLAYS AFTER A BLOCK CHANGE: silence, or the server's chosen station. Bench clip.
//
// ★★★ Stuart, 2026-11-12 (RSP1A, beta70): "when switching to certain multiplexes I get a weird squealy broken audio
//     until i select a station", and the meter read "Multiplex weak · 100 % frames lost" at MER 23 dB. Cause:
//     DabReceiver::reset() emptied the ensemble but KEPT the old service's sub-channel selection, so after a block
//     change the receiver decoded that slice of the NEW multiplex's CIFs as the OLD codec — garbage super frames
//     counted as lost (DAB+), garbage Layer II played (MP2). Then his rule for what SHOULD play
//     (vibe_dab_last_service.h): the same station, else the last one here, else the first; a named one always wins;
//     the server's own moves (DX sweep, scans) choose nothing.
//
// ★ The clip is ONE multiplex (12C), so a "block change" here retunes the service to another block's name with the
//   same IQ — the receiver cannot tell, which is the point: before the fix, the old selection decoded it into REAL
//   audio with no service chosen. Same EId and SIds on both "blocks", so rule 1 vs rule 2 is told apart with the
//   store file (a fresh service that never played on it), and the pure ordering is test-dab-last-service.
//
// Needs vibeserver/bench-clip/dab-bench-clean.vbu8 (make-clip.sh) or VIBE_DAB_CLIP=; exit 3 = NOT RUN without it.
#include "vibe_dab_service.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++fails; } else { std::printf("  ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

static long field(const std::string& j, const char* k) {
    const std::string key = std::string("\"") + k + "\":";
    const size_t p = j.find(key);
    return p == std::string::npos ? -1 : std::atol(j.c_str() + p + key.size());
}
static bool locked(const std::string& j) { return j.find("\"locked\":true") != std::string::npos; }

struct Clip { double rate = 0, centre = 0; uint32_t sid = 0; std::vector<float> iq; };
static Clip g_clip;
static int g_blk = -1;

/** A DabService on the clip, and a feeder that keeps the clip looping through it. */
struct Rig {
    vibedab::DabService svc;
    size_t pos = 0;
    std::vector<float> pcm = std::vector<float>(48000 * 2);
    explicit Rig(const std::string& lastFile = "") {
        svc.setRfRate(g_clip.rate); svc.setRfCentre(g_clip.centre);
        if (!lastFile.empty()) svc.setLastServiceFile(lastFile);
        svc.setChannel(g_blk);
    }
    /** Feed `secs` of IQ; returns the loudest sample taken from the audio queue. */
    float feed(double secs) {
        const size_t step = size_t(g_clip.rate / 20) * 2;      // 50 ms of interleaved I/Q
        float peak = 0;
        for (double fed = 0; fed < secs; ) {
            const size_t n = std::min(step, g_clip.iq.size() - pos);
            svc.feed(g_clip.iq.data() + pos, n / 2);
            pos = (pos + n) % g_clip.iq.size();
            fed += double(n / 2) / g_clip.rate;
            for (int w = 0; w < 2000 && svc.backlogForTest() > size_t(g_clip.rate / 10); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const size_t got = svc.takePcm(pcm.data(), pcm.size() / 2);
            for (size_t i = 0; i < got * 2; ++i) peak = std::max(peak, std::fabs(pcm[i]));
        }
        return peak;
    }
    /** Move to block `idx` as the shim does: the decoder's block, the radio's centre, the settle after the retune. */
    void step(int idx) {
        svc.setChannel(idx);
        svc.setRfCentre(double(vibedab::kBandIII[idx].centreHz));
        svc.armRetune();
        svc.takePcm(pcm.data(), pcm.size() / 2);   // what was queued before the change belongs to the old block
    }
    /** Feed until the report's sid is non-zero (or `secs` pass). */
    long waitSid(double secs) {
        for (double t = 0; t < secs; t += 0.5) { feed(0.5); const long s = field(svc.json(), "sid"); if (s > 0) return s; }
        return field(svc.json(), "sid");
    }
};

/** The services the report lists, in its order (what every client shows). */
static std::vector<uint32_t> listed(const std::string& j) {
    std::vector<uint32_t> out;
    const size_t a = j.find("\"services\":[");
    if (a == std::string::npos) return out;
    const size_t b = j.find("]}", a);   // ★ rows are flat objects; their arrays never close with "]}" before the list does
    size_t p = a;
    while ((p = j.find("{\"sid\":", p)) != std::string::npos && p < b) { out.push_back(uint32_t(std::atol(j.c_str() + p + 7))); p += 7; }
    return out;
}

/** The listed service whose codec is `codec` ("MP2"), or 0. */
static uint32_t withCodec(const std::string& j, const char* codec) {
    size_t p = j.find("\"services\":[");
    while (p != std::string::npos && (p = j.find("{\"sid\":", p)) != std::string::npos) {
        const uint32_t sid = uint32_t(std::atol(j.c_str() + p + 7));
        const size_t q = j.find("\"codec\":\"", p), next = j.find("{\"sid\":", p + 7);
        if (q != std::string::npos && (next == std::string::npos || q < next) && j.compare(q + 9, std::strlen(codec) + 1, std::string(codec) + "\"") == 0) return sid;
        p += 7;
    }
    return 0;
}

int main() {
    const char* env = std::getenv("VIBE_DAB_CLIP");
    const std::string path = env && *env ? env : "vibeserver/bench-clip/dab-bench-clean.vbu8";
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { std::printf("  no bench clip (%s; vibeserver/bench-clip/make-clip.sh or VIBE_DAB_CLIP=) — NOT RUN\n", path.c_str()); return 3; }
    char magic[8] = {0};
    bool ok = std::fread(magic, 1, 8, fp) == 8 && std::memcmp(magic, "VIBEBU81", 8) == 0
           && std::fread(&g_clip.rate, sizeof g_clip.rate, 1, fp) == 1 && std::fread(&g_clip.centre, sizeof g_clip.centre, 1, fp) == 1
           && std::fread(&g_clip.sid, sizeof g_clip.sid, 1, fp) == 1;
    std::vector<uint8_t> u; { uint8_t b[65536]; size_t n; while (ok && (n = std::fread(b, 1, sizeof b, fp)) > 0) u.insert(u.end(), b, b + n); }
    std::fclose(fp);
    if (!ok || u.size() < size_t(g_clip.rate)) { std::printf("  unreadable clip %s\n", path.c_str()); return 1; }
    g_clip.iq.resize(u.size());
    for (size_t i = 0; i < u.size(); ++i) g_clip.iq[i] = (float(u[i]) - 127.5f) / 127.5f;
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - g_clip.centre) < 1000.0) { g_blk = i; break; }
    if (g_blk < 0) { std::printf("  clip centre is not a Band III block\n"); return 1; }
    const int other = g_blk == 0 ? 1 : 0;
    const char* A = vibedab::kBandIII[g_blk].name; const char* B = vibedab::kBandIII[other].name;
    char tmpl[] = "/tmp/vibe-dab-choice-XXXXXX";
    { const int fd = mkstemp(tmpl); if (fd >= 0) close(fd); }
    const std::string lastFile = tmpl;

    uint32_t eid = 0; std::vector<uint32_t> list;
    std::printf("── 1. no station chosen = silence (the stale sub-channel) ──\n");
    {
        Rig r;
        r.svc.setService(g_clip.sid);
        r.feed(12.0);
        const std::string j1 = r.svc.json();
        eid = uint32_t(field(j1, "eid")); list = listed(j1);
        std::printf("  playing %04X on %s: sfOk %ld, %zu services listed, EId %04X\n", g_clip.sid, A, field(j1, "sfOk"), list.size(), eid);
        CHECK(field(j1, "sfOk") > 0, "the clip's service decodes first (else the rest proves nothing)");
        CHECK(list.size() >= 3 && eid != 0, "the list and EId are read");

        // The server's own move (the DX sweep, a scan): no station named, nothing carried.
        r.step(other);
        r.svc.disarmAutoChoice(/*clearWant=*/true);
        const float after = r.feed(8.0);
        const std::string j2 = r.svc.json();
        std::printf("  on %s, the server's own move: locked %s, sid %ld, sfTried %ld, auIn %ld, mp2In %ld, peak %.4f\n", B,
                    locked(j2) ? "yes" : "no", field(j2, "sid"), field(j2, "sfTried"), field(j2, "auIn"), field(j2, "mp2In"), after);
        CHECK(locked(j2), "the new block locks (the IQ is a multiplex)");
        CHECK(field(j2, "sid") == 0, "the DX sweep / a scan chooses no station");
        CHECK(field(j2, "sfTried") == 0 && field(j2, "auIn") == 0 && field(j2, "mp2In") == 0, "nothing is decoded with no service");
        CHECK(after == 0.0f, "the audio is silent");

        // A bookmark to a station this multiplex does not carry: wait for it in silence, never the old one's slice.
        r.step(g_blk);
        r.svc.setService(0xDEAD);
        const float waitPeak = r.feed(6.0);
        const std::string j3 = r.svc.json();
        CHECK(field(j3, "sid") == 0 && field(j3, "sfTried") == 0 && waitPeak == 0.0f, "a named station not here yet: silent, no counters");
    }
    if (list.size() < 3) { std::printf("dab station choice: %d FAILED (no list)\n", fails + 1); unlink(tmpl); return 1; }
    const uint32_t first = list.front(), second = list[1], third = list[2];

    std::printf("── 2. first visit: the first in the list ──\n");
    {
        unlink(tmpl);
        Rig r(lastFile);
        r.svc.armAutoChoice();
        const float silentWait = r.feed(0.3);
        CHECK(silentWait == 0.0f && field(r.svc.json(), "sid") == 0, "silent until the list has arrived");
        const long got = r.waitSid(12.0);
        std::printf("  chosen %04lX (first listed %04X)\n", got, first);
        CHECK(got == long(first), "the first audio service, in the clients' order");
        const float p = r.feed(4.0);
        CHECK(field(r.svc.json(), "sfOk") > 0 || field(r.svc.json(), "mp2In") > 0, "…and it plays (peak %.3f)", p);

        // Choose another; step away (the server's move) and back as a listener: rule 1 gives it back by SId.
        r.svc.setService(second);
        r.feed(4.0);
        CHECK(r.svc.lastServiceForTest(eid, A) == second, "the station played is remembered for this multiplex");
        r.step(other); r.svc.armAutoChoice();
        CHECK(r.waitSid(12.0) == long(second), "the same station (by SId) on the next multiplex");
    }

    std::printf("── 3. last on this multiplex, across a restart ──\n");
    {
        // The file the last section left says `second` for this EId. A fresh service (a restart) that played nothing:
        // no same station to follow, so rule 2 decides.
        Rig r(lastFile);
        CHECK(r.svc.lastServiceForTest(eid, A) == second, "the store came back from the file");
        r.svc.armAutoChoice();
        const long got = r.waitSid(12.0);
        CHECK(got == long(second), "the last one played here, not the first (%04lX)", got);
    }

    std::printf("── 4. the same station outranks the last one here ──\n");
    {
        Rig r(lastFile);                     // remembers `second` for this EId
        r.svc.setService(third);
        r.feed(8.0);
        // Back to the store saying `second` (as if `third` had been heard on ANOTHER multiplex) — then step.
        { vibedablast::Store s; s.remember(eid, A, second); s.remember(eid, B, second); s.save(lastFile); }
        r.step(other);
        r.svc.setLastServiceFile(lastFile);
        r.svc.armAutoChoice();
        const long got = r.waitSid(12.0);
        CHECK(got == long(third), "the station that was playing (%04lX), not the remembered %04X", got, second);
    }

    std::printf("── 5. a named station always wins ──\n");
    {
        Rig r(lastFile);
        r.svc.armAutoChoice();
        r.svc.setService(third);              // the listener's tap, before the list arrived
        CHECK(!r.svc.autoChoiceArmed(), "naming a station disarms the choice");
        CHECK(r.waitSid(12.0) == long(third), "the named station plays");
        r.feed(2.0);
        CHECK(field(r.svc.json(), "sid") == long(third), "…and stays");
    }

    std::printf("── 6. a scan's read never chooses ──\n");
    {
        Rig r(lastFile);
        r.svc.armAutoChoice();
        r.svc.rereadChannel(g_blk);
        r.feed(8.0);
        CHECK(field(r.svc.json(), "sid") == 0, "rereadChannel (the quick scan) leaves the multiplex silent");
    }
    std::printf("── 7. Layer II: the stale MP2 slice is gone, and a chosen MP2 service starts clean ──\n");
    {
        Rig r;
        r.svc.setService(g_clip.sid);
        r.feed(8.0);
        const uint32_t mp2 = withCodec(r.svc.json(), "MP2");
        CHECK(mp2 != 0, "the clip carries a Layer II service (%04X)", mp2);
        if (mp2) {
            r.svc.setService(mp2);
            r.feed(6.0);
            const std::string j0 = r.svc.json();
            CHECK(field(j0, "mp2In") > 50, "it plays (mp2In %ld, bad %ld)", field(j0, "mp2In"), field(j0, "mp2Bad"));
            // The server's move away: the MP2 decoder must not be fed the old service's slice of the new block.
            r.step(other);
            r.svc.disarmAutoChoice(true);
            const float peak = r.feed(6.0);
            const std::string j1 = r.svc.json();
            CHECK(field(j1, "mp2In") == 0 && peak == 0.0f, "no service: no Layer II decoded, silent (mp2In %ld, peak %.4f)", field(j1, "mp2In"), peak);
            // A listener names the MP2 service on the new block: it starts clean — no bad frames while it settles.
            r.svc.setService(mp2);
            r.feed(6.0);
            const std::string j2 = r.svc.json();
            const long in = field(j2, "mp2In"), bad = field(j2, "mp2Bad");
            CHECK(in > 50 && bad * 100 <= in, "named after the block change: clean from the start (mp2In %ld, bad %ld)", in, bad);
        }
    }
    unlink(tmpl); unlink((lastFile + ".tmp").c_str());
    std::printf(fails ? "dab station choice: %d FAILED\n" : "dab station choice: all passed\n", fails);
    return fails ? 1 : 0;
}
