// VibeSDR V4 — FT8 / FT4 decoder (wraps kgoba's ft8_lib, MIT).
//
// Buffers mono audio, aligns to UTC FT8 (15 s) / FT4 (7.5 s) slots, runs the
// ft8_lib STFT monitor + candidate search + LDPC decode, and reports each
// decoded message via onSpot. The shim turns those into {type:'digital_spot'}
// JSON frames on /ws/dxcluster so the existing VibeSDR digital-spots list shows
// local decodes (same mechanism as the UberSDR server skimmer feed).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <functional>
#include <string>
#include <vector>

// ft8_lib C core
extern "C" {
#include "common/monitor.h"
#include "ft8/decode.h"
}

namespace vibe {

/* ★★★ THE HASHED-CALLSIGN TABLE (audit 2026-10-04, row 10). A non-standard call ("PJ4/K1ABC") goes over the air as a
 *  10-, 12- or 22-bit hash, and only a table of calls already heard in full can turn it back into a call. The old one
 *  held 512 calls, never forgot one, and stopped SAVING when full — every decoded call is saved, so on a busy band it
 *  filled within hours and the receiver could resolve nothing it heard after that. And it returned the FIRST call
 *  whose hash matched: 512 calls in 1024 ten-bit buckets put a second call under a 10-bit hash about half the time,
 *  so a hashed call printed as somebody else's.
 *  ★ Now: least-recently-used (each entry carries the stamp of its last save or lookup; a full table evicts the
 *    oldest), and a hash that matches two DIFFERENT calls is unknown — "<...>", which the spot filter drops —
 *    rather than a guess. WSJT-X keeps its table the same way (recent calls, never a guess). */
class Ft8CallHashTable {
public:
    static constexpr int kCapacity = 512;
    /** ft8_lib's save_hash: `call` heard in full, its 22-bit hash `n22` (the 12- and 10-bit hashes are its top bits). */
    void save(const char* call, uint32_t n22);
    /** ft8_lib's lookup_hash: `bits` = 10, 12 or 22. Writes the call (NUL-terminated, at most 11 chars) into `out[12]`,
     *  or "" and false when no call — or more than one different call — has that hash. */
    bool lookup(int bits, uint32_t hash, char* out);
    void clear();
    int  size();
private:
    struct Entry { char call[12]; uint32_t n22; uint64_t stamp; };   // stamp 0 = empty
    std::mutex mtx_;
    Entry e_[kCapacity] = {};
    uint64_t clock_ = 0;
};
/** The one table every FT8/FT4 decoder shares (call hashes are global — a call heard on FT8 resolves on FT4). */
Ft8CallHashTable& ft8CallHashes();

class Ft8Decoder {
public:
    Ft8Decoder(int sampleRate, bool ft4);
    ~Ft8Decoder();

    // Feed mono int16 audio at the construction sample rate.
    void process(const int16_t* mono, int count);

    bool isFt4() const { return ft4; }

    /** ★ The callsign a spot may carry, from ft8_lib's de-call field: false for an empty one or an unresolved hash
     *  ("<...>"); a resolved hash ("<PJ4/K1ABC>") loses its brackets. Nothing with a '<' or '>' is ever spotted —
     *  the host's whitelist turned "<...>" into "" and the map got an EMPTY-callsign spot (audit 2026-10-04). */
    static bool spotCallsign(const char* deField, std::string& out);

    // call_to, call_de, grid (any may be empty), snr dB, audio offset Hz.
    std::function<void(const std::string& callTo, const std::string& callDe,
                       const std::string& grid, int snr, float audioHz)> onSpot;

private:
    /* ★★★ THE DECODE RUNS ON ITS OWN THREAD (2026-09-19). process() is called from the server's
     *  audio handler, and it used to call runDecode() INLINE when a slot filled: the whole candidate
     *  search and LDPC decode, every 15 s (FT8) and 7.5 s (FT4), on the thread that makes the audio.
     *  A fast machine hides it; a phone-class core does not — Stuart saw FT8 hang SDR++ Brown's
     *  audio on a Moto G35, and on a Pi 2 (VibeServer Lite) it would be a hitch every cycle.
     *  ★ Two spectrograms, used alternately: a finished slot is handed to the worker and the next one
     *    starts filling at once. If a decode is still running when the next slot completes (it had
     *    a whole slot's time), that slot is SKIPPED and counted — never waited for. */
    void runDecode(const monitor_t& m);
    void workerLoop();

    bool   ft4;
    int    rate;
    float  slotPeriod;
    int    capSamples;     // allocated buffer length (full slot)
    int    numSamples;     // active samples decoded per slot
    std::vector<float> samples;
    int    inPos = 0, framePos = 0;
    bool   tsync = false;
    bool   ok = false;
    monitor_t mon[2];
    int       active = 0;            // the spectrogram process() is filling
    std::thread worker;
    std::mutex  wm;
    std::condition_variable wcv;
    int  pending = -1;               // index handed to the worker, or -1
    bool busy = false, stop = false;
    std::atomic<bool> abortDecode{false};
    std::vector<ftx_candidate_t> cands;   // ★ per decoder — it was a function-local STATIC, shared by FT8 and FT4
public:
    std::atomic<unsigned> slotsSkipped{0};
};

} // namespace vibe
