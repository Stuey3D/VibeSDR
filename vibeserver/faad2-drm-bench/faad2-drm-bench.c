/* faad2-drm-bench.c — FAAD2's DRM build on its own: decode a .drmau file (drm-au-dump.cpp) the way DrmFaad does
 * (vibe_drm_audiodec.h: the CRC-8 byte in front of each frame, float output), time it, and write the PCM for a
 * comparison against another build of the library (faad2-drm-compare.py). Standalone C, no VibeServer code, so the one
 * binary can be copied to a scratch directory on a test machine (a Raspberry Pi 2) and run there with nothing else.
 *
 *   faad2-drm-bench <in.drmau> [-o out.f32] [-r repeats] [-c DRMCH] [-x N]
 *     -o   write the first pass's PCM: "F32PCM\0\0", u32 rate, u32 channels, then interleaved float32 (native order)
 *     -r   decode the whole file this many times (a fresh decoder each pass) and report the best and median pass
 *     -c   override the channel mode (DRMCH_* in neaacdec.h): 1 mono, 2 stereo, 3 SBR mono, 4 SBR stereo, 5 SBR+PS
 *     -x   damage every Nth unit on purpose (every third byte of its second half XOR 0x5A: past the CRC-8-checked side
 *          information, into the spectral and SBR data), to compare two builds on what they do with garbage too
 *
 * Reports decoder CPU time per second of audio (% of one core) — CLOCK_THREAD_CPUTIME_ID around NeAACDecDecode only.
 * GPL-2.0-or-later (it links FAAD2). */
#define _POSIX_C_SOURCE 200809L
#include <neaacdec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const unsigned char* p; size_t n; } Unit;

static double now_cpu(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static int cmpd(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    const char *in = NULL, *outp = NULL; int repeats = 1, chOverride = 0, damage = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) repeats = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chOverride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) damage = atoi(argv[++i]);
        else in = argv[i];
    }
    if (!in || repeats < 1) { fprintf(stderr, "usage: faad2-drm-bench <in.drmau> [-o out.f32] [-r repeats] [-c DRMCH] [-x N]\n"); return 2; }
    FILE* f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }
    fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz || sz < 16 || memcmp(buf, "DRMAU1", 6)) { fprintf(stderr, "%s: not a .drmau\n", in); return 1; }
    fclose(f);
    const uint32_t rate = (uint32_t)buf[8] | (uint32_t)buf[9] << 8 | (uint32_t)buf[10] << 16 | (uint32_t)buf[11] << 24;
    const unsigned char ch = chOverride ? (unsigned char)chOverride : buf[12];
    size_t nu = 0, cap = 1024; Unit* u = malloc(cap * sizeof *u);
    for (size_t o = 16; o + 2 <= (size_t)sz;) {
        const size_t n = (size_t)buf[o] | (size_t)buf[o + 1] << 8; o += 2;
        if (o + n > (size_t)sz) break;
        if (nu == cap) u = realloc(u, (cap *= 2) * sizeof *u);
        if (damage > 0 && (nu + 1) % (size_t)damage == 0)
            for (size_t k = n / 2; k < n; k += 3) buf[o + k] ^= 0x5A;
        u[nu].p = buf + o; u[nu].n = n; ++nu; o += n;
    }
    FILE* of = outp ? fopen(outp, "wb") : NULL;
    double* passCpu = calloc((size_t)repeats, sizeof(double));
    double audioS = 0; long good = 0, bad = 0; unsigned long outRate = 0; unsigned char outCh = 0;
    for (int r = 0; r < repeats; ++r) {
        NeAACDecHandle h = NeAACDecOpen();
        if (NeAACDecInitDRM(&h, rate, ch) < 0) { fprintf(stderr, "NeAACDecInitDRM(%u, %u) refused\n", rate, ch); return 1; }
        NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(h);
        c->outputFormat = FAAD_FMT_FLOAT;
        NeAACDecSetConfiguration(h, c);
        double cpu = 0; long samples = 0;
        for (size_t i = 0; i < nu; ++i) {
            NeAACDecFrameInfo fi; memset(&fi, 0, sizeof fi);
            const double t0 = now_cpu();
            void* pcm = NeAACDecDecode(h, &fi, (unsigned char*)u[i].p, (unsigned long)u[i].n);
            cpu += now_cpu() - t0;
            if (r) continue;
            if (!pcm || fi.error || !fi.samples || !fi.channels) { ++bad; continue; }
            ++good; samples += (long)fi.samples;
            if (!outRate) {
                outRate = fi.samplerate; outCh = fi.channels;
                if (of) { const uint32_t hdr[2] = { (uint32_t)outRate, outCh }; fwrite("F32PCM\0\0", 1, 8, of); fwrite(hdr, 4, 2, of); }
            }
            if (of && fi.channels == outCh) fwrite(pcm, sizeof(float), fi.samples, of);
        }
        if (!r && outRate) audioS = (double)samples / (double)outCh / (double)outRate;
        passCpu[r] = cpu;
        NeAACDecClose(h);
    }
    if (of) fclose(of);
    qsort(passCpu, (size_t)repeats, sizeof(double), cmpd);
    const double best = passCpu[0], med = passCpu[repeats / 2];
    char* id = NULL; char* cp = NULL; NeAACDecGetVersion(&id, &cp);
    printf("%s: FAAD2 %s, DRMCH %u at %u Hz -> %lu Hz x%u; %zu units, %ld decoded, %ld failed; %.1f s of audio\n",
           in, id ? id : "?", ch, rate, outRate, outCh, nu, good, bad, audioS);
    if (audioS > 0)
        printf("cpu: best %.4f s, median %.4f s of %d passes = %.3f %% / %.3f %% of a core (%.1f us a frame)\n",
               best, med, repeats, 100.0 * best / audioS, 100.0 * med / audioS, 1e6 * med / (double)nu);
    return 0;
}
