// pbcusb: test-passband-cost's own costAt(), USB on the shared 2.048 MS/s chain only, widths 200/500/2700,
// best-of-3 inside costAt and 5 outer repeats — to see whether narrow USB got dearer in absolute terms.
#define main passband_cost_main
#include "test-passband-cost.cpp"
#undef main
int main() {
    const double fs = 2048000.0, off = 200000.0;
    auto iq = makeIq(fs, off, false);
    for (int rep = 0; rep < 5; ++rep) {
        for (double w : {2700.0, 200.0, 500.0, 1500.0}) {
            const Result b = costAt(fs, iq, off, M::SSB_USB, 0.0, w);
            const Result d = costAt(fs, iq, off, M::SSB_USB, 2700.0, w);
            std::printf("USB %6.0f  built %.3f%%  dragged %.3f%%  chFs %.0f taps %d (%d) weaver %d\n", w, b.cpu * 100, d.cpu * 100,
                        b.shape.chFs, b.shape.chanTaps, b.shape.lastTaps, b.shape.weaverTaps);
        }
    }
}
