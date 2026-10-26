#!/usr/bin/env bash
# run-tests.sh — build and run every C++ test in vibeserver/.
#
# ★★ THEY WERE NOT REGISTERED ANYWHERE. Each test-*.cpp was compiled by hand when it was written
#    and never again, so nothing would have told us if one started failing — and on this project
#    the tests are the method, not the paperwork. A suite nobody runs is a suite that is already
#    broken and has not been told yet.
#
# ★ Each test states its own dependencies. Ones that need downloaded data (geoip, asn) are run and
#   reported honestly rather than quietly skipped: "needs data" is a result, not a pass.
set -uo pipefail
cd "$(dirname "$0")/.."

SRC=vibeserver
OUT="${TMPDIR:-/tmp}/vibeserver-tests"
mkdir -p "$OUT"

VDSP=android/app/src/main/cpp/vibedsp
KISS="$VDSP/third_party/kissfft"

# test name -> extra compiler flags it needs
flags_for() {
  case "$1" in
    # ★ The DSP engine lives outside vibeserver/ and carries its own vendored kissfft, so this one
    #   needs both on the include path. Worth it: it is the only test that drives the real audio
    #   chain end to end.
    test-wfm-stereo) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ Same deps as test-wfm-stereo: it drives the same real audio chain, which is the only way
    #   to measure a feature that responds to FM's triangular noise spectrum.
    test-stereo-highblend) echo "-O2 -I $VDSP -I $KISS" ;;
    test-multipath-meter)  echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★★ Our own gain step through the real chain (2026-10-19): the smoother + the pipeline's guard,
    #     every case run with and without the step on the same noise seed. Optimised: ~30 runs of 8 s.
    test-gain-step)        echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ A cost RATIO: optimised like the shipping build, or the idle side is all loop overhead.
    test-gain-step-cost)   echo "-O2" ;;
    # ★ The Advanced RDS instrument (MpxMeasure) against a signal of known deviation, at several
    #   capture rates and passbands — the real pipeline end to end, so the same deps again.
    test-mpx-measure|test-rds-ps-frames) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ WFM DSP cost vs passband — a narrow width once cost ~5x the whole chain (the Sony's
    #   161-276 % of real time). Optimised like the server, or the ratio is not the server's.
    test-wfm-narrow-cost)  echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★ V12 TRANSCRIBE's SSB auto-tune (vibedsp/ssb_offset): the estimator, the shift, the decision and the
    #    anti-aliased recogniser feed. (test-ssb-offset-clips, on the research's first UberSDR clips, was retired
    #    2026-10-17: those folders are gone, and the two-stage estimator is measured on new ones — test-ssb-offset-real.)
    test-ssb-offset) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★★ …and on REAL off-air audio (2026-10-17): VOLMET at eleven known dials, QSOs demodulated from busy-band I/Q,
    #     two stations in one passband — where the two-stage estimator's gates were chosen, with a held-out set.
    #     NOT RUN (exit 3) unless VIBE_TEST_AUDIO points at the folder holding ssb/ and iq/ (~1 GB, not in the repo).
    #     Takes a few minutes (it demodulates ~80 two-minute clips). VIBE_SSB_OWNER=<m4a> adds Stuart's recording.
    test-ssb-offset-real) echo "-O2 -I $VDSP -I $KISS -lpthread" ;;
    # ★★ V12 BINAURAL TUNING (vibedsp/binaural): the spatialiser, the WFM hand-off and the SSB source (FAST estimator).
    test-binaural) echo "-O2 -I $VDSP -I $KISS -lpthread" ;;
    # ★★★ …and between TWO signals (Stuart on 483: "blending them both and swirling them about"): one below the dial, one
    #     above, USB/LSB/CW, at once / in turns / one stronger. Synthetic always; Stuart's 483 CW clip and off-air VOLMET
    #     pairs too when VIBE_TEST_AUDIO points at the folder holding binaural/ and ssb/ (reported NOT RUN otherwise).
    test-binaural-two) echo "-O2 -I $VDSP -I $KISS -lpthread" ;;
    # ★ Every mode's DSP cost vs passband, both directions and past the ceiling — the audit that
    #   followed the WFM cliff (2026-09-29). Optimised for the same reason.
    test-passband-cost)    echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★★ Every chain delivers exactly 48 000 audio samples per second of capture (2026-10-07: the RSPs
    #     ran 32-34 ppm short through a rounded, approximated resampler ratio). Optimised: 21 s of input per chain.
    test-resampler-rate)   echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★★ FirDecimator/RealFir against the direct convolution, sample for sample (2026-10-18) — on x86
    #     the SSE2 aligned tap banks, on ARM the NEON kernel. The server's own flags for vibedsp.
    test-fir-simd)         echo "-O3 -ffp-contract=fast -I $VDSP" ;;
    # ★ The real decoders on real signals (RTTY, WEFAX, an encoded FT8 slot) — optimised so FT8's
    #   slot decode finishes in a second rather than ten.
    test-decoder-hosts)    echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★★ BACKGROUND DECODING + THE GALLERY (V12, 2026-10-22): the real DSP chain AND the real decoders, end to end.
    test-bgdecode|test-bg-priority|test-cw) echo "-O2 -I $VDSP -I $KISS -I android/app/src/main/cpp/ft8_lib" ;;
    test-decfeed-resampler) echo "-O2 -I android/app/src/main/cpp" ;;
    test-bm-names) echo "-O2 -I android/app/src/main/cpp" ;;
    # ★ The benchmark's decoder rows — optimised like the server, or the costs are not the server's.
    test-bench-decoders)   echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ Hostile attach messages through the real decoders (audit 2026-10-03) — same deps as the hosts.
    test-decoder-hardening) echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ SSTV geometry + colour on synthetic pictures with a known sender clock error (audit 2026-10-04)
    #   — 16 full frames, ~25 min of audio; optimised or it takes minutes.
    test-sstv-quality)     echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ FT8's hashed-callsign table + spot filter (audit 2026-10-04, row 10), through ft8_lib's real pack/unpack.
    test-ft8-callhash)     echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ AirspyHfSource against a STUB libairspyhf (the test defines the C API) — the vendored header
    #   for the types, and the fd entry points switched on (2026-10-05).
    test-airspyhf-restart) echo "-DVIBE_HAVE_AIRSPYHF -DVIBE_AIRSPYHF_HAS_FD -I android/app/src/main/cpp/libairspyhf -lpthread" ;;
    # ★ HackRfSource + AirspySource against STUB libhackrf/libairspy, and vibe_usb_recovery.h's rules
    #   (park-not-release, the fresh-fd back-off, the re-plug advice) — 2026-10-05.
    test-fd-radio-recovery) echo "-DVIBE_HAS_HACKRF -DVIBE_HACKRF_HAS_FD -DVIBE_HAVE_AIRSPY -I android/app/src/main/cpp/libairspy -lpthread" ;;
    # ★★ FT8's extra passes, their CPU gate and the capture-clock slot (2026-10-05) — optimised, it times passes.
    test-ft8-passes)       echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★★ FT8's fast kernels against ft8_lib's originals, bit for bit (2026-10-25) — the server's -O2.
    test-ft8-fast)         echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★★ V12 ADS-B (vibe_adsb.h): CRC, fields and CPR on the published worked examples, then the demodulator on
    #    frames modulated into noisy IQ at every capture rate a radio runs — optimised, it demodulates ~40 s of IQ.
    test-adsb)             echo "-O2" ;;
    # ★★ The ADS-B gain rule after Stuart's FlightAware box: the top, down only for dynamic range, never for strong aircraft.
    test-adsb-gain)        echo "-O2 -lpthread" ;;
    # ★★ ADS-B's sample supply (vibe_adsb_capture.h — the live capture %), the boosted-rate stall/crash guard
    #    (vibe_rate_guard.h) and the benchmark's ADS-B row on a fake source (vibe_benchmark_adsb.h, which pulls in the
    #    benchmark's headers). ~12 s: two parts feed in real time, as the server does.
    test-adsb-capture)     echo "-O2 -I $VDSP -I $KISS -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★ V12 aircraft data: builder/reader/enrichment/logo zip installer on vibeserver/testdata-adsb (VIBE_ADSB_RAW
    #    = a folder with the real downloads also checks known aircraft and routes — scripts/build-adsb-data.sh)
    test-adsb-data)        echo "-O2 -lpthread" ;;
    # ★★ V12 ISM (vibe_ism*.h): every protocol on its source's worked examples, then encoded from the published specs into
    #    noisy IQ at 1–10 MS/s through the real front end; drift, SNR, pure noise, the privacy rule on the JSON, a CPU row.
    test-ism)              echo "-O2 -lpthread" ;;
    test-ais|test-aprs)    echo "-O2 -lpthread" ;;
    # ★★ V12 ISM IN THE GALLERY (vibe_ism_gallery.h, 2026-10-25): the day's log (the line, the RAM buffer, the fold, parts),
    #    the summary card, one file per UTC day per band, a restart, the privacy rule in every file, the ISM profile config.
    test-ism-gallery)      echo "-O2 -lpthread" ;;
    *)               echo "" ;;
  esac
}

# test name -> the sources it needs besides itself
deps_for() {
  case "$1" in
    test-wfm-stereo)    echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-stereo-highblend) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-gain-step) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-multipath-meter) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-mpx-measure|test-rds-ps-frames) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-wfm-narrow-cost) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-passband-cost)   echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-resampler-rate)  echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-ssb-offset) echo "$VDSP/ssb_offset.cpp $VDSP/resampler.cpp $VDSP/fft.cpp $VDSP/ddc.cpp \
                              $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-ssb-offset-real) echo "$VDSP/binaural.cpp $VDSP/ssb_offset.cpp $VDSP/resampler.cpp $VDSP/fft.cpp $VDSP/ddc.cpp \
                              $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-binaural|test-binaural-two) echo "$VDSP/binaural.cpp $VDSP/ssb_offset.cpp $VDSP/resampler.cpp $VDSP/fft.cpp $VDSP/ddc.cpp \
                              $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    # ★★ proc.cpp goes with anything that SHELLS OUT (curl, mostly): geoip, asndb and radiodns all
    #    call vibeproc::run. It was missing, so those three "did not build" — and a suite that does
    #    not build is not a suite that passes, it is one nobody is running. Caught 2026-09-21.
    test-fir-simd)      echo "$VDSP/ddc.cpp" ;;
    test-config-radios) echo "$SRC/vibeserver_config.cpp" ;;
    # ★ No $KISS here: the ft8 C objects (cobjs_for) already carry kiss_fft, and two copies do not link — nor pffft,
    #   which they carry too since monitor.c's STFT runs through it (2026-10-25).
    test-bgdecode|test-bg-priority|test-cw) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp \
                              android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp \
                              android/app/src/main/cpp/decoders/wefax_decoder.cpp android/app/src/main/cpp/decoders/sstv_decoder.cpp \
                              android/app/src/main/cpp/decoders/time_decoder.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp \
                              android/app/src/main/cpp/decoders/cw_skimmer.cpp" ;;
    test-converter)     echo "$SRC/vibeserver_config.cpp" ;;
    test-rtl-eeprom)    echo "$SRC/rtl_eeprom.cpp" ;;
    test-fd-passing)    echo "android/app/src/main/cpp/fd_passing.cpp" ;;
    # ★★ The HF+'s recovery ORDER — hung-restart deadline, fd dup ownership, rate guard, fresh-fd reopen.
    test-airspyhf-restart) echo "android/app/src/main/cpp/airspyhf_source.cpp" ;;
    # ★★ The same recovery for the HackRF and the R2/Mini — dup ownership, deadline release, fresh-fd replay.
    test-fd-radio-recovery) echo "android/app/src/main/cpp/hackrf_source.cpp android/app/src/main/cpp/airspy_source.cpp" ;;
    test-parent-watch)  echo "$SRC/parent_watch.cpp" ;;
    # ★★ The pocket image's daemon side: captive answers, the first-run claim, the Wi-Fi request.
    test-pocket)        echo "$SRC/pocket.cpp" ;;
    # ★★★ The dual-stack listener and IPv6 client classes (2026-10-25): both families on one socket, an IPv4 client
    #     reading exactly as before, the LAN prefixes and the /64 lockout key.
    test-ipclass)       echo "android/app/src/main/cpp/net_shim.cpp" ;;
    # ★★ The config survives a pulled plug: atomic save, last-good .bak, refusal of a truncated file.
    test-config-durable) echo "$SRC/vibeserver_config.cpp" ;;
    # ★★★ Going back from a V12 beta keeps V12's settings: unknown config keys survive a load + save (11.0.10-4).
    test-config-keep)   echo "$SRC/vibeserver_config.cpp" ;;
    test-connlog)       echo "" ;;
    # ★★★ An admin read must never hold the connection log's lock while it works (Pi 2 IQ overruns
    #     with the admin page open, 2026-10-01). Header-only; slow resolvers make it CPU-independent.
    test-connlog-lock)  echo "" ;;
    # ★★ The map's shared byte budget (vibe_bulk_pace.h), on a synthetic clock.
    test-bulk-pace)     echo "" ;;
    # ★★ A burst of tunes: the newest wins, paced by the server's load (vibe_tune_pace.h, 2026-10-05).
    test-tune-pace)     echo "" ;;
    # ★★★ …and DAB block steps through the same hold (2026-10-21): a burst re-acquires on its last block.
    test-dab-block-pace) echo "" ;;
    # ★★★ The shared chain's passband edges in config (vibe_room_passband.h, 2026-10-17): a listener's
    #     sliders follow the room's, in the mode they were chosen in.
    test-room-passband) echo "" ;;
    test-time-decoder)  echo "android/app/src/main/cpp/decoders/time_decoder.cpp" ;;
    test-radiodns-ecc)  echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-radiodns-name) echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-geoip)         echo "$SRC/geoip.cpp $SRC/proc.cpp" ;;
    test-asndb)         echo "$SRC/asndb.cpp $SRC/proc.cpp" ;;
    test-moveloc)       echo "$SRC/moveloc.cpp $SRC/proc.cpp" ;;
    test-admin-banlist) echo "" ;;
    # ★★ The POWER card's verdict (vibe_power.h, 2026-10-16): brief supply dips amber; red only for a dip
    #    held ≥ 10 s, near-continuous dips, USB over-current, or a USB device lost in a dip. Header-only,
    #    with a fake /sys and /dev for the rpi_volt alarm and the USB scans.
    test-power-dips)    echo "" ;;
    test-decoder-hardening) echo "$VDSP/ddc.cpp android/app/src/main/cpp/spyserver/spyserver_messages.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp" ;;  # ★ pffft comes with the ft8 objects (cobjs_for)
    # ★ NAVTEX's CCIR 476 coder, word by word (audit 2026-10-04, rows 11-13); the audio half is scripts/test-navtex.sh.
    test-navtex-fec)    echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;
    # ★★ RTTY's letters/figures shift across a lost signal: a lost LTRS → letters, a synop line → figures (2026-10-13).
    test-rtty-shift)    echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;
    test-ft8-callhash)  echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-ft8-passes)    echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-ft8-fast)      echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-decoder-hosts|test-bench-decoders) echo "$VDSP/ddc.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp" ;;  # ★ pffft comes with the ft8 objects (cobjs_for)
    test-sstv-quality)  echo "$VDSP/ddc.cpp android/app/src/main/cpp/decoders/sstv_decoder.cpp" ;;   # ★ ddc: the host carries APRS (V12)
    # ★★ V12 AIS + APRS (docs/v12/AIS-APRS.md): published worked examples, every message type, the modems at many SNRs,
    #    ten minutes of noise per modem with zero frames. The vibedsp FIR/NCO (NEON / SSE2) are what they run on.
    test-ais|test-aprs) echo "$VDSP/ddc.cpp" ;;
    test-wefax-phasing) echo "android/app/src/main/cpp/decoders/wefax_decoder.cpp" ;;
    *)                  echo "" ;;
  esac
}

# test name -> C sources it needs. ★ Compiled as C, separately: g++ would compile a .c file as C++,
#   and ft8_lib is C that C++ refuses (malloc without a cast). Prints the object files built.
FT8C=android/app/src/main/cpp/ft8_lib
cobjs_for() {
  case "$1" in
    test-decoder-hosts|test-bench-decoders|test-decoder-hardening|test-sstv-quality|test-ft8-callhash|test-ft8-passes|test-ft8-fast|test-bgdecode|test-bg-priority|test-cw)
      local d="$OUT/cobj-ft8"; mkdir -p "$d"
      # ★ + pffft.c: monitor.c's STFT runs through PFFFT (2026-10-25). ★ Rebuilt when any ft8_lib header is newer
      #   too: ldpc_tables.h / vibe_simd4.h changes would otherwise leave a stale object passing the tests.
      for f in $FT8C/ft8/*.c $FT8C/fft/kiss_fft.c $FT8C/fft/kiss_fftr.c $FT8C/common/monitor.c $VDSP/third_party/pffft/pffft.c; do
        local o="$d/$(basename "$f" .c).o"
        { [ "$o" -nt "$f" ] && [ -z "$(find $FT8C -name '*.h' -newer "$o" 2>/dev/null | head -1)" ]; } || cc -O2 -c -I "$FT8C" -o "$o" "$f" || return 1
        printf '%s ' "$o"
      done ;;
    *) ;;
  esac
}

pass=0; fail=0; broke=0; notrun=0
for t in "$SRC"/test-*.cpp; do
  name="$(basename "$t" .cpp)"
  # ★ Not every test fits this harness. It compiles ONE .cpp with a named handful of deps, which is
  #   what keeps it fast and dependency-free — but a test that needs the whole core (real drivers,
  #   the shim, opus, fftw) cannot be expressed that way and is a CMake target instead. Listing it
  #   here rather than renaming it keeps it discoverable beside its siblings.
  #     test-radio-api — links vibeserver_core; built by `cmake --build … --target test-radio-api`.
  #     test-icom-source — links vibe_icom (libusb, ALSA/CoreAudio, the source's threads); ctest `icom-source`.
  case "$name" in
    test-radio-api|test-icom-source) printf '\n\033[1m── %s ──\033[0m\n   \033[2mskipped — a CMake target (needs the whole core); build it with cmake\033[0m\n' "$name"; continue ;;
  esac
  printf '\n\033[1m── %s ──\033[0m\n' "$name"
  # shellcheck disable=SC2046
  if ! cobj="$(cobjs_for "$name" 2>"$OUT/$name.buildlog")" || \
     ! g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for "$name") \
        -o "$OUT/$name" "$t" $(deps_for "$name") $cobj 2>>"$OUT/$name.buildlog"; then
    printf '   \033[33mdid not build\033[0m — %s\n' "$OUT/$name.buildlog"
    head -5 "$OUT/$name.buildlog" | sed 's/^/     /'
    broke=$((broke+1)); continue
  fi
  # ★ Exit 3 = NOT RUN (its data is not here) — counted apart, never as a pass. Same rule as the end-to-end suites.
  "$OUT/$name"; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
done

# ★ The setup page is a C++ raw string, so no compiler ever looks at its JavaScript. A syntax
#   error there ships silently and the page simply does nothing — the worst failure this tree has,
#   because that page is what a new owner meets first.
printf '\n\033[1m── setup page ──\033[0m\n'
if node scripts/check-setup-page.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ The August RTL stutter fixes (gain write off modeMtx/the DSP, one transfer, the client's hwinfo
#     signature skip) — a gain change must never again stall the audio. See check-gain-write-path.mjs.
printf '\n\033[1m── RSP hwinfo -> controls, per model ──\033[0m\n'
if node scripts/check-rsp-caps.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── device filters (RSP only from Android 8.0) ──\033[0m\n'
if node scripts/check-device-filter.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── gain write path ──\033[0m\n'
if node scripts/check-gain-write-path.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ A Wi-Fi name typed with the wrong apostrophe is saved as it is on the air (Portable road test, 2026-10-14).
if node scripts/test-setup-wifi-name.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node scripts/test-setup-wifi-wired.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ New Wi-Fi hardware on a set-up box (a USB dongle added later, 2026-10-25): "configure it now?" — Configure / Not now.
if node scripts/test-setup-wifi-new.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ Fork labelling (directory forkOf): a fork's name is shown, checked; official names refused as fork names.
if node --no-warnings directory/scripts/test-fork.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ "In use by another app" through the directory (2026-10-18): the listing carries it, a held radio is never FREE,
#     and a radio whose process did not answer (503) does not borrow the door's 0 listeners and read FREE either.
if node --no-warnings directory/scripts/test-in-use.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE DIRECTORY PAGE (2026-10-13): this check existed but was never run here, so it failed for a month after the dial gained
#    its second marker and nobody saw. The directory is what everyone meets first, with nothing to download.
if node scripts/check-directory-page.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ Behaviour, not syntax: the page's own functions driven against a stubbed DOM, because the bug
#   this catches — one radio's sample rate landing in another's config during a tab switch — is
#   perfectly valid JavaScript and perfectly well-formed HTML.
# ★★ Background decoding + "let another program use it" share the idle radio BY TIME (2026-10-22): the setup page's line.
if node scripts/test-setup-bg-release.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ Background decoding and the decoder slots (2026-10-22): one radio → VFOs capped at the slots; several → the tier list.
if node scripts/test-setup-bg-tiers.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE GOVERNOR MENU OFFERS ONLY WHAT THIS MACHINE HAS (2026-10-19): the Lenovo's intel_pstate has no schedutil.
if node scripts/test-setup-governors.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE WATERFALL RATE MENU SHOWS THE RATE THE SERVER RUNS (11.0.9): a hand-set 15 is "15 fps (set by hand)", never "Full · 20".
if node scripts/test-setup-maxfps.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 RATE SWITCH: "change the sample rate when a mode needs it" (dabRateBoost); the RTL 3.2 MS/s ADS-B option stays removed.
if node scripts/test-setup-mode-rate.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 ADS-B AS THE LANDING: drawn only where ADS-B can run, keep-running only beneath it and never ticked by default,
#    and when ticked the Background Decoding and release cards say the idle radio is ADS-B's.
if node scripts/test-setup-adsb-landing.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 BACKGROUND DECODING's editor (2026-10-22): the page's real code — Nick's profile round-trips, decoders follow the
#    demodulator, "add another profile" only under a timed one, delete, the dial hint.
if node scripts/test-setup-bgdecode.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and an ISM PROFILE (2026-10-25): offered only where ISM can run (not an HF+, an Icom, a locked radio, ISM blocked),
#    only the bands the radio reaches; a band instead of a centre and VFOs; saved as {"ism":"433"} and read back.
if node scripts/test-setup-bg-ism.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ A TUNNEL A PREVIOUS PROCESS LEFT ON OUR PORT IS RETIRED (2026-10-19, Kiko's four tunnels). Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-tunnel-orphan.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node scripts/test-setup-tabswitch.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ The admin log folds a visit's per-radio rows into one. Pure logic, so it is testable without a
#   browser — and the cases that matter are the ones it must NOT fold (session-less refusals).
if node scripts/test-visit-grouping.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ The admin page's real module in jsdom (no browser, no audio): health cards, POWER's amber/red verdicts, bans, polling.
if node scripts/test-admin-page.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ A DATA MIGRATION, so it is tested: bookmarks move from being keyed on the URL you arrived by
#    to the server's own identity. The failure modes are somebody's bookmarks going invisible on
#    upgrade, or one server adopting another's — neither of which looks wrong in a screenshot.
if node scripts/test-bookmark-scope.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE NR SLIDER'S TWO CONVERSIONS MUST BE EXACT INVERSES. The server echoes the strength it is
#     using and the client renders it back onto the slider, then dispatches `input` — which sends
#     it again. In that loop a units mismatch is not an off-by-a-bit, it is a RATCHET: the setting
#     climbed 30 -> 100% on its own, on air (2026-08-26). Neither half looks wrong on its own.
if node scripts/test-nr-roundtrip.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ The two ends of the advanced-RDS message must agree on field NAMES. A stray "R." prefix meant
#   the deviation readout never populated at all, and neither half looked wrong on its own.
if node scripts/check-rdsx-wire.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The pocket box's Wi-Fi state machine against a fake NetworkManager: strict 1/2/3 order, moving
#    up when network 1 returns, never pulling a hotspot from under a phone, no password in any log.
if python3 vibeserver/linux/pocket/test_vibeserver_pocket.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ DOWNLOAD DIAGNOSTICS (2026-10-20): the file an owner sends us never carries a secret — a Wi-Fi password, the admin
#     password, a PIN, the directory key, planted in NetworkManager, the config, a status file and the journal.
if python3 vibeserver/linux/test_vibeserver_diagnostics.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ A PORTABLE UPDATES FULLY WITHOUT A NEW CARD (Stuart, 2026-10-13). vibeserver-pocket-converge on a fake root: a no-op
#     without the marker, idempotent, config.txt lines once, an owner's own change kept, the package list drives what
#     Update installs — and vibeserver-maintenance's `update` with apt + overlayroot-chroot stubbed.
if python3 vibeserver/linux/pocket/test_pocket_converge.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE READ-ONLY SEAL ON BOTH BOOT CHAINS (2026-10-25, image/sbc): cmdline.txt on a Raspberry Pi, extraargs= in
#    armbianEnv.txt / dietpiEnv.txt on a U-Boot board — once, the owner's own arguments kept, nothing without the marker.
#    And the build the card's processor can run (vibeserver-pocket-cpu): ARMv6 on a Pi Zero / Pi 1, nothing elsewhere.
#    The owner's SSH switch (11.0.11): password on stdin only, only the hash kept, restored after a sealed reboot, off = locked.
if python3 vibeserver/linux/pocket/test_pocket_reset.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if python3 vibeserver/linux/pocket/test_pocket_ssh.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if python3 vibeserver/linux/pocket/test_pocket_cpu.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if python3 vibeserver/linux/pocket/test_pocket_seal.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ RECEIVE BETA UPDATES (2026-10-22). The root helper's beta-on / beta-off / update-stable with apt stubbed, and the
#     choice of what Update VibeServer installs (beta on/off × installed × listed, the downgrade back to stable included).
if python3 vibeserver/linux/test_vibeserver_maintenance.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …what the admin page says about it, and which action its button sends (updateOffer.ts).
if node --no-warnings scripts/test_update_offer.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ …and the apt repository's two suites: a beta publish never touches stable (and the reverse), one revision never
#     names two files, the prune is per suite. publish-apt.sh itself, index-only, with the Debian tools stubbed.
if python3 scripts/test_publish_apt_suites.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ The web playout (worklet AND the main-thread fallback) must come back clean by itself after the
#   server has delivered audio in bursts — the Sony's narrow-WFM overload, 2026-09-29. Silent.
if node scripts/test-web-playout-burst.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ The web decoder socket says whose it is (user_session_id) and takes a refusal — shown in the
#   server's words and FORGOTTEN, so the 3-s reconnect cannot turn one "no" into a loop (B6).
if node scripts/test-web-decoder-refusal.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE WEB CONTROL COLOURS (controlColours.ts): the default look sets no variable (today, to the
#    pixel), every swatch combination stays ≥ 3:1, SOLID is opaque with no backdrop-filter anywhere, the
#    keys travel as VIEW_KEYS, and an older browser starts solid unless a stored choice says otherwise.
if node --no-warnings scripts/test-web-control-colours.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE DESKTOP BAR STAYS GONE. The compact card (#mcard) is the web layout at every width; the old
#     bar shipped hidden for months, still wired, and fixes landed on it that reached nobody. Checks the
#     BUILT page (web/dist — run node scripts/build-web.mjs): no removed id as an element, a selector or
#     a script literal, and the bar's real leftovers (#linkStats, search, bandwidth row) where they live.
if node scripts/test-web-no-desktop-bar.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE FACEPLATE RULES (app). Nixie One is neon in every chassis × display × colour, dot/seg can
#     never reach white by any route (picker, migration, stored prefs), and the DEFAULT settings
#     resolve to today's literal colours — the pixel-identical promise, checked as data.
#  ★ Node runs the .ts directly (type stripping), so this needs nothing installed.
if node --no-warnings scripts/test_faceplate.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ MPX POWER IN dBr (mpxPower.ts): relative to BS.412's reference, so a figure below 0 reads as compliant, not a fault.
if node --no-warnings scripts/test_mpx_power.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ LOCKED VFO CENTRE (tuneWindow.ts): every app tune clamps into the owner's window, the server's own rule (Stuart 2026-10-18: the app tuned into the black).
if node --no-warnings scripts/test_tune_window.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ proxy.kiwisdr.com IS 8073 ONLY (2026-10-07): 47 % of public Kiwis are listed portless (= port 80, refused).
if node --no-warnings scripts/test_kiwi_proxy_port.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ SERVER ADDRESSES (serverUrl.ts): scheme and port are a PAIR — https://host:8073 stays TLS (GitHub #52, OWRX+ native TLS).
if node --no-warnings scripts/test_server_url.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 BINAURAL TUNING (binauralOffset.ts): the offset source per mode, AM/CW/FM readings with known offsets, the tracker.
if node --no-warnings scripts/test_binauralOffset.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 BINAURAL — THE WATERFALL CUE ON A SWEEP (2026-10-18, Stuart on 482: "it's not panning as I tune through the
#     band"): the real tracker over spectra from real voices, the dial clicking 100/500 Hz. NOT RUN (exit 3) without
#     VIBE_TEST_AUDIO (the I/Q is not in the repo). ~2 minutes (it builds a composite band from the recordings).
node --no-warnings --max-old-space-size=4000 scripts/test_binauralSweep.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 TRANSCRIBE AUTO ORDER (transcribeOrder.ts): EiBi, the SDR's local language's reserved lane, the ★ list, the ⚠.
if node --no-warnings scripts/test_transcribeOrder.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 RECORD BUFFER (recordBuffer.ts): the retune rule — per-mode allowance, measured from the ANCHOR, mode/source clear.
if node --no-warnings scripts/test_recordBuffer.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 CW (cwList.ts): the app's CW box against the web's — order, confidence colours, spots, the scroll pin, the segment drawn.
if node --no-warnings scripts/test_cw_list.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 ISM (src/services/ism.ts): the app's ISM against the web's — sanitiser, lines in every unit, status, filter, capability.
if npx tsx scripts/test_ism_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the iOS/Mac ring + the prefix-then-live writer order (macOS only: needs xcrun swiftc).
if command -v xcrun >/dev/null 2>&1; then if bash scripts/test_recordBuffer_swift.sh; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi; fi
# ★★★ V12 ICOM, CLIENT SIDE (icomRadio.ts): hwinfo.radio + the status line as the server builds them, the hide list against the
#     contract, owner lock / RF admin, the span ladder, the S-meter — and an icom_control that cannot carry a transmit field.
if node --no-warnings scripts/test_icom_radio.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the APP's gates on an Icom (SDRScreen / LocalHardwarePanel / MenuSheet / the WS client): every IQ control hidden, the radio's drawn.
if node scripts/check-icom-app-gates.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the app's OWN server screen (ServerModeScreen, Lite too): the five icom* keys = what VibeIcom.prepareStart reads, the
#     baud list = the setup page's, every IQ card hidden for an Icom, the Icom card drawn, nothing that transmits.
if node scripts/check-icom-servermode.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ LAN DISCOVERY (mdnsBrowse.ts): SEARCH AGAIN and the return from the background really restart the browse; stale rows go (Stuart 2026-10-20).
if node --no-warnings scripts/test_mdns_browse.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ PER-SERVER CERTIFICATE PINS (tlsPin.ts, GitHub #52): the key, the fingerprint, what a certificate says, and which
#     prompt — offer / "certificate has changed" / refuse (expired, public cert for another name) / nothing.
if node --no-warnings scripts/test_tls_pin.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ OWRX S-METER CALIBRATION (owrxSmeterCal.ts): the wire level is not dBm — owner's plugin figures, else noise at S2.
if node --no-warnings scripts/test_owrxSmeterCal.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ VIBESERVER SQUELCH ON/OFF (vibeSquelch.ts): a threshold below -100 dBFS is a threshold, not "off" — the
#     Airspy HF+ airband auto squelch that never closed (2026-10-14). Server half: vibeserver/test-squelch-msg.cpp.
if node --no-warnings scripts/test_vibeSquelch.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ WHAT A VFD CAN SHOW (faceplates §7). Every printable ASCII character is exactly one DSEG14
#    cell (its space is 200 wide and its ! is the BLANK cell), accents fold like a display ROM,
#    units keep their case, and a string that folds to nothing shows the frequency, never tofu.
if node --no-warnings scripts/test_faceplate_text.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE VCR MODE BOX (2026-10-06): every label the app composes fits the ten-cell field whole (WFM keeps the
#    rings' slot, every other mode gets those cells), and the "-88+88 dB F S" readout lights only electrodes it has.
if node --no-warnings scripts/test_faceplate_segfield.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ EVERY dB ON THE VCR HAS A LOWER-CASE d (2026-10-06, Stuart: "nitpickers will have us for it"): the VTS strip,
#    the DAB meter, the notices, the status row and its chips, the mode box — and no D in a word (DAB, BBC, AUDIO…) moves.
if node --no-warnings scripts/test_faceplate_vcr_db.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ EVERY BIG ON-SCREEN ELEMENT FOLLOWS THE MAIN DISPLAY'S FONT (2026-10-06): one source (faceplate.ts ScreenText),
#    every listed element reading it, Nixie One's glyphs and widths from its TTF, and the status row fitting in it.
if node --no-warnings scripts/test_faceplate_screenfont.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE DOT MODE BOX (2026-10-06): the same fields as true 5 × 7 dot-matrix cells — Doto's own dots (read out of the
#    TTF and checked against it), every label whole, the rings in dots, "S9+27 dBFS" / "kHz" fixed width.
if node --no-warnings scripts/test_faceplate_dotfield.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ NIXIE: real hardware never adds or removes a tube. Units change only the lit bulb, leading
#     zeros are switched off, and the TUBE shrinks to its window (smallest window first) — domes
#     are never clipped.
if node --no-warnings scripts/test_faceplate_nixie.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE METERS (faceplates §4): one deck height across meter × shared, the squelch ring on the SAME
#     table the LEDs light from, Φ((μ − T)/σ) with its σ floor, the eye filter as the only easing, the
#     steady-LED hysteresis, and the needle's 300 ms / 1 % ballistics — the maths, checked as data.
if node --no-warnings scripts/test_faceplate_meters.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE READOUT COMES FIRST, THE TUNING ROW IS NEVER SMALLER THAN THE MENU ROW (2026-10-16, iPhone 12): the
#     portrait deck at 320–430 pt and tablets, every chassis × meter × shared × TUNE (drums / keys / DAB MUX).
if node --no-warnings scripts/test_deck_readout_first.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ WORKLETS AS THE UI THREAD RUNS THEM (the 11 B7 LED VU crash): a default parameter that names a
#     module constant THROWS on the UI thread (the plugin unpacks captures inside the body), so every
#     worklet in src/ is compiled with the app's plugin and checked, and the meters' frame callbacks are
#     rebuilt from the plugin's output with no module scope and run — the test above cannot see it.
if node --no-warnings scripts/test_worklet_defaults.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ No photo leaves this repo carrying EXIF — GPS above all (2026-10-03: twelve did, two of them on the live website).
if node --no-warnings scripts/test_safe_text.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ Untrusted URLs (deep links, directories, a server's landing link) and the map page's literal/path guards.
if node --no-warnings scripts/test_safe_url.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The web client's `host` is an authority (+ /r/<id>) and nothing else (CodeQL #97-#99, 2026-10-05).
if node --no-warnings scripts/test_server_host.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The server screen's settings are read in ONE store call (Lite's "eternity" to load, 2026-10-06).
if node --no-warnings scripts/test_server_prefs.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ A gzipped spectrum-socket frame cannot inflate past its ceiling (gzip bomb).
if node --no-warnings scripts/test_bounded_inflate.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The map WebView page parses, for all three kinds (a template-literal page tsc cannot check).
if node scripts/check-map-overlay.mjs >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ node scripts/check-map-overlay.mjs"; fi
# ★★ The DAB transmitter-site importers (2026-10-06): coordinates, the three regulators' file shapes,
#    provenance in every generated header. Fixtures only — no network. (test-dab-txdb.cpp above is the matching.)
if python3 scripts/dab-sites/test_dabsites.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ python3 scripts/dab-sites/test_dabsites.py"; fi
# ★★ …and the panel says whose record each licensed site is (app + web share dabLicensedTail).
if node --no-warnings scripts/test_dab_licensed.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 ADS-B in the app: the report's dump1090 fields → the Aircraft the box AND the watch read; the per-radio
#    capability (no field = no control, false = disabled WITH the reason, never hidden). adsbTypes.ts.
if node --no-warnings scripts/test_adsb_report.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ Two rules, two readers each (2026-10-24): the ADS-B capture warning (app + web map bars) and the boosted-rate
#    guard's red line (app server screen + setup page) — the real functions on the same cases.
if node --no-warnings scripts/test_capture_and_rate_guard.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ …and the map's live list (adsbList.ts): row text, nearest first without shuffling, clear of the zoom stack.
if node --no-warnings scripts/test_adsb_list.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ ADS-B FURTHEST key: the web map and the app map page name the same aircraft (one rule, two readers).
if npx tsx scripts/test_adsb_furthest.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ …and V12 aircraft data on the clients (adsbInfo.ts): the adsb_info sanitiser + store, and the web list's rows with it.
if node --no-warnings scripts/test_adsb_info.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
echo "── image metadata (EXIF / GPS / XMP) ──"
if python3 scripts/strip-image-metadata.py --check; then pass=$((pass+1)); echo "  none"; else fail=$((fail+1)); echo "  ✗ run: python3 scripts/strip-image-metadata.py"; fi
# ★★★ A FACEPLATE CANNOT LOCK YOU OUT (faceplate.ts CRASH SAFETY): the mark is armed before a risky
#     faceplate draws and cleared after 5 s / on leaving the foreground; a launch that finds it comes up
#     on HYPER / BAR / DEFAULT with the choice kept aside — a crash, a clean exit, a swipe-away, a torn
#     mark and a refusing disk, across fake launches.
if node --no-warnings scripts/test_faceplate_safety.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE DRUM WELLS (faceplates §6): the default well is today's drum as data, the needle is gone on
#    every chassis, the aluminium notch pair inverts (and its draw order is a token), the LED pool's
#    geometry, the controls colour at the brief's brightness, and the tuner keys' 31 % / 34 % layout.
if node --no-warnings scripts/test_faceplate_wells.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE LANDSCAPE DECK (faceplates §9): never taller than today's band at 568 → 1366 pt for every
#     chassis × meter × shared, four equal keys, no negative or overlapping column, the drums' 80 pt,
#     real Nixie tubes that fit the window, and the SE's fall-backs (no labels; the bar).
if node --no-warnings scripts/test_faceplate_landscape.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE STEREO LIGHT IN DAB is the playing service's (its audio headers), never the FM pilot's — the
#    server's `stereo` and, for an older server, its codec line; app and web share the one function.
if node --no-warnings scripts/test_dab_stereo.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE DAB RECEPTION METER (2026-10-06): three bars + a sentence in place of the signal bar, judged on ~5 s
#     of the server's counters with hysteresis; thresholds held to the on-air cases (app + web share it).
if node --no-warnings scripts/test_dab_quality.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ An unchanged DAB service list keeps its array and objects across the once-a-second reports.
if node --no-warnings scripts/test_dab_share_services.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ DAB tuning keys: a burst of presses is ONE block change on the server (the one stopped on), and
#    leaving DAB drops the DAB station from the VTS (Stuart, 2026-10-05 — both).
if node --no-warnings scripts/test_dab_stepper.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ ANSWERS TO "Anyone know what this is?" (Stuart, 2026-10-09): none on the everyday pad, only the dial's band while a
#     question is open, every link a Signal Identification Wiki page, every DECODE a decoder we run, the watches agree.
if node --no-warnings scripts/test_dial_answers.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ ASK BEFORE TUNING, the clients' half (2026-10-21): the lock is READ from the dial message (an old server's silence
#     = not locked), the banner says what to do and never runs wider, and app + web gate every tuning send on it.
if node --no-warnings scripts/test_ask_before_tuning.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE CANNED CHAT IN EVERY LANGUAGE (2026-10-22): i18n/chat/*.json → the app, the browser and both watches through
#     scripts/gen-chat-phrases.mjs (stale output fails here); every language has every id and fits English's widths (the
#     banner's 28, the shared-tuner banner, the watch); the choice order picked > device > country > English; the menu; and
#     the server telling each listener only its own country.
if node --no-warnings scripts/test_chat_i18n.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ The room's passband on a shared dial (2026-10-17): the web client adopts it from config and sends nothing;
#     the app and Jr read the same fields. The server half is test-room-passband.cpp above.
if node --no-warnings scripts/test_room_passband.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ NO CLIENT SENDS BACK A DIAL IT WAS GIVEN (2026-10-22, the Lenovo: Safari tuned 657 → 693 and the V12 iPhone sent
#     every step back, "684 after 693"): the app's audio-socket tune door (audioTuneForward.ts) never forwards the
#     server's own dial, a person's change always goes (the drum included), and the web client follows silently.
if node --no-warnings scripts/test_dial_echo.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE ANTENNA SWITCH SETUP RULES (2026-10-09): what Lite's screen / the setup page send the server.
if node --no-warnings scripts/test_antenna_switch_cfg.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_antenna_bands_presets.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ A RADIO WITH ITS OWN PIN IS NEVER OPENED WITHOUT IT (2026-10-09, Buddy → the Pi 500's Airspy, blank screen).
if node --no-warnings scripts/test_radio_pin_paths.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ TUNE PACING ON THE CLIENT (tunePace.ts, Stuart 2026-10-05: "if the server's CPU is reporting that it is
#     struggling we need to slow down the amount of tune commands"): the health rung, the snail and the ping
#     set the gap; latest wins, the first tune goes at once, the last one always lands.
if node --no-warnings scripts/test_tune_pace.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_sprite_sizing.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_sprite_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ The waterfall jitter buffer's pooled frame copies (framePool.ts): independent copies, reuse, bounded.
if node --no-warnings scripts/test_frame_pool.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_platformCopy.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The diagnostics header's RC label (version.ts RELEASE_LABEL) matches About's newest release (2026-10-07).
if node --no-warnings scripts/test_release_label.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_grid.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node scripts/test_derived_values_pure.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE DECODER BOXES (faceplates §10.2): every text role ≥ 4.5:1 (WCAG luminance, composited over
#    what it sits on) in every chassis × controls colour × text colour × Transparency on/off; the
#    meaning colours never move, the text colour never reaches a box, no blur on silver / black or OFF.
if node --no-warnings scripts/test_decoder_contrast.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ TRANSPARENCY EFFECTS: each low-end signal alone turns the default OFF, a stored choice always
#     wins and the auto default is never saved as chosen, decoderBg migrates (solid = chosen OFF,
#     transparent = NOT chosen), OFF is alpha 1.0 exactly with no scrim or drop shadow, no darker/lighter than the glass on a
#     dark waterfall, and every file that draws a BlurView reads the switch.
if node --no-warnings scripts/test_transparency.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ FRAME RATE (power audit 2026-10-01): the 60 Hz row is hidden on a 60 Hz panel and when the binary
#     cannot say, labelled with the panel's real rate, defaults to no cap, survives the crash reset,
#     and both native modules export the two methods JS calls.
if node --no-warnings scripts/test_frame_rate.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ MAC VOLUME + MUTE (2026-10-01): shown only on iOS-on-a-Mac, unity gain everywhere else, MUTE is gain 0,
#     every native audio ingress plays through the one mixer that carries the gain, recordings are taken
#     before it, and the deck's speaker key shows a prohibition sign in the legend colour (never red).
if node --no-warnings scripts/test_mac_audio.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ POPUPS TAKE THE CHASSIS (faceplates §10.3): the default chassis is today's gold, value for value;
#     no popup holds a hard-coded gold; engraved text clears 4.5:1 on silver and black at the plate's
#     worst lighting; the tune entry follows the Display (Nixie One only ever neon); and with
#     Transparency OFF no popup dims the waterfall or blurs it.
if node --no-warnings scripts/test_popup.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ "USE AS SERVER" IN THE USER'S COLOURS (Stuart 2026-10-12): text = text colour, keys = controls colour, meanings
#     by the clash rule; every role ≥ 4.5:1 across controls × text × display and for Lite; and ServerModeScreen never
#     imports PopupShell / FaceplateContext (VibeServer Lite bundles it on RN 0.73 with no Reanimated).
if node --no-warnings scripts/test_server_colours.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_server_colours.ts"; fi
# ★★★ EDGE CHIPS ON THE GLASS (edgeChipGeometry.ts, Stuart 2026-10-06): the health / time cards and their tabs sit at
#     the PHYSICAL edge with content padded by the safe-area inset — portrait, both landscapes, iPad, a Mac resize.
if node --no-warnings scripts/test_edge_chip.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_edge_chip.ts"; fi
# ★★★ THE CANNED CHAT PAD (chatPad.ts, capSheen.ts): the phrase pad scrolls inside a cap that never
#     overflows the drawer on any phone (the SE lost phrases 7–14 below its edge), and a dome key's
#     sheen is flex shares, never percentage heights (Yoga drew ghost slabs over a multi-line wrap).
if node --no-warnings scripts/test_chat_pad.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The FM-DX screen grows its dial + logo into a big window; a phone-sized window keeps today's layout exactly.
if node --no-warnings scripts/test_fmdx_layout.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_fmdx_layout.ts"; fi
if node --no-warnings scripts/test_wefax_align.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wefax_align.ts"; fi
if node --no-warnings scripts/test_wefax_crisp.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wefax_crisp.ts"; fi
if node --no-warnings scripts/test_wefax_lost.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wefax_lost.ts"; fi
# ★★ NAVTEX MESSAGES (src/utils/navtex.ts, 2026-10-05): ZCZC…NNNN blocks from the text stream with a damaged header
#    or trailer, [start lost] / [end lost], one PREV like WEFAX, every chunk size.
if node --no-warnings scripts/test_navtex.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_navtex.ts"; fi
# ★★ NAVTEX STATIONS (V12 beta 4): scripts/navtex-stations/navtex-stations.json (NGA Pub. 117) → the clients' and the
#    server's readers through scripts/gen-navtex-stations.mjs — stale output fails here. The rule's cases are in
#    test_navtex.ts above and vibeserver/test-bgdecode.cpp.
if node --no-warnings scripts/test_gallery_pager.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_gallery_pager.ts"; fi
# ★ The gallery's TRANSLATE (2026-10-23): which lines are sent, the source language and its hint, the layout kept.
if node --no-warnings scripts/test_gallery_translate.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_gallery_translate.ts"; fi
if node --no-warnings scripts/test_admin_bg.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_admin_bg.ts"; fi
if node scripts/gen-navtex-stations.mjs --check >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/gen-navtex-stations.mjs --check (stale — run it)"; fi
if bash scripts/test-rtty-auto.sh >/dev/null 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test-rtty-auto.sh"; fi
if node --no-warnings scripts/test_tune_hint.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_tune_hint.ts"; fi
# ★★★ A REOPENED SPECTRUM SOCKET CARRIES THE LISTENER'S OWN VFO BACK on a per-listener dial, and NEVER on a
#     shared one (2026-10-05: the RSP on a locked range reset to its landing on every resume from background).
if node --no-warnings scripts/test_reopenTune.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_reopenTune.ts"; fi
# ★ The directory Worker against real SQLite: dead-address pages, and the 90-day retention (PRIVACY.md, Play form).
for t in directory/scripts/test-gone.mjs directory/scripts/test-retention.mjs; do
  if node --no-warnings "$t" >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ $t"; fi
done
if node --no-warnings scripts/test_rtty_spec.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_rtty_spec.ts"; fi
# ★★ NAVTEX on audio (audit 2026-10-04): impulse bursts + selective fading, CER against the old decoder's numbers, and
#    the BEL control byte never in the output.
if bash scripts/test-navtex.sh >/dev/null 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test-navtex.sh"; fi
# ★★★ SHARE A STATION (canned chat, app + web): a bookmark's LABEL never reaches the payload, the server's
#     relayed line is what is drawn, and TUNE asks first on a shared dial somebody else is on. The server half
#     (validation, naming, the line itself) is vibeserver/test-chat-share.cpp, run with the C++ tests above.
if node --no-warnings scripts/test_chat_share.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ HIDE THIS USER (chatHide.ts, 2026-10-05): a hidden sender's lines go, past and new; own and system lines
#     never; the name is matched exactly; session only, client-side only, never in canned mode.
if node --no-warnings scripts/test_chat_hide.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE SMALL-SCREEN STATUS ROW (faceplates §8.2): dropped strictly in order (IF first, the recording
#     timer last), SHARED TUNER shortens before it goes, the connection meter never goes, portrait never
#     drops, the row packs before anything drops, and a 1 pt wobble cannot flap (hysteresis).
if node --no-warnings scripts/test_faceplate_status.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ FACEPLATE LIGHTING + VFD GLASS (briefs/BRIEF-lighting-and-vfd-glass.md): the filament wires (count by
#    height, device-pixel snapped), the one light angle (LEFT = today exactly), MOTION EFFECTS, LIGHT ANGLE.
if node --no-warnings scripts/test_faceplate_lighting.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE BLIND TUNES OBEY THE SHARED DIAL (blindTuneGate.ts): lock-screen / headset / car ⏮⏭, a car
#     pick and Siri are refused on a shared dial with others listening (and on spectator / listen-only),
#     as FM-DX's always were; alone or exclusive they work; the native switch and a press agree.
if node --no-warnings scripts/test_blindTuneGate.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ WEB WATERFALL ROW PACING (wfPacing.ts): one whole row every N display frames, N fixed — Default on
#    a Retina 60 Hz panel is a steady 30 rows/s (it flipped 1 <-> 2 on the 1.5 knife-edge: Stuart's
#    "sticky" Safari waterfall, 2026-10-20), never two rows in a frame, Sharp on a cadence too.
if node --no-warnings scripts/test_wf_pacing.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE ROW TARGET FOLLOWS THE FEED (src/assets/wfRowTarget.ts, Stuart 10-20): Default min(20, 3x), Smooth min(60, 6x),
#    snapped to a divisor of 60 — the app, the web client and the native engine (wf_row_target.h) share the rule.
if node --no-warnings scripts/test_wf_row_target.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE 10-FPS DEFAULT (2026-10-20): the serving phone's tiers read Smoother · 20 / Default · 10 / Half · 5, the stored
#    keys still mean 20 / 10 / 5, and a new server starts on Default · 10.
if node --no-warnings scripts/test_fps_tiers.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ "IS THIS ACTUALLY NEWS?" (renderChurn.ts): a repeated RDS label / bookmark list keeps the old
#    object so the radio screen does not re-render for it, any drawn change still gets through, and
#    the controls-bar clock re-renders on the minute, not every second (power audit, 2026-10-01).
if node --no-warnings scripts/test_renderChurn.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ CONNECTION REFRESH (connectionRefresh.ts): every backend the radio screen hosts gets one, the
#    audio it restarts matches the screen's own mount gates (native Opus after re-registration, the
#    VibeServer/dongle pump, the OWRX/Kiwi adapter), and the row is hidden wherever it is a no-op.
if node --no-warnings scripts/test_connectionRefresh.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE VTS STATION LINE (vtsLine.ts), app AND web: "PI: C363 / Name: RadioText" with no dangling
#    punctuation for a missing part, DAB's SId never called a PI, the coloured runs identical to the
#    plain line, and the web pill dropping band → RDS mark → flag only as far as its measured widths
#    demand (2026-10-01).
if node --no-warnings scripts/test_vtsLine.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE SHARED CHUNK CACHE (chunkCache.ts / chunkVerify.ts / directory store.html): a stored map or
#     admin chunk runs ONLY if it hashes to this server's own page; the build's embedded hash is the
#     served file's (web/dist AND vibe_web_page.h); server B loads from the store with zero fetches; a
#     tampered entry is refused and refetched; the store answers only *.vibeserver.vibesdr.net. Needs
#     web/dist (node scripts/build-web.mjs).
if node --no-warnings scripts/test_chunk_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE DECODER SOCKET ONLY WHILE SOMETHING NEEDS IT (2026-10-10, "light everywhere — light on CPU on client and server
#     and light on data"): web and app open /ws/dxcluster on a decoder / spots / chat and close it 8 s after the last.
if npx tsx scripts/test_decoder_socket_idle.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE DAB REPORT IN PIECES (2026-10-10): 74 % of a DAB listener's traffic was the report — the station list resent
#     twice a second. The reassembler's rules (ageing, the tuned row at 0, legacy pass-through, never another mux's list).
if npx tsx scripts/test_dab_assemble.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ THE L/R DEVIATION METERS' display rules (2026-10-10): thresholds, never a false zero — app and web share them.
if npx tsx scripts/test_lr_meter.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ LITE'S "CHECK FOR UPDATES" (2026-10-10): which release is newer — rc order, final above rc, never a test build,
#    never a downgrade.
if npx tsx scripts/test_lite_update.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ RECORDINGS BY SERVER, THEN NEWEST FIRST (2026-10-10): the receiver you are on first, others A–Z, no-server last.
if npx tsx scripts/test_recording_groups.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ PORTABLE WI-FI wording shared by the app and the web page (never through the tunnel; "leave this session…").
if npx tsx scripts/test_pocket_connection.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ GAMEPAD (Stuart's mapping, 2026-10-10): the highlight's direction picking and the stick-rotation jog wheel.
if npx tsx scripts/test_gamepad_nav.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE CONTROLLER GUIDE (2026-10-19): the shared drawing data, its words against the scheme, the layout rules.
if npx tsx scripts/test_pad_guide.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE CONTROLLER KEYBOARD ON THE WEB PAGE (decided 2026-10-20): its promises read from its source (padKeyboard.ts /
#    gamepad.ts). The shared layouts and hand-over rule (test_kb_layouts / test_kb_handover) run once, with the app's below.
if npx tsx scripts/test_web_pad_keyboard.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE KEYBOARD, ONE SCHEME (Stuart, 2026-10-18): arrows/WASD move, Enter select, Backspace/Escape back, < > tune, - + zoom.
if npx tsx scripts/test_key_actions.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ A PANEL'S HIGHLIGHT: PanelNav's movement rule, and the frequency card drawn as a grid (V12 485: the frequency box
#    hid after the units, where no arrow went).
if npx tsx scripts/test_nav_move.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ GAME CONTROLLERS IN THE APP (V12): Stuart's two profiles onto the keyboard's own keys, HOLD START, the jog wheels.
if npx tsx scripts/test_pad_actions.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE RADIO CHOOSER BY PAD (controller audit §1, 2026-10-20): its focus order, where the highlight starts, and Enter
#     on a blocked radio moving to the box that would open it.
if npx tsx scripts/test_radio_chooser_nav.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ ONE OVERLAY RULE (controller audit, 2026-10-20): the top surface owns the keys, Back peels one layer, arrows never
#     reach the screen beneath, tune passes, a raised tour stays on top, BrowserOverlay keeps Back.
if npx tsx scripts/test_overlay_stack.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ appAlert (controller audit, 2026-10-20): button order = focus order, never starting on a destructive key, Back = cancel.
if npx tsx scripts/test_app_alert.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ NAV COVERAGE (controller audit, 2026-10-20): a touchable on a pad-driven surface with no nav registration fails
#     once its file passes its baseline — the next silent skip fails here instead of on Stuart's sofa.
if npx tsx scripts/test_nav_coverage.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE SERVER LIST'S FOCUS RULES (controller audit group H): Back from the list position, Enter = tap on directory rows.
if npx tsx scripts/test_picker_nav.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 489: the picker's focus order under a REAL React renderer — a bailed-out render emptied the render-time bank
#     (src/utils/renderBank.ts), so the ring, reveal and Enter disagreed. Real key bus, real pad mapper, both enters.
if npx tsx scripts/test_render_bank.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ CONTROLLER KEYBOARD (brief, decided 2026-10-20): the last input source — touch / keyboard / pad — set centrally.
if npx tsx scripts/test_input_source.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ …its layouts (every key reachable, ↵ / Back / ⌫ on every layout, wide keys, wrap), its editing rules (one point,
#     kHz↔MHz, PIN digits, caret, caps, never logged) and the live hand-over between it and the system keyboard.
if npx tsx scripts/test_kb_layouts.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_controller_keyboard.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_kb_handover.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_kb_fonts.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE BOOKMARK NAME, PRE-FILLED (decided 10-20): RDS name, then station name, then the frequency.
if npx tsx scripts/test_bookmark_name.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE REAL SERVER, END TO END (B6): per-listener decoders on a locked range, the decoder limit's
#     refusal, Advanced RDS only to whoever asked on a shared dial, and an audio socket that opens
#     first keeping its codec — through the same WebSockets the clients use, against fake-rtl-tcp.
#  ★ It needs a vibeserver BUILT FROM THIS TREE (VIBESERVER_BIN=…). Without one it is reported as
#    NOT RUN — counted separately, never as a pass: a stale binary would test yesterday's server.
printf '\n\033[1m── server decoders (end to end) ──\033[0m\n'
node scripts/test-server-decoders.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ DOWNLOAD DIAGNOSTICS on the admin API (2026-10-20): admin only, an attachment a phone saves, and the daemon's own
#     scrub of the admin password / PINs / directory key. Same VIBESERVER_BIN rule.
printf '\n\033[1m── download diagnostics (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-admin-diagnostics.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ AUDIO FIRST (2026-10-20, docs/BRIEF-portable-wifi-realtime.md Phase 2): a listener whose audio backs up gets a
#     ~2 fps waterfall until it has been clear for ~1 s; another listener never notices; an encoder burst is not a
#     backlog. The backed-up half needs a kernel that honours a small receive buffer (Linux) — elsewhere it says NOT RUN.
#     Same VIBESERVER_BIN rule.
printf '\n\033[1m── audio first: the waterfall yields (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-audio-first.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE THREAD LADDER ON THE RUNNING SERVER (2026-10-20, Pi 500: socket writers at nice 0 under a -19 DSP was the
#     finding). Read from /proc: audio send ≥ DSP > waterfall send ≥ spectrum; then one busy loop per core at -19 and
#     the audio must keep its rate. Linux only; the negative rungs and the load half need CAP_SYS_NICE (NOT RUN
#     without). Same VIBESERVER_BIN rule.
printf '\n\033[1m── thread ladder: writers above the DSP (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-thread-rank.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A RUNNING DECODER KEEPS THE AUDIO CHAIN (2026-10-07, Pi 500 Airspy: the listener's sockets blipped, the
#     chain idled, and the WEFAX socket got nothing for 8 s). Decoder socket alone on a shared dial with
#     --idle-grace 0: lines keep coming, no idle and no park, presence unchanged, and it idles once the
#     decoder stops. Same VIBESERVER_BIN rule.
# ★★★ ONE CANNED VOCABULARY IN FIVE PLACES (2026-10-08, nine social phrases): server, app, web and both watches list
#     the same ids, and a live shared dial carries every one and drops an unknown one. Same VIBESERVER_BIN rule.
printf '\n\033[1m── shared-dial chat phrases (end to end) ──\033[0m\n'
node scripts/test-chat-phrases.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ ASK BEFORE TUNING end to end (2026-10-21): the server refuses a locked listener's tune at the socket and tells an
#     old client why, a YES / no answer in 15 s lets them in, a NO holds. ~40 s, silent. Same VIBESERVER_BIN rule.
printf '\n\033[1m── ask before tuning (end to end) ──\033[0m\n'
node scripts/test-ask-before-tuning.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ BACKGROUND DECODING + THE GALLERY end to end (V12, 2026-10-22, NickB's NAVTEX): the last listener leaves → after the
#     (test-shortened) wait the radio goes to the profile centre and its decoders run; a profile boundary switches; a
#     listener arriving stops it at once and lands on the dial the radio was LEFT on; the gallery over HTTP (path
#     traversal refused); the config API round-trips profiles and refuses overlaps / an out-of-window VFO. ~45 s, silent.
printf '\n\033[1m── background decoding + the gallery (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bg-decode.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ …AND ON A MULTI-RADIO BOX (Lenovo, 12.0.0~beta2): a save made through the FRONT DOOR reaches the radio's own
#     process (it watches config.json), the park/release is held off while a profile is due, an unrelated save restarts
#     nothing, switching it off gives the dial back. Front door + one radio process, as systemd runs them. ~40 s.
printf '\n\033[1m── background decoding through the front door (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bg-decode-frontdoor.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ LISTENERS FIRST ACROSS THE BOX (Stuart, 2026-10-22): two radio processes, 3 slots; radio A's background holds them all;
#     a listener on radio B starts a decoder at once and exactly A's lowest VFO pauses; it resumes when the listener goes.
printf '\n\033[1m── background decoding gives way to listeners, lowest VFO first (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bg-decode-slots.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ IN RANGE WHILE LISTENERS ARE ON, AND LOCKED RADIOS (Stuart, 2026-10-22): a listener on 648 kHz keeps Nick's NAVTEX VFOs
#     decoding without moving anything; tuning away pauses them; the last VFO gives way to a listener's decoder; switched
#     off it stops as before; a locked radio is never moved, starts at once, gives up user slots to real listeners. ~60 s.
printf '\n\033[1m── background decoding in range while listeners are on, and on locked radios (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bg-decode-inrange.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE ADMIN PAGE'S BACKGROUND-DECODING LINE (V12 beta 4): waiting → running with MEASURED CPU per VFO and for the
#    channeliser → carrying on with a listener → a VFO paused for a listener's decoder, and why → paused; an FT8 VFO's
#    live spot count; none of it without the admin password. ~40 s. The wording: scripts/test_admin_bg.ts.
# ★★ THE GALLERY IN PAGES (V12 beta 4): gallery.json offset/limit + the filter's total, an old request unchanged; the
#    REAL built page: 25 a page, the pager, the filter kept / reset, BACK to the same page, scroll and row. ~25 s, silent.
printf '\n\033[1m── the gallery in pages (end to end, headless) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-gallery-pages.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── background decoding: the admin line (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bg-decode-admin.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ …AND SHARED BY TIME WITH "LET ANOTHER PROGRAM USE THIS RADIO" (Stuart, 2026-10-22): outside the profile hours the
#     release works as set; at the start a released radio is taken back; never released during them; at the end the
#     dial back and the release after Let go after; a radio another program holds is retried every 60 s. ~2 min.
printf '\n\033[1m── background decoding and the release share by time (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bg-decode-release.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A TUNNEL WHOSE PROCESS LIVES BUT WHOSE ADDRESS IS DEAD IS REPLACED (2026-10-09, the Sony "online but not
#     responding all day"): a stand-in cloudflared prints a hostname that does not exist and stays alive; the server
#     must fetch its own address, see it dead, replace the tunnel and tell a loopback stand-in directory the new one.
# ★★★ THE EXTERNAL ANTENNA SWITCH (2026-10-09): the real server against a fake MQTT broker playing a Tasmota board —
#     antennas in hwinfo, read-back, break before make, per-band preset, the owner's lock, the admin-only search.
printf '\n\033[1m── the external antenna switch (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-antenna-switch.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ BULK BOOKMARK IMPORT (2026-10-09, the 1,478-row UK ATC list took minutes one POST at a time): POST /bookmarks/import
#    lands a batch with ONE save, keeps mode + passband, is admin-only from the LAN, and an older server's EMPTY 400 is
#    the app's fall-back signal. Same VIBESERVER_BIN rule.
printf '\n\033[1m── bulk bookmark import (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bookmark-import.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A BOOKMARK NAME SURVIVES A RESTART UNCHANGED (2026-10-19): the loader took names RAW while the writer escapes them,
#     so a `\` doubled on every save until the Sony's Lite could not get past loading its list. Round trip, a bloated
#     file loading promptly and trimmed, a 16 MB+ file set aside. Same VIBESERVER_BIN rule.
printf '\n\033[1m── bookmark names across restarts (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bookmark-escape.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ AN IDLE DECODER SOCKET IS PINGED (2026-10-10): behind Cloudflare's ~100 s idle cut-off the /ws/dxcluster socket
#    reconnected for ever ("an error log as long as my arm"). ~30 s. Same VIBESERVER_BIN rule.
# ★★★ …and end to end on the real server and the bench multiplex: a legacy socket unchanged, a dab=2 socket's
#     reassembled report EQUAL to it, a fraction of the bytes, scopes on/off, resync, a new session. Needs the bench clip
#     (make-clip.sh) as well as VIBESERVER_BIN — NOT RUN without either.
printf '\n\033[1m── the DAB report in pieces (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dab-report-pieces.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A LONG SESSION COSTS NO MORE THAN A SHORT ONE (2026-10-19): Stuart's app read 93-103 KB/s in DAB after 23 minutes
#     of tuning, 45 after a restart — an Advanced RDS subscription made on FM kept every rdsx coming in DAB. rdsx only
#     while the listener is on WFM; per-type bytes flat after 12 multiplexes, DAB ↔ FM, a joiner, the Signal pane.
#     Same rule: VIBESERVER_BIN + the bench clip, NOT RUN without either. ~2 minutes.
printf '\n\033[1m── a long session stays flat on the wire (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-wire-session-flat.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ AN OFF-AIR PICTURE LOADS ONCE (2026-10-19): ETag + 304 on dabslide / dablogoair / dabmot, ?h= immutable, and in a
#     real MUTED browser the station list (rebuilt twice a second) fetches the picture once and never again after a
#     reload. Same rule: VIBESERVER_BIN + the bench clip (the browser part also wants Edge). ~40 s.
printf '\n\033[1m── DAB pictures: once, then from cache (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dab-slide-cache.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A CLIENT VANISHING MID RAW-IQ STREAM MUST NOT WEDGE THE RADIO (2026-10-10, the Pi 500's V4L froze: the close path
#     stopped the full-rate stream under clientMtx, and its applyAutoIf() took clientMtx again). Same VIBESERVER_BIN rule.
printf '\n\033[1m── raw IQ: a vanished client does not wedge the radio (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-iq-close-deadlock.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ ONE .local NAME ON A PORTABLE (2026-10-25): the server hands its name to the root service (avahi publishes it) and
#     runs no responder of its own there; anywhere else its responder runs as before.
printf '\n\033[1m── pocket: one .local name, through avahi (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-pocket-mdns.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ DUAL-STACK (2026-10-25, Nick's Orange Pi One was reachable by name on IPv6 only): an IPv4 and an IPv6 client on
#     one listener, each classified (local / lan) and logged exactly as an IPv4 client always was.
printf '\n\033[1m── dual-stack: IPv4 and IPv6 clients, classified and logged (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dualstack-clients.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ PORTABLE WI-FI (2026-10-10): on a pocket box, on its own network, the menu offers Network | Own Wi-Fi with the
#     link's figures, the weak-Wi-Fi notice appears, Switch asks first and writes the kick; through loopback, nothing.
printf '\n\033[1m── pocket: an added Airspy HF+ is offered its own rate, and Save finishes (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-pocket-added-radio.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m── game controllers on the served page (simulated pad, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-gamepad.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ DAB (Stuart, 2026-10-18): the decoder box works as in the app — one stop; enter (or DAB's first press) hands it the
#    D-pad, the list walks and scrolls, ✕ picks, ○ gives it back. The bench multiplex through the fake radio.
printf '\n\033[1m── a game controller in DAB on the served page (simulated pad, bench multiplex) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-gamepad-dab.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ HAMMERING THE MULTIPLEX BUTTON TUNES ONCE (Stuart, 2026-10-21: "rapidly tune down to 7D … dont tune the SDR just
#     hold it and tune on final move"; "I tried to rapid tune the Pi2 earlier and it kept snapping back"): the server
#     holds a raw socket's per-press `dab` steps latest-wins (a lone press at once), and the served page sends one
#     `dab` per burst with no snap-back. Same VIBESERVER_BIN rule; the browser half wants Edge. ~40 s.
printf '\n\033[1m── DAB: a burst of multiplex presses is one tune (server + served page, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dab-burst.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE SHARED-DIAL CONTRACT, END TO END (2026-10-22, the Lenovo echo): one listener walks the dial; the app's
#     audio-socket door with the OLD rule echoes it (reproduced from the server's own log), with the NEW rule sends
#     nothing, the served web page sends nothing, and a person on the app still tunes (a step and straight back
#     included). Same VIBESERVER_BIN rule; the web page half wants Edge. ~45 s.
printf '\n\033[1m── shared dial: nobody echoes another listener’s tune (server + app rule + served page, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --no-warnings scripts/test-dial-echo.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ ADMIN (Stuart, 2026-10-18: "in the admin menu there is no way to navigate it with the controller"): the highlight goes
#    into the panel and stays there, carries on after the tables redraw, and ○ closes it.
printf '\n\033[1m── a game controller in the admin panel (simulated pad, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-gamepad-admin.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 ICOM, THE WEB CLIENT (icomUi.ts): the real server on the fake IC-7300, a muted headless browser on this tree's
#     web/dist — the panel from the radio's lists, every IQ control hidden, the owner lock for a listener, the admin's
#     mode/FIL/span/zoom/ATT reaching the fake, the radio's S-meter, the TX banner, phone width and the looks.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-icom.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE KEYBOARD, ONE SCHEME (Stuart, 2026-10-18): arrows/WASD move the highlight, Enter, Backspace/Escape back,
#     < > , . tune, - + = zoom; typing in the frequency field never hijacked; the old shortcuts do nothing.
printf '\n\033[1m── the keyboard on the served page (real key events, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-keyboard.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE CONTROLLER GUIDE (Stuart approved the prototype 2026-10-19): SELECT opens and closes it, back closes it, the
#     D-pad steps the tabs, it opens on the layout in use and follows HOLD START, the menu key opens it, and the layout
#     follows the window (lines when wide, a list when narrow).
printf '\n\033[1m── the controller guide on the served page (simulated pad, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-pad-guide.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE CONTROLLER KEYBOARD (decided 2026-10-20): only for a pad that is being USED; open by pad, type, submit; touch,
#     physical-key and pad hand-overs keep the text and caret; pad back lets go of a box; password mode; both profiles.
printf '\n\033[1m── the controller keyboard on the served page (simulated pad, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-pad-keyboard.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ FREQUENCY AT THE POINTER (Kiko, 2026-10-16, "like OpenWebRX"): a MOUSE over the waterfall/spectrum shows the
#     frequency a click there tunes to (and the click agrees), hides on leave/press, never on touch; the menu switch
#     turns it off and is remembered; the switch is not offered on a touch-only device.
printf '\n\033[1m── frequency at the pointer on the served page (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-pointer-frequency.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m── portable Wi-Fi on the served page (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-pocket-connection-web.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m── idle decoder socket keep-alive (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dx-keepalive.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m── a dead tunnel address is replaced (end to end) ──\033[0m\n'
node scripts/test-tunnel-selfcheck.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ "START THE TUNNEL AGAIN AFTER A REBOOT" (Stuart, 2026-10-16): settled + written at start (on everywhere, off on a
#    Portable, the Portable's old dirResume carried over); OFF unlists after a REBOOT, never after a settings restart.
printf '\n\033[1m── the tunnel after a reboot (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-tunnel-resume.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m── a decoder alone keeps the audio chain (end to end) ──\033[0m\n'
node scripts/test-server-decoder-alone.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A POCKETED APP KEEPS ITS CHANNEL (Stuart, 2026-10-05: "why did the full socket drop when minimised
#     though, especially as I had audio and a decoder running"): on a per-VFO radio, closing ONLY the
#     spectrum socket keeps audio and the decoder running; the returning socket adopts the same channel
#     (its first config is its own VFO); closing everything leaves the memo. Same VIBESERVER_BIN rule.
printf '\n\033[1m── per-VFO channel survives the background (end to end) ──\033[0m\n'
node scripts/test-server-vfo-background.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A RADIO SET TO START IN DAB STARTS IN DAB (Stuart, 2026-10-05: "No server seems to honour the start
#     in DAB mode"): the owner's admin arrival first on a shared dial, a client's restored tune refused, a
#     stranger joining, the exemption standing after the start, and a no-landing negative. Same VIBESERVER_BIN rule.
printf '\n\033[1m── DAB landing (end to end) ──\033[0m\n'
node scripts/test-server-dab-landing.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ V12 ADS-B AS THE LANDING + KEEP IT RUNNING (Stuart, 2026-10-24: "a new connection instantly sees a list of planes
#     and a full map"): in ADS-B with aircraft before anybody connects, a client's list within ~1 s, EXIT to the plain
#     landing, back into ADS-B when they leave; keep-running off lands the first client; background decoding gives way;
#     a radio that cannot run ADS-B falls back. Needs VIBESERVER_BIN and VIBE_ADSB_CLIP (exit 3 = not run).
printf '\n\033[1m── ADS-B landing + keep running (end to end) ──\033[0m\n'
node scripts/test-server-adsb-landing.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ V12 ISM MODE (docs/v12/ISM-DECODER.md): the real server on a fake rtl_tcp replaying a synthetic 433 MHz clip made by
#     test-ism --write-clip — the capability, the device list and its readable lines, the privacy rule ON THE WIRE, band
#     steps (clamped), an exact frequency, EXIT giving the dial back; then the web client in headless Edge (--mute-audio).
#     Needs VIBESERVER_BIN (exit 3 = not run); the web half also needs Edge.
printf '\n\033[1m── ISM mode (end to end: server, then the web client) ──\033[0m\n'
if [ -x "$OUT/test-ism" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-ism" --write-clip "$OUT/ism-clip.u8" 434130000 2400000 >/dev/null
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-server-ism.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-web-ism.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 ISM IN THE GALLERY (2026-10-25): a listener's session writes the day's log + card (nothing while it ran, the
  #    list's own lines, no ID anywhere), then an ISM background profile runs with nobody here and gives way to a listener;
  #    then the web gallery's ISM chip in headless Edge at desktop and 390 px.
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-server-ism-gallery.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-web-ism-gallery.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/ism-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-ism build'; notrun=$((notrun+1))
fi

# ★★★ V12 AIS + APRS (docs/v12/AIS-APRS.md): the real server on a fake rtl_tcp replaying synthetic clips made by
#     test-ais / test-aprs --write-clip — AIS mode (both channels, the ship list, a track, the dial given back, the gallery)
#     and the APRS decoder on a listener's NFM (stations, weather, an object, a message, the gallery); then the web client
#     in headless Edge (--mute-audio) at desktop and 390 px. Needs VIBESERVER_BIN (exit 3 = not run); the web half Edge.
printf '\n\033[1m── AIS mode + the APRS decoder (end to end: server, then the web client) ──\033[0m\n'
if [ -x "$OUT/test-ais" ] && [ -x "$OUT/test-aprs" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-ais" --write-clip "$OUT/ais-clip.cu8" 2400000 2>/dev/null
  "$OUT/test-aprs" --write-clip "$OUT/aprs-clip.cu8" 2400000 2>/dev/null
  VIBE_AIS_CLIP="$OUT/ais-clip.cu8" VIBE_APRS_CLIP="$OUT/aprs-clip.cu8" node scripts/test-server-ais-aprs.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_AIS_CLIP="$OUT/ais-clip.cu8" VIBE_APRS_CLIP="$OUT/aprs-clip.cu8" node scripts/test-web-ais-aprs.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/ais-clip.cu8" "$OUT/aprs-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-ais / test-aprs builds'; notrun=$((notrun+1))
fi
# ★ The ship/station layer's generated copies (web client, app, /mapgl/vendor) match web/mapkit/tracklayer.js.
if node scripts/gen-tracklayer-source.mjs --check; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ V12 CW (2026-10-25): the real server on a fake rtl_tcp replaying the bench's synthetic HF clip (five stations around
#     7.030 MHz): the skimmer on a one-pipeline radio and a range-locked one, a second listener's own segment, ONE SIGNAL's
#     text and its spot's RF (test-server-cw); then the web client in headless Edge (--mute-audio) — the CW button, the
#     skimmer's list and spots, a tap to tune, ONE SIGNAL, 390 px, the gallery's CW chip (test-web-cw). The engine's own
#     numbers are vibeserver/test-cw.cpp above; the measuring bench is scripts/test-cw.sh. Needs VIBESERVER_BIN.
printf '\n\033[1m── CW (end to end: server, then the web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && scripts/test-cw.sh clip "$OUT/cw-clip.cu8" >/dev/null; then
  VIBE_CW_CLIP="$OUT/cw-clip.cu8" node scripts/test-server-cw.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_CW_CLIP="$OUT/cw-clip.cu8" node scripts/test-web-cw.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/cw-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and the bench to build: scripts/test-cw.sh)'; notrun=$((notrun+1))
fi

# ★★ THE RATE PIN AND THE SHARED-DIAL RATE RULE BIND LISTENERS, NOT THE SIGNED-IN ADMIN (Stuart, 2026-10-24: "sample rate
#    change isnt working … I am in admin mode"). Same VIBESERVER_BIN rule.
printf '\n\033[1m── rate pin: listener refused, admin allowed (end to end) ──\033[0m\n'
node scripts/test-rate-pin-admin.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ EXIT DAB STICKS (Stuart, 2026-10-05 23:06, Pi 2: the box "popped up again … over the MW signal"): no
#     `dab` report after `dab_off` over many cycles, none in reply to a service pick out of DAB, no re-entry,
#     and the log names who asked. Same VIBESERVER_BIN rule.
printf '\n\033[1m── DAB exit (end to end) ──\033[0m\n'
node scripts/test-server-dab-exit.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A STOP THAT LANDS ON A DAB START STILL STOPS (Pi 500, 2026-10-16: a settings save, a listener asking for
#     DAB in the same second, "Stopping…" and then 90 s until systemd's SIGKILL): a rate change held open across
#     the SIGTERM, late DAB/NFM/de-emphasis requests, a plain stop's speed, and the shutdown watchdog naming the
#     step a wedged stop was on. Same VIBESERVER_BIN rule.
printf '\n\033[1m── stop during a DAB start (end to end) ──\033[0m\n'
node scripts/test-server-stop-during-dab.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A RADIO THAT FALLS OVER AT A RATE A MODE BOOSTED IT TO (vibe_rate_guard.h, 2026-10-24): a fake rtl_tcp that stops
#     at 2.4 MS/s — ADS-B left, the radio back at its base rate, the switch persisted off; then a planted crash marker
#     found at start, recorded, cleared. Same VIBESERVER_BIN rule. ~30 s.
printf '\n\033[1m── boosted-rate stall and crash (end to end) ──\033[0m\n'
node scripts/test-rate-guard.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ SHARE A STATION THROUGH THE REAL SERVER: two listeners on a shared dial; the line the room receives is
#     named from the receiver's own store, a smuggled label is not relayed, out-of-range / closed-mode shares
#     are refused with a reason, flood control covers shares. Same VIBESERVER_BIN rule: not run without one.
printf '\n\033[1m── tune burst (end to end) ──\033[0m\n'
# ★★★ A BURST OF TUNES LANDS ON ITS LAST FREQUENCY (vibe_tune_pace.h, 2026-10-05 — the Pi 2 in the garage):
#     50 tunes in a second, the same 50 as one clump, a mode switch inside a clump, and a loaded server's
#     hold timer landing the trailing tune on its own. Same VIBESERVER_BIN rule: not run without one.
node scripts/test-server-tune-burst.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m── chat share (end to end) ──\033[0m\n'
node scripts/test-server-chat-share.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE WEB CLIENT, AS THE SERVER HANDS IT OUT: the page and every /vs/ script, in every encoding,
#     byte for byte against web/dist, `immutable` on the scripts and no-store on the page, through a
#     /r/<id>/ prefix as well. Same VIBESERVER_BIN rule as above: not run without one.
printf '\n\033[1m── web client serving (end to end) ──\033[0m\n'
node scripts/test-web-serving.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A PERMANENT SCROLLBAR NEVER COVERS OR SQUEEZES A PANEL'S CONTROLS (Stuart, 2026-10-01: Windows /
#     Edge keeps the bar on screen). Every panel at 390 and 1280 px with a classic 15 px bar forced on:
#     the gutter is reserved, nothing scrolls sideways, every control ends left of the bar. Headless
#     Edge, --mute-audio, throwaway profile. Same VIBESERVER_BIN rule.
printf '\n\033[1m── web panels: the scrollbar lane (real browser) ──\033[0m\n'
node scripts/test-web-scrollbar-lane.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ THE SHARED CHUNK CACHE IN A REAL BROWSER, ACROSS REAL SERVERS: two vibeservers behind a local
#     https front as a/b/c.vibeserver.vibesdr.net, this tree's store.html as the directory's. Server B
#     opens the map and the admin panel with ZERO requests for either; a tampered IndexedDB entry is
#     not run on C. Headless Edge, --mute-audio, throwaway profile. Same VIBESERVER_BIN rule.
printf '\n\033[1m── shared chunk cache (real browser) ──\033[0m\n'
node scripts/test-web-chunk-cache.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A LISTENER WHOSE TURN RAN OUT, BACK ON A FREE RADIO (B10, Kiko's server): ended → refused inside
#     the cooldown → after it, admitted on BORROWED time and kept until somebody else wants the radio →
#     handed over with notice when somebody does. Real server, through the LAN address (loopback is
#     exempt). ★ ~4 minutes (a 1-minute turn + the 2-minute cooldown), so only with VIBESERVER_SLOW=1;
#     test-session-turns.cpp above covers the same rule on a synthetic clock in milliseconds.
printf '\n\033[1m── session turn: borrowed time after the cooldown (end to end) ──\033[0m\n'
if [ "${VIBESERVER_SLOW:-0}" = "1" ]; then
  node scripts/test-session-turn.mjs; rc=$?
else
  echo '   not run — ~4 minutes; set VIBESERVER_SLOW=1 (test-session-turns covers the rule quickly)'; rc=3
fi
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ AN OPEN DECODER KEEPS ITS LISTENER'S TURN (2026-10-04, Stuart's minimised + muted WEFAX on the Airspy came
#     back to a FRESH 30 minutes). Shared dial, hard 1-minute limit: decoder-only listener ended at the end of the
#     ORIGINAL turn, decoder refused inside the cooldown. ~80 s, so only with VIBESERVER_SLOW=1.
printf '\n\033[1m── session turn: an open decoder is present (end to end) ──\033[0m\n'
if [ "${VIBESERVER_SLOW:-0}" = "1" ]; then
  node scripts/test-session-decoder-presence.mjs; rc=$?
else
  echo '   not run — ~80 s; set VIBESERVER_SLOW=1'; rc=3
fi
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ MODERN ROOTS FOR OLD PHONES (B10: a Lite on Android 5.1 could not fetch RIPE/APNIC, so Europe had
#     no flags). VibeTls.kt itself, on the desktop JVM, against the real chains, with an old store
#     simulated — and still refusing expired / self-signed / untrusted / wrong-host. Needs the network.
printf '\n\033[1m── TLS roots for old Android (VibeTls) ──\033[0m\n'
bash scripts/test-tls-roots.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ PER-SERVER CERTIFICATE PINS, NATIVE (GitHub #52): the app's own VibeTlsCore.m (Mac, Security.framework) and
#     VibePinnedTls.kt (desktop JVM, real OkHttp) against self-signed servers on 127.0.0.1 — refused without a pin and
#     the server never sees the request; the pin accepted for THAT host:port only; changed / expired refused; forget.
printf '\n\033[1m── certificate pins, Apple (VibeTlsCore) ──\033[0m\n'
bash scripts/test-tls-pin-apple.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── certificate pins, Android (VibePinnedTls) ──\033[0m\n'
bash scripts/test-tls-pin-android.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ ONE VERSION, EVERYWHERE IT IS WRITTEN DOWN. app.json does NOT reach the iOS build — the
#     pbxproj owns MARKETING_VERSION and only `expo prebuild` would copy it across, which this
#     project deliberately never runs — so the App Store shipped 10.2 while the app's own About
#     overlay, its User-Agent and the Android build all said 10.3. Caught only because a build was
#     inspected by hand before submitting.
if node scripts/check-versions.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build, %d not run\033[0m\n' "$pass" "$fail" "$broke" "$notrun"
[ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]
