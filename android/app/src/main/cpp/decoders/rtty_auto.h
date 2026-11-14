// rtty_auto — RTTY that sets itself up: the shift, centre, baud rate and polarity found from the signal.
//
// ★★ WHY (Stuart, 2026-10-04): "is there a way of having it auto config the baud and shift etc so again its a one click
//    use for users" — after a DWD session that printed nothing because the decoder had been left on the ham preset.
//
// How:
//  • TONES — RTTY is two steady tones, so the audio spectrum (averaged over a couple of seconds) shows two peaks. Their
//    midpoint is the centre and their spacing the shift; a spacing within 12 % of a standard shift (170, 200, 425, 450,
//    850 Hz) is snapped to it.
//  • BAUD + POLARITY + STOP — twelve FskDecoders run side by side on the same audio (45.45, 50, 75 baud × normal,
//    reverse × 1.5 or 1 stop bit) and
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

/** ★★ THE SEARCH'S RULES, SHARED (V12, 2026-11-14): the one-signal decoder (RttyAuto) and the RTTY skimmer
 *  (decoders/rtty_skimmer.h) try the same settings, score them the same way and pick a winner by the same rule — so a
 *  fix to one improves both (Stuart: "shared with the one-signal decoder so both improve together"). */
namespace rttyscore {
struct Setting { double baud; bool inv; bool oneStop; };
/** The standard speeds (45.45, 50, 75 baud) and shifts (170, 200, 425, 450, 850 Hz). */
const std::vector<double>& bauds();
const std::vector<double>& shifts();
/** The twelve settings a full search tries — every speed × 1.5 or 1 stop bit × normal or reverse, in RttyAuto's order. */
const std::vector<Setting>& all();
/** ★ The NEAREST standard shift within 12 % of a measured spacing (445 Hz measured is DWD's 450, not 425); 0 = none. */
double snapShift(double measuredHz);
/** A candidate's score for a stretch: frames that passed the start/stop check, less three for each that failed. */
inline double frameScore(double good, double bad) { return good - 3.0 * bad; }
/** ★ THE WINNER RULE: the best score, at least `minScore`, and at least `margin` ahead of every candidate with a
 *  DIFFERENT speed or polarity (a 1-stop decoder that waits for each start bit reads a 1.5-stop signal too, so the two
 *  stop lengths of the right speed tie — between them the higher score wins). Returns its index, or -1. */
int pickWinner(const Setting* s, const double* score, int n, double minScore = 12, double margin = 6);
/** "45" / "50" / "75" — how a speed is written to a person. */
const char* baudWord(double baud);
}

class RttyAuto {
public:
    explicit RttyAuto(int sampleRate);
    void process(const int16_t* samples, int count);
    std::function<void(char32_t)> onChar;
    std::function<void(int)>      onState;
    /** ★★ TUNING GUIDE (Stuart, 2026-10-04: "something like < 100Hz or 500Hz >"): how far the tones should move IN AUDIO
     *  PITCH (+ = higher) to sit comfortably in the passband, rounded to 100 Hz; 0 = fine (clears it). Sent only when
     *  it changes. The client turns it into a dial direction from its mode (USB: lower the dial to raise the pitch). */
    std::function<void(int)>      onTuneHint;
    // Health, as FskDecoder's (the admin page): the WINNER's, or the first candidate's while searching.
    unsigned long resyncs() const;
    double        audioLevel() const;
    double        audioThreshold() const;
    int           stateNow() const;
    /** What it settled on, for tests and logs ("" while searching). */
    std::string   chosen() const { return chosenText_; }

private:
    struct Cand {
        std::unique_ptr<FskDecoder> d; double baud; bool inv; bool oneStop = false;
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
    double lastCentre_ = 0, lastShift_ = 0;        // ★ the tones as last measured — a look-again starts there (AFC)
    // candidates
    std::vector<Cand> cands_;
    int winner_ = -1;
    long samplesSinceEval_ = 0, searchSamples_ = 0, badWinnerSec_ = 0;
    std::string chosenText_;
    int lastState_ = -1;
    int lastHint_ = 0, hintVotes_ = 0, pendingHint_ = 0;
    void hint_(int audioHz);
};

} // namespace vibe
