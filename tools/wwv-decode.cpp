// Replay a captured UberSDR stream through the REAL time decoder.
//
//   c++ -std=c++17 -O2 -I../android/app/src/main/cpp tools/wwv-decode.cpp \
//       android/app/src/main/cpp/decoders/time_decoder.cpp -lopus -o /tmp/wwv-decode
//   /tmp/wwv-decode /tmp/wwv-capture.bin [WWV|WWVB|MSF|DCF77|RWM]
//
// ★★★ THE POINT IS THE LIVE SIGNAL. WWV's synthetic test passes and shipped nothing, because the
//     station cannot be received from the UK — and this project already knows what a synthetic
//     test is worth when the author's assumption is the bug (MSF read LSB-first decoded a
//     confident "2064-02-22" and the test agreed). tools/wwv-capture.py fetches a real minute from
//     a receiver near Fort Collins; this plays it through the same TimeDecoder the app runs.
//
// ★★ It prints the BITS AND THE PULSE WIDTHS, not just the verdict. "No timestamp" is the symptom
//    both a framing bug and a dead band produce, and they want opposite work — the widths say
//    immediately which one you have (WWV's are 170 / 470 / 770 ms).
#include "decoders/time_decoder.h"

#include <opus/opus.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using vibe::TimeDecoder;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: wwv-decode <capture.bin> [station] [out.s16]\n"); return 2; }
    const std::string want = argc > 2 ? argv[2] : "WWV";

    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 1; }
    char magic[8];
    uint32_t rate = 0, count = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "WWVCAP01", 8) != 0) {
        std::fprintf(stderr, "not a capture file\n"); return 1;
    }
    std::fread(&rate, 4, 1, f);
    std::fread(&count, 4, 1, f);

    // ★★★ EVERY FRAME CARRIES A 21-BYTE HEADER AND THE OPUS PAYLOAD STARTS AFTER IT.
    //     UberSDR's own client (app.js, handleBinaryMessage) documents it:
    //       v1 (13 bytes): timestamp u64 · sample_rate u32 · channels u8
    //       v2 (21 bytes): the above + baseband_power f32 + noise_density f32
    //     Fed the whole message, libopus rejected half the packets and "decoded" the rest into
    //     junk — a TOC byte read out of a timestamp is occasionally a valid one, which is why the
    //     failure looked like a lossy 141 s out of 200 s rather than like garbage.
    // ★★ AND THE RATE COMES FROM THE HEADER, NOT FROM AN ASSUMPTION: this stream is 24 kHz, not
    //    the 48 kHz I first hard-coded. A decoder that measures 170/470/770 ms pulses against the
    //    wrong sample rate mis-measures every one of them by 2x.
    constexpr size_t kHdrV2 = 21;
    int err = 0;
    OpusDecoder* dec = opus_decoder_create((opus_int32)rate, 1, &err);
    if (!dec || err != OPUS_OK) { std::fprintf(stderr, "opus: %s\n", opus_strerror(err)); return 1; }

    TimeDecoder::Station st = TimeDecoder::Station::WWV;
    if (want == "WWVB") st = TimeDecoder::Station::WWVB;
    else if (want == "MSF") st = TimeDecoder::Station::MSF;
    else if (want == "DCF77") st = TimeDecoder::Station::DCF77;
    else if (want == "RWM") st = TimeDecoder::Station::RWM;

    TimeDecoder td((int)rate, st);
    int stamps = 0, bits = 0;
    size_t samples = 0;
    // Start time from the sidecar, so a bit can be placed against real UTC.
    double startUnix = 0;
    { const std::string meta = std::string(argv[1]) + ".json";
      FILE* mf = std::fopen(meta.c_str(), "rb");
      if (mf) { char buf[512]; const size_t n = std::fread(buf, 1, sizeof buf - 1, mf); buf[n] = 0;
                std::fclose(mf);
                const char* k = std::strstr(buf, "\"started_unix\":");
                if (k) startUnix = atof(k + 15); } }
    td.onState = [](TimeDecoder::State s) {
        const char* n[] = {"NoSignal", "Searching", "Reading", "Locked"};
        std::printf("  [state] %s\n", n[(int)s]);
    };
    // ★★★ THE DECODER'S SECOND NUMBER BESIDE THE REAL ONE. Everything else is guesswork without
    //     this: a frame can look internally consistent and still be anchored in the wrong place,
    //     and the only way to tell is to compare what the decoder CALLS second N against the UTC
    //     that sample actually arrived at. The capture's start time is in its sidecar .json.
    td.onBit = [&](int second, int bit) {
        bits++;
        const double t = startUnix + (double)samples / (double)rate;
        const long long utcSec = (long long)t % 60;
        std::printf("  bit s=%-3d %d   (real second %02lld)\n", second, bit, utcSec);
    };
    td.onTime = [&](const TimeDecoder::TimeStamp& t) {
        stamps++;
        std::printf("  ★ TIMESTAMP %04d-%02d-%02d %02d:%02d UTC  dst=%d leap=%d\n",
                    t.year, t.month, t.day, t.hour, t.minute, (int)t.dst, (int)t.leapSecondPending);
    };

    // ★ Raw mono s16 alongside, so the signal itself can be looked at (a spectrum, an envelope)
    //   when the decoder says nothing — "no timestamp" and "no station" are indistinguishable
    //   from the decoder's output alone.
    FILE* wav = argc > 3 ? std::fopen(argv[3], "wb") : nullptr;
    int bad = 0;
    std::vector<int16_t> pcm(5760);            // 120 ms at 48 kHz — larger than any Opus frame
    std::vector<uint8_t> pkt;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = 0;
        if (std::fread(&len, 4, 1, f) != 1) break;
        pkt.resize(len);
        if (std::fread(pkt.data(), 1, len, f) != len) break;
        if (len <= kHdrV2) { bad++; continue; }
        const int n = opus_decode(dec, pkt.data() + kHdrV2, (opus_int32)(len - kHdrV2),
                                  pcm.data(), (int)pcm.size(), 0);
        // ★★★ A DROPPED PACKET IS NOT A NEUTRAL LOSS HERE. This decoder measures PULSE WIDTHS —
        //     170 / 470 / 770 ms — against a sample count, so audio that silently goes missing
        //     SHORTENS every pulse that spans it and no amount of framing work will recover the
        //     timecode. Count them out loud rather than `continue`-ing past.
        if (n <= 0) { bad++; continue; }
        if (wav) std::fwrite(pcm.data(), 2, (size_t)n, wav);
        td.process(pcm.data(), n);
        samples += (size_t)n;
    }
    std::fclose(f);
    if (wav) std::fclose(wav);
    opus_decoder_destroy(dec);

    std::printf("\n%zu samples (%.1f s), %d undecodable packet(s), %d bits, %d timestamp(s)"
                " — station %s\n",
                samples, (double)samples / rate, bad, bits, stamps, want.c_str());
    return stamps > 0 ? 0 : 1;
}
