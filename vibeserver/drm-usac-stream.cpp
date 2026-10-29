// drm-usac-stream.cpp — an MP4 (AAC/USAC, one audio track) → the stream VibeServer's DRM path hands FFmpeg.
//
//   drm-usac-stream in.mp4 out.mkv [out.loas]   → prints "units N rate R frame F aot A"
//   ★ out.mkv: vibedrm::MkvStream — what vibe_drm_audiodec.h now feeds the FFmpeg child (the config as CodecPrivate).
//   ★ out.loas (optional): vibedrm::loasFrame() — the previous transport, kept to show FFmpeg refusing a USAC config
//     carried in-band (the finding that moved the path to Matroska, 2026-10-29).
//
// ★★ WHY (V12, 2026-10-29 — the bundled FFmpeg): no xHE-AAC DRM station has been receivable during this work, and we
//    build no USAC encoder. So the xHE-AAC path is tested on ISO's USAC CONFORMANCE STREAMS as published in FFmpeg's own
//    test suite (fate-suite.ffmpeg.org/aac/usac/ and aac/Fd_2_c1_Ms_*, each with its reference decode): this tool takes
//    the stream's AudioSpecificConfig and access units out of the MP4 and writes them with vibedrm::MkvStream — the very
//    writer vibe_drm_audiodec.h feeds the FFmpeg child with — so what reaches FFmpeg is the shape a DRM xHE-AAC station
//    produces. (The packages now decode xHE-AAC with libxaac in process — test-drm-xaac; this tool stays for FFmpeg checks.)
// ★ A deliberately small MP4 reader: moov/trak/mdia/minf/stbl → stsd (mp4a/esds → DecoderSpecificInfo), stsz, stsc,
//   stco/co64. Enough for the conformance files; not a general demuxer.
#include "../android/app/src/main/cpp/decoders/drm/drm_audio.h"
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

static std::vector<uint8_t> file;
static uint32_t be32(size_t o) { return (uint32_t(file[o]) << 24) | (uint32_t(file[o + 1]) << 16) | (uint32_t(file[o + 2]) << 8) | file[o + 3]; }
static uint64_t be64(size_t o) { return (uint64_t(be32(o)) << 32) | be32(o + 4); }

struct Track { uint32_t timescale = 0, delta = 0; std::vector<uint8_t> asc; std::vector<uint32_t> sizes; std::vector<uint64_t> chunks;
               std::vector<std::array<uint32_t, 2>> stsc; bool audio = false; };

static size_t findTag(size_t p, size_t end, uint8_t tag, size_t& len) {   // MPEG-4 descriptor walk inside esds
    while (p < end) {
        const uint8_t t = file[p++]; size_t l = 0;
        for (int i = 0; i < 4 && p < end; ++i) { const uint8_t b = file[p++]; l = (l << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        if (t == tag) { len = l; return p; }
        if (t == 0x03) { p += 3; continue; }            // ES_Descriptor: ES_ID(2) + flags(1), then children
        if (t == 0x04) { p += 13; continue; }           // DecoderConfigDescriptor: fixed 13 bytes, then children
        p += l;
    }
    return 0;
}

static void walk(size_t p, size_t end, Track& t) {
    while (p + 8 <= end) {
        uint64_t sz = be32(p); const std::string ty(reinterpret_cast<const char*>(&file[p + 4]), 4); size_t hdr = 8;
        if (sz == 1) { sz = be64(p + 8); hdr = 16; } else if (sz == 0) sz = end - p;
        const size_t b = p + hdr, e = p + size_t(sz);
        if (ty == "moov" || ty == "trak" || ty == "mdia" || ty == "minf" || ty == "stbl") walk(b, e, t);
        else if (ty == "mdhd") t.timescale = file[b] == 1 ? be32(b + 20) : be32(b + 12);
        else if (ty == "stts" && t.audio && be32(b + 4) > 0) t.delta = be32(b + 12);
        else if (ty == "hdlr") t.audio = std::string(reinterpret_cast<const char*>(&file[b + 8]), 4) == "soun";
        else if (ty == "stsd" && t.audio) {
            const size_t ent = b + 8;                                     // first sample entry (mp4a)
            for (size_t q = ent + 36; q + 8 <= e; ) {                     // its children after the audio sample entry fields
                const uint32_t csz = be32(q); const std::string cty(reinterpret_cast<const char*>(&file[q + 4]), 4);
                if (cty == "esds") { size_t l = 0; const size_t d = findTag(q + 12, q + csz, 0x05, l); if (d) t.asc.assign(&file[d], &file[d + l]); }
                if (csz < 8) break; q += csz;
            }
        }
        else if (ty == "stsz" && t.audio) { const uint32_t fixed = be32(b + 4), n = be32(b + 8);
            for (uint32_t i = 0; i < n; ++i) t.sizes.push_back(fixed ? fixed : be32(b + 12 + 4 * i)); }
        else if (ty == "stsc" && t.audio) { const uint32_t n = be32(b + 4);
            for (uint32_t i = 0; i < n; ++i) t.stsc.push_back({be32(b + 8 + 12 * i), be32(b + 12 + 12 * i)}); }
        else if (ty == "stco" && t.audio) { const uint32_t n = be32(b + 4); for (uint32_t i = 0; i < n; ++i) t.chunks.push_back(be32(b + 8 + 4 * i)); }
        else if (ty == "co64" && t.audio) { const uint32_t n = be32(b + 4); for (uint32_t i = 0; i < n; ++i) t.chunks.push_back(be64(b + 8 + 8 * i)); }
        if (ty == "trak" && t.audio && !t.sizes.empty()) return;
        p = e;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s in.mp4 out.mkv [out.loas]\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb"); if (!f) { std::perror(argv[1]); return 1; }
    std::fseek(f, 0, SEEK_END); file.resize(size_t(std::ftell(f))); std::fseek(f, 0, SEEK_SET);
    if (std::fread(file.data(), 1, file.size(), f) != file.size()) return 1; std::fclose(f);
    Track t; walk(0, file.size(), t);
    if (t.asc.empty() || t.sizes.empty() || t.chunks.empty() || t.stsc.empty()) { std::fprintf(stderr, "no AAC track\n"); return 1; }
    FILE* o = std::fopen(argv[2], "wb"); if (!o) { std::perror(argv[2]); return 1; }
    FILE* lo = argc > 3 ? std::fopen(argv[3], "wb") : nullptr;
    vibedrm::MkvStream mkv;
    const auto head = mkv.header(t.asc, int(t.timescale), 2, int(t.delta));
    std::fwrite(head.data(), 1, head.size(), o);
    size_t s = 0;
    for (size_t c = 0; c < t.chunks.size() && s < t.sizes.size(); ++c) {
        uint32_t per = t.stsc[0][1];
        for (const auto& r : t.stsc) if (c + 1 >= r[0]) per = r[1];
        uint64_t off = t.chunks[c];
        for (uint32_t k = 0; k < per && s < t.sizes.size(); ++k, ++s) {
            const auto blk = mkv.block(&file[size_t(off)], t.sizes[s]);
            std::fwrite(blk.data(), 1, blk.size(), o);
            if (lo) { const auto fr = vibedrm::loasFrame(t.asc, t.asc.size() * 8, &file[size_t(off)], t.sizes[s]);
                      std::fwrite(fr.data(), 1, fr.size(), lo); }
            off += t.sizes[s];
        }
    }
    std::fclose(o); if (lo) std::fclose(lo);
    int aot = t.asc[0] >> 3;
    if (aot == 31 && t.asc.size() > 1) aot = 32 + (((t.asc[0] & 7) << 3) | (t.asc[1] >> 5));
    // ★ USAC: the escaped object type (11 bits), the ASC's samplingFrequencyIndex (4, +24 when escaped) and
    //   channelConfiguration (4); then UsacConfig: usacSamplingFrequencyIndex (5, +24 when escaped) and
    //   coreSbrFrameLengthIndex — 0/1 = no SBR, 2/3/4 = SBR 8:3, 2:1, 4:1 (DRM's xHE-AAC uses 2–4).
    int sbrIdx = -1;
    if (aot == 42 && t.asc.size() >= 5) {
        auto bit = [&](size_t i) { return (t.asc[i >> 3] >> (7 - (i & 7))) & 1; };
        size_t p = 11; int a = 0; for (int i = 0; i < 4; ++i) a = (a << 1) | bit(p++);
        if (a == 0xF) p += 24;
        p += 4;
        int sfi = 0; for (int i = 0; i < 5; ++i) sfi = (sfi << 1) | bit(p++);
        if (sfi == 0x1F) p += 24;
        sbrIdx = 0; for (int i = 0; i < 3; ++i) sbrIdx = (sbrIdx << 1) | bit(p++);
    }
    std::printf("units %zu rate %u frame %u aot %d coreSbrFrameLengthIndex %d\n", s, t.timescale, t.delta, aot, sbrIdx);
    return 0;
}
