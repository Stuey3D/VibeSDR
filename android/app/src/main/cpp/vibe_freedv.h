// vibe_freedv.h — FreeDV DIGITAL VOICE AS A MODE (V12, docs/v12/FREEDV.md): the radio demodulates USB as usual, and the
// demodulated audio goes through the FreeDV modem and the Codec 2 vocoder; the decoded speech replaces the SSB audio.
// Silent until there is speech to play; the VTS says why ({"type":"freedv"} status reports).
//
// ★★★ THE LIBRARY IS THE SYSTEM'S, LOADED AT RUN TIME. Codec 2 (LGPL-2.1, David Rowe) is the only definition of the
//     vocoder, so it is linked rather than rewritten — DYNAMICALLY, and only if the owner's system has it (Debian/Ubuntu/
//     Raspberry Pi OS package libcodec2-1.x, Homebrew `codec2` on a Mac). Nothing of codec2 is compiled into this tree:
//     the few FreeDV API calls below are declared here by their published C signatures and found with dlsym. A server
//     without the library runs exactly as before and simply does not offer the mode ("no dead controls" — hwinfo's
//     `modes` lists "freedv" only when this loads, and the clients draw the button from that).
// ★★ WHY dlopen AND NOT A LINK-TIME DEPENDENCY: the .deb names nothing but libc6 in Depends (CMakeLists "NOTHING FROM
//    THE SYSTEM BUT glibc"); a hard libcodec2 dependency would refuse to install wherever the package has another
//    soname. The package RECOMMENDS libcodec2 instead, and a missing one costs only this mode.
// ★★ THREADING (Network > Audio > Spectrum > Decoders — and here the decoded speech IS the audio): the audio callback
//    only COPIES its block into a bounded queue (oldest dropped and counted past 2 s) and takes back exactly the frames
//    it is sending from a PCM buffer (silence until primed, silence on underrun, the oldest dropped past 1.5 s). The
//    modem and the vocoder run on their own vibe-freedv thread at the audio-codec rung, below the chain that feeds it —
//    as DAB+'s FAAD2 does. A slow modem can never stall the audio thread: at worst it is late and the listener hears a gap.
// ★★ AUTO: the priority modes (RADE V1, 700D, 700E, 1600) are all demodulated while searching, and the first to hold sync
//    for a second wins; the others are closed (memory and CPU back) and AUTO searches again only after 8 s without sync.
//    A manual choice runs that one mode only — the fallback for when AUTO cannot settle (house rule: auto decoders keep
//    manual controls underneath).
// ★★★ RADE V1 (V12, 2026-11-16 — what nearly every FreeDV station sends today) is NOT codec2: it is FreeDV's own C port,
//     rade_c, with Opus's FARGAN vocoder (all BSD), COMPILED IN where the build found it (VIBE_HAVE_RADE, librade_vibe.a —
//     rade/vibe_rade.h, vibeserver/linux/build-rade.sh). So a server without libcodec2 still offers FreeDV, with RADE
//     alone; a server with neither does not offer the mode at all. Its end-of-over callsign (CRC-checked) is reported as
//     "last" once the over ends — it names the station just heard, never the one talking now.
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(VIBE_HAVE_RADE)
#include "vibe_rade.h"   // ★ the RADE V1 receiver (librade_vibe.a); its transmitter (librade_vibe_tx.a) only in tests
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(_WIN32) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define VIBE_FREEDV_NO_DLOPEN 1
#else
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vibe {
namespace freedv {

/** ★ The sub-modes we offer. 0 = AUTO. The numbers are ours, not codec2's (RADE is not codec2's at all). The picker's
 *  order is subsJson's: RADE first — it is what is on air. */
enum Sub : int { kAuto = 0, k700D = 1, k700E = 2, k1600 = 3, kRade = 4 };
constexpr int kSubCount = 5;
inline const char* subName(int s) {
    switch (s) {
        case k700D: return "700D"; case k700E: return "700E"; case k1600: return "1600"; case kRade: return "RADE";
        default: return "auto";
    }
}
/** "700D" / "700d" / "auto" → Sub; -1 when it is none of ours. */
inline int subFromName(const std::string& n) {
    std::string u;
    for (char c : n) u += char(std::toupper((unsigned char)c));
    if (u == "AUTO" || u.empty()) return kAuto;
    if (u == "700D") return k700D;
    if (u == "700E") return k700E;
    if (u == "1600") return k1600;
    if (u == "RADE" || u == "RADEV1") return kRade;
    return -1;
}
/** ★ The sideband choice (Stuart 2026-11-16): AUTO follows FreeDV's SSB convention — LSB below 10 MHz, USB from 10 MHz
 *  up — and USB / LSB override it, for a station (or a net, or a band plan) that does not. An inverted modem signal never
 *  syncs, so this is the fallback when AUTO finds nothing on a signal that is plainly there. */
enum Sb : int { kSbAuto = 0, kSbUsb = 1, kSbLsb = 2 };
inline const char* sbName(int s) { return s == kSbUsb ? "usb" : s == kSbLsb ? "lsb" : "auto"; }
/** "usb" / "LSB" / "auto" → Sb; -1 when it is none of ours. */
inline int sbFromName(const std::string& n) {
    std::string u;
    for (char c : n) u += char(std::tolower((unsigned char)c));
    if (u == "auto" || u.empty()) return kSbAuto;
    if (u == "usb") return kSbUsb;
    if (u == "lsb") return kSbLsb;
    return -1;
}
/** ★ THE ONE SIDEBAND RULE: true = demodulate LSB. Every demodulator site (shared and per listener) asks this. */
inline bool lowerSideband(double dialHz, int sb) {
    if (sb == kSbLsb) return true;
    if (sb == kSbUsb) return false;
    // ★ 60 m is USB (the FreeDV application's own rule: USB from 10 MHz up, or anywhere in the 60 m band, 5.25–5.45 MHz)
    if (dialHz >= 5.25e6 && dialHz <= 5.45e6) return false;
    return dialHz > 0 && dialHz < 10e6;
}

/** codec2's own mode numbers (freedv_api.h, stable since 1.0: FREEDV_MODE_1600 0, _700D 7, _700E 13). */
inline int codec2Mode(int s) { return s == k700D ? 7 : s == k700E ? 13 : s == k1600 ? 0 : -1; }

// ── The FreeDV API, by its published C signatures (freedv_api.h) — the struct itself stays opaque ──────────────────────
struct FdvOpaque;
struct Api {
    bool ok = false;
    std::string why;            ///< why not, in words, when !ok ("libcodec2 is not installed")
    std::string path;           ///< the library that loaded
    int version = 0;            ///< freedv_get_version()
    bool sub[kSubCount] = {false, false, false, false, false};   ///< which sub-modes run here: 700D/700E/1600 (libcodec2), RADE (compiled in)
    FdvOpaque* (*open)(int) = nullptr;
    void (*close)(FdvOpaque*) = nullptr;
    int (*rx)(FdvOpaque*, short*, short*) = nullptr;
    int (*nin)(FdvOpaque*) = nullptr;
    void (*tx)(FdvOpaque*, short*, short*) = nullptr;
    int (*nMaxSpeech)(FdvOpaque*) = nullptr;
    int (*nMaxModem)(FdvOpaque*) = nullptr;
    int (*nSpeech)(FdvOpaque*) = nullptr;
    int (*nNomModem)(FdvOpaque*) = nullptr;
    int (*speechRate)(FdvOpaque*) = nullptr;
    int (*modemRate)(FdvOpaque*) = nullptr;
    void (*stats)(FdvOpaque*, int*, float*) = nullptr;
    int (*rxStatus)(FdvOpaque*) = nullptr;
    void (*squelch)(FdvOpaque*, bool) = nullptr;
    void (*setTxt)(FdvOpaque*, void (*)(void*, char), char (*)(void*), void*) = nullptr;
    int (*getVersion)() = nullptr;
    /** ★ Optional (the tests' transmitter only): the complex modulator — the modem signal as its analytic baseband. */
    void (*comptx)(FdvOpaque*, void* /*COMP[] = {float re, im}*/, short*) = nullptr;
    /** True when at least one of our sub-modes can be opened — the whole question "is the mode offered?". `ok` is about
     *  libcodec2 only: RADE needs none of it. (The owner's RADE switch is applied by runs(), below.) */
    bool anySub() const;
    /** ["RADE","700D","700E","1600"] — what this server runs, for hwinfo, in the picker's order. */
    std::string subsJson() const;
};

#if !defined(VIBE_FREEDV_NO_DLOPEN)
/** ★★ A LIBRARY WE WILL RUN, ONLY FROM A FILE NOBODY ELSE COULD HAVE PUT THERE. The Mac app runs with
 *  disable-library-validation (the SDRplay API needs it), so nothing but us checks what dlopen loads. An absolute path
 *  is accepted only when its real file is owned by root or by the user this server runs as, and is writable by neither
 *  group nor others — Homebrew's own files pass (they belong to the admin who installed Homebrew); a file planted in a
 *  world-writable place does not. Linux loads by SONAME through the system loader's own trusted directories. */
inline bool trustedFile(const std::string& p) {
    char real[PATH_MAX];
    if (!::realpath(p.c_str(), real)) return false;
    struct stat st{};
    if (::stat(real, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (st.st_uid != 0 && st.st_uid != ::geteuid()) return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH)) return false;
    return true;
}
#endif

/** ★★ THE LIBRARY ITSELF, opened once for every user of it (FreeDV here, M17's vocoder in vibe_m17.h) — one dlopen, one
 *  trust check, one answer. `handle` is null when there is no library; `why` says so in words. VIBE_CODEC2_DISABLE=1 makes
 *  the library "not installed" for everybody (the M17 tests); VIBE_FREEDV_DISABLE=1 (below) switches off FreeDV alone. */
struct Lib { void* handle = nullptr; std::string path, why; };
inline Lib openLib() {
    Lib l;
#if defined(VIBE_FREEDV_NO_DLOPEN)
    l.why = "Codec 2 is not available on this platform yet";
    return l;
#else
    if (const char* d = std::getenv("VIBE_CODEC2_DISABLE")) if (d[0] && d[0] != '0') { l.why = "libcodec2 switched off (VIBE_CODEC2_DISABLE)"; return l; }
    void* h = nullptr;
#if defined(__APPLE__)
    static const char* const kTry[] = { "/opt/homebrew/opt/codec2/lib/libcodec2.dylib",
                                        "/usr/local/opt/codec2/lib/libcodec2.dylib",
                                        "/opt/local/lib/libcodec2.dylib" };
    for (const char* p : kTry) {
        if (!trustedFile(p)) continue;
        h = ::dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (h) { l.path = p; break; }
    }
#else
    // ★ measured 2026-11-16: Debian 12 bookworm libcodec2-1.0 (1.0.5), Debian 13 trixie libcodec2-1.2 (1.2.0)
    static const char* const kTry[] = { "libcodec2.so.1.2", "libcodec2.so.1.1", "libcodec2.so.1.0", "libcodec2.so" };
    for (const char* p : kTry) {
        h = ::dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (h) { l.path = p; break; }
    }
#endif
    if (!h) { l.why = "libcodec2 is not installed"; return l; }
    l.handle = h;   // ★ never closed: the library lives as long as the process (a static's lifetime)
    return l;
#endif
}
inline const Lib& lib() {
    static const Lib l = openLib();
    return l;
}

/** ★ RADE V1: compiled in, and a receiver opens (it allocates ~0.5 MB). VIBE_RADE_DISABLE=1 takes RADE alone away (the
 *  tests: the mode with libcodec2 only); VIBE_FREEDV_DISABLE=1 takes the whole mode away, RADE included. */
inline bool radeRuns() {
#if defined(VIBE_HAVE_RADE)
    if (const char* d = std::getenv("VIBE_RADE_DISABLE")) if (d[0] && d[0] != '0') return false;
    vrade_rx* r = vrade_rx_open();
    if (!r) return false;
    vrade_rx_close(r);
    return true;
#else
    return false;
#endif
}

inline Api loadCodec2Api();
/** Load once, on first ask; thread-safe (a function-local static). VIBE_FREEDV_DISABLE=1 makes it report "not
 *  installed" — for the tests that prove the mode disappears without the library. */
inline Api loadApi() {
    if (const char* d = std::getenv("VIBE_FREEDV_DISABLE")) if (d[0] && d[0] != '0') { Api a; a.why = "FreeDV switched off (VIBE_FREEDV_DISABLE)"; return a; }
    Api a = loadCodec2Api();
    a.sub[kRade] = radeRuns();
    if (a.sub[kRade] && !a.ok) a.why += " (RADE V1 runs without it)";
    return a;
}
/** The libcodec2 half: 700D, 700E and 1600. */
inline Api loadCodec2Api() {
    Api a;
#if defined(VIBE_FREEDV_NO_DLOPEN)
    a.why = "FreeDV is not available on this platform yet";
    return a;
#else
    const Lib& L = lib();
    void* h = L.handle;
    if (!h) { a.why = L.why; return a; }
    a.path = L.path;
    bool all = true;
    auto sym = [&](const char* n) { void* s = ::dlsym(h, n); if (!s) all = false; return s; };
    a.open       = reinterpret_cast<FdvOpaque* (*)(int)>(sym("freedv_open"));
    a.close      = reinterpret_cast<void (*)(FdvOpaque*)>(sym("freedv_close"));
    a.rx         = reinterpret_cast<int (*)(FdvOpaque*, short*, short*)>(sym("freedv_rx"));
    a.nin        = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_nin"));
    a.tx         = reinterpret_cast<void (*)(FdvOpaque*, short*, short*)>(sym("freedv_tx"));
    a.nMaxSpeech = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_n_max_speech_samples"));
    a.nMaxModem  = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_n_max_modem_samples"));
    a.nSpeech    = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_n_speech_samples"));
    a.nNomModem  = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_n_nom_modem_samples"));
    a.speechRate = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_speech_sample_rate"));
    a.modemRate  = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_modem_sample_rate"));
    a.stats      = reinterpret_cast<void (*)(FdvOpaque*, int*, float*)>(sym("freedv_get_modem_stats"));
    a.rxStatus   = reinterpret_cast<int (*)(FdvOpaque*)>(sym("freedv_get_rx_status"));
    a.squelch    = reinterpret_cast<void (*)(FdvOpaque*, bool)>(sym("freedv_set_squelch_en"));
    a.setTxt     = reinterpret_cast<void (*)(FdvOpaque*, void (*)(void*, char), char (*)(void*), void*)>(sym("freedv_set_callback_txt"));
    a.getVersion = reinterpret_cast<int (*)()>(sym("freedv_get_version"));
    a.comptx     = reinterpret_cast<void (*)(FdvOpaque*, void*, short*)>(::dlsym(h, "freedv_comptx"));   // ★ optional
    if (!all) { a = Api(); a.why = "libcodec2 is too old (the FreeDV API is incomplete)"; return a; }
    a.ok = true;
    a.version = a.getVersion();
    // ★ What THIS library can open — 700E arrived with codec2 1.0; a build may leave modes out (FREEDV_MODE_*_EN).
    for (int s = k700D; s <= k1600; ++s) {
        FdvOpaque* f = a.open(codec2Mode(s));
        if (f) { a.sub[s] = true; a.close(f); }
    }
    if (!a.anySub()) { a.ok = false; a.why = "libcodec2 offers none of 700D, 700E or 1600"; }
    return a;
#endif
}
inline const Api& api() {
    static const Api a = loadApi();
    return a;
}
/** ★★ THE OWNER'S RADE SWITCH ("rade", under FreeDV on the setup page — vibe_setup_page.h BLOCKABLE). The server sets this
 *  to its blocked-modes question (vsModeBlocked("rade")); unset (the tests) RADE is never blocked. The benchmark's dec_rade
 *  row ticks it OFF on a fresh setup where RADE is not green — the owner can always tick it back (never hidden). */
inline bool (*&radeBlockedFn())() { static bool (*f)() = nullptr; return f; }
/** RADE is in this build and a receiver opens — whatever the owner chose (the setup page shows the switch on this). */
inline bool radeBuilt() { return api().sub[kRade]; }
/** Which sub-mode `s` runs on this server now: 700D… through libcodec2; RADE compiled in and not switched off. */
inline bool runs(int s) {
    if (s <= kAuto || s >= kSubCount || !api().sub[s]) return false;
    if (s != kRade) return api().ok;
    return !(radeBlockedFn() && radeBlockedFn()());
}
inline bool Api::anySub() const {
    if (ok && (sub[k700D] || sub[k700E] || sub[k1600])) return true;
    return sub[kRade] && !(radeBlockedFn() && radeBlockedFn()());
}
inline std::string Api::subsJson() const {
    std::string j = "[";
    bool first = true;
    for (int s : {int(kRade), int(k700D), int(k700E), int(k1600)}) {
        const bool on = sub[s] && (s == kRade ? !(radeBlockedFn() && radeBlockedFn()()) : ok);
        if (on) { j += std::string(first ? "\"" : ",\"") + subName(s) + "\""; first = false; }
    }
    return j + "]";
}
/** ★ The one question every reader asks — hwinfo's `modes`, vsModeKnown, the clients' picker through hwinfo. */
inline bool available() { return api().anySub(); }

// ── ★★ V12 THE VOCODER ALONE (M17, vibe_m17.h): Codec 2's own API from the SAME library — codec2.h's published C ─────
//    signatures, found with dlsym in the handle FreeDV uses. M17 carries Codec 2 3200 (voice) or 1600 (voice + data) in
//    its stream frames; no modem of codec2's is involved. Independent of FreeDV's answer: a library whose FreeDV API is
//    incomplete may still have the vocoder, and VIBE_FREEDV_DISABLE does not take it away.
struct C2Opaque;
struct Vocoder {
    bool ok = false;
    std::string why;
    bool mode3200 = false, mode1600 = false;   ///< which of M17's two rates this library can create
    C2Opaque* (*create)(int) = nullptr;
    void (*destroy)(C2Opaque*) = nullptr;
    void (*encode)(C2Opaque*, unsigned char*, short*) = nullptr;
    void (*decode)(C2Opaque*, short*, const unsigned char*) = nullptr;
    int (*samplesPerFrame)(C2Opaque*) = nullptr;
    int (*bitsPerFrame)(C2Opaque*) = nullptr;
};
/** codec2.h's mode numbers (stable since 0.x): CODEC2_MODE_3200 0, CODEC2_MODE_1600 2. */
constexpr int kC2Mode3200 = 0, kC2Mode1600 = 2;
inline Vocoder loadVocoder() {
    Vocoder v;
#if defined(VIBE_FREEDV_NO_DLOPEN)
    v.why = "Codec 2 is not available on this platform yet";
    return v;
#else
    const Lib& L = lib();
    if (!L.handle) { v.why = L.why; return v; }
    bool all = true;
    auto sym = [&](const char* n) { void* s = ::dlsym(L.handle, n); if (!s) all = false; return s; };
    v.create          = reinterpret_cast<C2Opaque* (*)(int)>(sym("codec2_create"));
    v.destroy         = reinterpret_cast<void (*)(C2Opaque*)>(sym("codec2_destroy"));
    v.encode          = reinterpret_cast<void (*)(C2Opaque*, unsigned char*, short*)>(sym("codec2_encode"));
    v.decode          = reinterpret_cast<void (*)(C2Opaque*, short*, const unsigned char*)>(sym("codec2_decode"));
    v.samplesPerFrame = reinterpret_cast<int (*)(C2Opaque*)>(sym("codec2_samples_per_frame"));
    v.bitsPerFrame    = reinterpret_cast<int (*)(C2Opaque*)>(sym("codec2_bits_per_frame"));
    if (!all) { v = Vocoder(); v.why = "libcodec2 has no vocoder API"; return v; }
    // ★ probed, as FreeDV's sub-modes are: a build may leave a mode out (CODEC2_MODE_*_EN). The frame shapes are checked
    //   too — M17 packs exactly 64 bits per 3200 frame (20 ms) and per 1600 frame (40 ms).
    auto probe = [&](int mode, int samples) {
        C2Opaque* c = v.create(mode);
        if (!c) return false;
        const bool good = v.samplesPerFrame(c) == samples && v.bitsPerFrame(c) == 64;
        v.destroy(c);
        return good;
    };
    v.mode3200 = probe(kC2Mode3200, 160);
    v.mode1600 = probe(kC2Mode1600, 320);
    v.ok = v.mode3200;   // ★ 3200 is M17's voice; without it there is no M17 voice to offer
    if (!v.ok) v.why = "libcodec2 cannot run Codec 2 3200";
    return v;
#endif
}
inline const Vocoder& vocoder() {
    static const Vocoder v = loadVocoder();
    return v;
}

/** ★ The tests' and the benchmark's synthetic voice: a buzz whose pitch wanders (120 ± 30 Hz) with its harmonics to
 *  3.5 kHz, in syllables (3.5 per second). `n` samples at `rate`. */
inline void synthVoice(std::vector<float>& out, int rate, long n, float amp) {
    out.resize(size_t(n));
    double ph = 0;
    for (long i = 0; i < n; ++i) {
        const double tt = double(i) / rate, f0 = 120 + 30 * std::sin(2 * M_PI * 0.7 * tt);
        ph += 2 * M_PI * f0 / rate;
        double v = 0;
        for (int h = 1; h * f0 < 3500; ++h) v += std::sin(h * ph) / h;
        out[size_t(i)] = float(amp * v * std::max(0.0, std::sin(2 * M_PI * 3.5 * tt)));
    }
}

#if defined(VIBE_HAVE_RADE) && defined(VIBE_HAVE_RADE_TX)
/** ★ A RADE V1 over as 8 kHz modem audio (nominal peak ±1), made by FreeDV's own transmitter (rade_c, the tests' archive
 *  librade_vibe_tx.a): `sec` of the synthetic voice, then the end-of-over frame carrying `callsign` ("" = none sent). */
inline std::vector<float> radeOver8k(double sec, const char* callsign) {
    std::vector<float> m8, sp;
    vrade_tx* t = vrade_tx_open();
    if (!t) return m8;
    synthVoice(sp, VRADE_SPEECH_RATE, long(sec * VRADE_SPEECH_RATE), 0.25f);
    m8.resize(size_t(vrade_tx_frame_samples()) * (2 + sp.size() / 1920));
    m8.resize(size_t(vrade_tx_speech(t, sp.data(), int(sp.size()), m8.data(), nullptr)));
    if (callsign) {
        std::vector<float> e(static_cast<size_t>(vrade_tx_eoo_samples()));
        e.resize(size_t(vrade_tx_eoo(t, callsign, e.data(), nullptr)));
        m8.insert(m8.end(), e.begin(), e.end());
    }
    vrade_tx_close(t);
    return m8;
}
#endif

/** ★ A FreeDV signal as 48 kHz USB audio, made by the library's OWN transmitter (freedv_tx; RADE's, rade_c's) from a
 *  synthetic voice — the benchmark's proof that its row decodes (vibe_benchmark_more.h dec_freedv). Empty when that
 *  transmitter is not here (RADE's only in the tests). */
inline std::vector<float> testAudio48k(int sub, double sec, float rms = 0.1f) {
    std::vector<float> out, m8;
    if (sub == kRade) {
#if defined(VIBE_HAVE_RADE) && defined(VIBE_HAVE_RADE_TX)
        m8 = radeOver8k(sec, "G4ABC");
#endif
    } else {
        const Api& a = api();
        if (!a.ok || sub < k700D || sub > k1600 || !a.sub[sub]) return out;
        FdvOpaque* f = a.open(codec2Mode(sub));
        if (!f) return out;
        const int ns = a.nSpeech(f), nm = a.nNomModem(f);
        std::vector<float> voice;
        std::vector<short> sp(static_cast<size_t>(ns)), mod(static_cast<size_t>(nm));
        const long total = long(std::ceil(sec * 8000 / ns)) * ns;
        synthVoice(voice, 8000, total, 4000.f);
        for (long at = 0; at + ns <= total; at += ns) {
            for (int i = 0; i < ns; ++i) sp[size_t(i)] = short(std::lrint(voice[size_t(at + i)]));
            a.tx(f, mod.data(), sp.data());
            for (short v : mod) m8.push_back(float(v));
        }
        a.close(f);
    }
    if (m8.empty()) return out;
    double p = 0; for (float v : m8) p += double(v) * v;
    const float k = float(rms / std::sqrt(p / double(m8.size())));
    for (float& v : m8) v *= k;
    vibedsp::RationalResampler up(8000, 48000);
    out.resize(static_cast<size_t>(up.maxOut(int(m8.size()))));
    out.resize(static_cast<size_t>(up.process(m8.data(), int(m8.size()), out.data())));
    return out;
}

// ── One receiver ──────────────────────────────────────────────────────────────────────────────────────────────────────
class FreeDvListener {
public:
    static constexpr int kOutRate = 48000;
    static constexpr int kModemRate = 8000;   // ★ 700D, 700E, 1600 and RADE all run their modem at 8 kHz (checked at open)

    /** `sbSel` / `lsb`: the sideband choice and the side demodulated, set BEFORE the worker's first report — a new
     *  receiver for a nudged dial must not report AUTO for a moment when the listener chose USB. */
    /** `withRade` false: AUTO never opens RADE (the benchmark's dec_freedv row measures the codec2 trio alone — RADE has
     *  its own row, dec_rade, and its own default). */
    explicit FreeDvListener(int sub = kAuto, int sbSel = kSbAuto, bool lsb = false, bool withRade = true) : want_(sub), withRade_(withRade) {
        if (sbSel >= kSbAuto && sbSel <= kSbLsb) sbSel_.store(sbSel);
        lsb_.store(lsb);
        // ★ the report a client reads before the worker's first publish carries the same choice (the placeholder
        //   below had no "sbSel" at all — a nudged shared dial briefly reported no choice, test-server-freedv caught it)
        status_ = std::string("{\"type\":\"freedv\",\"sub\":\"auto\",\"sb\":\"") + (lsb ? "lsb" : "usb")
                + "\",\"sbSel\":\"" + sbName(sbSel_.load()) + "\",\"state\":\"search\",\"mode\":\"\"}";
        th_ = std::thread([this] { loop(); });
    }
    ~FreeDvListener() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    FreeDvListener(const FreeDvListener&) = delete;
    FreeDvListener& operator=(const FreeDvListener&) = delete;

    /** The demodulated USB audio (any rate; `stride` floats between frames — 1 for mono, 2 for the left of a stereo
     *  pair). Never blocks for long: a copy under a leaf mutex. */
    void feedAudio(const float* x, int frames, int stride, double rateHz) {
        if (frames <= 0 || !(rateHz > 0)) return;
        Block b; b.rate = rateHz; b.pcm.resize(size_t(frames));
        for (int i = 0; i < frames; ++i) b.pcm[size_t(i)] = x[size_t(i) * size_t(stride)];
        std::lock_guard<std::mutex> lk(m_);
        queued_ += size_t(frames);
        q_.push_back(std::move(b));
        while (queued_ > size_t(rateHz * 2.0) && q_.size() > 1) { queued_ -= q_.front().pcm.size(); q_.pop_front(); ++dropped_; }
        cv_.notify_one();
    }

    /** Exactly `frames` frames of 48 kHz mono speech (zeros while there is none). */
    void takePcm(float* mono, int frames) {
        std::lock_guard<std::mutex> lk(pm_);
        if (!primed_ && pcm_.size() >= size_t(kPrimeFrames)) primed_ = true;
        int got = 0;
        if (primed_) {
            got = std::min<int>(frames, int(pcm_.size()));
            std::copy(pcm_.begin(), pcm_.begin() + got, mono);
            pcm_.erase(pcm_.begin(), pcm_.begin() + got);
            if (got < frames) { primed_ = false; ++underruns_; }
        }
        std::fill(mono + got, mono + frames, 0.0f);
    }

    /** ★ The listener's choice: AUTO or one sub-mode. Applied by the worker before its next block. */
    void setSub(int sub) { if (sub >= kAuto && sub < kSubCount) want_.store(sub); }
    /** ★ Which sideband the radio demodulates for it (the server's rule: FreeDV's SSB convention) — reported, so a client
     *  draws the passband on the right side of the dial without a rule of its own. */
    void setSideband(bool lsb) { lsb_.store(lsb, std::memory_order_relaxed); }
    /** ★ What the listener (or the room) chose — AUTO / USB / LSB — reported as "sbSel" so a picker shows the choice
     *  while "sb" shows what is demodulated. */
    void setSidebandChoice(int sb) { if (sb >= kSbAuto && sb <= kSbLsb) sbSel_.store(sb, std::memory_order_relaxed); }
    int sub() const { return want_.load(); }

    /** ★ The report and a counter that moves when it changes (the caller sends on change). */
    std::string statusJson(uint64_t* version) const {
        std::lock_guard<std::mutex> lk(sm_);
        if (version) *version = statusVer_;
        return status_;
    }
    uint64_t droppedBlocks() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    /** Input frames waiting for the worker (the tests pace on it; the server never needs to). */
    size_t queuedFrames() const { std::lock_guard<std::mutex> lk(m_); return queued_; }
    uint64_t underruns() const { std::lock_guard<std::mutex> lk(pm_); return underruns_; }
    /** Vocoder output blocks produced in sync since this receiver began — the tests' "did it decode?". */
    uint64_t speechBlocks() const { return speechBlocks_.load(std::memory_order_relaxed); }
    /** The sub-mode in sync right now (k700D…), or kAuto while searching. */
    int lockedSub() const { return locked_.load(std::memory_order_relaxed); }
    /** ★ The modem's SNR estimate in dB while in sync, else NaN. */
    float snrDb() const { return snr_.load(std::memory_order_relaxed); }
    /** How many times AUTO has chosen sub-mode `s` since this receiver began (the tests: a false win shows here). */
    int winsOf(int s) const { return s >= 0 && s < kSubCount ? wins_[s].load(std::memory_order_relaxed) : 0; }
    /** AUTO winners that lost sync within kProbationSec and sent it back to the full search. */
    int probationLosses() const { return probationLosses_.load(std::memory_order_relaxed); }
    /** Modems open right now — every sub-mode while AUTO searches, 1 once locked (the test checks the others were closed). */
    int modemsOpen() const { return open_.load(std::memory_order_relaxed); }
    /** ★ RADE: the last end-of-over callsign heard on this receiver ("" before any) — the tests' "did the EOO decode?". */
    std::string lastCallsign() const { std::lock_guard<std::mutex> lk(sm_); return last_; }

private:
    static constexpr int kPrimeFrames = kOutRate * 3 / 10;   // 0.3 s — a 700D modem frame is 160 ms of speech
    static constexpr int kMaxFrames = kOutRate * 3 / 2;      // 1.5 s, then the oldest goes (latency never grows)
    static constexpr double kLockSec = 1.0;                  // AUTO: sync held this long wins
    /* ★★ A SYNC IS BELIEVED ONLY ONCE IT HAS HELD (measured 2026-11-16, test-server-freedv): 1600's demodulator, chosen by
     *  hand on a 700D station, flickered into sync for a frame or two — a "sync" on the VTS for a signal it cannot decode, and
     *  a codec2 chirp if its squelch let that frame through. Reported and played only after 0.5 s held. */
    static constexpr double kHeldSec = 0.5;
    static constexpr double kRelockSec = 8.0;                // AUTO: this long without sync searches again
    /* ★★ …EXCEPT A WINNER THAT DID NOT LAST. A sync held for a second can still be the wrong sub-mode's demodulator on a
     *  marginal signal, or a fade at the very moment of the win. Waiting the full 8 s then is 8 s of silence on a station
     *  that is still there. So a winner that decodes no clean frame for a second within 4 s of winning sends AUTO straight
     *  back to the full search (counted: probationLosses()). A station that has held longer gets the patient 8 s. */
    static constexpr double kProbationSec = 4.0, kProbationLostSec = 1.0;
    // ★ "lost", for AUTO, is the time since the last clean frame (Modem::sinceGoodSec) — see decide()
    static constexpr double kSnrEverySec = 1.0;              // the report's SNR moves at most once a second

    struct Block { std::vector<float> pcm; double rate = 0; };
    struct Modem {
        int sub = 0;
        FdvOpaque* f = nullptr;
#if defined(VIBE_HAVE_RADE)
        vrade_rx* rade = nullptr;   // ★ kRade: FreeDV's RADE V1 receiver instead of a codec2 FreeDV object
        std::vector<float> rin, rout;
#endif
        std::vector<short> in;      // modem-rate samples waiting
        size_t head = 0;
        std::vector<short> out;     // one freedv_rx call's speech
        double syncSec = 0, lostSec = 0;
        int clean = 0, dirty = 0;   // frames decoded since sync came: FEC clean / with errors left (freedv_get_rx_status)
        /** Time since this demodulator last decoded a CLEAN frame (in sync, bits out, no errors left). NOT reset by a sync
         *  that comes and goes: a demodulator holding a false sync flickers in and out, and each flicker used to restart
         *  the 8 s "lost" clock — measured 2026-11-16, 700E on a 1600 station, never giving up. */
        double sinceGoodSec = 0;
        float snr = 0;
        bool sync = false;
        std::unique_ptr<vibedsp::RationalResampler> up;   // speech rate → 48 kHz (the active modem only)
        std::string line, text;                           // the varicode text channel
        std::string recent[4];                            // the last lines received, newest first
    };

    static void onTxtChar(void* st, char c) {
        auto* m = static_cast<Modem*>(st);
        if (c == '\r' || c == '\n') {
            if (!m->line.empty()) {
                // ★ shown only once it has come twice the same: plain varicode has no check, and noise reads as text
                /* ★ …twice the same, among the last four: one line garbled in between (a fade, a burst of noise — or the
                 *  bench clip's loop cutting one in half) must not stop a callsign that keeps coming in clean from showing. */
                for (const auto& r : m->recent) if (!r.empty() && r == m->line) { m->text = m->line; break; }
                for (int i = 3; i > 0; --i) m->recent[i] = m->recent[i - 1];
                m->recent[0] = m->line;
                m->line.clear();
            }
            return;
        }
        if (c >= 32 && c < 127 && m->line.size() < 40) m->line += c;
    }

    void openModem(int sub) {
        const Api& a = api();
        if (!runs(sub)) return;
        auto m = std::make_unique<Modem>();
        m->sub = sub;
#if defined(VIBE_HAVE_RADE)
        if (sub == kRade) {
            m->rade = vrade_rx_open();
            if (!m->rade) return;
            m->rin.resize(size_t(vrade_rx_nin_max()));
            m->rout.resize(size_t(vrade_rx_pcm_max()));
            m->in.reserve(size_t(vrade_rx_nin_max()) * 4);
            modems_.push_back(std::move(m));
            return;
        }
#endif
        m->f = a.open(codec2Mode(sub));
        if (!m->f) return;
        if (a.modemRate(m->f) != kModemRate) { a.close(m->f); return; }
        a.squelch(m->f, true);   // ★ silence, not noise, when the modem is out of sync
        a.setTxt(m->f, &FreeDvListener::onTxtChar, nullptr, m.get());   // ★ the Modem's address is stable (unique_ptr)
        m->out.resize(size_t(std::max(1, a.nMaxSpeech(m->f))));
        m->in.reserve(size_t(std::max(1, a.nMaxModem(m->f))) * 4);
        modems_.push_back(std::move(m));
    }
    static void closeModem(Modem& m) {
        if (m.f) { api().close(m.f); m.f = nullptr; }
#if defined(VIBE_HAVE_RADE)
        if (m.rade) { vrade_rx_close(m.rade); m.rade = nullptr; }
#endif
    }
    void closeAll() {
        for (auto& m : modems_) if (m) closeModem(*m);
        modems_.clear();
        active_ = nullptr;
    }
    /** (Re)build for `sub`: AUTO opens every sub-mode the library has, a manual choice opens that one. */
    void rebuild(int sub) {
        closeAll();
        cur_ = sub;
        if (sub == kAuto) { for (int s : {int(kRade), int(k700D), int(k700E), int(k1600)}) if (s != kRade || withRade_) openModem(s); }
        else openModem(sub);
        if (sub != kAuto && !modems_.empty()) active_ = modems_.front().get();
        locked_.store(kAuto);
        open_.store(int(modems_.size()));
        snr_.store(NAN);
        { std::lock_guard<std::mutex> lk(pm_); pcm_.clear(); primed_ = false; }
        if (active_) makeUp(*active_);
        publish(true);
    }
    void makeUp(Modem& m) {
#if defined(VIBE_HAVE_RADE)
        if (m.rade) { m.up.reset(new vibedsp::RationalResampler(VRADE_SPEECH_RATE, kOutRate)); return; }   // ★ FARGAN: 16 kHz
#endif
        const int sr = api().speechRate(m.f);
        m.up.reset(new vibedsp::RationalResampler(sr > 0 ? sr : 8000, kOutRate));
    }

    void loop() {
        vibeAudioDecodeThread("vibe-freedv");
        rebuild(want_.load());
        std::vector<float> m8;
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) break;
                b = std::move(q_.front()); q_.pop_front(); queued_ -= b.pcm.size();
            }
            if (want_.load() != cur_) rebuild(want_.load());
            if (modems_.empty()) continue;
            // ★ to the modem rate (48 kHz → 8 kHz is an exact 1/6; any other input rate gets its own resampler)
            const int inRate = int(std::lround(b.rate));
            if (!down_ || downIn_ != inRate) { down_.reset(new vibedsp::RationalResampler(inRate, kModemRate)); downIn_ = inRate; }
            m8.resize(size_t(down_->maxOut(int(b.pcm.size()))));
            const int n8 = down_->process(b.pcm.data(), int(b.pcm.size()), m8.data());
            streamSec_ += double(n8) / kModemRate;
            for (auto& m : modems_) {
                for (int i = 0; i < n8; ++i) {
                    // ★ float ±1 → the modem's 16-bit scale; the demodulators normalise their own level
                    const float v = std::max(-1.0f, std::min(1.0f, m8[size_t(i)])) * 16000.0f;
                    m->in.push_back(short(std::lrint(v)));
                }
                runModem(*m);
            }
            decide();
            publish(false);
        }
        closeAll();
    }

    /** ★ Decoded speech from the active modem, at its own rate, into the 48 kHz PCM buffer the audio callback takes from. */
    void pushSpeech(Modem& m, const float* sp, int n) {
        speechBlocks_.fetch_add(1, std::memory_order_relaxed);
        spO_.resize(size_t(m.up->maxOut(n)));
        const int no = m.up->process(sp, n, spO_.data());
        std::lock_guard<std::mutex> lk(pm_);
        pcm_.insert(pcm_.end(), spO_.begin(), spO_.begin() + no);
        if (pcm_.size() > size_t(kMaxFrames)) pcm_.erase(pcm_.begin(), pcm_.end() - kPrimeFrames);
    }

#if defined(VIBE_HAVE_RADE)
    /* ★★ RADE V1, one modem frame (120 ms) per call. Its own state machine decides sync — four pilot detections in a row
     *  (~0.5 s) before it calls it, so no extra hold here (kHeldSec is for codec2's flickering OFDM syncs). "Clean" is a
     *  decoded frame whose unique word (the auxiliary bits the decoder hands back) came through without errors — RADE has
     *  no FEC to report, and the UW is what its own receiver uses to drop a false sync. The end-of-over frame carries the
     *  callsign, LDPC-coded and CRC-checked (FreeDV's reliable text): kept as "last", never shown as the station talking. */
    void runRade(Modem& m) {
        int zeroRuns = 0;
        for (;;) {
            const int nin = vrade_rx_nin(m.rade);
            if (nin < 0 || m.in.size() - m.head < size_t(nin)) break;
            if (nin == 0 && ++zeroRuns > 4) break;
            for (int i = 0; i < nin; ++i) m.rin[size_t(i)] = float(m.in[m.head + size_t(i)]) * (1.0f / 16000.0f);
            int info = 0;
            const int nout = vrade_rx_process(m.rade, m.rin.data(), m.rout.data(), &info);
            m.head += size_t(nin);
            const double dt = double(nin) / kModemRate;
            m.sync = vrade_rx_sync(m.rade) != 0;
            m.sinceGoodSec += dt;
            if (m.sync || (info & VRADE_INFO_FRAME)) {
                m.syncSec += dt; m.lostSec = 0; m.snr = vrade_rx_snr_db(m.rade);
                if (info & VRADE_INFO_FRAME) {
                    if (info & VRADE_INFO_UWERR) ++m.dirty;
                    else { ++m.clean; m.sinceGoodSec = 0; }
                }
            } else {
                if (m.syncSec > 0 && &m == active_) note("lost", m);
                m.syncSec = 0; m.lostSec += dt; m.clean = m.dirty = 0;
            }
            if (info & VRADE_INFO_CALL) {
                std::string c = vrade_rx_callsign(m.rade);
                note("callsign", m);
                if (!c.empty()) { std::lock_guard<std::mutex> lk(sm_); last_ = c; }
            }
            if (&m == active_ && nout > 0) pushSpeech(m, m.rout.data(), nout);
        }
        if (m.head > 0 && (m.head >= m.in.size() / 2 || m.head > 16384)) {
            m.in.erase(m.in.begin(), m.in.begin() + long(m.head));
            m.head = 0;
        }
        if (m.in.size() > size_t(kModemRate) * 4) { m.in.clear(); m.head = 0; }   // never more than 4 s behind
    }
#endif

    void runModem(Modem& m) {
#if defined(VIBE_HAVE_RADE)
        if (m.rade) { runRade(m); return; }
#endif
        const Api& a = api();
        /* ★★★ nin == 0 IS A REAL ANSWER (found 2026-11-16, the intermittent test-server-freedv failure). The OFDM modes' timing
         *  loop asks for FEWER samples to slip back, and now and then for none at all. This loop used to read 0 as "nothing to
         *  do" and stop — and since nothing then moved the modem on, it asked for 0 again on every block: that receiver was
         *  wedged for good, deaf until AUTO's 8 s re-search (or forever, on a manual choice). Measured on 700E: a noise lead-in
         *  of 0.37 s or 2 s before the station wedged it every time; 5 s did not. freedv_rx with nin 0 consumes nothing and
         *  moves the state on, so it is called — a few times at most per pass, so a library that kept answering 0 could not
         *  spin this thread. */
        int zeroRuns = 0;
        for (;;) {
            const int nin = a.nin(m.f);
            if (nin < 0 || m.in.size() - m.head < size_t(nin)) break;
            if (nin == 0 && ++zeroRuns > 4) break;
            const int nout = a.rx(m.f, m.out.data(), m.in.data() + m.head);
            m.head += size_t(nin);
            const double dt = double(nin) / kModemRate;
            int sync = 0; float snr = 0;
            a.stats(m.f, &sync, &snr);
            m.sync = sync != 0;
            const int st = a.rxStatus(m.f);
            m.sinceGoodSec += dt;
            if (m.sync) {
                m.syncSec += dt; m.lostSec = 0; m.snr = snr;
                if (st & 0x4 /*FREEDV_RX_BITS*/) {
                    if (st & 0x8 /*FREEDV_RX_BIT_ERRORS*/) ++m.dirty;
                    else { ++m.clean; m.sinceGoodSec = 0; }
                }
            } else {
                if (m.syncSec > 0 && &m == active_) note("lost", m);
                m.syncSec = 0; m.lostSec += dt; m.clean = m.dirty = 0;
            }
            if (&m == active_ && nout > 0 && m.syncSec >= kHeldSec) {
                spF_.resize(size_t(nout));
                for (int i = 0; i < nout; ++i) spF_[size_t(i)] = float(m.out[size_t(i)]) * (1.0f / 32768.0f);
                pushSpeech(m, spF_.data(), nout);
            }
        }
        // ★ bounded: compact once the consumed part is the bulk of it
        if (m.head > 0 && (m.head >= m.in.size() / 2 || m.head > 16384)) {
            m.in.erase(m.in.begin(), m.in.begin() + long(m.head));
            m.head = 0;
        }
        if (m.in.size() > size_t(kModemRate) * 4) { m.in.clear(); m.head = 0; }   // never more than 4 s behind
    }

    /** AUTO's choice, and the reported state for every mode. */
    void decide() {
        if (cur_ == kAuto) {
            if (!active_) {
                Modem* best = nullptr;
                /* ★★★ A WIN IS SYNC *AND* DECODING (2026-11-16, the intermittent test-server-freedv failure). An OFDM demodulator
                 *  can hold "sync" on a signal that is not its own — measured: 700D kept sync for 2.9 s on a 1600 station, every
                 *  frame with errors left after the LDPC, SNR ~0 — and a second of that won AUTO, closed the 1600 demodulator
                 *  that was about to win, and left the listener 8 s+ in silence. So a candidate must also have decoded more
                 *  clean frames than failed ones (freedv_get_rx_status). 1600 has no FEC and never reports errors; its false
                 *  syncs are flickers the 1 s hold already refuses. Ties: the cleaner, then the stronger. */
                for (auto& m : modems_) {
                    if (m->syncSec < kLockSec || m->clean < 2 || m->clean <= m->dirty) continue;
                    if (!best || m->clean - m->dirty > best->clean - best->dirty
                        || (m->clean - m->dirty == best->clean - best->dirty && m->snr > best->snr)) best = m.get();
                }
                if (best) {
                    note("win", *best);
                    wins_[best->sub].fetch_add(1, std::memory_order_relaxed);
                    // ★ the winner stays; the others go (memory and CPU back)
                    std::unique_ptr<Modem> keep;
                    for (auto& m : modems_) if (m.get() == best) keep = std::move(m);
                    for (auto& m : modems_) if (m) closeModem(*m);
                    modems_.clear();
                    modems_.push_back(std::move(keep));
                    active_ = modems_.front().get();
                    makeUp(*active_);
                    open_.store(1);
                    winAt_ = streamSec_;
                }
            } else if (streamSec_ - winAt_ < kProbationSec + kProbationLostSec && active_->sinceGoodSec >= kProbationLostSec) {
                note("probation lost", *active_);
                probationLosses_.fetch_add(1, std::memory_order_relaxed);
                rebuild(kAuto);   // ★ the win did not last: every sub-mode searches again, now
                return;
            } else if (active_->sinceGoodSec >= kRelockSec) {
                // ★ nothing decoded cleanly for 8 s: out of sync, or "in sync" on failed frames (a station gone, a false hold)
                note("search again", *active_);
                rebuild(kAuto);   // ★ the station went — search every sub-mode again
                return;
            }
        }
        const bool in = heldSync(active_);
        locked_.store(in ? active_->sub : kAuto);
        snr_.store(in ? active_->snr : NAN);
    }

    /** ★ DIAGNOSTIC: VIBE_FREEDV_LOG=1 prints AUTO's decisions to stderr (stream time, sub-mode, how long it held sync,
     *  its SNR and its frames clean / with errors). Off unless set. */
    void note(const char* what, const Modem& m) {
        static const bool on = [] { const char* e = std::getenv("VIBE_FREEDV_LOG"); return e && e[0] && e[0] != '0'; }();
        if (!on) return;
        std::fprintf(stderr, "[freedv %p] %7.2f s %-12s %s sync %.2f s snr %.1f clean %d dirty %d\n", static_cast<void*>(this),
                     streamSec_, what, subName(m.sub), m.syncSec, m.snr, m.clean, m.dirty);
    }

    static void jsonText(std::string& j, const std::string& s) {
        for (char c : s) { if (c == '"' || c == '\\') j += '\\'; j += c; }
    }
    /** In sync for the report and the speaker: codec2's modes once the sync has held kHeldSec; RADE as soon as its own
     *  receiver says so (it has already seen four pilots in a row). */
    static bool heldSync(const Modem* m) {
        if (!m || !m->sync) return false;
        return m->sub == kRade ? true : m->syncSec >= kHeldSec;
    }
    /** {"type":"freedv","sub":"auto"|"RADE"|"700D"…,"sb":"usb"|"lsb","sbSel":"auto"|"usb"|"lsb","state":"search"|"sync",
     *   "mode":"RADE"|"700D"|"","snr":5,"text":"…","last":"G4ABC"} — "last": RADE's end-of-over callsign, once an over ended */
    void publish(bool force) {
        const bool in = heldSync(active_);
        int snr = 0;
        if (in) {
            const double now = streamSec_;   // ★ stream time, not the wall clock: the same signal, the same reports
            if (force || !wasIn_ || now - snrAt_ >= kSnrEverySec) { snrShown_ = int(std::lround(active_->snr)); snrAt_ = now; }
            snr = snrShown_;
        }
        wasIn_ = in;
        std::string j = "{\"type\":\"freedv\",\"sub\":\"";
        j += subName(cur_);
        j += lsb_.load(std::memory_order_relaxed) ? "\",\"sb\":\"lsb" : "\",\"sb\":\"usb";
        j += "\",\"sbSel\":\""; j += sbName(sbSel_.load(std::memory_order_relaxed));
        j += "\",\"state\":\""; j += in ? "sync" : "search";
        j += "\",\"mode\":\""; j += in ? subName(active_->sub) : "";
        j += "\"";
        if (in) j += ",\"snr\":" + std::to_string(snr);
        if (active_ && !active_->text.empty()) { j += ",\"text\":\""; jsonText(j, active_->text); j += "\""; }
        std::lock_guard<std::mutex> lk(sm_);
        if (!last_.empty()) { j += ",\"last\":\""; jsonText(j, last_); j += "\""; }
        j += "}";
        if (j != status_) { status_ = j; ++statusVer_; }
    }

    std::atomic<int> want_;
    const bool withRade_ = true;
    std::atomic<bool> lsb_{false};
    std::atomic<int> sbSel_{kSbAuto};
    int cur_ = -1;
    std::vector<std::unique_ptr<Modem>> modems_;   // worker thread only
    Modem* active_ = nullptr;                       // worker thread only
    std::unique_ptr<vibedsp::RationalResampler> down_;
    int downIn_ = 0;
    std::vector<float> spF_, spO_;
    std::atomic<int> locked_{kAuto}, open_{0};
    std::atomic<float> snr_{NAN};
    std::atomic<uint64_t> speechBlocks_{0};
    std::atomic<int> wins_[kSubCount] = {{0}, {0}, {0}, {0}, {0}};
    std::atomic<int> probationLosses_{0};
    double winAt_ = -1e9;
    double snrAt_ = -1e9, streamSec_ = 0; int snrShown_ = 0; bool wasIn_ = false;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Block> q_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0;
    bool stop_ = false;

    mutable std::mutex pm_;
    std::deque<float> pcm_;
    bool primed_ = false;
    uint64_t underruns_ = 0;

    mutable std::mutex sm_;
    std::string last_;   // ★ RADE's last end-of-over callsign (guarded by sm_; the worker writes, the report reads)
    std::string status_ = "{\"type\":\"freedv\",\"sub\":\"auto\",\"sb\":\"usb\",\"state\":\"search\",\"mode\":\"\"}";
    uint64_t statusVer_ = 1;

    std::thread th_;
};

}  // namespace freedv
}  // namespace vibe
