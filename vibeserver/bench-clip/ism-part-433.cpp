// ism-part-433.cpp — make-ism-records' 433 MHz half: test-ism.cpp's own encoders (written from the published specs) and
// its front end, reached by compiling the test with its main() renamed. Tool only; never in a server build.
#define main test_ism_main
#include "../test-ism.cpp"
#undef main

/** Synthetic sensors only, no noise: an Oregon v3 THGR810, a Fine Offset WH1080 and a fixed-code remote — the test's own
 *  made-up IDs and codes — at their usual 433 MHz frequencies, `lenS` long. */
std::vector<cf> ism433Synthetic(double centre, double rate, double lenS) {
    std::vector<Sig> s;
    Sig a{oregonV3(withChecksum(oregonNibbles("F824" "2" "6B" "0" "481" "0" "26" "0"), 17))}; a.offHz = 433.92e6 - centre; a.startS = 0.02; s.push_back(a);
    Sig b{wh1080("a1820d5a0306004e08d3")}; b.offHz = 433.85e6 - centre; b.startS = 0.25; s.push_back(b);
    Sig d{fixedCodeTx(0xA5C3E8)}; d.offHz = 434.00e6 - centre; d.startS = 0.30; s.push_back(d);
    return synth(s, rate, lenS, 0.0, 1);
}
/** Bursts the front end finds in `x` (the 433 band's setup), and the readings decoded from them. */
size_t ism433Bursts(const std::vector<cf>& x, double rate, std::vector<std::string>* models) {
    std::vector<Burst> bursts;
    const auto r = runFront(x, rate, &bursts);
    if (models) for (const auto& v : r) models->push_back(v.model);
    return bursts.size();
}
