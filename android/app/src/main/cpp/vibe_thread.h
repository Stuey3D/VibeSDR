// vibe_thread.h — naming and prioritising the threads that must keep real time.
//
// ★★★ ONE DEFINITION, BECAUSE THERE ARE NOW THREE READERS. This lived inside local_sdr_shim.cpp,
//     so the DAB decoder in vibe_dab_service.h — a 64-state Viterbi over every CIF, four times per
//     96 ms frame, and the single heaviest consumer in the product — could not reach it and ran
//     nameless at default priority. A comment in the shim asserted the opposite ("every consumer
//     (vibe-dsp x2, vibe-dab) already runs at -19 via vibeAudioThread"); there was no vibe-dab.
//     That is the one-rule-two-readers shape this project keeps paying for, so the rule gets a
//     home of its own rather than a second copy.
//
// ★★★ AND IT WAS ANDROID-ONLY, WHICH IS WHY THE PI NEVER HAD ANY OF IT. The guard was
//     `#if defined(__ANDROID__)`. prctl(PR_SET_NAME) and setpriority() are plain Linux APIs;
//     Android has them because Android IS Linux. Every apt server, every Pi and the amd64 box got
//     neither a thread name nor audio priority, silently.
//
// ★★ THE PRIORITY IS NOT FREE ON A SYSTEMD BOX AND THE CODE MUST NOT PRETEND IT IS. A unit running
//    as User=vibeserver cannot renice itself downward without CAP_SYS_NICE, so setpriority()
//    returns -1 and the thread stays at the default. Measured on the Pi 2026-09-08: nice 0,
//    SCHED_OTHER, no capabilities. vibeserver@.service now asks for the capability; when it is
//    refused anyway this says so ONCE, because a silent failure here is indistinguishable from
//    having the priority.
//
// ★ WHAT GETS -19 AND WHAT DOES NOT. Only the real-time chain — the source reader, the DSP, the
//   DAB decoder and each listener's encode — takes audio priority. The accept loop, the hand-off
//   threads and the housekeeping tick keep the default, so raising the chain actually separates it
//   from the rest instead of moving everything up together. A thread that merely must not be
//   forgotten gets a NAME and nothing else.
#pragma once
// ★★ EVERY ROLE THAT RUNS DSP ALSO FLUSHES DENORMALS (2026-10-18) — audio, spectrum, decoder and the
//    IQ input, on every platform. One call at the top of the thread, folded in here because these are
//    already the one place each real-time thread announces itself. Why: vibedsp/vibe_denormals.h.
//    Network, bulk and plain named threads do no DSP and are left alone.
#include "vibedsp/vibe_denormals.h"

#if defined(__ANDROID__) || defined(__linux__)
  #include <sys/resource.h>   // setpriority
  #include <sys/prctl.h>      // PR_SET_NAME
  #include <atomic>
  #include <cerrno>
  #include <cstdio>
  #include <cstring>

  namespace vibeprio {
      constexpr int kNet       = -20;   // the audio socket's send, and the IQ input
      constexpr int kAudio     = -19;   // the real-time chain (= THREAD_PRIORITY_URGENT_AUDIO)
      constexpr int kAudioDecode = -17; // an audio codec on its own thread, fed by the chain (vibe-faad2)
      constexpr int kSendWf    = -10;   // the waterfall/control/decoder socket's send
      constexpr int kSpectrum  = -5;
      constexpr int kDecoder   = 10;
      constexpr int kBulk      = 15;
  }

  inline void vibeThreadName(const char* name) { prctl(PR_SET_NAME, name); }

  inline void vibeAudioThread(const char* name) {
      prctl(PR_SET_NAME, name);
      vibe::vibeFlushDenormals();
      errno = 0;
      // = Android's Process.THREAD_PRIORITY_URGENT_AUDIO.
      if (setpriority(PRIO_PROCESS, 0, vibeprio::kAudio) != 0 && errno != 0) {
          static std::atomic<bool> warned{false};
          if (!warned.exchange(true))
              std::fprintf(stderr,
                  "[VibeLocalSDR] ★ '%s' could not take audio priority (%s) — the real-time "
                  "threads run at the default. Grant it in the unit with "
                  "AmbientCapabilities=CAP_SYS_NICE, or set Nice=-10 for a blunter version.\n",
                  name, std::strerror(errno));
      }
  }
  /* ★★★ THE PRIORITY ORDER (Stuart, 2026-09-19): NETWORK > AUDIO > SPECTRUM > DECODERS.
   *  Audio that is decoded but never sent is silence, so the send threads sit one step ABOVE audio;
   *  a spectrum frame late is a slower waterfall; a decoder behind loses text, never sound.
   *  Raising a nice value needs no privilege, lowering one needs CAP_SYS_NICE (the unit grants it). */
  inline void vibeNiceThread_(const char* name, int nice) {
      prctl(PR_SET_NAME, name);
      errno = 0;
      (void)setpriority(PRIO_PROCESS, 0, nice);   // ★ best effort — vibeAudioThread warns for all
  }
  /* ★★★ THE LADDER, IN ONE PLACE (2026-10-20). Network sends sit at the top, the waterfall's send
   *  one rung BELOW the audio chain and one ABOVE the spectrum work that feeds it:
   *     audio send -20  ≥  IQ input -20  >  audio chain (DSP, demod, DAB, encode) -19
   *     >  waterfall/control send -10  >  spectrum -5  >  default 0  >  decoders 10  >  bulk HTTP 15
   *  ★★ WHY THE WATERFALL'S SEND STEPS DOWN: on a multi-radio server every radio is its own process,
   *     and nice is compared across all of them. One radio's DSP must not lose to another radio's
   *     waterfall — but it must never beat another radio's AUDIO send either, which is the rule
   *     (Network > Audio > Spectrum > Decoders) read across processes. A send thread spends its life
   *     asleep in send() or on its queue, so the rung costs nothing while idle. */
  /* ★★ A REFUSED -20 FALLS BACK TO -19, as the IQ input already did: without the fallback a box that
   *  allows -19 but not -20 (an RLIMIT_NICE of 39) would leave the send thread at 0 under a -19 DSP —
   *  the exact inversion this ladder exists to prevent. vibeAudioThread warns, once, if that fails too. */
  inline void vibeNetThread(const char* name) {
      prctl(PR_SET_NAME, name);
      errno = 0;
      if (setpriority(PRIO_PROCESS, 0, vibeprio::kNet) != 0 && errno != 0) vibeAudioThread(name);
  }
  /* ★★★ A SEND THREAD LEARNS WHAT IT CARRIES AFTER IT STARTS. The outbox writer starts at the network
   *  rung (the handshake and a refusal go out there), and re-ranks itself once acceptWs has said which
   *  socket it serves: audio stays at the top, anything else steps down to kSendWf. Renamed too, so a
   *  look at /proc tells the two apart (vibeTx-audio / vibeTx-wf, both under 16 bytes).
   *  ★ Stepping DOWN needs no privilege; stepping up from -10 to -20 does, and a writer only ever goes
   *    one way (it is classified once). */
  inline void vibeSendRank(bool audio) {
      if (audio) { vibeNetThread("vibeTx-audio"); return; }
      vibeNiceThread_("vibeTx-wf", vibeprio::kSendWf);
  }
  inline void vibeSpectrumThread(const char* name) { vibe::vibeFlushDenormals(); vibeNiceThread_(name, vibeprio::kSpectrum); }
  inline void vibeDecoderThread(const char* name)  { vibe::vibeFlushDenormals(); vibeNiceThread_(name, vibeprio::kDecoder); }
  /* ★★ AN AUDIO CODEC OFF THE CHAIN'S OWN THREADS (V12, 2026-10-26 — DAB+ through FAAD2, vibe-faad2). It IS the audio
   *  path, so it sits far above spectrum and decoders — but one rung BELOW the chain that feeds it (-19: the IQ reader,
   *  the DAB OFDM/Viterbi, the DSP): a unit that never leaves the demodulator cannot be decoded at all, and a codec a
   *  few ms late is absorbed by the PCM buffer (~2 s) where a late demodulator loses samples for good. CFS weights:
   *  -17 weighs 0.64x of -19 and ~15x of spectrum's -5 — on a busy Pi 2 it still gets its time, just after the chain's. */
  inline void vibeAudioDecodeThread(const char* name) {
      prctl(PR_SET_NAME, name);
      vibe::vibeFlushDenormals();
      errno = 0;
      if (setpriority(PRIO_PROCESS, 0, vibeprio::kAudioDecode) != 0 && errno != 0) vibeAudioThread(name);   // ★ warns once
  }
  /* ★★★ BULK HTTP — BELOW THE WHOLE ORDER. Map tiles, the map data, the web client's scripts:
   *  bytes somebody is waiting for, but nobody HEARS late. A zoom-out on the GPU map fires dozens of
   *  PMTiles range reads at once, each on its own connection thread, and those threads sat at nice
   *  0 beside a Pi 2 whose WFM chain already needs most of a core ("zooming out of the map caused a
   *  tiny stutter", Stuart, 2026-10-01). Called by a CONNECTION thread once it knows the request is
   *  a plain file and will hold no radio lock; the thread ends with the request (Connection:
   *  close), so nothing has to put the priority back.
   *  ★★ NOT for the admin API or anything that takes clientMtx/modeMtx: a low-priority thread that
   *     holds a lock the DSP needs is priority inversion, which is worse than the contention. */
  inline void vibeBulkThread(const char* name)     { vibeNiceThread_(name, vibeprio::kBulk); }
  /* ★★★ THE IQ INPUT SITS ABOVE THE WHOLE ORDER — Network > Audio > Spectrum > Decoders all
   *  consume what it delivers, and a sample it fails to collect is lost for every one of them
   *  (never late: GONE — a radio library drops the buffer). So it takes the top nice, level with
   *  the send threads, falling back to audio priority where -20 is refused.
   *  ★★ WHY IT EXISTS (2026-09-30): the Airspy libraries' own USB and consumer threads ran at nice 0
   *     under a dozen of our -19 threads — a CFS weight of 1024 against ~70 000 each — so on a
   *     loaded Pi they were starved past their ring and dropped IQ, and the listener's RDS block
   *     errors tracked the load (~25 % full, ~1.5 % idle). The dongle's reader was already at -19
   *     and read 0-4 % on the same box at the same time. It does little work, so ranking it first
   *     costs the rest nothing. */
  inline void vibeIqThread(const char* name) {
      prctl(PR_SET_NAME, name);
      vibe::vibeFlushDenormals();
      errno = 0;
      if (setpriority(PRIO_PROCESS, 0, vibeprio::kNet) != 0 && errno != 0) vibeAudioThread(name);
  }
#elif defined(__APPLE__)
  #include <pthread.h>
  #include <pthread/qos.h>
  // ★ macOS names the CALLING thread and takes no handle.
  inline void vibeThreadName(const char* name) { pthread_setname_np(name); }
  /* ★★★ THE MAC BUILD IS A SERVER TOO, and this used to do nothing but name the thread. "The Mac
   *  build is a desktop receiver, not a loaded shared server" was the comment here — and then the
   *  notarised Mac VibeServer shipped (V5), with DAB, serving the same tunnel the Pi does. Its
   *  20 ms audio clock, its USB reader and its DSP loop were all running at the default QoS,
   *  which macOS is free to throttle behind a Safari tab.
   *  ★ QOS_CLASS_USER_INTERACTIVE is the highest class a process may ask for without entitlements
   *    and is what CoreAudio's own render threads sit near; it is the honest equivalent of the
   *    nice -19 the Linux and Android builds take. A time-constraint policy would be stronger and
   *    is deliberately not used: it needs the thread's period and computation declared, and a
   *    wrong declaration is worse than a good QoS. */
  inline void vibeAudioThread(const char* name) {
      pthread_setname_np(name);
      vibe::vibeFlushDenormals();
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  }
  // ★ macOS has no finer class above USER_INTERACTIVE; network shares it, the rest step down.
  inline void vibeNetThread(const char* name) {
      pthread_setname_np(name);
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  }
  // ★★ The send thread's re-rank (see the Linux note): audio level with the DSP, the waterfall's send
  //    level with the spectrum work — macOS has no class between USER_INTERACTIVE and USER_INITIATED.
  inline void vibeSendRank(bool audio) {
      pthread_setname_np(audio ? "vibeTx-audio" : "vibeTx-wf");
      pthread_set_qos_class_self_np(audio ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
  }
  inline void vibeSpectrumThread(const char* name) {
      pthread_setname_np(name);
      vibe::vibeFlushDenormals();
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  }
  inline void vibeDecoderThread(const char* name) {
      pthread_setname_np(name);
      vibe::vibeFlushDenormals();
      pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  }
  // ★ The audio codec's own thread (see the Linux note): the audio class, one step below the chain within it.
  inline void vibeAudioDecodeThread(const char* name) {
      pthread_setname_np(name);
      vibe::vibeFlushDenormals();
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, -2);
  }
  // ★ Bulk HTTP (see the Linux note): the lowest class that still makes steady progress.
  inline void vibeBulkThread(const char* name) {
      pthread_setname_np(name);
      pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  }
  // ★ The IQ input — see the Linux note. macOS has nothing above USER_INTERACTIVE to give it.
  inline void vibeIqThread(const char* name) {
      pthread_setname_np(name);
      vibe::vibeFlushDenormals();
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  }
#else
  inline void vibeThreadName(const char*) {}
  inline void vibeAudioThread(const char*) { vibe::vibeFlushDenormals(); }
  inline void vibeNetThread(const char*) {}
  inline void vibeSendRank(bool) {}
  inline void vibeSpectrumThread(const char*) { vibe::vibeFlushDenormals(); }
  inline void vibeDecoderThread(const char*) { vibe::vibeFlushDenormals(); }
  inline void vibeAudioDecodeThread(const char*) { vibe::vibeFlushDenormals(); }
  inline void vibeBulkThread(const char*) {}
  inline void vibeIqThread(const char*) { vibe::vibeFlushDenormals(); }
#endif
