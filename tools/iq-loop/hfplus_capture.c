/* hfplus_capture — record raw complex-float IQ straight off an Airspy HF+ at FIXED gain.
 *
 * For the website's Buzzer demo (BRIEF-waterfall-demo-iq-capture.md §1). VibeServer's own raw IQ out is not
 * used: it is refused on a shared dial, it is unsigned 8-bit, and it carries a slow auto-level (instant
 * attack, 3 dB/s release) that would pump the noise floor on every buzz — exactly what gives a loop away.
 * The radio's VibeServer instance must be STOPPED while this runs (one process owns the USB device).
 *
 *   usage: hfplus_capture <serial-hex> <centre-Hz> <rate> <seconds> <att 0..8> <lna 0|1> <out.cf32>
 *   Writes interleaved float32 I,Q (little-endian, native), headerless. Prints peak |x| and drops each second.
 *   build: cc -O2 -o hfplus_capture hfplus_capture.c -lairspyhf
 */
#include <libairspyhf/airspyhf.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *out;
static volatile uint64_t got = 0, dropped = 0;
static volatile float peak = 0;
static volatile int stop = 0;

static int cb(airspyhf_transfer_t *t) {
    const float *s = (const float *)t->samples;
    float pk = peak;
    for (int i = 0; i < t->sample_count * 2; i++) { float a = fabsf(s[i]); if (a > pk) pk = a; }
    peak = pk;
    fwrite(t->samples, sizeof(airspyhf_complex_float_t), t->sample_count, out);
    got += t->sample_count;
    dropped += t->dropped_samples;
    return 0;
}
static void onsig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv) {
    if (argc != 8) { fprintf(stderr, "usage: %s serial-hex centreHz rate seconds att lna out.cf32\n", argv[0]); return 2; }
    const uint64_t serial = strtoull(argv[1], NULL, 16);
    const uint32_t hz = (uint32_t)strtoul(argv[2], NULL, 10), rate = (uint32_t)strtoul(argv[3], NULL, 10);
    const int secs = atoi(argv[4]), att = atoi(argv[5]), lna = atoi(argv[6]);
    struct airspyhf_device *dev = NULL;
    if (airspyhf_open_sn(&dev, serial) != AIRSPYHF_SUCCESS) { fprintf(stderr, "open %s failed (is VibeServer still holding it?)\n", argv[1]); return 1; }
    uint32_t n = 0; airspyhf_get_samplerates(dev, &n, 0);
    uint32_t *rates = calloc(n ? n : 1, sizeof *rates); airspyhf_get_samplerates(dev, rates, n);
    fprintf(stderr, "rates:"); for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %u", rates[i]); fprintf(stderr, "\n");
    int rc = 0;
    rc |= airspyhf_set_samplerate(dev, rate);
    rc |= airspyhf_set_hf_agc(dev, 0);          /* ★ fixed gain: the radio's own AGC off */
    rc |= airspyhf_set_hf_att(dev, (uint8_t)att);
    rc |= airspyhf_set_hf_lna(dev, (uint8_t)lna);
    rc |= airspyhf_set_freq(dev, hz);
    if (rc) { fprintf(stderr, "setup failed (%d)\n", rc); airspyhf_close(dev); return 1; }
    out = fopen(argv[7], "wb");
    if (!out) { perror("out"); airspyhf_close(dev); return 1; }
    signal(SIGINT, onsig); signal(SIGTERM, onsig);
    const time_t t0 = time(NULL);
    fprintf(stderr, "start %s UTC  centre %u Hz  rate %u  att %d (%d dB)  lna %d\n",
            asctime(gmtime(&t0)), hz, rate, att, att * 6, lna);
    if (airspyhf_start(dev, cb, NULL) != AIRSPYHF_SUCCESS) { fprintf(stderr, "start failed\n"); return 1; }
    for (int s = 0; s < secs && !stop; s++) {
        sleep(1);
        fprintf(stderr, "%3ds  samples %" PRIu64 "  dropped %" PRIu64 "  peak %.3f (%.1f dBFS)\n",
                s + 1, (uint64_t)got, (uint64_t)dropped, peak, 20 * log10f(peak + 1e-12f));
        peak = 0;
    }
    airspyhf_stop(dev);
    airspyhf_close(dev);
    fclose(out);
    fprintf(stderr, "done: %" PRIu64 " samples, %" PRIu64 " dropped\n", (uint64_t)got, (uint64_t)dropped);
    return 0;
}
