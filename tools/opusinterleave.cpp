// Does the Opus encoder keep L and R the right way round ACROSS A CHANNEL-COUNT CHANGE?
//
// ★★★ THE BUG THIS CATCHES. `buf_` is an interleaved carry-over, so its alignment is load-bearing:
//     leave an ODD number of samples in it while stereo and every later sample shifts one place,
//     swapping L and R for the rest of the stream. That inverts the difference channel — L+R
//     becomes L-R — which on WFM is the stereo image turning inside out. It needs a mono/stereo
//     transition to leave a partial frame behind, so it is intermittent and never fires in the
//     mono modes that get tested most.
// ★ Encodes only; the assertion is on the encoder's own buffer alignment, which is the invariant
//   that was broken. A full round-trip would need a decoder linked in and would test Opus, not us.
#include "opus_audio_encoder.h"
#include <cstdio>
#include <vector>

int main() {
    vibe::OpusAudioEncoder enc;
    std::vector<std::vector<uint8_t>> out;
    // A deliberately AWKWARD sequence: counts that do not divide the 960-sample frame, and a
    // mono->stereo->mono walk, which is what a WFM pilot flapping in and out actually produces.
    const int seq[][2] = { {1000,2}, {517,2}, {733,1}, {291,1}, {1024,2}, {13,2}, {960,2}, {7,1} };
    bool ok = true;
    for (auto& s : seq) {
        const int count = s[0], ch = s[1];
        std::vector<int16_t> pcm((size_t)count * ch);
        // L = +1000, R = -1000: any swap is unmissable, and it survives Opus.
        for (int i = 0; i < count; i++) {
            pcm[(size_t)i * ch] = 1000;
            if (ch == 2) pcm[(size_t)i * 2 + 1] = -1000;
        }
        out.clear();
        enc.encode(pcm.data(), count, ch, out);
        const size_t left = enc.pendingSamples();
        if (ch == 2 && (left & 1)) {
            printf("  FAIL: after %d frames x %dch, %zu samples left — ODD, so L/R are swapped\n",
                   count, ch, left);
            ok = false;
        } else {
            printf("  ok: %5d x %dch -> %2zu packets, %4zu samples carried (aligned)\n",
                   count, ch, out.size(), left);
        }
    }
    printf("\n%s\n", ok ? "PASS — the interleaved carry-over stays frame-aligned."
                        : "FAIL — carry-over misaligned: L and R will swap.");
    return ok ? 0 : 1;
}
