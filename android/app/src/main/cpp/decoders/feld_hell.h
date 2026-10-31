// decoders/feld_hell.h — ★ V12 (2026-10-31): FELD HELL on the listener's audio, drawn — not read. Hell is a facsimile of
// text: the eye reads it, so the receiver paints what it hears into a strip of columns and sends the strip down the image
// path (as WEFAX's 0x01 lines); no character recognition anywhere.
//
// ★ CLEAN ROOM. From published descriptions only: Wikipedia "Hellschreiber" (the receiver prints each received column
//   twice, one copy below the other, so that slight timing errors slant the text instead of cutting it; on-off keying)
//   and the Signal Identification Wiki's Hellschreiber table (Feld Hell: 122.5 Bd, about 2.5 characters a second,
//   a column every 1/17.5 s, OOK, about 350 Hz). So a column lasts 1/17.5 s and holds 122.5 / 17.5 = 7 elements; it is
//   sampled here at twice that (14 half-elements, the usual half-pixel), and a character (2.5 a second) is 7 columns.
//   Which end of a column is sent first was settled by drawing the public reference recordings (Wikimedia Commons
//   "Hellschreiber.ogg" / "Wikipedia-Slogan Hellschreiber.ogg", sigidwiki "FeldHell.mp3"): bottom first (section 13 of
//   docs/v12/DECODERS-WAVE2.md). Unsynchronised by design, as the original printer: the receiver's own clock, and the
//   doubled print absorbs the drift.
//
// ★ COST: one real FFT (PFFFT, its own NEON/SSE kernels) of 1024 points every 64 ms to find the tone; the mixer four
//   samples a lane (q65v); a running box filter (sequential, two adds a sample, scalar). No divide or round per sample.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class FeldHellDecoder {
public:
    static constexpr int kRows = 14;        ///< half-elements in a column (one copy)
    static constexpr int kStripCols = 210;  ///< columns in a strip: 12 s, 30 characters

    explicit FeldHellDecoder(double inRate);
    ~FeldHellDecoder();
    void feed(const float* x, int n);

    /** a row of the strip being drawn (`lineNo` = strip · 2·kRows + row, top row first; `width` = kStripCols; 255 =
     *  paper, 0 = ink). The strip's rows are sent again as it fills (every character), so a client that draws line y
     *  at y sees it grow in place; a finished strip is sent once more, complete. */
    std::function<void(uint32_t lineNo, uint32_t width, const uint8_t* px)> onLine;
    std::function<void(const std::string& json)> onStatus;   ///< {"hz":…,"signal":0|1} when it changes

    float hz() const;            ///< the tone being drawn (audio Hz), 0 before one is found
    bool signal() const;         ///< a keyed tone is there now

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

/** ★ Transmit side, for tests and benches only: `text` in Feld Hell at `audioHz`, unit amplitude, `fs` samples a second,
 *  `clockPpm` off the nominal timing (a sender's clock is never exact). Only A–Z, 0–9, space and a few marks
 *  (the font is this test's own 7 × 7 drawing, not a copy of anyone's). */
std::vector<float> feldHellSynth(const std::string& text, double fs, double audioHz, double clockPpm = 0);

} // namespace vibe
