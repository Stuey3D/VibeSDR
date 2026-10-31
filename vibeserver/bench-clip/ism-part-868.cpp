// ism-part-868.cpp — make-ism-records' 868 MHz half: test-wmbus.cpp's own frames (the design documents' worked examples)
// and its front end, reached by compiling the test with its main() renamed. Tool only; never in a server build.
#define main test_wmbus_main
#include "../test-wmbus.cpp"
#undef main

/** Synthetic meters only, no noise: the test's Sensus (T1) and Kamstrup (C1-A) frames — documentation examples, no real
 *  meter — `lenS` long, the radio at `centre`. */
std::vector<cf> ism868Synthetic(double centre, double rate, double lenS) {
    std::vector<Tx> txs;
    Tx a; a.chips = chipsT(frameA(sensusBody(0x68, 0, 0x2A))); a.offHz = 868.95e6 - centre; a.startS = 0.02; txs.push_back(a);
    Tx k; k.chips = chipsC(frameA(kamBody()), false); k.offHz = 868.95e6 - centre + 10e3; k.devHz = 45e3; k.startS = 0.25; txs.push_back(k);
    return synth(txs, rate, lenS, 0.0, 1);
}
/** Bursts the 868 front end finds in `x` (radio at `hw`), and the readings decoded from them. */
size_t ism868Bursts(const std::vector<cf>& x, double rate, double hw, std::vector<std::string>* models) {
    size_t nb = 0;
    const auto r = runFront(x, rate, nullptr, hw, &nb);
    if (models) for (const auto& v : r) models->push_back(v.model);
    return nb;
}
