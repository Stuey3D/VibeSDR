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

#if defined(__ANDROID__) || defined(__linux__)
  #include <sys/resource.h>   // setpriority
  #include <sys/prctl.h>      // PR_SET_NAME
  #include <atomic>
  #include <cerrno>
  #include <cstdio>
  #include <cstring>

  inline void vibeThreadName(const char* name) { prctl(PR_SET_NAME, name); }

  inline void vibeAudioThread(const char* name) {
      prctl(PR_SET_NAME, name);
      errno = 0;
      // = Android's Process.THREAD_PRIORITY_URGENT_AUDIO.
      if (setpriority(PRIO_PROCESS, 0, -19) != 0 && errno != 0) {
          static std::atomic<bool> warned{false};
          if (!warned.exchange(true))
              std::fprintf(stderr,
                  "[VibeLocalSDR] ★ '%s' could not take audio priority (%s) — the real-time "
                  "threads run at the default. Grant it in the unit with "
                  "AmbientCapabilities=CAP_SYS_NICE, or set Nice=-10 for a blunter version.\n",
                  name, std::strerror(errno));
      }
  }
#elif defined(__APPLE__)
  #include <pthread.h>
  // ★ macOS names the CALLING thread and takes no handle. There is no portable nice() equivalent
  //   worth reaching for here — the Mac build is a desktop receiver, not a loaded shared server.
  inline void vibeThreadName(const char* name) { pthread_setname_np(name); }
  inline void vibeAudioThread(const char* name) { pthread_setname_np(name); }
#else
  inline void vibeThreadName(const char*) {}
  inline void vibeAudioThread(const char*) {}
#endif
