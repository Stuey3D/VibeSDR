// tool-dec-replay.cpp — feed a decoder capture (VIBE_DEC_AUDIO_DUMP, android/app/src/main/cpp/vibe_dec_dump.h) back
// through a real DecoderHost and print every frame it sends. docs/v12/APRS-ON-AIR-2.md.
//
//   tool-dec-replay <dir>/dec-<unix>-aprs-0            the capture's base path: .txt (ext + params), .wav, .cf32 if there
//   tool-dec-replay --ext aprs --msg '{"packet":"1200"}' --wav f.wav [--iq f.cf32 --iq-rate 37500]
//   --params '<json>'   override the attach message the capture recorded (e.g. '{"packet":"1200"}' to leave 9600 out)
//   --quiet             the counts only, not each frame
// Any 16-bit or float WAV works (another receiver's recording too); it must be 48 kHz — the host's rate.
// Built like test-decoder-hosts (scripts/run-tests.sh deps_for); nothing is sent anywhere, nothing played.
#include "dec_replay.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string base, ext, msg, wav, iqPath, params;
    double iqRate = 0;
    bool quiet = false, haveParams = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--ext") ext = next();
        else if (a == "--msg" || a == "--params") { params = next(); haveParams = true; }
        else if (a == "--wav") wav = next();
        else if (a == "--iq") iqPath = next();
        else if (a == "--iq-rate") iqRate = std::atof(next().c_str());
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") { std::printf("see the top of tool-dec-replay.cpp\n"); return 0; }
        else base = a;
    }
    if (!base.empty()) {
        for (const char* sfx : { ".txt", ".wav", ".cf32" })
            if (base.size() > std::strlen(sfx) && base.compare(base.size() - std::strlen(sfx), std::strlen(sfx), sfx) == 0) base.resize(base.size() - std::strlen(sfx));
        const decreplay::Sidecar s = decreplay::readSidecar(base + ".txt");
        if (!s.ok) { std::fprintf(stderr, "%s.txt: not a decoder capture\n", base.c_str()); return 2; }
        if (ext.empty()) ext = s.ext;
        msg = s.params;
        if (wav.empty()) wav = base + ".wav";
        if (iqPath.empty() && s.iqRate > 0) { iqPath = base + ".cf32"; iqRate = s.iqRate; }
    }
    if (haveParams) msg = params;
    if (ext.empty() || wav.empty()) { std::fprintf(stderr, "usage: tool-dec-replay <capture base> | --ext E --msg JSON --wav F [--iq F --iq-rate R]\n"); return 2; }
    std::vector<float> audio; int rate = 0;
    if (!vibedec::DecDump::readWav(wav, audio, rate)) { std::fprintf(stderr, "%s: not a 16-bit or float WAV\n", wav.c_str()); return 2; }
    if (rate != 48000) { std::fprintf(stderr, "%s is %d Hz — the decoders take 48000\n", wav.c_str(), rate); return 2; }
    std::vector<std::complex<float>> iq;
    if (!iqPath.empty() && iqRate > 0 && !decreplay::readCf32(iqPath, iq)) std::fprintf(stderr, "%s: no channel IQ (audio only)\n", iqPath.c_str());
    std::printf("replay: %s, %.1f s of audio%s, ext %s, params %s\n", wav.c_str(), audio.size() / 48000.0,
                iq.empty() ? "" : (", " + std::to_string(iq.size() / iqRate).substr(0, 6) + " s of channel IQ at " + std::to_string((int)iqRate) + " S/s").c_str(),
                ext.c_str(), msg.c_str());
    decreplay::Peer peer;
    peer.echo = !quiet;
    std::string why;
    const auto t0 = std::chrono::steady_clock::now();
    if (!decreplay::replay(ext, msg, audio, iq, iqRate, peer, &why)) { std::fprintf(stderr, "%s\n", why.c_str()); return 1; }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto raws = decreplay::aprsRaws(peer);
    if (ext == "aprs") for (const auto& r : raws) std::printf("APRS  %s\n", r.c_str());
    std::printf("done: %zu text frames, %zu binary frames%s, %zu APRS packets, %.1f s to replay%s%s\n", peer.txt.size(), peer.bins,
                "", raws.size(), secs, why.empty() ? "" : " — ", why.c_str());
    return 0;
}
