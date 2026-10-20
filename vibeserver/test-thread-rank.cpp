// ★★★ THE THREAD LADDER, READ BACK (vibe_thread.h, 2026-10-20) — Network > Audio > Spectrum > Decoders.
//
// Each role is taken on its own thread exactly as the server takes it, and the rank the kernel actually holds is
// read back — never the value we asked for:
//   Linux/Android: getpriority(PRIO_PROCESS, tid) AND field 19 of /proc/self/task/<tid>/stat (the number `top` and
//                  today's Pi 500 measurement read), so a helper that "succeeds" without moving the thread fails here.
//   macOS:         pthread_get_qos_class_np on the thread itself.
// The order that must hold: audio send ≥ audio chain (DSP) > waterfall send ≥ spectrum > default > decoders > bulk.
// ★★ The bug this guards (Pi 500, 2026-10-20 finding): one radio process's DSP at -19 outranking another radio's
//    socket writer — on a multi-radio server nice is compared across processes, so the writer's rung is absolute.
// ★ WITHOUT THE PRIVILEGE (Linux, no CAP_SYS_NICE — a developer's shell, a container): a negative nice is refused,
//   so those rungs are reported NOT RUN (exit 77 = ctest SKIPPED, never a pass) and the rest of the ladder — the parts any user may take —
//   is still checked, along with "a refused send rung did not leave the thread ABOVE the audio chain".
#include "vibe_thread.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#if defined(__linux__) || defined(__ANDROID__)
  #include <sys/syscall.h>
  #include <unistd.h>
#endif

static int fails = 0;
static void ok(bool c, const std::string& w) {
    std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
static void skip(const std::string& w) { std::printf("  \033[2mNOT RUN\033[0m %s\n", w.c_str()); }

#if defined(__linux__) || defined(__ANDROID__)
struct Rank { int nice = 999; int procNice = 999; std::string comm; };

static int procStatNice(long tid) {
    char path[64]; std::snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
    FILE* f = std::fopen(path, "r"); if (!f) return 999;
    char buf[1024]; size_t n = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f); buf[n] = 0;
    const char* p = std::strrchr(buf, ')');          // comm may hold spaces; fields restart after the last ')'
    if (!p) return 999;
    int field = 2, nice = 999;
    for (const char* q = p + 1; *q; q++) {
        if (*q == ' ') { field++; if (field == 19) { nice = std::atoi(q + 1); break; } }
    }
    return nice;
}
static std::string procComm(long tid) {
    char path[64]; std::snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
    FILE* f = std::fopen(path, "r"); if (!f) return "";
    char buf[32] = {0}; if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0; std::fclose(f);
    std::string s(buf); while (!s.empty() && (s.back() == '\n')) s.pop_back(); return s;
}
/** Run `take` on a fresh thread (which starts at the process's nice, as a server thread does) and read its rank. */
static Rank on(const std::function<void()>& take) {
    Rank r;
    std::thread([&] {
        take();
        const long tid = (long)syscall(SYS_gettid);
        errno = 0;
        r.nice = getpriority(PRIO_PROCESS, (id_t)tid);
        r.procNice = procStatNice(tid);
        r.comm = procComm(tid);
    }).join();
    return r;
}

int main() {
    std::printf("── the thread ladder (Linux/Android: nice, read back) ──\n");
    const int base = getpriority(PRIO_PROCESS, 0);
    const Rank net   = on([] { vibeNetThread("vibeTx"); });
    const Rank sendA = on([] { vibeNetThread("vibeTx"); vibeSendRank(true); });
    const Rank sendW = on([] { vibeNetThread("vibeTx"); vibeSendRank(false); });
    const Rank audio = on([] { vibeAudioThread("vibe-dsp"); });
    const Rank iq    = on([] { vibeIqThread("vibe-rtl"); });
    const Rank spec  = on([] { vibeSpectrumThread("vibe-spec"); });
    const Rank dec   = on([] { vibeDecoderThread("vibe-decode"); });
    const Rank bulk  = on([] { vibeBulkThread("vibe-bulk"); });
    std::printf("  process nice %d · vibeTx %d · vibeTx-audio %d · vibeTx-wf %d · vibe-dsp %d · vibe-rtl %d · vibe-spec %d"
                " · vibe-decode %d · vibe-bulk %d\n",
                base, net.nice, sendA.nice, sendW.nice, audio.nice, iq.nice, spec.nice, dec.nice, bulk.nice);

    for (const Rank* r : { &net, &sendA, &sendW, &audio, &iq, &spec, &dec, &bulk })
        ok(r->nice == r->procNice, "getpriority and /proc/<tid>/stat agree for " + r->comm + " (" + std::to_string(r->nice)
           + " / " + std::to_string(r->procNice) + ") — what we read is what top shows");
    ok(sendA.comm == "vibeTx-audio" && sendW.comm == "vibeTx-wf",
       "a classified writer says what it carries in /proc (" + sendA.comm + ", " + sendW.comm + ")");

    // ★ The rungs any user may take: raising nice never needs a privilege.
    ok(dec.nice == std::max(base, vibeprio::kDecoder), "decoders at nice " + std::to_string(vibeprio::kDecoder) + " (" + std::to_string(dec.nice) + ")");
    ok(bulk.nice == std::max(base, vibeprio::kBulk), "bulk HTTP at nice " + std::to_string(vibeprio::kBulk) + " (" + std::to_string(bulk.nice) + ")");
    // ★ Started niced (`nice ctest`, base 10), spectrum and decoders both sit AT the base, so only the order that can
    //   still differ is checked — a niced runner is not a fault in the ladder.
    if (base < vibeprio::kDecoder) ok(dec.nice > spec.nice && bulk.nice > dec.nice, "bulk below decoders below spectrum");
    else ok(bulk.nice >= dec.nice && dec.nice >= spec.nice, "bulk not above decoders not above spectrum (started at nice " + std::to_string(base) + ")");

    const bool privileged = audio.nice == vibeprio::kAudio;
    if (!privileged) {
        skip("the negative rungs — this process may not lower nice (no CAP_SYS_NICE / RLIMIT_NICE): vibe-dsp stayed at "
             + std::to_string(audio.nice) + ". Run as the unit does (AmbientCapabilities=CAP_SYS_NICE) to check them.");
        // ★★ Even refused, nothing may end up ABOVE the audio chain that belongs below it, nor the audio send below it.
        ok(sendA.nice <= audio.nice, "refused: the audio send is still not below the audio chain");
        ok(sendW.nice >= audio.nice, "refused: the waterfall send is still not above the audio chain");
    } else {
        ok(net.nice == vibeprio::kNet, "a fresh writer at the network rung, " + std::to_string(vibeprio::kNet) + " (" + std::to_string(net.nice) + ")");
        ok(sendA.nice == vibeprio::kNet, "★★★ the AUDIO send at " + std::to_string(vibeprio::kNet) + " (" + std::to_string(sendA.nice) + ")");
        ok(audio.nice == vibeprio::kAudio, "the audio chain (DSP) at " + std::to_string(vibeprio::kAudio));
        ok(iq.nice == vibeprio::kNet, "the IQ input at " + std::to_string(vibeprio::kNet) + " (" + std::to_string(iq.nice) + ")");
        ok(sendW.nice == vibeprio::kSendWf, "the waterfall send stepped down to " + std::to_string(vibeprio::kSendWf) + " (" + std::to_string(sendW.nice) + ")");
        ok(spec.nice == vibeprio::kSpectrum, "spectrum at " + std::to_string(vibeprio::kSpectrum) + " (" + std::to_string(spec.nice) + ")");
        ok(sendA.nice <= audio.nice, "★★★ audio send ≥ DSP — no radio's DSP outranks another radio's audio writer");
        ok(audio.nice < sendW.nice, "★★ DSP > waterfall send");
        ok(sendW.nice <= spec.nice, "waterfall send ≥ the spectrum work feeding it");
        ok(spec.nice < 0 && 0 < dec.nice, "spectrum > default > decoders");
    }
    // ★ Unprivileged: the checks that could run passed, but the ladder's point was not checked — ctest says SKIPPED
    //   (77, SKIP_RETURN_CODE), never Passed.
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : privileged ? "all ok" : "partly NOT RUN", fails);
    return fails ? 1 : privileged ? 0 : 77;
}

#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
static qos_class_t on(const std::function<void()>& take) {
    qos_class_t q = QOS_CLASS_UNSPECIFIED;
    std::thread([&] { take(); int rel = 0; pthread_get_qos_class_np(pthread_self(), &q, &rel); }).join();
    return q;
}
static const char* nm(qos_class_t q) {
    switch (q) {
        case QOS_CLASS_USER_INTERACTIVE: return "USER_INTERACTIVE";
        case QOS_CLASS_USER_INITIATED:   return "USER_INITIATED";
        case QOS_CLASS_DEFAULT:          return "DEFAULT";
        case QOS_CLASS_UTILITY:          return "UTILITY";
        case QOS_CLASS_BACKGROUND:       return "BACKGROUND";
        default:                         return "UNSPECIFIED";
    }
}
int main() {
    std::printf("── the thread ladder (macOS: QoS class, read back) ──\n");
    const qos_class_t sendA = on([] { vibeNetThread("vibeTx"); vibeSendRank(true); });
    const qos_class_t sendW = on([] { vibeNetThread("vibeTx"); vibeSendRank(false); });
    const qos_class_t audio = on([] { vibeAudioThread("vibe-dsp"); });
    const qos_class_t spec  = on([] { vibeSpectrumThread("vibe-spec"); });
    const qos_class_t dec   = on([] { vibeDecoderThread("vibe-decode"); });
    std::printf("  vibeTx-audio %s · vibe-dsp %s · vibeTx-wf %s · vibe-spec %s · vibe-decode %s\n",
                nm(sendA), nm(audio), nm(sendW), nm(spec), nm(dec));
    // ★ QoS classes are ordered numerically: a larger value is the higher class.
    ok(sendA == QOS_CLASS_USER_INTERACTIVE, "★★★ the AUDIO send at USER_INTERACTIVE");
    ok(sendA >= audio, "★★★ audio send ≥ DSP");
    ok(audio > sendW, "★★ DSP > waterfall send (a writer CAN step down its own class)");
    ok(sendW >= spec, "waterfall send ≥ spectrum");
    ok(spec > dec, "spectrum > decoders");
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails ? 1 : 0;
}
#else
int main() { skip("no thread ranks on this platform"); return 0; }
#endif
