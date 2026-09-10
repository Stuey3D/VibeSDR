// vibeserver/fuzz-dab.cpp — libFuzzer entry points for the DAB parsers that eat bytes off the air.
//
// ★★★ WHY THESE FIVE. Everything here is fed by a demodulator whose input is NOISE half the time:
//     a fading multiplex hands the parsers byte soup on every deep fade, and they must survive it
//     without reading past an array. That is not a hypothetical — the five DSP bugs of
//     2026-09-07 all presented as "a burst of errors three frames later", which is what a small
//     over-read looks like from the outside. A fuzzer finds them at the instruction instead.
//
// ★★ ONE BINARY, ONE TARGET, chosen by VIBE_FUZZ_TARGET at compile time — libFuzzer wants a
//    single LLVMFuzzerTestOneInput per binary, and a corpus per parser is worth having anyway.
//
//   scripts/fuzz-dab.sh            build them all and run each for a minute
//   scripts/fuzz-dab.sh 600 mot    ten minutes on the MOT assembler alone
//
// ★ Built with ASan + UBSan, so an over-read is a report rather than a wrong answer.
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

#include "vibe_dab_fic.h"
#include "vibe_dab_mot.h"
#include "vibe_dab_pad.h"
#include "vibe_dab_spi.h"
#include "vibe_dab_epg.h"

using namespace vibedab;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
#if defined(VIBE_FUZZ_FIB)
    /* A FIB is exactly 32 bytes and the CRC is checked inside; feed every 32-byte window so a
     * single input exercises a whole run of them against ONE ensemble, which is how the real
     * decoder sees them — state carried across FIBs is where the interesting bugs live. */
    Ensemble e;
    for (size_t off = 0; off + 32 <= size; off += 32) parseFib(data + off, e);

#elif defined(VIBE_FUZZ_MOT)
    /* MOT data groups arrive length-delimited from the packet layer; the first byte picks the
     * split so the fuzzer can build multi-segment objects and exercise reassembly, which is the
     * part with the buffers. Both the assembler and the carousel, since they differ. */
    if (size < 2) return 0;
    MotAssembler asm_;
    MotCarousel car;
    const size_t chunk = 1 + (size_t)data[0] * 4;      // 1..1021 bytes
    for (size_t off = 1; off < size; off += chunk) {
        const size_t n = (off + chunk <= size) ? chunk : (size - off);
        asm_.feedDataGroup(data + off, n);
        car.feedDataGroup(data + off, n);
    }

#elif defined(VIBE_FUZZ_PAD)
    /* X-PAD is fed as the tail of an audio frame, with `exact` deciding whether the length is
     * trusted — both paths, because the byte-reversal bug of 2026-09-07 lived in exactly this
     * indexing. The DLS assembler is driven through it. */
    if (size < 1) return 0;
    PadReader pad;
    const bool exact = (data[0] & 1) != 0;
    const size_t chunk = 2 + (size_t)(data[0] >> 1);   // 2..129 bytes, the real X-PAD range
    for (size_t off = 1; off < size; off += chunk) {
        const size_t n = (off + chunk <= size) ? chunk : (size - off);
        pad.feed(data + off, n, exact);
    }

#elif defined(VIBE_FUZZ_SPI)
    SpiDocument::parse(data, size);          // the binary XML of a station's own service info

#elif defined(VIBE_FUZZ_EPG)
    EpgDocument::parse(data, size);         // likewise, the schedule
#else
#  error "define one of VIBE_FUZZ_FIB / _MOT / _PAD / _SPI / _EPG"
#endif
    return 0;
}
