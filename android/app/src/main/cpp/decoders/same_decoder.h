// decoders/same_decoder.h — EAS / SAME: the US Emergency Alert System header codes, as NOAA Weather Radio and every EAS
// participant send them (V12, 2026-10-31).
//
// ★★★ THE STANDARD: 47 CFR § 11.31 (eCFR, public), read 2026-10-31 — "AFSK at 520.83 bits per second", mark 2083.3 Hz,
//     space 1562.5 Hz, mark and space time 1.92 ms; 7-bit ASCII (ANSI X3.4-1977) with an eighth null bit; a preamble of
//     sixteen bytes of AB hex before each header and EOM; the header ZCZC-ORG-EEE-PSSCCC(-PSSCCC…)+TTTT-JJJHHMM-LLLLLLLL-
//     sent three times, at least one second apart; then the attention signal (853 + 960 Hz, 8–25 s), the message, and
//     NNNN three times. ORG (EAS, CIV, WXR, PEP), the event codes (national: EAN, NIC, NPT, RMT, RWT; state/local: the
//     § 11.31(e) list), PSSCCC (P the county part 0–9, SS the state number, CCC the county, 000 = the whole state;
//     marine areas 57–98), TTTT (valid for, hh mm), JJJHHMM (day of the year, UTC), LLLLLLLL (the sender, "/" not "-").
//   ★ Bit order: least significant bit first — the null eighth bit then falls last in each byte (as the "eighth null
//     bit to form a full byte" requires); the preamble's 0xAB then reads 1101 0101 on the air. A test pins it.
//
// ★★ THE RECEIVER (audio at any rate ≥ 8 kHz; the host gives 48 kHz): box-average to ≈ 12 kHz; the two tones mixed down
//    from exact tables (at 12 kHz a bit is 23.04 samples and 25 bits are 576 — both tones complete whole cycles there,
//    100 and 75, so one 576-entry table is exact for ever); one-bit boxcar correlators; d = |mark|² − |space|²; a DPLL on
//    d's zero crossings (a crossing is half a bit before the decision); bits LSB first into a 32-bit register.
//    Acquire on four preamble bytes (0xABABABAB) — 32 exact bits —; then bytes: the first non-preamble byte must be 'Z'
//    (header) or 'N' (EOM), every byte's eighth bit must be 0 and the character printable. The header is read to its end
//    (three '-' after the '+') and parsed strictly.
// ★★ THE THREE BURSTS ARE COMBINED SOFT: each bit's decision value (|mark|² − |space|², normalised by their sum) is kept,
//    and the bursts of one alert are summed bit by bit before the bytes are decided — so a bit wrong in one burst is
//    outvoted by its strength in the others, not by a coin toss. SAME has no checksum, so two independent readings must
//    agree before an alert is shown: two bursts with identical characters, or the soft sum of three equal to their
//    character majority. A header heard in one burst only (tuned in mid-alert) is not shown — measured: its clean-looking
//    bits still misread digits. One alert per header (10 minutes).
// ★★★ NEVER ON NOISE: acquisition needs 32 exact preamble bits, then 'ZCZC' and a strict parse; measured on hours of noise.
// ★ COST: the mixers are q65v lanes (NEON / SSE2 / scalar, identical); the boxcar running sums and the DPLL are per-sample
//   feedback, ≈ 20 flops a sample at 12 kHz (≈ 0.25 M flops a second) — not vectorised, nothing to put in lanes.
//   No divide or round per sample.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vibe {

class SameDecoder {
public:
    struct Alert {
        std::string raw;          ///< the header as heard ("ZCZC-WXR-TOR-024031+0030-3041345-KLWX/NWS-")
        std::string org, event;   ///< "WXR", "TOR"
        std::vector<std::string> areas;   ///< PSSCCC
        int validMin = 0;         ///< TTTT in minutes
        int day = 0, hour = 0, minute = 0;   ///< JJJHHMM (UTC)
        std::string sender;       ///< LLLLLLLL
        int heard = 0;            ///< bursts combined (2 or 3)
        float margin = 0;         ///< the weakest bit's combined margin (0..1)
    };
    explicit SameDecoder(double rateHz);
    /** Audio (any scale). */
    void feed(const float* x, int n);
    void reset();
    std::function<void(const Alert&)> onAlert;
    std::function<void()> onEom;               ///< NNNN (once per run of three)
    std::function<void(int)> onState;          ///< 0 listening · 1 reading a burst
    std::function<void(const std::string&)> onBurst;   ///< each burst's hard characters (the test's view)

    // ── for the test and the clients ──
    /** Parse a header strictly; false if anything is out of the § 11.31 format. */
    static bool parse(const std::string& raw, Alert& a);
    static const char* eventName(const std::string& eee);   ///< "Tornado Warning", or "" if unknown
    static const char* orgName(const std::string& org);
    static const char* stateAbbr(int ss);                   ///< "MD" for 24, "" if unknown
    /** One line of text for the decoder box. */
    static std::string line(const Alert& a);

private:
    void bit(int b, float soft);
    void byteIn(uint8_t c);
    void endBurst();
    void resolve();
    int dec_ = 4;            // input samples per 12 kHz sample (box average)
    double fs_ = 12000;
    int decN_ = 0; float decAcc_ = 0;
    std::vector<float> cm_, sm_, cs_, ss_;   // tables (576 + 3 padding): cos/sin of mark, space
    std::vector<float> pMr_, pMi_, pSr_, pSi_;   // the block's mixer products, planar
    int tPos_ = 0;
    // boxcar
    int L_ = 23;
    std::vector<float> ring_; int rPos_ = 0;   // 4 products a sample
    float sMr_ = 0, sMi_ = 0, sSr_ = 0, sSi_ = 0;
    float dPrev_ = 0; float ph_ = 0, inc_ = 0, dInc_ = 0;
    int warm_ = 0;
    uint32_t sr_ = 0; int st_ = 0;  // 0 hunt, 1 preamble, 2 reading
    int nBit_ = 0; uint8_t cur_ = 0;
    std::string buf_; int dashes_ = -1; int badRun_ = 0;
    std::vector<std::vector<float>> bursts_; std::vector<float> soft_, last8_; int invalidRun_ = 0; double lastBurstT_ = -1e9, t_ = 0;
    double lastEomT_ = -1e9;
    std::string lastAlert_; double lastAlertT_ = -1e9;
    bool eom_ = false;
    int state_ = 0;
};

} // namespace vibe
