// VibeSDR V4 — FT8 / FT4 decoder wrapper around ft8_lib (MIT).
#include "ft8_decoder.h"
#include "../vibe_clock.h"
#include <cmath>
#include <cstring>
#include <ctime>

extern "C" {
#include "ft8/decode.h"
#include "ft8/message.h"
#include "ft8/constants.h"
}

namespace vibe {

// ── Minimal callsign hashtable (for non-standard call resolution) ────────────
// ft8_lib needs a hash store so hashed callsigns ("<...>") can be resolved
// across messages. A small global table is fine — call hashes are global.
namespace {
struct HashEntry { char callsign[12]; uint32_t hash; };
// ★ The callsign hash table is shared by every decoder, and decodes now run on worker threads
//   (FT8 and FT4 can overlap), so both callbacks take this.
std::mutex g_htMtx;
constexpr int HT_MAX = 512;
HashEntry g_ht[HT_MAX] = {};

bool ht_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    std::lock_guard<std::mutex> lk(g_htMtx);
    int shift = (type == FTX_CALLSIGN_HASH_10_BITS) ? 12
              : (type == FTX_CALLSIGN_HASH_12_BITS) ? 10 : 0;
    for (int i = 0; i < HT_MAX; i++) {
        if (g_ht[i].callsign[0] && (g_ht[i].hash >> shift) == hash) {
            std::strncpy(callsign, g_ht[i].callsign, 11);
            return true;
        }
    }
    callsign[0] = '\0';
    return false;
}
void ht_save(const char* callsign, uint32_t n22) {
    std::lock_guard<std::mutex> lk(g_htMtx);
    uint16_t h10 = (n22 >> 12) & 0x3FF;
    int idx = (h10 * 23) % HT_MAX;
    for (int n = 0; n < HT_MAX; n++) {
        int j = (idx + n) % HT_MAX;
        if (!g_ht[j].callsign[0] ||
            (g_ht[j].hash == n22 && std::strncmp(g_ht[j].callsign, callsign, 11) == 0)) {
            std::strncpy(g_ht[j].callsign, callsign, 11);
            g_ht[j].callsign[11] = '\0';
            g_ht[j].hash = n22;
            return;
        }
    }
}
ftx_callsign_hash_interface_t g_hashIf = { ht_lookup, ht_save };

constexpr int kMaxCandidates = 140;
constexpr int kMaxDecoded    = 50;
constexpr int kLdpcIters     = 25;
constexpr int kMinScore      = 10;
constexpr int kFreqOsr       = 2;
constexpr int kTimeOsr       = 2;
} // namespace

Ft8Decoder::Ft8Decoder(int sampleRate, bool ft4_)
    : ft4(ft4_), rate(sampleRate) {
    slotPeriod = ft4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    capSamples = (int)(slotPeriod * rate);
    numSamples = (int)((slotPeriod - 0.4f) * rate);
    samples.assign(capSamples, 0.0f);

    monitor_config_t cfg = {};
    cfg.f_min = 100.0f;
    cfg.f_max = 3500.0f;
    cfg.sample_rate = rate;
    cfg.time_osr = kTimeOsr;
    cfg.freq_osr = kFreqOsr;
    cfg.protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    monitor_init(&mon[0], &cfg);
    monitor_init(&mon[1], &cfg);
    cands.resize(kMaxCandidates);
    ok = true;
    worker = std::thread([this] { workerLoop(); });
}

Ft8Decoder::~Ft8Decoder() {
    // ★ Abort a decode in progress (checked between candidates) rather than wait seconds for it:
    //   the caller — stopSpots — holds the lock the audio thread's feedSpots needs.
    abortDecode = true;
    { std::lock_guard<std::mutex> lk(wm); stop = true; }
    wcv.notify_all();
    if (worker.joinable()) worker.join();
    if (ok) { monitor_free(&mon[0]); monitor_free(&mon[1]); }
}

void Ft8Decoder::workerLoop() {
    std::unique_lock<std::mutex> lk(wm);
    for (;;) {
        wcv.wait(lk, [this] { return pending >= 0 || stop; });
        if (stop) return;
        const int idx = pending; pending = -1; busy = true;
        lk.unlock();
        runDecode(mon[idx]);
        monitor_reset(&mon[idx]);
        lk.lock();
        busy = false;
    }
}

void Ft8Decoder::process(const int16_t* in, int count) {
    if (!ok) return;

    // Slot alignment: wait until ~start of a UTC FT8/FT4 slot before buffering.
    if (!tsync) {
        const double timeShift = 0.8;
        // ★ Corrected UTC, not the system clock — see vibe_clock.h (a TV 2.6 s slow decoded ONE FT8 signal).
        double now = vibe::vibeUtcNow();
        double within = std::fmod(now - timeShift, slotPeriod);
        if (within < 0) within += slotPeriod;
        if (within > slotPeriod / 4) return;   // not at a slot boundary yet
        inPos = framePos = 0;
        tsync = true;
    }

    for (int i = 0; i < count && inPos < capSamples; i++)
        samples[inPos++] = (float)in[i] / 32768.0f;

    int blk = mon[active].block_size;
    while (inPos >= framePos + blk && framePos < numSamples) {
        monitor_process(&mon[active], samples.data() + framePos);
        framePos += blk;
    }
    if (framePos < numSamples) return;

    {
        std::lock_guard<std::mutex> lk(wm);
        if (!busy && pending < 0) {
            pending = active;                 // ★ hand the finished slot over...
            active = 1 - active;              // ...and fill the other one (reset by the worker)
            wcv.notify_one();
        } else {
            slotsSkipped.fetch_add(1);        // ★ the previous decode is still running: skip, never wait
            monitor_reset(&mon[active]);
        }
    }
    tsync = false;
}

void Ft8Decoder::runDecode(const monitor_t& mon) {
    const ftx_waterfall_t* wf = &mon.wf;
    int n = ftx_find_candidates(wf, kMaxCandidates, cands.data(), kMinScore);

    ftx_message_t decoded[kMaxDecoded];
    ftx_message_t* table[kMaxDecoded] = {};

    for (int idx = 0; idx < n; idx++) {
        if (abortDecode.load(std::memory_order_relaxed)) return;
        const ftx_candidate_t* c = &cands[idx];
        ftx_message_t msg; ftx_decode_status_t st;
        if (!ftx_decode_candidate(wf, c, kLdpcIters, &msg, &st)) continue;

        // Dedupe identical payloads within this slot.
        int h = msg.hash % kMaxDecoded;
        bool dup = false, empty = false;
        do {
            if (!table[h]) { empty = true; }
            else if (table[h]->hash == msg.hash &&
                     std::memcmp(table[h]->payload, msg.payload, sizeof(msg.payload)) == 0) { dup = true; }
            else h = (h + 1) % kMaxDecoded;
        } while (!empty && !dup);
        if (dup) continue;
        decoded[h] = msg; table[h] = &decoded[h];

        char callTo[24] = {}, callDe[24] = {}, grid[24] = {};
        ftx_field_t fields[FTX_MAX_MESSAGE_FIELDS];
        ftx_message_rc_t rc = ftx_message_decode_std(&msg, &g_hashIf, callTo, callDe, grid, fields);
        if (rc != FTX_MESSAGE_RC_OK) continue;
        if (!callDe[0]) continue;

        float audioHz = (mon.min_bin + c->freq_offset + (float)c->freq_sub / wf->freq_osr) / mon.symbol_period;
        int snr = (int)std::lround(c->score * 0.5f - 24.0f);   // score→dB, rough offset
        // ★★ ft8_lib's third field is "grid OR report": "IO92", but also "-12", "R-12", "73", "RRR" and
        //    "RR73". Only a real 4-character locator is a grid (B10: the map warned about ~40 "unparseable
        //    grids" on every update, and every RR73 sign-off — a VALID-LOOKING square near the North Pole —
        //    was plotted in the Arctic). Anything else is sent as no grid at all.
        const bool realGrid = std::strlen(grid) == 4
            && grid[0] >= 'A' && grid[0] <= 'R' && grid[1] >= 'A' && grid[1] <= 'R'
            && grid[2] >= '0' && grid[2] <= '9' && grid[3] >= '0' && grid[3] <= '9'
            && std::strcmp(grid, "RR73") != 0;
        if (!realGrid) grid[0] = 0;
        if (onSpot) onSpot(callTo, callDe, grid, snr, audioHz);
    }
}

} // namespace vibe
