// rtty_auto — RTTY that sets itself up: the shift, centre, baud rate and polarity found from the signal.
//
// ★★ WHY (Stuart, 2026-10-04): "is there a way of having it auto config the baud and shift etc so again its a one click
//    use for users" — after a DWD session that printed nothing because the decoder had been left on the ham preset.
//
// How:
//  • TONES — RTTY is two steady tones, so the audio spectrum (averaged over a couple of seconds) shows two peaks. Their
//    midpoint is the centre and their spacing the shift; a spacing within 12 % of a standard shift (170, 200, 425, 450,
//    850 Hz) is snapped to it.
//  • BAUD + POLARITY — six FskDecoders run side by side on the same audio (45.45, 50, 75 baud × normal, reverse) and
//    are scored on frames that pass the start/stop check against frames that fail it (FskDecoder::goodFrames /
//    framingErrors). The winner's text is shown, starting with what it had already decoded while the choice was made.
//  • It says what it chose, IN THE TEXT ("[RTTY auto: 50 baud, 450 Hz shift, reverse]"), so the app and the web client
//    both show it with no protocol change.
//  • It keeps watching: tones that move (a retune) or a winner whose errors climb start the search again.
#pragma once
#include "fsk_decoder.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class RttyAuto {
public:
    explicit RttyAuto(int sampleRate);
    void process(const int16_t* samples, int count);
    std::function<void(char32_t)> onChar;
    std::function<void(int)>      onState;
    // Health, as FskDecoder's (the admin page): the WINNER's, or the first candidate's while searching.
    unsigned long resyncs() const;
    double        audioLevel() const;
    double        audioThreshold() const;
    int           stateNow() const;
    /** What it settled on, for tests and logs ("" while searching). */
    std::string   chosen() const { return chosenText_; }

private:
    struct Cand {
        std::unique_ptr<FskDecoder> d; double baud; bool inv;
        std::string pending;                       // text decoded before the choice (replayed to the winner)
        unsigned long lastGood = 0, lastBad = 0;
        double score = 0;                          // recent good − 3 × bad, decayed
    };
    void spectrumStep_();
    void startCandidates_(double centre, double shift);
    void evaluate_();
    void say_(const std::string& s);

    int sr_;
    // spectrum
    static constexpr int kN = 8192;               // 5.9 Hz bins at 48 kHz
    std::vector<float> ring_;                      // last kN samples
    int ringPos_ = 0, sinceFft_ = 0;
    std::vector<double> psd_;                      // smoothed power, bins 0..kN/2
    int psdFrames_ = 0;
    double centre_ = 0, shift_ = 0;                // what the candidates were started with (0 = none)
    // candidates
    std::vector<Cand> cands_;
    int winner_ = -1;
    long samplesSinceEval_ = 0, searchSamples_ = 0, badWinnerSec_ = 0;
    std::string chosenText_;
    int lastState_ = -1;
};

} // namespace vibe
