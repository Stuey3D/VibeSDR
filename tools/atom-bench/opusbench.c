/* opusbench: the server's per-listener Opus settings (48 kHz, stereo, AUDIO, MUSIC, complexity 6),
 * 20 ms frames, 60 s of a synthetic stereo programme (tones + noise), bitrate from argv. */
#include <opus/opus.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    int br = argc > 1 ? atoi(argv[1]) : 96000, err;
    OpusEncoder* e = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(e, OPUS_SET_BITRATE(br)); opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(6));
    float pcm[960 * 2]; unsigned char out[4000]; unsigned r = 1; long bytes = 0; double t = 0;
    for (int f = 0; f < 3000; ++f) {
        for (int i = 0; i < 960; ++i, t += 1.0 / 48000) {
            r = r * 1103515245u + 12345u; float n = ((r >> 9) & 0xFFFF) / 65536.0f - 0.5f;
            pcm[2*i]   = 0.3f * sinf(2 * M_PI * 440 * t) * (0.6f + 0.4f * sinf(2 * M_PI * 0.7 * t)) + 0.05f * n;
            pcm[2*i+1] = 0.3f * sinf(2 * M_PI * 660 * t + 0.3) + 0.2f * sinf(2 * M_PI * 3100 * t) + 0.05f * n;
        }
        bytes += opus_encode_float(e, pcm, 960, out, sizeof out);
    }
    printf("%ld bytes\n", bytes);
    return 0;
}
