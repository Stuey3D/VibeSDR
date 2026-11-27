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
# ★ V12 digital spots: every mode engine in decoders/digimodes/ (the builds glob it) goes wherever digi_modes.cpp goes.
DIGIMODES="$(ls android/app/src/main/cpp/decoders/digimodes/*.cpp 2>/dev/null | tr '\n' ' ')"
# ★ V12 DRM: FAAD2's DRM build, where this machine has one (a release's deps prefix via VIBE_DEPS, else an installed copy) —
#   without it test-drm-faad reports NOT RUN rather than passing.
FAAD_FLAGS=""; FAAD_LIB=""   # ★ the library goes AFTER the source (deps_for) — a static libfaad_drm.a before it links nothing on Linux
for d in "${VIBE_DEPS:-/nonexistent}" /opt/homebrew/opt/faad2 /usr/local /usr; do
  if [ -f "$d/include/neaacdec.h" ] && ls "$d"/lib/libfaad_drm.* >/dev/null 2>&1; then
    FAAD_FLAGS="-DVIBE_HAVE_FAAD2 -I $d/include -L $d/lib -Wl,-rpath,$d/lib"; FAAD_LIB="-lfaad_drm"; break
  fi
done
# ★ V12 DAB+: FAAD2's PLAIN build, prefixed (libfaad_dab.a from build-deps) — from a release's deps prefix or the Mac's;
#   without it test-dab-faad reports NOT RUN. Its inputs are real off-air unit logs that live only in
#   ~/VibeSDR-test-audio/dab/logs (other people's broadcasts) — none there, NOT RUN as well.
FAAD_DAB_FLAGS=""; FAAD_DAB_LIB=""
for d in "${VIBE_DEPS:-/nonexistent}" vibeserver/mac/deps/prefix; do
  if [ -f "$d/lib/libfaad_dab.a" ]; then
    inc="$d/include"; [ -f "$inc/neaacdec.h" ] || inc=/opt/homebrew/opt/faad2/include
    FAAD_DAB_FLAGS="-DVIBE_HAVE_FAAD2_DAB -I $inc -I android/app/src/main/cpp/vibedsp"; FAAD_DAB_LIB="$d/lib/libfaad_dab.a"; break
  fi
done
# ★ V12 xHE-AAC: libxaac (libxaacdec.a from build-deps — a release's deps prefix or the Mac's); without it
#   test-drm-xaac reports NOT RUN. Its inputs: ~/VibeSDR-test-audio/usac (conformance + usac/xhe, README.txt there).
XAAC_FLAGS=""; XAAC_LIB=""
for d in "${VIBE_DEPS:-/nonexistent}" vibeserver/mac/deps/prefix; do
  if [ -f "$d/lib/libxaacdec.a" ]; then
    XAAC_FLAGS="-DVIBE_HAVE_LIBXAAC -DVIBE_LIBXAAC_VERSION=\"dev\""; XAAC_LIB="$d/lib/libxaacdec.a"; break
  fi
done

# ★ V12 FreeDV RADE V1: the receiver (librade_vibe.a) and the tests' transmitter (librade_vibe_tx.a) from build-rade.sh — a
#   release's deps prefix, VIBE_RADE_PREFIX, or the Mac's deps prefix. Without them test-freedv runs its codec2 half only
#   (and says the RADE half is NOT RUN). The transmitter's archive goes FIRST: it takes its end-of-over bits from the receiver's.
RADE_FLAGS=""; RADE_LIBS=""
for d in "${VIBE_DEPS:-/nonexistent}" "${VIBE_RADE_PREFIX:-/nonexistent}" vibeserver/mac/deps/prefix; do
  if [ -f "$d/lib/librade_vibe.a" ] && [ -f "$d/lib/librade_vibe_tx.a" ]; then
    RADE_FLAGS="-DVIBE_HAVE_RADE -DVIBE_HAVE_RADE_TX -I $d/include"; RADE_LIBS="$d/lib/librade_vibe_tx.a $d/lib/librade_vibe.a"; break
  fi
done

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
    test-gain-step|test-rsp-vibeagc) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★ V12 THE IDLE DX SCANNER: the log's rules (header-only) and its FM hop — several RDS stations from one wide
    #    capture, decoded across worker threads (vibe_dxscan_fm.h). Optimised: ~25 s of 2.4 MS/s capture is synthesised.
    test-dxscan)    echo "-O1" ;;
    test-dxscan-fm) echo "-O2 -I $VDSP -I $KISS -lpthread" ;;
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
    # ★★★ V12 raw IQ out's own wide tap (docs/v12/IQOUT-WIDE.md): tones in/out of band at every rate, exact sample count,
    #     the listener's audio bit-identical on/off, and the old-vs-new cost. Optimised: ~40 runs of 20 s are synthesised.
    test-iqout-wide)       echo "-O2 -I $VDSP -I $KISS" ;;
    test-watch|test-watch-cost) echo "-O2 -I $VDSP -I $KISS -lpthread" ;;   # ★ V12 VFO A/B (vibe_watch.h)
    test-am-sync)          echo "-O2 -I $VDSP -I $KISS" ;;   # ★ V12 SAM / ECSS / AM stereo / AM auto bandwidth (amsync.cpp)
    test-auto-bw)          echo "-O2 -I $VDSP -I $KISS" ;;
    test-aprs-onair)       echo "-O2 -I $VDSP -I $KISS -lpthread" ;;   # ★ APRS off air through the real NFM chain (docs/v12/APRS-ON-AIR-2.md)   # ★★ V12 SSB / CW auto bandwidth (autobw.cpp) through the real pipeline
    # ★★ V12 FreeDV as a mode (vibe_freedv.h): codec2's own transmitter → noise → our modem host and the real USB chain.
    #    NOT RUN (exit 3) without libcodec2 (apt install libcodec2-1.2 / brew install codec2) — it is dlopen'd, never linked.
    test-freedv)           echo "-O2 -I $VDSP -I $KISS $RADE_FLAGS -lpthread $( [ "$(uname)" = Linux ] && echo -ldl)" ;;
    # ★★ V12 M17 as a mode (vibe_m17_fec.h, vibe_m17.h): our own encoder from the spec → RRC 4-FSK → noise → our receiver, and
    #    the real RxPipeline in NFM. The voice checks are NOT RUN (exit 3) without libcodec2 — the FEC, packets and callsigns
    #    still run and must pass.
    test-m17)              echo "-O2 -I $VDSP -I $KISS -lpthread $( [ "$(uname)" = Linux ] && echo -ldl)" ;;
    # ★★ V12 DV signalling (vibe_dvsig*.h — DMR, D-STAR, YSF, NXDN, dPMR identities, never voice): the specs' vectors, the
    #    correlator kernels SIMD = scalar, our transmitter for each system → FM → noise → our receiver, minutes of noise and
    #    analogue FM speech with nothing logged, the two report copies, the listener thread, CPU. `clip` writes the e2e clip.
    test-dvsig)            echo "-O2 -I $VDSP -lpthread" ;;
    # ★★★ Every chain delivers exactly 48 000 audio samples per second of capture (2026-10-07: the RSPs
    #     ran 32-34 ppm short through a rounded, approximated resampler ratio). Optimised: 21 s of input per chain.
    test-resampler-rate)   echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★★ FirDecimator/RealFir against the direct convolution, sample for sample (2026-10-18) — on x86
    #     the SSE2 aligned tap banks, on ARM the NEON kernel. The server's own flags for vibedsp.
    test-fir-simd)         echo "-O3 -ffp-contract=fast -I $VDSP" ;;
    # ★ The real decoders on real signals (RTTY, WEFAX, an encoded FT8 slot) — optimised so FT8's
    #   slot decode finishes in a second rather than ten.
    test-sat-skim) echo "-O2 -I android/app/src/main/cpp/ft8_lib -lpthread" ;;   # ★★★ beta97 the satellite digital-mode skimmer (decoders/sat_skim.h) behind the finder's mixer
    test-decoder-hosts|test-dec-dump|test-shared-decoders|test-sat-ft4) echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;   # ★ test-dec-dump: the decoder capture + replay (docs/v12/APRS-ON-AIR-2.md)
    # ★ V12 METEOR LRPT (beta91): optimised as ctest builds it (-O3 there) — unoptimised the suite took 42 s, and the
    #   live-pass cases (a 35 s fade through the server's Run, twice) would double that.
    test-lrpt)             echo "-O2" ;;
    # ★★★ V12 PAGING's gate (vibe_paging_gate.h): built in, as the desktop VibeServer now is for the R36S — and still dormant
    #     outside the Android app / the handheld's radio process.
    test-paging-gate)      echo "-O2 -DVIBE_PAGING=1 -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★ V12 DIGITAL SPOTS (2026-10-26): the mode registry, the band table and stepping, Band mode's tuner, a synthetic
    #    20 m slice (FT8 + FT4 on their own dials) decoded end to end, the lower sideband rejected, noise → no spot.
    test-digi-spots)       echo "-O2 -I $VDSP -I android/app/src/main/cpp/ft8_lib" ;;
    # ★★ V12 Band spots through the SERVER'S route: the channeliser slice (vibe_wide_tap.h / the per-listener extract) — tones at
    #    every mode's passband edges on every band at five capture rates, and the slice the right way up (2026-10-27).
    test-band-slice)       echo "-O2 -I $VDSP -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★ V12 Band spots and the dongle's DC (vibe_band_slice.h bandDcCentre, 2026-10-27): where the DC fell on every band,
    #    and that the placement keeps it off every mode and out of every slice. Needs the registry (the engines link in).
    test-band-dc)          echo "-O2 -I $VDSP -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★ V12 a FREE VFO and the tuner's DC (vibe_vfo_dc.h vfoDcZone/vfoDcCentre, 2026-10-27): the zone per mode, never
    #    stricter than the locked VFO, one retune per crossing, the limits and VFO B's zone honoured. Header-only, no deps.
    test-vfo-dc)           echo "-O2" ;;
    # ★★ BACKGROUND DECODING + THE GALLERY (V12, 2026-10-22): the real DSP chain AND the real decoders, end to end.
    # ★★ …and the RTTY skimmer (V12 2026-11-14, test-rtty-skimmer): the engine and "rttyskim" through a DecoderHost.
    test-bgdecode|test-navtex-quality|test-lrpt-host|test-lrpt-geo|test-fctlm-gallery|test-bg-priority|test-bg-heavy|test-bg-band|test-bg-skim|test-cw|test-dsc|test-sitora|test-psk|test-jtty|test-rtty-skimmer) echo "-O2 -I $VDSP -I $KISS -I android/app/src/main/cpp/ft8_lib" ;;
    # ★★ V12 2026-10-27: …and the NDB skimmer as a background VFO, beside NAVTEX on one capture (ndb_synth.h's beacons)
    test-bg-ndb)           echo "-O2 -I $VDSP -I $KISS -I android/app/src/main/cpp/ft8_lib -I tools/ndb-bench -lpthread" ;;
    test-decfeed-resampler) echo "-O2 -I android/app/src/main/cpp" ;;
    test-bm-names) echo "-O2 -I android/app/src/main/cpp" ;;
    # ★ The benchmark's decoder rows — optimised like the server, or the costs are not the server's.
    # ★★ …and every row after the first eight (vibe_benchmark_more.h, 2026-10-31): the host's decoders, every digital-spots
    #    engine, NDB / AIS / ISM, tone ID and the audio filters. VIBE_BENCH_CLIP=<decoders-bench.vbu8> runs the clip rows.
    test-bench-decoders)   echo "-O2 -DVIBE_DAB_PFFFT -I android/app/src/main/cpp/ft8_lib -I $VDSP -I $KISS $RADE_FLAGS -lpthread $( [ "$(uname)" = Linux ] && echo -ldl)" ;;   # ★ PFFFT as the server builds it (ISM/LoRa DetFft)
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
    # ★★ FT8's clock hunt + spot DT (2026-11-14, the R36S 4.1 s slow) — the live decoder over a real recording.
    test-ft8-hunt)         echo "-O2 -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★★ FT8/FT4/FT2 sensitivity (2026-10-26): the second look, OSD pairs, a-priori decoding, their guards, noise
    #     with every candidate tried — optimised, it decodes ~400 slots.
    test-ftx-sensitivity)  echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★★ JS8 (Normal/Fast/Turbo/Slow) on the FT8 core: its LDPC(174,87), frames, every submode on a synthetic band,
    #    noise, the live decoder's four slots (2026-10-26) — optimised, it decodes ~60 slots.
    test-js8)              echo "-O2 -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★ FT2 (FtxMode::FT2 of the FT8/FT4 decoder): threshold, clock range, noise, the 3.75 s live slots (2026-10-26).
    test-ft2)              echo "-O2 -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★ V12 Q65, our own (docs/v12/DECODERS-WAVE2.md): the frame against the shipped encoder's vectors, sensitivity, noise.
    test-q65)              echo "-O2 -I android/app/src/main/cpp/ft8_lib -I tools/q65-bench -lpthread" ;;
    test-fst4)             echo "-O2 -I android/app/src/main/cpp/ft8_lib -I $VDSP -I tools/fst4-bench -lpthread" ;;
    test-msk144)           echo "-O2 -I android/app/src/main/cpp/ft8_lib -I $VDSP -I tools/msk144-bench -lpthread" ;;
    # ★★ V12 ADS-B (vibe_adsb.h): CRC, fields and CPR on the published worked examples, then the demodulator on
    #    frames modulated into noisy IQ at every capture rate a radio runs — optimised, it demodulates ~40 s of IQ.
    test-adsb)             echo "-O2" ;;
    # ★ V12 Mode S Comm-B (vibe_adsb_commb.h): the Riddle's BDS examples, then inference through the Tracker — ADS-B
    #   cross-checks, second-reply confirmation, data parity, the 1,7 veto, ACAS RA, 2,0 naming, random-field false rate.
    test-adsb-commb)       echo "-O2" ;;
    # ★ V12 beta94 the ADS-B RANGE OUTLINE (vibe_adsb_range.h): bins, the rolling window, the record, the file, reset, a move.
    test-adsb-range)       echo "-O2" ;;
    # ★★ b96-rangefeed the outline counts the shared table's other ADS-B (vibe_adsb_range_feed.h): the rule on rows, then a
    #    synthetic Beast feed → the FeedSet → the table → the follower → /adsb/range's bins. 127.0.0.1 only. ~13 s.
    test-adsb-range-feed)  echo "-O2 -pthread" ;;
    # ★ V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS (vibe_beast.h): Beast round trip, settings, the local server and a push
    #   feed against 127.0.0.1 test sockets only (never a real aggregator), the service's sink end to end.
    test-adsb-feed)        echo "-O2 -pthread" ;;
    # ★★ V12 beta96 SERVER-LEVEL AIRCRAFT FEEDS (vibe_feeds.h, vibe_sbs.h): BaseStation parsing (dumphfdl's line + junk),
    #    labels (HFDL never ADS-B), a LISTEN source, two sources merged in the shared table, the banner, and the hub as a
    #    middle man (each reply once, byte-faithful, HFDL never forwarded, the loop guard, own-only). 127.0.0.1 only. ~12 s.
    test-aircraft-feeds)   echo "-O2 -pthread" ;;
    # ★★ beta96 follow-up: TEST A FEED BEFORE SAVING, AUTO-DETECT and the UberSDR HFDL source (vibe_feed_test.h, vibe_uberhfdl.h;
    #    docs/v12/FEED-TEST.md): fake Beast / BaseStation / UberSDR servers on 127.0.0.1, the captured fixture. ~25 s.
    test-feed-test)        echo "-O2 -pthread" ;;
    # ★★ V12 beta96 THE SHARED AIRCRAFT TABLE AS DELTAS (vibe_aircraft_delta.h) + the standalone map's viewer tickets
    #    (vibe_airsea_viewers.h): rows rebuilt field for field, cursors, the 150-aircraft bytes (full / gzip / delta), the gate.
    test-aircraft-delta)   echo "-O2 -pthread" ;;
    # ★★ V12 beta96 the ADS-B DELTA protocol's encoder (vibe_adsb_delta.h): quantising, the header walk, ids, keyframes, the
    #    150-aircraft synthetic sky's bytes and cost. scripts/test_adsb_delta.ts runs its --stream against the clients' store.
    test-adsb-delta)       echo "-O2" ;;
    # ★ V12 TDF / ALS162 (decoders/tdf_phase.h): synthetic phase code through the TIME decoder — noise, drift, timing, CPU.
    #   `test-tdf --wav <file> [start_utc]` replays an UberSDR recording (docs/v12/TIME-TDF.md).
    test-tdf)              echo "-O2" ;;
    # ★ V12 the NCDXF / IARU beacon tracker (decoders/ncdxf_tracker.h, vibe_ncdxf_run.h): synthetic CW at the published
    #   schedule — steps vs SNR, noise, the callsign, clock, pitch, QSB, the grid, the host's run. `--wav` replays a recording.
    test-ncdxf)            echo "-O2" ;;
    # ★ V12 balloons (decoders/balloon_*.h): UKHAS RTTY, Horus Binary, SSDV — our transmitter into our receiver, false-positive
    #   minutes, SIMD = scalar, cost. `test-balloon wav <file> [rate]` replays a recording; `clip` writes the e2e clip.
    test-balloon)          echo "-O2" ;;
    # ★ V12 tone decoders (decoders/tones_*.h): DTMF (Q.23/Q.24) and 5-tone selcall — our transmitter into our receiver, every
    #   digit and table, twist/timing/frequency limits, minutes of noise / speech-like / music-like audio with nothing decoded,
    #   SIMD = scalar, cost. `test-tones wav <file>…` runs recordings as a talk-off corpus (exit 1 when anything decodes).
    test-tones)            echo "-O2" ;;
    # ★★ V12 VOR / ILS (docs/v12/VOR-ILS.md): 40 radials × 4 channel rates, 20 dB, offsets, voice; ILS DDM; idents; 5 min of
    #    noise and of AM speech → nothing; SIMD = scalar; cost. `test-vor clip …` writes the e2e clip. Header-only.
    test-vor)              echo "-O2" ;;
    # ★★ V12 VOR / ILS MAP MODE (vibe_vormap_service.h, docs/v12/VOR-MAP.md): the window plan (RTL 5, RSP 2, under 1 MS/s none);
    #    one window with every channel decoded at once at 2.4 and 8 MS/s (four VORs, two on adjacent channels, and a
    #    localiser: radials ±2°, idents, the listed one named, nothing on the empty channels); SIMD = scalar; the sweep finds
    #    a beacon only in window 3; fresh → faded after a sweep; ★ beta94 one read + a matching radial names it (the same-channel
    #    trap, wrap-around, too close / too far). ~15 s.
    test-vormap)           echo "-O2 -lpthread -I $VDSP -I $KISS" ;;
    # ★★★ beta94 THE TRANSPONDER FINDER (vibe_xpdr.h, vibe_xpdr_run.h — synthetic IQ: voices, CW, the follow, the engine's follow)
    test-xpdr)             echo "-O2 -I $VDSP" ;;
    # ★★ V12 COSPAS-SARSAT 406 MHz (docs/v12/SARSAT.md): C/S T.001 Annex B's worked examples, the BCH codes, the protocols,
    #    the modem down to 34 dB-Hz, an hour of noise and beacon-shaped random bursts → nothing. `clip …` writes the e2e clip.
    test-sarsat)           echo "-O2" ;;
    test-horus-names)      echo "-O1 -lpthread" ;;   # ★ V12 Horus payload names: the list's format, once a day, bounds
    # ★★ The ADS-B gain rule after Stuart's FlightAware box: the top, down only for dynamic range, never for strong aircraft.
    test-adsb-gain)        echo "-O2 -lpthread" ;;
    # ★★ ADS-B's sample supply (vibe_adsb_capture.h — the live capture %), the boosted-rate stall/crash guard
    #    (vibe_rate_guard.h) and the benchmark's ADS-B row on a fake source (vibe_benchmark_adsb.h, which pulls in the
    #    benchmark's headers). ~12 s: two parts feed in real time, as the server does.
    test-adsb-capture)     echo "-O2 -I $VDSP -I $KISS -I android/app/src/main/cpp/ft8_lib -lpthread" ;;
    # ★★ V12 aircraft data: builder/reader/enrichment/logo zip installer on vibeserver/testdata-adsb (VIBE_ADSB_RAW
    #    = a folder with the real downloads also checks known aircraft and routes — scripts/build-adsb-data.sh)
    test-adsb-data)        echo "-O2 -lpthread" ;;
    # ★★★ the updater's detached thread while exit() tears the statics down (the Pi 2's settings-save SIGSEGV, 10-26)
    test-adsb-data-exit)   echo "-O2 -lpthread" ;;
    # ★★ V12 ISM (vibe_ism*.h): every protocol on its source's worked examples, then encoded from the published specs into
    #    noisy IQ at 1–10 MS/s through the real front end; drift, SNR, pure noise, the privacy rule on the JSON, a CPU row.
    test-ism)              echo "-O2 -lpthread" ;;
    # ★★ beta94 ISM's unidentified bursts (vibe_ism_unknown.h): OOK PWM/PPM/Manchester and FSK described right, a steady
    #    carrier told from a burst, the descriptions bounded and sample-free, the service's report.
    test-ism-unknown)      echo "-O2 -lpthread" ;;
    test-ais|test-aprs|test-ais-store|test-ais-binary|test-ax25hs) echo "-O2 -lpthread" ;;
    # ★★ V12 beta90 THE STATION STORE (vibe_station_store.h — the DIGI MODES MAP + PSK Reporter): validation, RAM, the day
    #    files, four processes flushing one day (fork), windows, the caps and pruning, subscribe(), cost. Header-only.
    test-station-store) echo "-O2 -lpthread" ;;
    # ★★ V12 beta92 WHICH RADIO HEARD IT (vibe_log_radio.h + the gallery's "ri"): the registry, two processes (fork) on one
    #    gallery with two radio ids, a shared item listing both, listJson radio= (a view), old lines = unknown. ~0.1 s.
    test-log-radio)    echo "-O2 -lpthread" ;;
    # ★★ V12 beta92 ONE DAY ITEM PER SERVER (vibe_gallery_shared.h): two forked processes / two radios write each decoder's
    #    day item — one item, the message both heard once, both radios in "ri" (AIS/APRS DayLog, DSC, NDB). ~0.2 s.
    test-shared-day)   echo "-O2 -lpthread" ;;
    # ★★ V12 beta92 THE FALSE-DECODE FILTER (vibe_callsign_entity.h, docs/v12/DIGI-MAP-FILTER.md): the ITU series table, 166
    #    real call + grid pairs (every continent, islands, Antarctic bases, /MM /P), Stuart's Pi 500 false decodes, cost.
    test-callsign-entity) echo "-O2" ;;
    # ★★★ V12 beta92 A GALLERY WEFAX CHART IS FINISHED AS THE CLIENT DRAWS IT (vibe_wefax_finish.h — the port of wefaxAlign.ts +
    #    wefaxCrisp.ts): parity with the TypeScript on ~90 charts (scripts/wefax-finish-golden.ts, the world corpus; needs node,
    #    else that half is NOT RUN), white paper, margin to the edge, a flat page untouched, cost. ~1 min.
    test-wefax-finish) echo "-O2" ;;
    # ★★ V12 ISM IN THE GALLERY (vibe_ism_gallery.h, 2026-10-25): the day's log (the line, the RAM buffer, the fold, parts),
    #    the summary card, one file per UTC day per band, a restart, the privacy rule in every file, the ISM profile config.
    test-ism-gallery)      echo "-O2 -lpthread" ;;
    # ★★★ beta96 maps2 THE DAY's SONDES AND VOR STATIONS (vibe_posgal.h): one card per kind per UTC day, never a write per report,
    #     the 10-field table, newest position / first heard / highest kept, a restart, midnight, the 300-row bound. Fast.
    test-posgal)           echo "-O2 -lpthread" ;;
    # ★★★ THE GALLERY'S STORE at any size (vibe_gallery.h, 2026-11-01): the owner's SIZE is the limit (no 5,000 cap), the
    #     index counted inside it, per-kind counts, oldest-first eviction fair between kinds, append-only + compaction,
    #     two processes on one folder, the 1,000,000 backstop. ~2 s.
    test-gallery-store)    echo "-O2 -lpthread" ;;
    # ★★ V12 R36S handheld (handheld_card.h, the gallery's CardLayout): card names in UTC, FAT-safe, " (2)" on a clash,
    #    every write temp + rename (no .part left; the sweep removes a cut-off one), the JSON reader, the gallery's items
    #    on the card with the index off it, traversal refused, and the layout OFF = the server's flat gallery. ~1 s.
    test-handheld-card)    echo "-O2 -lpthread" ;;
    # ★★ V12 NDB skimmer (decoders/ndb_skimmer.h, vibe_ndb_*.h): the vote, one beacon at +10/-5 dB on both tones and A1A,
    #    -10 dB by the fold, 5 and 12 WPM, 5 min of noise, the published list (Woodley, ≈, candidates, Find NDB), the
    #    service's report and a harmonic, the gallery's card and map and a restart. ~40 s.
    test-ndb|test-ndb-suspect|test-community) echo "-O2 -lpthread -I tools/ndb-bench -I $VDSP -I $KISS" ;;
    # ★★ V12 the one-time NDB card clean-up (vibe_ndb_gallery_clean.h, 2026-11-04): all-dot / all-dash unlisted idents set
    #    aside on REAL cards (vibeserver/testdata-ndb-gallery), counts / furthest / best / titles recomputed, the marker,
    #    an open card skipped, the day's writer keeping the section after a restart. Fast.
    test-ndb-gallery-clean) echo "-O2 -lpthread" ;;
    # ★★ V12 DRM (decoders/drm/, docs/v12/DRM.md): the standard's tables, the coding layer, the test transmitter through annex
    #    B's channels in every robustness mode (every frame byte-checked), ±1.2 kHz / ±30 ppm, 10 min of noise + an AM signal =
    #    nothing, the audio framing, a CPU row. VIBE_TEST_AUDIO adds RRI off air (drm/). Optimised: ~2 min.
    test-drm|test-drm-signal|test-drm-constellation) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★ DRM's AAC through FAAD2 on the real RRI recording: decodes, programme, concealment without clicks or gaps, CPU.
    test-drm-xaac)         echo "-O2 -I android/app/src/main/cpp -I $VDSP $XAAC_FLAGS" ;;
    test-drm-faad)         echo "-O2 -I $VDSP -I $KISS $FAAD_FLAGS $XAAC_FLAGS $( [ "$(uname)" = Darwin ] && echo "-framework AudioToolbox -framework CoreFoundation")" ;;
    # ★ DAB+ through FAAD2 (vibe_dab_faad.h) on REAL unit logs; a 960 reference only where a developer's ffmpeg is installed.
    # ★ V12: FAAD2 on its own thread (vibe-faad2) — same audio as inline, a full ring drops (concealed) never waits,
    #   resets mid-queue, the service's decoder is FAAD2. Same deps as test-dab-faad.
    test-dab-faad|test-dab-faad-queue|test-dab-tii-air) echo "-O3 -ffp-contract=fast $FAAD_DAB_FLAGS $( [ "$(uname)" = Darwin ] && echo "-framework AudioToolbox -framework CoreFoundation")" ;;
    # ★★ V12 WSPR (decoders/digimodes/wspr_decoder.h): protocol, synthetic slots −20…−28 dB with drift and DT, hashed calls, pure
    #    noise, subtraction, the live decoder — optimised, it decodes ~50 two-minute slots. VIBE_TEST_AUDIO adds 40 m + 20 m
    #    off air, with the WSPRnet-scale SNR held to ±1 dB (mean) / ±3 dB (each) of Stuart's UberSDR's own reports.
    test-wspr)             echo "-O2 -lpthread" ;;
    # ★★ V12 WIRELESS M-BUS METERS (vibe_wmbus.h, 2026-10-26): CRC, fields, 3-out-of-6, formats A/B, headers, records,
    #    the line, the interval; T1 / C1-A / C1-B / S1 through the real ISM front end at 1-2.4 MS/s with drift, a
    #    sensitivity sweep, noise and carriers = nothing, the privacy rule (serial yes, ciphertext never), the day-log
    #    throttle, a busy street. VIBE_WMBUS_REC=<868.6 MHz u8 at 2.4 MS/s> also replays a real capture (0 meters).
    test-wmbus)            echo "-O2 -lpthread" ;;
    # ★★ V12 LoRa (vibe_lora*.h, vibe_meshtastic.h, vibe_meshcore.h, vibe_lorawan.h, 2026-10-27): AES/SHA/HMAC vectors, the
    #    codec, 96 round trips through the channeliser, the sensitivity sweep against Semtech's table (~1 min; LORA_QUICK=1
    #    skips it), the whole 868 band at once, the parsers, the privacy rule on the JSON, noise → nothing, CPU rows.
    test-lora)             echo "-O2 -lpthread" ;;
    # ★★ V12 the ISM map (2026-11-01): positions parsed, the node table's cap / 24 h expiry / newest wins, the privacy rule
    #    (a person's node rounded to ~1 km), the gallery's "stations received" card. Fast (no DSP).
    test-lora-nodes)       echo "-O2 -lpthread" ;;
    # ★ V12 LoRa-APRS, FANET, MeshCom (vibe_lora_ham.h): payload vectors from each published description, then modulated into
    #   noisy IQ through the real engine on the 433 / 868 plans; the privacy rule, the owner's per-network switches, noise.
    test-lora-ham)         echo "-O2 -lpthread" ;;
    # ★★ V12 the NAVTEX map (2026-11-01): gallery titles read back, the stations placed by the label's own resolver, the
    #    time of day / history / message text where it could not decide, the fold, the real fixture set. Fast.
    test-navtex-map)       echo "-O2 -lpthread" ;;
    # ★★ V12 the NAVTEX map's warnings (2026-11-01): strict positions (garbled ones refused, LAT/LONG, Greek hemispheres),
    #    the multilingual hazard lexicon, series / cancellations / windows, one marker per warning, the index. Fast.
    test-navtex-warn)      echo "-O2 -lpthread" ;;
    # ★★ V12 the shipping map kept built in the background (vibe_navtex_warm.h, 2026-11-20): the CPU gate, the slice, the
    #    worker pausing and resuming, and the cost of the backfill in warm-up slices over 3 000 synthetic messages (~15 s).
    test-navtex-warm)      echo "-O2 -lpthread" ;;
    # ★★ V12 the NAVTEX map's met labels (vibe_navtex_met.h, 2026-11-01): headings, the first period only, wind / gale words
    #    and their negations / sea state from words or wave height / visibility; native gale words set only the flag; the
    #    latest per area; then every forecast of the fixture set. Fast.
    test-navtex-met)       echo "-O2 -lpthread" ;;
    # ★★ V12 AUTO-PPM (vibe_afc.h, 2026-11-12): the reading → clock error, minSamples / spread / outliers / regroup, the
    #    step cap and interval, DRM only corroborating, the loop on a simulated warming radio with latency (no reversals),
    #    the DAB/DRM gates, the hardware/digital split, the glide, the per-radio store. Fast.
    test-afc)              echo "-O2" ;;
    # ★★ V12 beta79 the R820T's tuning step out of the capture (test-pll-iq): a tone through the modelled tune, the capture
    #    cleaner, the one mix pass and a listener's channel lands within 1 Hz; DC; phase across a retune; the gate; the cost.
    test-pll-iq)           echo "-O2" ;;
    # ★ V12 aviation: -O2 — each test times its receiver against real time (CPU per capture second).
    test-acars|test-acars-apps|test-vdl2|test-hfdl|test-avi-service|test-aircraft-share) echo "-O2 -lpthread" ;;
    test-maritime-combo)   echo "-O2 -lpthread" ;;   # ★ beta96 AIS + DSC ch 70 from one wide capture (vibe_maritime_combo.h)
    # ★ V12 RS41 radiosondes (docs/v12/RS41.md): -O2, the receiver timed against real time.
    test-rs41|test-rs41-service|test-sondes-types) echo "-O2 -lpthread" ;;
    # ★ V12 satellite engine (docs/v12/SATELLITE-ENGINE.md): SGP4 vectors, passes vs Skyfield, the catalogue, the lock.
    test-sat-orbit|test-sat-engine|test-sat-labels|test-sat-if|test-sat-observer|test-sat-offer) echo "-O2 -lpthread" ;;   # ★ beta91 test-sat-labels: the waterfall's satellite labels · beta94 test-sat-offer: "sat" in the published modes
    # ★ V12 beta90 FUNcube BPSK 1200 telemetry (vibe_bpsk_rx.h + vibe_fctlm.h): FEC, noise (0 frames), the whole receiver
    #   from our own transmitter, SIMD = scalar, the values; ★★★ §10 the REAL signal (test/fixtures/funcube/, AO-73 on air
    #   2026-11-24, 23.5 s of the Pi 500 V4L decoder input — it decoded nothing live). -O2: ~4 s.
    test-fctlm)            echo "-O2" ;;
    # ★ V12 ORBCOMM (vibe_orbcomm.h, docs/v12/ORBCOMM.md): Kenny's published packets, the receiver on synthetic SDPSK
    #   (offsets, Doppler slope, clock error, two carriers, noise), and a synthetic pass end to end through the run + the
    #   sky plot's store / JSON / PNG (test-orbcomm-run). -O2: ~3 s each.
    test-orbcomm|test-orbcomm-run) echo "-O2" ;;
    # ★★★ beta94 the LIGHT ORBCOMM sky plot (vibe_orbcomm_sky.h): named by its Doppler curve, wrong places refused, the cost
    #   against the full decoder (~8 s); and which unattended satellite work runs — Meteor, ORBCOMM, both (vibe_sat_pass_sched.h)
    test-orbcomm-sky)      echo "-O2" ;;
    # ★★★ beta94 the sky plot by POSITION (vibe_skyplot.h cells): grid, aggregation, expiry, ≤ 400 KB, AUTO, the heatmap PNG, reset
    test-skyplot-cells)    echo "-O2" ;;
    test-sat-pass-sched)   echo "-O2 -lpthread" ;;
    # ★★★ beta96 the landing map + keep-running modes (vibe_keep_run.h): migration, the rate gate, the shared 137 MHz capture
    test-keep-run)         echo "-O2 -lpthread" ;;
    # ★ ORBCOMM through the server's OWN slice paths (shared FFT + one-pipeline WideTap) at an HF+'s 912/768/456 kS/s, float IQ, a dial move
    test-orbcomm-slice)    echo "-O2 -I $VDSP -I $KISS -lpthread" ;;
    # ★ V12 beta90 a satellite CW beacon through the listener's CW decoder (locked residual pinned; lock off printed)
    test-sat-cw)           echo "-O2 -I $VDSP" ;;
    # ★ the server's zone name reads the Mac's own zone through CoreFoundation (vibe_tz_id.h).
    test-tz-id) [ "$(uname)" = Darwin ] && echo "-framework CoreFoundation" || echo "" ;;
    *)               echo "" ;;
  esac
}

# test name -> the sources it needs besides itself
deps_for() {
  case "$1" in
    test-wspr)          echo "android/app/src/main/cpp/decoders/digimodes/wspr_decoder.cpp $VDSP/third_party/pffft/pffft.c" ;;
    test-balloon)       echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;   # ★ the RTTY decoder the balloon bank runs
    test-sat-cw)        echo "android/app/src/main/cpp/decoders/cw_skimmer.cpp $VDSP/third_party/pffft/pffft.c" ;;   # ★ beta90
    test-dxscan-fm)     echo "$VDSP/fft.cpp $VDSP/channelizer.cpp $VDSP/ddc.cpp $VDSP/stereo.cpp $VDSP/rds.cpp" ;;   # ★ kiss + pffft: cobjs_for
    test-wfm-stereo)    echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-stereo-highblend) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-gain-step|test-rsp-vibeagc) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-multipath-meter) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-mpx-measure|test-rds-ps-frames) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-wfm-narrow-cost) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-iqout-wide)      echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-passband-cost)   echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-pll-iq)          echo "$VDSP/ddc.cpp $VDSP/iqclean.cpp" ;;
    test-am-sync)         echo "$VDSP/amsync.cpp $VDSP/ddc.cpp $VDSP/fft.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-freedv) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c $RADE_LIBS" ;;
    test-dvsig)           echo "$VDSP/resampler.cpp $VDSP/ddc.cpp" ;;
    test-auto-bw|test-m17|test-aprs-onair) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-watch|test-watch-cost) echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-resampler-rate)  echo "$VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
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
    test-usrp-caps)     echo "$SRC/vibeserver_config.cpp" ;;   # ★ V12: the USRP owner options round-trip through vsconfig
    # ★ No $KISS here: the ft8 C objects (cobjs_for) already carry kiss_fft, and two copies do not link — nor pffft,
    #   which they carry too since monitor.c's STFT runs through it (2026-10-25).
    # ★ V12 2026-10-27: test-bg-ndb — the same, plus the NDB skimmer (the background NDB skimmer VFO, vibe_bgdecode_ndb.h)
    test-bgdecode|test-navtex-quality|test-lrpt-host|test-lrpt-geo|test-fctlm-gallery|test-bg-priority|test-bg-heavy|test-bg-band|test-bg-skim|test-bg-ndb|test-cw|test-dsc|test-sitora|test-psk|test-jtty|test-rtty-skimmer) echo "android/app/src/main/cpp/decoders/ndb_skimmer.cpp $VDSP/pipeline.cpp $VDSP/amsync.cpp $VDSP/autobw.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp \
                              android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp \
                              android/app/src/main/cpp/decoders/wefax_decoder.cpp android/app/src/main/cpp/decoders/sstv_decoder.cpp \
                              android/app/src/main/cpp/decoders/time_decoder.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp \
                              android/app/src/main/cpp/decoders/cw_skimmer.cpp android/app/src/main/cpp/decoders/psk_skimmer.cpp android/app/src/main/cpp/decoders/rtty_skimmer.cpp android/app/src/main/cpp/decoders/digi_modes.cpp android/app/src/main/cpp/decoders/digi_tuner.cpp android/app/src/main/cpp/decoders/digi_spotter.cpp android/app/src/main/cpp/decoders/sat_skim.cpp $DIGIMODES android/app/src/main/cpp/decoders/dsc_decoder.cpp android/app/src/main/cpp/decoders/sitor_a.cpp android/app/src/main/cpp/decoders/same_decoder.cpp android/app/src/main/cpp/decoders/selcal_decoder.cpp android/app/src/main/cpp/decoders/textmodes.cpp android/app/src/main/cpp/decoders/mt63.cpp android/app/src/main/cpp/decoders/ale_decoder.cpp android/app/src/main/cpp/decoders/feld_hell.cpp android/app/src/main/cpp/decoders/jtty_decoder.cpp" ;;
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
    # ★★ "Also give it a vibesdr.net address" (dirAlias) round-trips, and an older config keeps the alias.
    test-config-alias)  echo "$SRC/vibeserver_config.cpp" ;;
    # ★★ V12 beta90 the PSK Reporter / WSPRnet / DMR ID lookup settings round-trip, OFF by default.
    test-config-pskr)   echo "$SRC/vibeserver_config.cpp" ;;
    # ★★ V12 beta96 sharing ADS-B with flight trackers: RadioConfig::adsbFeed round-trips, OFF by default.
    test-config-adsbfeed) echo "$SRC/vibeserver_config.cpp" ;;
    # ★★ The owner's Broadcast FM defaults + REMEMBER / RETURN (vibe_fm_defaults.h) and their config round trip.
    test-fm-defaults)   echo "$SRC/vibeserver_config.cpp" ;;
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
    # ★★ V12 wide-rate DAB (vibe_dab_ddc.h): 17 capture rates × block offsets, aliases swept to the capture's edge, and
    #    its cost table — optimised, or the sweep takes minutes and the cost line means nothing.
    test-dab-ddc) echo "-O2" ;;
    # ★★ V12 §11.1 the wide-capture filter bank against the DDC (vibe_dab_bank.h) — optimised (its cost lines), and threaded.
    test-dab-bank) echo "-O2 -pthread" ;;
    # ★★★ A SLOW SAMPLE CLOCK (2026-11-12, a B210 4 ppm out on MTVNL 7D): the null walks to the window's front and the tracker
    #     pinned there — the lock broke up hours into a listen. The window re-centres (advanceAfterPush); the old loop must
    #     still show the fault. ~2,000 synthetic frames through the real receiver, so optimised.
    test-dab-clockwalk) echo "-O2" ;;
    # ★★★ WHAT PLAYS AFTER A BLOCK CHANGE (2026-11-12, Stuart's RSP1A: squeal + "100 % frames lost" with no station chosen):
    #     the old service's sub-channel is no longer decoded from the new block, and the server's own choice — same
    #     station, last on the multiplex (a file), first — never for a scan. The bench clip through DabService (exit 3 =
    #     NOT RUN without it: vibeserver/bench-clip/make-clip.sh or VIBE_DAB_CLIP=). The rule alone: test-dab-last-service.
    # ★★ V12 each block's grade — working / marginal / not working (vibe_dab_block_quality.h): the rule, the live grader's
    #     settle and span, the memory (written on a change or a 30-min refresh), its JSON and file. Header only.
    test-dab-block-quality) echo "-O2" ;;
    test-dab-station-choice) echo "-O2 -lpthread $( [ "$(uname)" = Darwin ] && echo "-framework AudioToolbox -framework CoreFoundation")" ;;
    # ★★ V12 DAB OWN STATION (docs/v12/DAB-OWN-STATION.md): the rule, the roster, and three stations on the bench clip each
    #    playing their own service while the radio's own decode reads as with none attached.
    test-dab-own) echo "-O2 -lpthread $( [ "$(uname)" = Darwin ] && echo "-framework AudioToolbox -framework CoreFoundation")" ;;
    # ★★★ The shared chain's passband edges in config (vibe_room_passband.h, 2026-10-17): a listener's
    #     sliders follow the room's, in the mode they were chosen in.
    test-room-passband) echo "" ;;
    test-time-decoder|test-tdf)  echo "android/app/src/main/cpp/decoders/time_decoder.cpp" ;;
    test-radiodns-ecc)  echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-radiodns-name) echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-geoip)         echo "$SRC/geoip.cpp $SRC/proc.cpp" ;;
    test-asndb)         echo "$SRC/asndb.cpp $SRC/proc.cpp" ;;
    test-moveloc)       echo "$SRC/moveloc.cpp $SRC/proc.cpp" ;;
    # ★★★ The EiBi season + its fallback (NickB, 2026-10-27: B26 a 404 two days after we switched to it).
    test-eibi)          echo "$SRC/eibi.cpp $SRC/proc.cpp" ;;
    test-satcat)        echo "$SRC/satcat.cpp $SRC/proc.cpp" ;;   # ★ V12 satellite engine: the CelesTrak cache
    test-admin-banlist) echo "" ;;
    # ★★ The POWER card's verdict (vibe_power.h, 2026-10-16): brief supply dips amber; red only for a dip
    #    held ≥ 10 s, near-continuous dips, USB over-current, or a USB device lost in a dip. Header-only,
    #    with a fake /sys and /dev for the rpi_volt alarm and the USB scans.
    test-power-dips)    echo "" ;;
    test-decoder-hardening) echo "android/app/src/main/cpp/decoders/ndb_skimmer.cpp $VDSP/ddc.cpp android/app/src/main/cpp/spyserver/spyserver_messages.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp android/app/src/main/cpp/decoders/psk_skimmer.cpp android/app/src/main/cpp/decoders/rtty_skimmer.cpp android/app/src/main/cpp/decoders/digi_modes.cpp android/app/src/main/cpp/decoders/digi_tuner.cpp android/app/src/main/cpp/decoders/digi_spotter.cpp android/app/src/main/cpp/decoders/sat_skim.cpp $VDSP/resampler.cpp $DIGIMODES android/app/src/main/cpp/decoders/dsc_decoder.cpp android/app/src/main/cpp/decoders/sitor_a.cpp android/app/src/main/cpp/decoders/same_decoder.cpp android/app/src/main/cpp/decoders/selcal_decoder.cpp android/app/src/main/cpp/decoders/textmodes.cpp android/app/src/main/cpp/decoders/mt63.cpp android/app/src/main/cpp/decoders/ale_decoder.cpp android/app/src/main/cpp/decoders/feld_hell.cpp android/app/src/main/cpp/decoders/jtty_decoder.cpp" ;;  # ★ pffft comes with the ft8 objects (cobjs_for)
    # ★ NAVTEX's CCIR 476 coder, word by word (audit 2026-10-04, rows 11-13); the audio half is scripts/test-navtex.sh.
    test-navtex-fec)    echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;
    # ★★ beta96 NAVTEX SNR (display only): ±1 dB at known SNRs in the bit bandwidth, 1 s slices, a mid-message fade.
    test-navtex-snr)    echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;
    # ★★ RTTY's letters/figures shift across a lost signal: a lost LTRS → letters, a synop line → figures (2026-10-13).
    test-rtty-shift)    echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;
    test-ft8-callhash)  echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-ft8-passes)    echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-ft8-fast)      echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-ft8-hunt)      echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-ftx-sensitivity) echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-js8)           echo "android/app/src/main/cpp/decoders/digimodes/js8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp android/app/src/main/cpp/decoders/digimodes/js8_ldpc.cpp android/app/src/main/cpp/decoders/digimodes/js8_message.cpp" ;;
    test-ft2)           echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-fst4)          echo "android/app/src/main/cpp/decoders/digimodes/fst4_code.cpp android/app/src/main/cpp/decoders/digimodes/fst4_decoder.cpp android/app/src/main/cpp/decoders/digimodes/ldpc_small.cpp $VDSP/ddc.cpp $VDSP/resampler.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-tone-id)       echo "android/app/src/main/cpp/decoders/tone_id.cpp" ;;
    # ★★ V12 PAGING (ITU-R M.584-2 Annex 1, POCSAG 512/1200/2400, AUTO): the ITU's sync/idle words as codewords, every rate both
    #    polarities, hours of noise with zero pages. Android local hardware only — the engine is all that is tested here.
    test-paging)        echo "android/app/src/main/cpp/decoders/paging_decoder.cpp" ;;
    test-paging-gate)   echo "$(deps_for test-decoder-hosts) android/app/src/main/cpp/decoders/paging_decoder.cpp" ;;
    # ★★ V12 EAS / SAME (47 CFR § 11.31): the header format, LSB-first bits, the three bursts soft-combined, hours of noise → 0.
    test-same)          echo "android/app/src/main/cpp/decoders/same_decoder.cpp" ;;
    # ★★ V12 SELCAL (ICAO Annex 10 Vol III, Amendment 91 Table 3-1): 300 random codes over 32 tones, the dial, noise, DTMF, speech.
    test-selcal)        echo "android/app/src/main/cpp/decoders/selcal_decoder.cpp" ;;
    test-ale)           echo "android/app/src/main/cpp/decoders/ale_decoder.cpp" ;;   # ★ V12 2G HF ALE (docs/v12/HF-DATA.md)
    test-textmodes)     echo "android/app/src/main/cpp/decoders/textmodes.cpp android/app/src/main/cpp/decoders/mt63.cpp" ;;   # ★ V12 MFSK / OLIVIA / THOR (pffft: cobj-ndb)
    # ★★ V12 the VARA HF tag (vibe_varatag.h — IDENTIFY ONLY, docs/v12/VARA-TAG.md): VARA-shaped generic OFDM tagged, every
    #    look-alike (PACTOR, single carriers, FreeDV/RADE, DRM, Olivia, MT63, real RADE + speech) 0 labels (pffft: cobj-ndb)
    test-varatag)       echo "-O2 $VDSP/channelizer.cpp $VDSP/fft.cpp android/app/src/main/cpp/decoders/textmodes.cpp android/app/src/main/cpp/decoders/mt63.cpp" ;;   # ★ -O2: ~40 simulated captures of 150 s at 192 kS/s (one case only — a second one above this shadowed the sources)
    test-feld-hell)     echo "android/app/src/main/cpp/decoders/feld_hell.cpp" ;;   # ★ V12 FELD HELL, drawn (pffft: cobj-ndb)
    test-msk144)        echo "android/app/src/main/cpp/decoders/digimodes/msk144_code.cpp android/app/src/main/cpp/decoders/digimodes/msk144_decoder.cpp android/app/src/main/cpp/decoders/digimodes/ldpc_small.cpp $VDSP/ddc.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-q65)           echo "android/app/src/main/cpp/decoders/digimodes/q65_code.cpp android/app/src/main/cpp/decoders/digimodes/q65_decoder.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp" ;;
    test-orbcomm-slice) echo "$VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/ddc.cpp $VDSP/resampler.cpp $VDSP/iqclean.cpp" ;;   # ★ kiss + pffft (cobjs_for, the ndb group)
    test-band-slice|test-band-dc) echo "$VDSP/channelizer.cpp $VDSP/fft.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp android/app/src/main/cpp/decoders/psk_skimmer.cpp android/app/src/main/cpp/decoders/rtty_skimmer.cpp android/app/src/main/cpp/decoders/digi_modes.cpp android/app/src/main/cpp/decoders/digi_tuner.cpp android/app/src/main/cpp/decoders/digi_spotter.cpp android/app/src/main/cpp/decoders/sat_skim.cpp $VDSP/resampler.cpp $DIGIMODES android/app/src/main/cpp/decoders/jtty_decoder.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp $VDSP/ddc.cpp $VDSP/iqclean.cpp" ;;   # ★ kiss + pffft from the ft8 objects (cobjs_for)
    test-bench-decoders) echo "$(deps_for test-decoder-hosts) android/app/src/main/cpp/decoders/tone_id.cpp android/app/src/main/cpp/decoders/audio_nr.cpp \
                              android/app/src/main/cpp/decoders/auto_notch.cpp $VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/rds.cpp $VDSP/stereo.cpp $RADE_LIBS" ;;
    test-decoder-hosts|test-digi-spots|test-dec-dump|test-shared-decoders|test-sat-ft4) echo "android/app/src/main/cpp/decoders/ndb_skimmer.cpp $VDSP/ddc.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp android/app/src/main/cpp/decoders/psk_skimmer.cpp android/app/src/main/cpp/decoders/rtty_skimmer.cpp android/app/src/main/cpp/decoders/digi_modes.cpp android/app/src/main/cpp/decoders/digi_tuner.cpp android/app/src/main/cpp/decoders/digi_spotter.cpp android/app/src/main/cpp/decoders/sat_skim.cpp $VDSP/resampler.cpp $DIGIMODES android/app/src/main/cpp/decoders/dsc_decoder.cpp android/app/src/main/cpp/decoders/sitor_a.cpp android/app/src/main/cpp/decoders/same_decoder.cpp android/app/src/main/cpp/decoders/selcal_decoder.cpp android/app/src/main/cpp/decoders/textmodes.cpp android/app/src/main/cpp/decoders/mt63.cpp android/app/src/main/cpp/decoders/ale_decoder.cpp android/app/src/main/cpp/decoders/feld_hell.cpp android/app/src/main/cpp/decoders/jtty_decoder.cpp" ;;  # ★ pffft comes with the ft8 objects (cobjs_for)
    test-sstv-quality)  echo "$VDSP/ddc.cpp $VDSP/resampler.cpp android/app/src/main/cpp/decoders/sstv_decoder.cpp" ;;   # ★ ddc + resampler: the host carries APRS and 9600 packet (V12)
    # ★★ V12 AIS + APRS (docs/v12/AIS-APRS.md): published worked examples, every message type, the modems at many SNRs,
    #    ten minutes of noise per modem with zero frames. The vibedsp FIR/NCO (NEON / SSE2) are what they run on.
    test-ais|test-aprs|test-ais-store|test-ais-binary) echo "$VDSP/ddc.cpp $VDSP/resampler.cpp" ;;   # ★ resampler: APRS carries 9600 (vibe_g3ruh.h)
    # ★★ V12 9600 packet, FX.25, IL2P (docs/v12/AX25-9600-FX25-IL2P.md): RS, a real off-air FX.25 block, the IL2P spec's
    #    example packets, the G3RUH modem sweep, 1200 AFSK with FEC, noise with zero frames, the run's frame log, CPU.
    test-ax25hs)        echo "$VDSP/ddc.cpp $VDSP/resampler.cpp" ;;
    test-wefax-phasing) echo "android/app/src/main/cpp/decoders/wefax_decoder.cpp" ;;
    test-drm|test-drm-signal|test-drm-constellation) echo "android/app/src/main/cpp/decoders/drm/drm_receiver.cpp $VDSP/fft.cpp" ;;   # ★ pffft + kiss from cobjs_for
    test-drm-faad) echo "android/app/src/main/cpp/decoders/drm/drm_receiver.cpp $VDSP/fft.cpp $FAAD_LIB $XAAC_LIB" ;;
    test-drm-xaac) echo "$XAAC_LIB" ;;   # ★ after the source, like FAAD2   # ★ pffft + kiss from cobjs_for
    test-dab-faad|test-dab-faad-queue|test-dab-tii-air) echo "$FAAD_DAB_LIB" ;;   # ★ after the source, so a Linux link resolves it
    test-sat-skim) echo "android/app/src/main/cpp/decoders/sat_skim.cpp android/app/src/main/cpp/decoders/digi_tuner.cpp android/app/src/main/cpp/decoders/ft8_decoder.cpp android/app/src/main/cpp/decoders/ftx_fine.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp" ;;   # ★ pffft from the ft8 objects (cobjs_for)
    test-xpdr) echo "android/app/src/main/cpp/decoders/cw_skimmer.cpp" ;;   # ★ pffft from cobjs_for (the ndb group)
    test-vormap) echo "android/app/src/main/cpp/decoders/ndb_skimmer.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp $VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/ddc.cpp" ;;   # ★ IdentVote + the datalink's front end; pffft + kiss from cobjs_for
    test-ndb|test-ndb-suspect|test-community) echo "android/app/src/main/cpp/decoders/ndb_skimmer.cpp android/app/src/main/cpp/decoders/cw_skimmer.cpp \
                              $VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/ddc.cpp" ;;   # ★ pffft + kiss_fft from cobjs_for (C); ddc: the Band tap's decimator (vibe_wide_tap.h, reached through the service)
    # ★ V12 AVIATION DATALINK (docs/v12/AVIATION.md): ACARS, VDL2, HFDL and the service that runs them share one front end
    #   (vibe_avi_front.h — the channeliser, FFT and decimators); pffft + kiss_fft from cobjs_for.
    test-acars|test-acars-apps|test-vdl2|test-hfdl|test-avi-service|test-aircraft-share) echo "$VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/ddc.cpp" ;;
    test-maritime-combo) echo "$VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/ddc.cpp android/app/src/main/cpp/decoders/dsc_decoder.cpp" ;;
    # ★ V12 RS41: the same channeliser / FFT / half-bands (vibe_rs41_rx.h); pffft + kiss_fft from cobjs_for.
    test-rs41|test-rs41-service|test-sondes-types) echo "$VDSP/channelizer.cpp $VDSP/fft.cpp $VDSP/ddc.cpp" ;;
    *)                  echo "" ;;
  esac
}

# test name -> C sources it needs. ★ Compiled as C, separately: g++ would compile a .c file as C++,
#   and ft8_lib is C that C++ refuses (malloc without a cast). Prints the object files built.
FT8C=android/app/src/main/cpp/ft8_lib
cobjs_for() {
  case "$1" in
    test-decoder-hosts|test-dec-dump|test-paging-gate|test-bench-decoders|test-digi-spots|test-band-slice|test-band-dc|test-decoder-hardening|test-sstv-quality|test-ft8-callhash|test-ft8-passes|test-ft8-fast|test-ft8-hunt|test-ftx-sensitivity|test-js8|test-ft2|test-q65|test-fst4|test-msk144|test-bgdecode|test-navtex-quality|test-lrpt-host|test-lrpt-geo|test-fctlm-gallery|test-bg-priority|test-bg-heavy|test-bg-band|test-bg-skim|test-bg-ndb|test-cw|test-dsc|test-sitora|test-psk|test-jtty|test-rtty-skimmer|test-shared-decoders|test-sat-ft4|test-sat-skim)
      local d="$OUT/cobj-ft8"; mkdir -p "$d"
      # ★ + pffft.c: monitor.c's STFT runs through PFFFT (2026-10-25). ★ Rebuilt when any ft8_lib header is newer
      #   too: ldpc_tables.h / vibe_simd4.h changes would otherwise leave a stale object passing the tests.
      for f in $FT8C/ft8/*.c $FT8C/fft/kiss_fft.c $FT8C/fft/kiss_fftr.c $FT8C/common/monitor.c $VDSP/third_party/pffft/pffft.c; do
        local o="$d/$(basename "$f" .c).o"
        { [ "$o" -nt "$f" ] && [ -z "$(find $FT8C -name '*.h' -newer "$o" 2>/dev/null | head -1)" ]; } || cc -O2 -c -I "$FT8C" -o "$o" "$f" || return 1
        printf '%s ' "$o"
      done ;;
    test-vormap|test-ndb|test-ndb-suspect|test-community|test-drm|test-drm-faad|test-drm-signal|test-drm-constellation|test-tone-id|test-textmodes|test-varatag|test-feld-hell|test-dxscan-fm|test-acars|test-acars-apps|test-vdl2|test-hfdl|test-avi-service|test-aircraft-share|test-rs41|test-rs41-service|test-sondes-types|test-dab-bank|test-orbcomm-slice|test-xpdr|test-maritime-combo)
      local d="$OUT/cobj-ndb"; mkdir -p "$d"
      for f in $VDSP/third_party/pffft/pffft.c $KISS/kiss_fft.c $KISS/kiss_fftr.c; do
        local o="$d/$(basename "$f" .c).o"
        [ "$o" -nt "$f" ] || cc -O2 -c -o "$o" "$f" || return 1
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
  # ★ VIBE_TESTS="test-a test-b": only those C++ suites, and stop after them (the Linux build container runs the DAB/DRM
  #   ones this way — it has no node for the page checks below).
  if [ -n "${VIBE_TESTS:-}" ]; then case " $VIBE_TESTS " in *" $name "*) ;; *) continue ;; esac; fi
  case "$name" in
    # ★ RETIRED FROM THE RUN 2026-11-09 (Stuart: "we have already ran all those tests and built on the results"): these run
    #   ONLY on off-air recordings, which were deleted from ~/VibeSDR-test-audio once their results were built in. The sources
    #   stay; run one by hand with VIBE_TESTS="<name>" after a fresh capture (the README/logs beside each folder say how).
    test-dab-faad|test-dab-faad-queue|test-dab-tii-air|test-drm-faad|test-drm-xaac|test-ssb-offset-real) [ -n "${VIBE_TESTS:-}" ] || continue ;;
    #   test-openrsp-names — OpenRSP's whole Enumerator (vendor/openrsp core + service sources); built and run by
    #   scripts/test-openrsp-names.sh further down, so here it only "did not build" (no -I vendor/openrsp/src).
    test-openrsp-names) printf '\n\033[1m── %s ──\033[0m\n   \033[2mskipped — built and run by scripts/test-openrsp-names.sh below\033[0m\n' "$name"; continue ;;
    #   test-adsb-restart, test-keep-restart (beta96) — the real shim in-process; CMake targets, run by scripts/test-adsb-restart.sh
    #   and scripts/test-keep-restart.sh further down (they read vibeserver/build), so here they only "did not build".
    test-adsb-restart|test-keep-restart|test-sattrack-restart) printf '\n\033[1m── %s ──\033[0m\n   \033[2mskipped — a CMake target, run by scripts/%s.sh below\033[0m\n' "$name" "$name"; continue ;;
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
  # ★★ 2026-11-24 tool-dec-replay is built WITH test-dec-dump, every time. It was built only in the end-to-end block below,
  #    which VIBE_TESTS never reaches — so `VIBE_TESTS=test-dec-dump` left a stale replay tool in the shared $OUT (one
  #    built 11-23 from beta92's tree replayed the Pi 500 capture as 0 packets and was taken for a host fault). A failed
  #    build removes the old binary rather than leaving it to be run.
  if [ "$name" = test-dec-dump ]; then
    g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for "$name") -o "$OUT/tool-dec-replay" \
        "$SRC/tool-dec-replay.cpp" $(deps_for "$name") $cobj 2>"$OUT/tool-dec-replay.buildlog" \
      || { rm -f "$OUT/tool-dec-replay"; echo '   (tool-dec-replay did not build — removed)'; }
  fi
  # ★ Exit 3 = NOT RUN (its data is not here) — counted apart, never as a pass. Same rule as the end-to-end suites.
  "$OUT/$name"; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
done
if [ -n "${VIBE_TESTS:-}" ]; then
  printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build, %d not run\033[0m\n' "$pass" "$fail" "$broke" "$notrun"
  [ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]; exit
fi

# ★ The setup page is a C++ raw string, so no compiler ever looks at its JavaScript. A syntax
#   error there ships silently and the page simply does nothing — the worst failure this tree has,
#   because that page is what a new owner meets first.
printf '\n\033[1m── setup page ──\033[0m\n'
if node scripts/check-setup-page.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 the USRP's owner options and its radio AGC, hwinfo → Setup page / app / web, from fake capability objects.
printf '\n\033[1m── USRP options (hwinfo → controls) ──\033[0m\n'
if node scripts/check-usrp-options.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The open ‹ Servers menu is the top layer — it opened under the DAB box twice (10-30, beta93).
if node scripts/check-servers-menu-layer.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ The August RTL stutter fixes (gain write off modeMtx/the DSP, one transfer, the client's hwinfo
#     signature skip) — a gain change must never again stall the audio. See check-gain-write-path.mjs.
printf '\n\033[1m── RSP hwinfo -> controls, per model ──\033[0m\n'
if node scripts/check-rsp-caps.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── device filters (RSP only from Android 8.0) ──\033[0m\n'
if node scripts/check-device-filter.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── the standard DAB path is untouched (front end, AGC, gain) ──\033[0m\n'
# ★★★ Stuart's HARD RULE (2026-11-16): own station splits the per-station back end only. Part 2 (weak captures, before vs
#     after) runs when DAB_WEAK_BEFORE / DAB_WEAK_AFTER name dab-weak builds; exit 3 = only the source half ran.
node scripts/test-dab-standard-path-guard.mjs; rc=$?
if [ $rc -eq 0 ] || [ $rc -eq 3 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 DAB PHASE 2 (docs/v12/DAB-OWN-STATION.md §11): in a wide LOCKED range each listener is in DAB on their own
#     multiplex and the radio never moves — the real server on fake rtl_tcp replaying the RSP1A's 8 MS/s 10C/10D/11A capture
#     (~/VibeSDR-test-audio/dab-wide; exit 3 = not run without it or without VIBESERVER_BIN). Silent.
printf '\n\033[1m── DAB in a wide locked range: a listener\x27s own multiplex (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-wide-lr.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the same through the §11.1 filter bank (a benchmark that measured it cheaper: one pass cuts every block).
DAB_LR_FRONT=bank VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-wide-lr.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 the WIDE BENCHMARK's HTTP side: the offer, the refusals, the run, wide.<serial> saved beside the quick result (2026-11-18).
printf '\n\033[1m── wide benchmark (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-bench-wide.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 the QUICK benchmark's DAB wide-capture rows go no faster than the radios here — an RTL: no filter bank, no 8 MS/s
#     DDC; an RSP: the bank at its own top rate, never 56 MS/s (Stuart, 2026-11-19). Two whole quick runs (~several min).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" VIBE_DAB_CLIP="${VIBE_DAB_CLIP:-}" node scripts/test-server-bench-dab-rate.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ A DAB request on a locked range is never dropped silently: a SHARED range at 2.048 MS/s refuses with the reason (and
#    logs it); a one-listener range is honoured as before (2026-11-17).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-lr-refuse.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 DAB ONLY: whole-radio and wide locked range — listeners land in DAB, may not leave; the admin may (2026-11-17).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-only.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 THE ONLY MODE, any whole-radio mode (ADS-B, ISM, AIS here; DAB as one case): lands, a listener may not leave
#    (refused + logged), the admin may; hwinfo onlyMode beside dabOnly (2026-11-18).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-only-mode.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and in the clients: one rule in the web page, the app and the R36S (an older server read from its blocked modes), and
#    the real web page in headless MUTED Edge: in DAB without asking, no EXIT DAB, "DAB only on this receiver".
if npx tsx scripts/test-dab-only-clients.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-dab-only.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and THE ONLY MODE in the clients, any whole-radio mode: one rule in the web page and the app (an older server read
#    from its blocked modes), the R36S and the setup page; and the real web page in headless MUTED Edge on an ADS-B-only
#    server: in ADS-B without asking, no EXIT ADS-B, "ADS-B only on this receiver", Escape does not leave (2026-11-18).
if npx tsx scripts/test-only-mode-clients.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-only-mode.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── gain write path ──\033[0m\n'
if node scripts/check-gain-write-path.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ A Wi-Fi name typed with the wrong apostrophe is saved as it is on the air (Portable road test, 2026-10-14).
if node scripts/test-setup-wifi-name.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node scripts/test-setup-wifi-wired.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ A new card's one update check, as the setup page shows it (2026-10-28): updated / up to date / offline / failed (never
#     asked again) / Cancel. The helper's half, with real apt + dpkg, is vibeserver/linux/test_first_update_docker.sh (docker).
if node scripts/test-setup-first-update.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ New Wi-Fi hardware on a set-up box (a USB dongle added later, 2026-10-25): "configure it now?" — Configure / Not now.
if node scripts/test-setup-wifi-new.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ Fork labelling (directory forkOf): a fork's name is shown, checked; official names refused as fork names.
if node --no-warnings directory/scripts/test-fork.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ "In use by another app" through the directory (2026-10-18): the listing carries it, a held radio is never FREE,
#     and a radio whose process did not answer (503) does not borrow the door's 0 listeners and read FREE either.
if node --no-warnings directory/scripts/test-in-use.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta90 the receiver rankings (directory/src/ranks.js): PSK Reporter's stats page + wspr.live, hourly, per-call lookup —
#    a fake network, the real SQLite schema.
if node --no-warnings directory/scripts/test-ranks.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
if node scripts/test-setup-bench-stale.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 the QUICK / WIDE benchmark on the page: the CPU/RAM lines, the WIDE button only for a wide radio, the USB verdict, the app's same words.
if node scripts/test-setup-bench-wide.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node scripts/test-rsp-notch-lr.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE GOVERNOR MENU OFFERS ONLY WHAT THIS MACHINE HAS (2026-10-19): the Lenovo's intel_pstate has no schedutil.
if node scripts/test-setup-governors.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE WATERFALL RATE MENU SHOWS THE RATE THE SERVER RUNS (11.0.9): a hand-set 15 is "15 fps (set by hand)", never "Full · 20".
if node scripts/test-setup-maxfps.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta88 "Remote access to server settings": the page on an Android phone's server shows only the phone's controls.
if node scripts/test-setup-android.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ …and EVERY setting on the phone's server screen is on that page, or listed device-only with why (Stuart 2026-11-20):
#     a new setting on the screen fails here until it is on the page; the page, the screen and every Start agree.
if node scripts/check-remote-keys.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the whole page signed in and saved against a phone's config, the save run through the phone's own translation
#    (the C++ suite above built $OUT/test-android-setup). Exit 3 = not run (no binary).
VIBE_ANDROID_SETUP_BIN="$OUT/test-android-setup" node scripts/test-setup-android-e2e.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 RATE SWITCH: "change the sample rate when a mode needs it" (dabRateBoost); the RTL 3.2 MS/s ADS-B option stays removed.
if node scripts/test-setup-mode-rate.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 ADS-B AS THE LANDING: drawn only where ADS-B can run, keep-running only beneath it and never ticked by default,
#    and when ticked the Background Decoding and release cards say the idle radio is ADS-B's.
if node scripts/test-setup-adsb-landing.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 the landing map + keep-running modes on the Setup page (the gate on the C++ cases, the card drawn) and the app's rule
if node scripts/test-setup-keep-run.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_keep_run.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 b96-setupux the app's server screen in sections, in Stuart's order (src/utils/setupSections.ts)
if node --no-warnings scripts/test_setup_sections.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 BACKGROUND DECODING's editor (2026-10-22): the page's real code — Nick's profile round-trips, decoders follow the
#    demodulator, "add another profile" only under a timed one, delete, the dial hint.
if node scripts/test-setup-bgdecode.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta90 UNATTENDED SATELLITE PASSES beside it: its own switch, saved inside the same setting as "satPasses",
#    ticks by catalogue number + label, nothing stored untouched, hidden on an Icom, the locked-range note.
if node scripts/test-setup-satpass.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and an ISM PROFILE (2026-10-25): offered only where ISM can run (not an HF+, an Icom, a locked radio, ISM blocked),
#    only the bands the radio reaches; a band instead of a centre and VFOs; saved as {"ism":"433"} and read back.
if node scripts/test-setup-bg-ism.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 2026-10-27: a Band digital-spots VFO in the editor — offered only where it can run, a BAND PICKER (only the bands the
#    capture holds) in place of the frequency, the demodulator greyed out, saved and read back.
if node scripts/test-setup-bg-band.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta90: the skimmers by band (CW / RTTY / PSK rows pick a band from skimBands, or "Own centre…")
if node scripts/test-setup-bg-skim.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the NDB SKIMMER VFO (2026-10-27): offered only where the radio reaches LF (HF+, RSP, an RTL in direct sampling; the
#    running radio's ndbHw first), never with NDB blocked, once a profile; frequency and demodulator greyed out; Stuart's
#    Lenovo profile round-trips; an older {"ndb":true} profile loads as a VFO profile with the skimmer.
if node scripts/test-setup-bg-ndb.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 (2026-11-05) EVERY RADIO'S TAB SHOWS ONLY WHAT IT CAN DO (vibe_radio_caps.h): the server's answer per driver
#     (test-radio-caps --json, compiled here) fed to the page's real modeBlockRender / capsApply — exactly the modes its caps
#     allow, the settings rows (usage, Raw IQ, NB, VFO A/B, FM defaults, DX) follow them, hiding ≠ clearing, the running
#     radio outranks the door, an older server draws everything. Then the Icom card itself (test-setup-icom).
if node scripts/test-setup-radio-caps.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node scripts/test-setup-icom.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 THE IDLE DX SCANNER end to end (brief §13): idle past the wait → the scan starts and logs the fake dongle's RDS
#     stations (their IQ images Suspect); a listener mid-hop gets the dial back and the hop is NOT logged; the cursor resumes;
#     an old client works normally; promote / clear; DAB in DX mode on the 12D reference capture when VIBE_TEST_AUDIO has it.
#     ~3 min. Same VIBESERVER_BIN rule (exit 3 = not run).
# ★★★ V12 R36S HANDHELD end to end (vibeserver --handheld, docs/v12/R36S-HANDHELD-IMAGE.md §0.2): no radio → the page and
#     "none"; a fake radio adopted with no setup step and reached through the hand-off; per-radio memory written on unplug
#     and restored when THAT radio returns; hot swap; Start+Select; the card (recordings with Range, import → done/ + .txt,
#     export, diagnostics); the fake hardware service (clamps, SSH password never logged, clock, events, shutdown ACK).
#     ~1 min. Same VIBESERVER_BIN rule (exit 3 = not run).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-handheld.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 R36S HANDHELD — the PAGE (web/client/src/handheld.ts) in headless MUTED Edge at 640×480 with a simulated pad:
#     "Please connect an SDR", straight into the receiver (no landing page, no start-audio screen), "Spectrum rate" and no
#     link figures, the three accordions (one open, ○ collapses, the D-pad and ✕), the overlays, the 30 fps cap, per-radio
#     memory across a hot swap, a recording onto the card, SPECTRUM RATE 5/10/20 (with indicative CPU figures), and an
#     ordinary server's landing page on the D-pad. ~4 min. Same VIBESERVER_BIN rule (exit 3 = not run).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-handheld.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 PAGING NEVER LEAVES THE DEVICE (2026-11-16): the desktop binary now compiles the engine for the R36S — a server
#     offers no paging and refuses it from the LAN and from loopback with no frame sent; the handheld's door is closed to the
#     LAN and refuses it for a radio not plugged into it. ~30 s. Same VIBESERVER_BIN rule (exit 3 = not run).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-paging-local-only.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dx-scan.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 THE OWNER'S SCANS (admin page DAB SCAN / FM SCAN) on a shared radio: the FM scan starts with a listener on,
#     locks them out (server refusal + fm_scan lock), finds the fake dongle's RDS stations into SCANNED bookmarks, puts
#     the dial back; cancel; dab_scan_get / fm_scan_get. ~2 min. Same VIBESERVER_BIN rule (exit 3 = not run).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-owner-scans.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ A TUNNEL A PREVIOUS PROCESS LEFT ON OUR PORT IS RETIRED (2026-10-19, Kiko's four tunnels). Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-tunnel-orphan.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 VFO A/B through the real server: lower VFO left / higher right in ONE stream, B's own squelch, the window refusal,
#    the listener-slot yield, an old client untouched, a shared dial's private stream. Same VIBESERVER_BIN rule.
# ★★★ THE APP'S LISTENER ID through two real radio processes: the app's second radio gets the web's answer under every owner
#    setting (maxRadiosPerIp default/2/0/1); two installs behind one address are two listeners. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-listener-id.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-vfo-ab.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta93 each VFO's OWN NR / notch / blanker, and THE NR ECHO STORM (Stuart 11-24: ~1 MB/s, NR sliding by itself, VFO B
#    would not switch off): B set/reported apart from A, no answer to an unchanged setting, a drag answered once, B's notch
#    on B's audio only, 30 s of an echoing page at the idle rate, VFO B OFF stays off. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-vfo-dsp.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 a FREE VFO walking across the tuner's DC (vibe_vfo_dc.h, GitHub #57): the DC moved out of the passband, one retune
#    per crossing, never on a range-locked radio, a shared dial moved only by a person's tune. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-vfo-dc.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 VFO A/B in the WEB client (headless Edge, --mute-audio): the tuning-menu toggle, the A/B key, B's needle, the
#    audio panel's STEREO switch, no toggle where the owner switched it off. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-vfo-ab.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ GitHub #57 / Stuart's hard rule: the waterfall through a LOCKED tune (the tuner following), zooms and a fast spin
#    stays one continuous trail (no torn rows, on the wire and on the canvas), and a resize keeps the history. Headless
#    Edge, --mute-audio, the real server. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-web-waterfall-history.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 THE TUNED STATION'S LABEL on the web waterfall (headless Edge, --mute-audio, no server): drawn at phone, tablet
#    and desktop widths, spectrum on and off, beside the needle, on screen at both edges, never over LISTEN's readout.
node scripts/test-web-tuned-label.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 the full-capture row (SPWB): only to a socket that asked `wide=1`, only while zoomed, one per 8 SPEC frames, no
#    extra frames, the capture's centre/span, a one-bin carrier outside the view kept. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-wide-rows.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 beta83 THE SPAN STAMP (NickB, HF+: a big keypad jump left the waterfall's span wrong for good): `span=1` gets v2
#    frames (the Hz per bin at 22, header 30), everyone else the v1 they always got; on every path the settled stamp IS the
#    config's span; through 7285 ⇄ 585 kHz jumps no stamp is a span the server never configured. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-span-stamp.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the clients' half, on the REAL app client and web client: rows placed by the frame's own span, a stale config healed
#    after a second with nothing in flight, never during a zoom of ours, v1 frames exactly as before.
if node --no-warnings scripts/test_span_stamp.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta83 DAB own stations: the banner names what is shared (ASK TO TUNE / EXIT DAB) and alternates "ANY STATION IN THE
#    LIST IS FREE TO CHOOSE"; the station list says every user can choose their own — app and web.
if node --no-warnings scripts/test_dab_free_banner.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta83 a LEAVE sends the dial (its listener count) — the banners read it; it went out only on a join (Stuart: the Pi 2
#    read 5/5 for 50 minutes after four listeners left). A held place still counts. Same VIBESERVER_BIN rule.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dial-leave.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 PRESENCE (Stuart 11-24: a closed app held a slot on the XCover for 3 minutes): "bye" and a clean 1001 close in the
#    foreground release a session AT ONCE; "background", an abnormal drop, the app's own 1000 pauses and an older client
#    that says nothing are held as before (idle grace 12 s → 12 s holds, ~1.5 min). Same VIBESERVER_BIN rule. Silent.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-presence.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 THE COUNT AND THE LOCK AGREE when a listener leaves (11-26: listeners 2/4 and ASK TO TUNE while the lock had
#    lifted): a web-shaped listener leaves four ways — a final departure drops count AND lock at once; a held place stays
#    counted but `away`, and every dial has askFirst exactly when listeners − away > 1 (~1 min). Same VIBESERVER_BIN rule. Silent.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-leave-count.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the clients' half: the app client says background/foreground (again on every new socket's first hwinfo) and
#    `leave` on destroy; the web page's presence/bye never raise "not connected" or wake a closed socket.
if node --no-warnings scripts/test_presence_client.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★ …and the admin page reads its progress lines into a bar (2026-11-05)
if node scripts/test-admin-diag-progress.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★★ vibeserver-radios' unit-name guard (root, systemctl) is unchanged, and the serial an Icom is given
#     ("IC-7300 03001234" → "IC-7300_03001234", vibe_serial.h) passes it — the IC-7300 that never started, 2026-11-03.
if python3 vibeserver/linux/test_vibeserver_radios.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ OpenRSP names every RSP apart — Almere's two "0000000001" clones were one device to it, and an RSP1 with no
#     serial string failed the whole scan (2026-11-18). The real Enumerator, a fake USB bus.
if bash scripts/test-openrsp-names.sh; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★ The page's panels where they belong: two dropped </div>s put the whole MAIN MENU inside a hidden decoder panel (2026-10-26).
printf '\n\033[1m── test-web-markup-nesting ──\033[0m\n'
if node scripts/test-web-markup-nesting.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE WEB MEMORY BUDGET (memBudget.ts): a 1 GB Pi or a small-screen handheld gets a small waterfall history, an
#    unknown desktop keeps today's 2560-row ring, and the ring never drops below the display (2026-11-01).
if node --no-warnings scripts/test-web-mem-budget.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★★ THE SDRplay AGC READOUT (rspAgcReadout.ts): "RF n · IF nn" on the status line, VibeAGC's state in the radio panel, amber
#     when it is stuck — latched, so a healthy radio never flickers it (rsp_frozen_readout_warning_design).
if node --no-warnings scripts/test_rsp_agc_readout.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ THE DX LOG's frequency column, tags and footer (utils/dxLog.ts): OIRT to two decimals, the ranges, locals bookmarked.
if node --no-warnings scripts/test_dxLog.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ MPX POWER IN dBr (mpxPower.ts): relative to BS.412's reference, so a figure below 0 reads as compliant, not a fault.
if node --no-warnings scripts/test_mpx_power.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ LOCKED VFO CENTRE (tuneWindow.ts): every app tune clamps into the owner's window, the server's own rule (Stuart 2026-10-18: the app tuned into the black).
if node --no-warnings scripts/test_tune_window.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE RECEIVER'S CLOCK FROM OUR OWN TZ RULES (serverZone.ts, 2026-11-12): the server's "tz" name, offset worked out here —
#    Kiko's pre-2019 box sent -120 "BRST" in November; Sao_Paulo is -03 all year. No tz / unknown zone = the old figures.
if node --no-warnings scripts/test_server_zone.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE APP'S LISTENER ID (listenerIdCore.ts): the web's `bid`, one per install, on every VibeServer socket — so the server's
#    radios-per-listener rule judges the app exactly as a browser (Stuart 2026-10-27, two radios on one server).
if node --no-warnings scripts/test_listener_id.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 THE SCANNER ENGINE (scanner.ts, docs/v12/SCANNER.md): fake backends of every family — hang cancel on an over-change,
#     noiseRef thresholds, ignores, priority, OWRX hijack PAUSED/resume, no tune ever outside the span (fuzzed), spikes never stop it.
if node --no-warnings scripts/test_scanner.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ EiBi SEASON FALLBACK (eibiSeason.ts, NickB 2026-10-27): an unpublished season (B26 a 404) uses the previous one,
#     says so in words, re-asks every 6 h — same scenarios as test-eibi.cpp; the Swift port in test_eibi_season_swift.sh.
if node --no-warnings scripts/test_eibi_season.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ proxy.kiwisdr.com IS 8073 ONLY (2026-10-07): 47 % of public Kiwis are listed portless (= port 80, refused).
if node --no-warnings scripts/test_kiwi_proxy_port.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ SERVER ADDRESSES (serverUrl.ts): scheme and port are a PAIR — https://host:8073 stays TLS (GitHub #52, OWRX+ native TLS).
if node --no-warnings scripts/test_server_url.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 BINAURAL TUNING (binauralOffset.ts): the offset source per mode, AM/CW/FM readings with known offsets, the tracker.
if node --no-warnings scripts/test_binauralOffset.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 VFO A/B, the app's half (src/services/vfoAB.ts): the wire, the A/B slots, the L/R tags (the server's rule).
if node --no-warnings scripts/test_vfoAB.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta93 the NR echo ring (a report paints, never sends) and the per-VFO NR / notch / blanker sets — web and app
if node --no-warnings scripts/test-vfo-dsp.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 TRANSCRIBE AUTO ORDER (transcribeOrder.ts): EiBi, the SDR's local language's reserved lane, the ★ list, the ⚠.
if node --no-warnings scripts/test_transcribeOrder.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 RECORD BUFFER (recordBuffer.ts): the retune rule — per-mode allowance, measured from the ANCHOR, mode/source clear.
if node --no-warnings scripts/test_recordBuffer.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 CW (cwList.ts): the app's CW box against the web's — order, confidence colours, spots, the scroll pin, the segment drawn.
if node --no-warnings scripts/test_cw_list.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 CW words (cwVocab / cwFormat): the PLAIN key's reading, the terms guide's search, the speed range, the QRZ links.
if node --no-warnings scripts/test_cw_vocab.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 ISM (src/services/ism.ts): the app's ISM against the web's — sanitiser, lines in every unit, status, filter, capability.
if npx tsx scripts/test_ism_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 the NAVTEX map's client rules (src/services/navtexMap.ts, navtexWarn.ts, navtexIcons.ts): sites, the why, the slot,
#    the app's pins; the warnings (summary, why, shapes, faded, the card); the met labels; the layers; the icons
if npx tsx scripts/test_navtex_map.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 2026-11-02 THE SHIPPING MAP's client rules (src/services/shippingMap.ts, navtexWind.ts): which layers are present,
#    ships from the live report and the store, DSC pins from the box and the gallery, the time filter, the KEY, the arrows
if npx tsx scripts/test_shipping_map.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_sea_areas.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 APP PARITY 2 (2026-10-26): the app's PSK / JTTY / DSC boxes against the web's own modules — dials, channels, parsers,
#    order, merges, where a tap tunes, the attach parameters, hints and colours, which keys a radio can reach.
if npx tsx scripts/test_psk_jtty_dsc_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 APP PARITY 2: the app's digital spots — the server's mode list, Band mode's capability and title, the governor's chips —
#    against the web's own code cut out of main.ts / decoders.ts.
if npx tsx scripts/test_spots_band_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta94 A SPOT'S MESSAGE (src/utils/spotMessage.ts): the app's parser keeps `msg`, cleaned and bounded; the expanded
#    row's own message line (none = no line); the web's parser cut out of decoders.ts and run on the same frames.
if npx tsx scripts/test_spot_message_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta92 THE BAND SKIMMER (src/services/bandSkim.ts): its name, the band_skim frame (and an older server's), the VFO's
#    width, the waterfall marks, "waiting for callsign" and the expanded CW / RTTY text — one rule for the web and the app.
if npx tsx scripts/test_band_skim.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 APP PARITY 2: the app's DRM box against the web's drmRender (the same words in the same order), the sanitiser,
#    the capability (vibeserver.json modes lists "drm").
if npx tsx scripts/test_drm_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_am_sync.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 synchronous AM (amSync.ts)
if node --no-warnings scripts/test_freedv.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 FreeDV as a mode (freedv.ts)
if node --no-warnings scripts/test_freedv_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 FreeDV in the app (key, VTS, sub-mode row, sideband)
if npx tsx scripts/test_packet.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 packet: AUTO / 1200 / 9600 / 300, FX.25, IL2P, the frame log (packet.ts)
if npx tsx scripts/test_m17.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 M17 as a mode (m17.ts)
if npx tsx scripts/test_dvsig.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 DV ID (dvsig.ts)
if npx tsx scripts/test_spot_ranks.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 beta90 the gallery's ranking card
if npx tsx scripts/test_adsb_feed.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 beta96 flight trackers: the app's copy
if node scripts/test-setup-adsb-feed.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 beta96 flight trackers: the Setup card
if npx tsx scripts/test_m17_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ V12 M17 in the app (key, VTS, no width, ghost reports)
if node --no-warnings scripts/test_bw_auto.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★★ V12 the AUTO key (bwAuto.ts)
# ★★ V12 NDB (src/services/ndb.ts): the app's NDB against the web's — sanitiser, every cell, bars (and the map layer's),
#    NM, rows, tiles, the map feed, Find / stats, the readings' memory, the capability, the wiring (V12 NDB).
if npx tsx scripts/test_ndb_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 AIS + APRS in the app (src/services/tracks.ts) against the web's tracks.ts — one rule, two readers
if npx tsx scripts/test_ais_aprs_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 THE AMATEUR MAP (src/utils/amateurMap.ts): the HEARD IN window for every layer, MESH (every ISM band's remembered
#     nodes) and BEACONS, the app's list in tabs (amListRows, TrackLiveList), the pages wired
if npx tsx scripts/test_amateur_map.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 THE MESH ROUTE (src/utils/meshRoute.ts, docs/v12/MESH-ROUTE.md): names, furthest direct / via mesh, the flood
#    path, matching, the ordered drawing + the path fit, the card's ROUTE key (only with a path), km / miles, the app pages
if npx tsx scripts/test_mesh_route.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 maps2 STORED SONDES AND VOR STATIONS on the AIR & SEA map (src/services/storedLayers.ts — the gallery's day cards)
if npx tsx scripts/test_stored_layers.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 THE MAP FOLLOWS THE MODE (Mac Ha 11-26: the AIS map's title over the ADS-B aircraft list) — the app's map page
#    kept with its kind and opening (src/utils/mapPage.ts), the screen's map precedence, one whole-radio map at a time (app + web).
if node --no-warnings scripts/test_map_page_kind.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 beta95 the band-plan strip's label room (src/utils/bandLabelPiece.ts — the web strip, the app's WaterfallView rule)
if npx tsx scripts/test_band_label_piece.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 NCDXF beacons: the shared client half (src/services/ncdxf.ts) and both clients' keys (2026-11-16).
if npx tsx scripts/test_ncdxf_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 balloons: the shared client half (src/services/balloons.ts) and both clients' keys.
if npx tsx scripts/test_balloon_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 DTMF + 5-tone: the shared client half (src/services/tones.ts), both clients' keys and options, the setup screens.
if npx tsx scripts/test_tones_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 VOR / ILS and 406 MHz beacons: the shared client half (src/services/navaids.ts — the radial's line, the FIX, the card),
#   both clients' keys, the setup screens, the copy.
if npx tsx scripts/test_navaids_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 THE VOR / ILS MAP: the shared client half (src/services/vorMap.ts — the report, the list, the map's items, the
#    receiver FIX and its words), both clients' wiring, the setup screens.
if npx tsx scripts/test_vormap_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 DIGITAL SPOTS (2026-10-26): the client half — band stepping (the server's rule, its cases), the band list parsed
#    defensively, the mode styles readable on both map themes and colour-blind safe (src/utils/digiModes.ts).
if npx tsx scripts/test_digi_modes.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_decoder_bands.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 aviation, the clients' half (src/services/aviation.ts): the datalink report parsed defensively, the HFDL status line
if npx tsx scripts/test_aviation_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 2026-11-16 THE AVIATION MAP's client rules (src/services/aviationMap.ts): the table parsed, live + table joined once, the layers present
if npx tsx scripts/test_aviation_map.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ b96-rangefeed where a map's receiver comes from (src/services/rxPosition.ts): lat/lon, else the locator square (approximate)
if npx tsx scripts/test_rx_position.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta90 THE DIGI MODES MAP's client halves: stations.json cleaned + merged, the overlay, APRS points, avPlaneAdsb.
if npx tsx scripts/test_digi_map.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_digi_map_stats.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi   # ★ beta92 the digi map's stats / legend / summary, shared app + web
# ★★★ V12 beta92 ONE AUTO FIT FOR EVERY MAP (src/utils/mapAutoFit.ts): ADS-B's glide / hysteresis / framing, the receiver in
#   view, the key's on / paused / off, and every map host wired to it.
if npx tsx scripts/test_map_autofit.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 radiosondes, the clients' half (src/services/sondes.ts): the report parsed defensively, no temperature unless the
#   calibration is whole, the lines, the map's items / rows / card and a chosen sonde's track
if npx tsx scripts/test_sondes_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 METEOR LRPT, the clients' half (src/services/lrpt.ts): lrpt_status parsed defensively, the status line, the attach
if npx tsx scripts/test_lrpt_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta97 who a gallery item came from (src/utils/galleryOrigin.ts, web + app): an unattended satellite pass's items
#   read BACKGROUND, a listener's LISTENER
if npx tsx scripts/test_gallery_origin.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta97 tuning inside a satellite pass (src/services/satTune.ts, web + app): the 500 Hz step and the listener's own
#   step back on the way out; a waterfall tap with the lock live takes the Doppler shift off and never snaps out of the passband
if npx tsx scripts/test_sat_tune.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 METEOR LRPT's MAP OUTLINE, the clients' rule (src/utils/lrptOutline.ts): OFF by default, the switch remembered, the
#   outline parsed defensively, "geo" / "gw", outline items never listed, DOWNLOAD following the switch.
if npx tsx scripts/test_lrpt_outline.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 2026-11-24 ORBCOMM + THE SKY PLOT, the clients' half (src/services/orbcomm.ts, src/utils/skyPlot.ts): orbcomm_status /
#    orbcomm_pkt parsed defensively, subscriber messages shown as "not decoded" with raw bytes only, the dome's geometry and the
#    server's SNR ramp, skyplot.json with radio ids and grey dots, an ORBCOMM pick's decoder and listening mode
if npx tsx scripts/test_orbcomm_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta94 the sky plot's heatmap in the clients (src/utils/skyPlot.ts): the server's cells, AUTO, the live pass, the overrides
if npx tsx scripts/test_skyplot_heat.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta94 THE TRANSPONDER FINDER, the clients' half (src/services/xpdr.ts): xpdr_status parsed defensively, the rows' order
#    and words, LISTEN only on voices (USB + xpdr_follow), the waterfall overlay through ORBCOMM's layout, the pick's decoder
if npx tsx scripts/test_xpdr_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 SILENT DECODERS (src/services/silentDecoder.ts): APRS and METEOR LRPT mute the speaker (or, with VFO B on, the VFO
#   they decode on) and put it back exactly as it was — the rule run every way, and both clients pinned to it
if npx tsx scripts/test_silent_decoder.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ V12 satellite engine, the clients' half (src/services/satellites.ts): parsing, the readout words, the picker rows, the cap
if npx tsx scripts/test_satellites_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta91 SATELLITE LABELS on the waterfall (knownSignals.ts parseSatLabels / satLabelSignals): the message checked, one
#    text ("ISS FM", "ORBCOMM FM114"), Doppler-shifted marks, ORBCOMM merged on the band and never a decoder, the layout.
if node --no-warnings scripts/test_sat_labels.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 SATELLITE MODE, the clients' pure half: the timetable's order + greying, the words, the box size, the half-globe
if TZ=UTC npx tsx scripts/test_satellite_mode.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 SCHEDULE A PASS, the pure half: which key, the book, overlaps + PRIORITISE, the runner, degrade (satSchedule.ts)
if TZ=UTC npx tsx scripts/test_sat_schedule.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 SINGLE MODE: SATELLITES, the pure half: who watches, the messages, the header / line / hybrid words, the timetable
if TZ=UTC npx tsx scripts/test_sat_single.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 satellite box polish: no address on the globe, SMALL without words + the glyph, the readout layout, opaque text
if npx tsx scripts/test_sat_layout.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta94 SATELLITE IN LOCAL HARDWARE: the key / the position prompt / which position / nothing published (satPlace.ts)
if npx tsx scripts/test_sat_place.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta94 SHOW (Stuart 11-25): which satellites' passes the list shows — groups from the downlinks, tick boxes, stored by
#    NORAD across servers, the pick always stays, reminders follow it (src/services/satellites.ts, both clients)
if TZ=UTC npx tsx scripts/test_sat_filter.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta91 TELEMETRY CHARTS (src/utils/tlmCharts.ts): the server's pass CSVs read back, the chart geometry, across passes
if TZ=UTC npx tsx scripts/test_tlm_charts.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 (2026-10-31, Stuart): every AUTO decoder starts in AUTO; a manual choice lasts the session only (decoderSession.ts).
if npx tsx scripts/test_decoder_session.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 SHARED DECODERS (2026-11-24, Stuart): on a shared dial the room's decoder is followed (the server's own extension and
#     parameters), a close is this listener's only and is remembered, a press wins — web and app alike (sharedDecoders.ts).
if npx tsx scripts/test_shared_decoders.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12 PAGE (2026-11-16, Stuart): the law's notice wherever PAGE is described, and RECORDINGS SILENT WHILE PAGE RUNS — every
#     audio sender in the engine passes through pagingAudioOut (what the Android recorder and the R36S tap record), raw IQ
#     out refused / closed, the app's words (test-paging-gate runs the rule end to end).
if npx tsx scripts/test_paging_rec.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 (2026-10-26, M7EIW): every spot's time is HH:MM:SS UTC, its slot's START — one helper for web, app and maps.
if npx tsx scripts/test_spot_time.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and Band's DEFAULT from the benchmark (the setup page's benchApplyDefaults + the app's applyBench), owner override kept.
if node scripts/test-spots-band-default.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and EVERY decoder's default (2026-10-31): the server's defaultOff list applied by both readers, every switch it can
#    name on both setup screens (vibe_benchmark_more.h ruleFor).
if node scripts/test-bench-defaults.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta91 FT8 / FT4 dig deeper: automatic, one owner control, nothing a listener sees (decoders/ft8_deep_auto.h)
if node scripts/test-ft8-deep-owner-only.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 (2026-11-20): the same benchmark in LOCAL HARDWARE (Android) — a plain verdict per thing this radio can do here,
#    no dead rows, pause + resume around the run, the engine's own-radio exception (src/services/localBench.ts).
if node scripts/test-local-bench.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 THE MODES LIST AS A TREE (2026-10-31): a decoder's options under its switch, greyed with the reason when it is
#    off, "LoRa: listen on every channel" (lora+all) and its benchmark rule — the real page in headless MUTED Edge.
if [ -n "${VIBESERVER_BIN:-}" ]; then
  node scripts/test-setup-mode-tree.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else notrun=$((notrun+1)); fi
# ★★★ beta96 NEW MODES OFF + the locked radio's Modes card (docs/v12/NEW-MODES-OFF.md): a 7-radio config shaped like Paul's
#     in headless MUTED Edge — the ADS-B tab shows its own ticks, NEW marks, All on / All off, the save's seen@.
if [ -n "${VIBESERVER_BIN:-}" ]; then
  node scripts/test-setup-paul-tabs.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else notrun=$((notrun+1)); fi
# ★★★ beta96 b96-setupux the Setup page's COLLAPSIBLE SECTIONS + the red "!" + the server's frame rate, in headless MUTED Edge.
if [ -n "${VIBESERVER_BIN:-}" ]; then
  node scripts/test-setup-sections.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else notrun=$((notrun+1)); fi
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
# ★★★ V12 ANDROID ↔ SETUP PAGE PARITY: every Setup-page control is on the phone's server screen (app + Lite) with the
#     engine applying it, or listed as not ported with why — Background Decoding sat on the Setup page from beta3
#     with the phone never hearing of it. A new Setup control fails here until it is classified.
if node scripts/check-android-settings-parity.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the model behind the phone's Background Decoding editor: the bgDecode JSON both ways, the window rules.
if node --no-warnings scripts/test_rsp_card.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_bg_decode_model.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 2026-11-22 WEFAX STATIONS: the Setup page's ticks (the server's list, 8 at most, "fax":[Hz…] byte for byte), and the
#    clients' WEFAX hints held to the server's one list (vibe_wefax_stations.h — decoderBands, knownSignals, wefaxAlign).
if node scripts/test-setup-bg-wefax.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wefax_stations.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta90: the listener's skimmer BAND picker (src/utils/skimBands.ts — web decoder box + app ModeSelector)
if node --no-warnings scripts/test_skim_bands_picker.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★ V12 NDB's map layer (web/mapkit/ndblayer.js): its generated copy, the bars rule, and the rings it SHARES with the ADS-B
#     map (Stuart: "same rings, for nm range") — the ladder, NM, four labels a ring. ADS-B's own layer test runs beside it.
if node --no-warnings scripts/test_ndb_layer.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_adsb_layer.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ ADS-B FURTHEST key: the web map and the app map page name the same aircraft (one rule, two readers).
if npx tsx scripts/test_adsb_furthest.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ …and V12 aircraft data on the clients (adsbInfo.ts): the adsb_info sanitiser + store, and the web list's rows with it.
# ★★ V12 beta96 THE ADS-B DELTA PROTOCOL end to end (vibe_adsb_delta.h → src/services/adsbDelta.ts): the table rebuilt from
#    the deltas equals the full report at every tick of a 150-aircraft sky — from the start, joining mid-stream, after a
#    lost delta (resync), the app's list order and emergencies; junk refused. Compiles one C++ file. ~15 s.
if nice -n 10 node --max-old-space-size=1024 --no-warnings scripts/test_adsb_delta.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★ …and the R36S's port of it (handheld-ui/src/dab_quality.h) replays the TS meter's own outputs, line for line.
if bash handheld-ui/test/run_dab_quality_test.sh; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 The R36S's port of the tuned station's label (handheld-ui/src/tuned_label.h) replays tunedStation.ts's own
#    outputs, line for line (Docker arm64 when the image is there, else the host compiler).
if sh handheld-ui/test/run_tuned_label.sh; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The R36S's VTS port (handheld-ui/src/vts_core.h) replays the app's VTS TS line for line — V12 with FreeDV's line from
#    src/services/freedv.ts (searching / a manual choice / in sync), its offer, its sideband and passband.
if sh handheld-ui/test/run_vts_golden.sh; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE R36S BATTERY BLOCK + CALIBRATION (2026-11-15): the live lines and the ≈ figures (battery_math.h), vibe-hwd's
#    calibration (started on the charger, a GENUINE full judged from the charger's physics — FullJudge, 2026-11-16 —
#    → a safe empty, resumed over a power-off, cancelled by charging, the stress load only at the unplug) and its
#    charge-port watch (the 2 A limit at the plug edge, the 5 s trace) — fake sysfs, no device.
if bash handheld-ui/test/run_battery_math_test.sh; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
for t in battery-smooth-test battery-cal-test charger-watch-test; do
  if python3 image/r36s/test/$t.py > /dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED: image/r36s/test/$t.py"; fi
done
# ★★★ THE DRM RECEPTION METER (V12, 2026-11-12): DAB's meter for DRM — the decoder's own SNR in the box (Stuart: 17–20 dB
#     decoded on a signal the meter read as 42 dB), the verdict on the programme's MER against its QAM and the audio frames.
if node --no-warnings scripts/test_drm_quality.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE WIDE VFO (V12, 2026-11-12): a whole-channel mode's drawn passband — DAB's 1.536 MHz block, ADS-B, AIS — drawn
#     only, never sent; edges only once it covers most of the view (app + web share it; the R36S's port is held to it too).
if node --no-warnings scripts/test_wide_vfo.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE STATION-LOGO CACHE (V12, 2026-10-30): air > RadioDNS > name, a failed fetch retried 1 min / 5 min / 30 min /
#     daily (never a permanent blank), replaced only when the published logo changed, LRU-bounded (app + web share it).
if node --no-warnings scripts/test_logo_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_logo_cache_web.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ An unchanged DAB service list keeps its array and objects across the once-a-second reports.
if node --no-warnings scripts/test_dab_share_services.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ DAB tuning keys: a burst of presses is ONE block change on the server (the one stopped on), and
#    leaving DAB drops the DAB station from the VTS (Stuart, 2026-10-05 — both).
if node --no-warnings scripts/test_dab_stepper.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 DAB on a wide radio: block labels with the multiplex names the receiver knows, and a click inside a block goes
#    to that multiplex — only while the view holds two whole blocks (Stuart, 2026-11-15, the Almere B210).
if node --no-warnings scripts/test_dab_block_view.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 hop keys: DAB, ISM and DIGI BAND own the tune keys (the drum swapped, the preference untouched), the current
#    block / band and its neighbours as labelled, wrap at the ends, one send per burst (Stuart, 2026-11-03).
if npx tsx scripts/test_hop_keys.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12: the DAB band scan's result list (app + web share src/services/dabBandScan.ts).
if npx tsx scripts/test_dab_band_scan.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★ V12 THE OWNER'S PORT NAMES, the clients' half (antennaPortNames.ts + the setup page's copy) — test-port-names.cpp is the server's.
if node --no-warnings scripts/test_antenna_port_names.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★ V12 TWO SERVERS AT ONCE (dualMix.ts — GitHub discussion #57): VFO A/B's pan law and glide on both voices, ears by
#    server (a swap never moves one), the balance, the background clock's pop-out, VibeServer-only, the pairing's state
#    machine, and the native terminal set read three times.
if node --no-warnings scripts/test_dual_server.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_dual_server.ts"; fi
# ★★ THE BOOKMARK UNDER THE POINTER (bookmarkHover.ts — GitHub #60): passband or mode default, nearest wins, "+N more",
#    tiered binary search agreeing with brute force, and the web pointer label's wiring.
if node --no-warnings scripts/test_bookmark_hover.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_bookmark_hover.ts"; fi
if node --no-warnings scripts/test_duo_layout.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_duo_layout.ts"; fi
if node --no-warnings scripts/test_decoder_shell_layout.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_decoder_shell_layout.ts"; fi
if node --no-warnings scripts/test_servers_chip.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_servers_chip.ts"; fi
if node --no-warnings scripts/test_shared_banner.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_shared_banner.ts"; fi
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
# ★★ beta96 THE MESSAGE SNR SCALE (src/utils/snrColour.ts, docs/v12/MESSAGE-SNR.md): every mode's typical good decode
#    green and a just-decoded one red; the chip text, contrast, NAVTEX's bands and the server's, the layer's ADS-B copy.
if node --no-warnings scripts/test_snr_colour.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_snr_colour.ts"; fi
# ★★ NAVTEX STATIONS (V12 beta 4): scripts/navtex-stations/navtex-stations.json (NGA Pub. 117) → the clients' and the
#    server's readers through scripts/gen-navtex-stations.mjs — stale output fails here. The rule's cases are in
#    test_navtex.ts above and vibeserver/test-bgdecode.cpp.
if node --no-warnings scripts/test_gallery_pager.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_gallery_pager.ts"; fi
# ★★ V12 beta92 the logs' RADIO filter rule (src/utils/logRadio.ts) — when it is offered, what passes it
if node --no-warnings scripts/test_log_radio.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_log_radio.ts"; fi
# ★ The gallery's TRANSLATE (2026-10-23): which lines are sent, the source language and its hint, the layout kept.
if node --no-warnings scripts/test_gallery_translate.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_gallery_translate.ts"; fi
if node --no-warnings scripts/test_admin_bg.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_admin_bg.ts"; fi
if node scripts/gen-navtex-stations.mjs --check >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/gen-navtex-stations.mjs --check (stale — run it)"; fi
if bash scripts/test-rtty-auto.sh >/dev/null 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test-rtty-auto.sh"; fi
if node --no-warnings scripts/test_tune_hint.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_tune_hint.ts"; fi
# ★★★ A REOPENED SPECTRUM SOCKET CARRIES THE LISTENER'S OWN VFO BACK on a per-listener dial, and NEVER on a
#     shared one (2026-10-05: the RSP on a locked range reset to its landing on every resume from background).
if node --no-warnings scripts/test_reopenTune.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_reopenTune.ts"; fi
# ★★★ BACK TO THE WHOLE-RADIO MODE AFTER THE APP WAS AWAY (2026-10-27, Stuart: the ADS-B / NDB map "dropped back to a
#     waterfall view on the previous frequency" after a switch to another app): no lapse while our own socket is closed,
#     one mode_resume ask per lapse, refusals said in words, an old server's silence given up on.
if node --no-warnings scripts/test_wholeModeResume.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wholeModeResume.ts"; fi
if node --no-warnings scripts/test_map_pack_read.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_map_pack_read.ts"; fi
# ★★★ V12 HELD SESSION — "VibeSDR paused" (2026-10-27): the note only in a quiet map mode on iOS, its words and deadline
#     (the hold, or a hard limit sooner), the permission asked once — and a CLOSED SQUELCH NEVER QUIET (read from source).
if node --no-warnings scripts/test_sessionHoldNotice.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_sessionHoldNotice.ts"; fi
# ★ The directory Worker against real SQLite: dead-address pages, and the 90-day retention (PRIVACY.md, Play form).
for t in directory/scripts/test-gone.mjs directory/scripts/test-retention.mjs directory/scripts/test-lan.mjs directory/scripts/test-alias.mjs; do
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
# ★★★ GitHub #57 / Stuart's hard rule: the APP's waterfall history — each row placed once against its view, a locked tune
#    and a zoom draw one continuous (bending / fanning) trail, nothing older is ever rewritten. Pure logic.
if node --no-warnings scripts/test_wf_row_place.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE APP'S WATERFALL SHADER, RUN FOR REAL (CanvasKit, the same WF_SKSL): a swept station is ONE continuous trail
#    (view pan AND tuner retune), control: dashes without motion compensation; steady rows pixel-identical; a zoom fans
#    with no slices and the screen ease has no seam; a resize keeps the rows. Skips if canvaskit-wasm is absent.
if node --no-warnings scripts/test_wf_shader_motion.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ A STEADY PACE through a tune (wfPace.ts, Stuart 2026-10-28): rows/s the same settled, interacting and after; no
#    lurch at the hand-overs; continuous every 120 Hz frame while interacting; a paused feed comes back at the same pace.
if node --no-warnings scripts/test_wf_pace.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ The web waterfall's ZOOM FAN (web/client/src/wfZoomFan.ts): a zoom re-maps the history to the new span, a tune
#    does not move it, in-then-out is lossless (Stuart 2026-10-27: zooming SLICED the web waterfall).
if node --no-warnings scripts/test_wf_zoom_fan.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 the full-capture layer under the web waterfall's history (wfWide.ts): SPWB parse, extent, placement, no-data.
if node --no-warnings scripts/test_wf_wide.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the APP's full-capture layer (src/utils/wfWide.ts): SPWB v2 parse, the layer ring, the fill through each row's fan map.
if node --no-warnings scripts/test_wf_wide_app.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★ `&wide=1` only for a server that said "wide" ≥ 2 in its preflight or hwinfo (src/services/wideCapsCore.ts).
if node --no-warnings scripts/test_wide_caps.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ The Android native spectrum engine's twins of the above (host builds): motion + zoom fan + layer, the engine, SPEC/SPWB decode.
SPX=android/app/src/main/cpp/spectrum
if g++ -std=c++17 -O2 "$SPX/test_wf_motion.cpp" -o "$OUT/test_wf_motion" && "$OUT/test_wf_motion"; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if g++ -std=c++17 -O2 -I "$VDSP" "$SPX/test_spectrum_engine.cpp" "$SPX/spectrum_engine.cpp" "$VDSP/spectrum_processor.cpp" -o "$OUT/test_spectrum_engine" && "$OUT/test_spectrum_engine"; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if g++ -std=c++17 -O2 "$SPX/test_spec_decoder.cpp" "$SPX/spec_decoder.cpp" -o "$OUT/test_spec_decoder" && "$OUT/test_spec_decoder"; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The web waterfall's GLIDE (web/client/src/wfGlide.ts): a fast tune draws one continuous trail, the noise floor unsmeared.
if node --no-warnings scripts/test_wf_glide.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE ROW TARGET FOLLOWS THE FEED (src/assets/wfRowTarget.ts, Stuart 10-20): Default min(20, 3x), Smooth min(60, 6x),
#    snapped to a divisor of 60 — the app, the web client and the native engine (wf_row_target.h) share the rule.
if node --no-warnings scripts/test_wf_row_target.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The V12 waterfall (backported to 11.0.16): the five speeds' link caps and live numbers, the FIXED OUTPUT CLOCK
#    (over-interpolate and discard; NATIVE untouched), the zoom-out fan by the bins' own span, and the interaction boost
#    + slope cap on a fast run of tune/zoom steps.
if node --no-warnings scripts/test_wf_speed_cap.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wf_fixed_clock.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wf_zoomout_fan.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wf_spin.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE WATERFALL WAITS FOR THE SOUND (src/assets/wfAvSync.ts, V12 2026-10-30): the fixed clock's frames held by the
#     audio's playout delay — at rest, after a ratchet, after a drag and a shrink within ±30 ms; growth carried by repeats
#     (Stuart: "simply duplicate the previous lines"), a shrink by discards; no hold while the dial moves; zero extra stalls
#     or bursts; NATIVE unchanged.
if node --no-warnings scripts/test_wf_avsync.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wf_boost_taper.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ The rest of the V12 waterfall backported for 11.0.16: the scale stamps (and a jump that keeps the history), the
#    history ring growing by RAM tier, and thinned frames COMBINED into the row (MINIMUM 2 · LOW 5 · ECO 10, display only).
if node --no-warnings scripts/test_wf_stamps.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE TUNED STATION'S LABEL AT THE VFO (src/utils/tunedStation.ts, V12 2026-11-15): [User] → [Server] → [EiBi] within the
#    VTS's ±99 Hz, EiBi only on air now, none while a live RDS/DAB/DRM name, the VTS's own line or a moving dial shows.
if node --no-warnings scripts/test_tuned_station.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wf_ring_grow.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if node --no-warnings scripts/test_wf_combine.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE 10-FPS DEFAULT (2026-10-20): the serving phone's tiers read Smoother · 20 / Default · 10 / Half · 5, the stored
#    keys still mean 20 / 10 / 5, and a new server starts on Default · 10.
if node --no-warnings scripts/test_fps_tiers.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 "Use as a server": the entry, VibeServer Simple, remote first-time setup, device-only password / PIN / reset
if node --no-warnings scripts/test_server_choice.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
# ★★★ V12 SAME NETWORK → GO DIRECT: a LAN address is used only when the box there answers with the server's own HMAC;
#     anything else keeps the tunnel (app picker + web hop share src/utils/lanDirect.ts).
if npx tsx scripts/test_lan_direct.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
if npx tsx scripts/test_lan_hop.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ NAV COVERAGE (controller audit, 2026-10-20): a touchable on a pad-driven surface with no nav registration fails
#     once its file passes its baseline — the next silent skip fails here instead of on Stuart's sofa.
if npx tsx scripts/test_nav_coverage.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta92 THE SPOTS LIST BY PAD / KEYBOARD (Stuart, 2026-11-23): BACK from any row is the header in one press, ↑ on the
#     first row too, enter tunes, new spots never move the highlight — the app's rules, then the web page's in Edge (muted).
if npx tsx scripts/test_spots_nav.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
node scripts/test-web-spots-nav.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ THE SERVER LIST'S FOCUS RULES (controller audit group H): Back from the list position, Enter = tap on directory rows.
if npx tsx scripts/test_picker_nav.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ BACK FROM A RADIO LANDS WHERE YOU LEFT (Stuart, 2026-10-27): the server list's place carried across a VibeServer's
#     reset, the missing-receiver fallback (this list's top, never the app's), and a front door's radio list.
if npx tsx scripts/test_picker_return.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
if npx tsx scripts/test_landing_markup.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ EDITING A BOOKMARK (V12, discussion #59): the shared form rules both clients use — frequency units, passband or the
#    mode's default, "what I'm listening to now", the server edit query, an older server's left-behind row, your own list.
if npx tsx scripts/test_bookmark_edit.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta94 THE RECEIVER'S PLACE (spotsmap-serverloc-v12, Stuart 11-25): spot distances and the time-station guess use the
#    server's /location first; a VibeServer with one never asks for the phone's; RR73 is never plotted as a grid square.
if npx tsx scripts/test_receiver_place.ts; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

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

# ★★★ THE RADIO MENU'S AGC CHOICE for an SDRplay (2026-10-30): rsp_control vagcmode refused for a listener, applied live
#     and saved for the owner (both ways, clamped), kept across a restart; rfAgc folded into the radio's entry too.
#     Same VIBESERVER_BIN rule.
printf '\n\033[1m── SDRplay AGC choice from the menu (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-rsp-vagc-control.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE OWNER'S AGC LOCK HOLDS AN SDRplay's RF AGC TOO (2026-11-17): a listener's RF AGC off, manual RF and IF gain refused
#     and told; AGC on allowed; the admin keeps every control; unlocked, nothing refused.
printf '\n\033[1m── SDRplay AGC lock covers the RF AGC (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-rsp-agc-lock.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ …and the setup page keeps an SDRplay's gain in ONE place, under its AGC rows (real page, headless MUTED Edge).
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-setup-rsp-gain-home.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 follow-up (docs/v12/FEED-TEST.md): the feeds card FINDS a fake UberSDR, ADD tests first (pass → added; a closed
#    port → why + ADD ANYWAY), TEST beside a source, Save → config.json, the UberSDR's aircraft on the map. Headless MUTED Edge.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-setup-feed-test.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ …and the web page's RADIO section on an SDRplay (a page shim dresses the fake radio as an RSP1A): the AGC row for
#    the owner and for a listener, and what IF AGC / RF AGC / AGC TARGET say per mode. Needs Edge; silent.
printf '\n\033[1m── SDRplay AGC rows on the web page (headless) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-rsp-agc-rows.mjs; rc=$?
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
# ★★ beta96 SERVER-LEVEL AIRCRAFT FEEDS (docs/v12/ADSB-FEED.md): a synthetic dump1090 (Beast) + dumphfdl (SBS, connecting in)
#    on a full server beside a fake RTL: the feed-radio migration, one row per aircraft with both sources, the feeds in
#    aircraft.json, the banner, the hub forwarding byte-faithfully (never HFDL), the loop guard. Local sockets only. ~40 s.
printf '\n\033[1m── server-level aircraft feeds (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-aircraft-feeds.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 follow-up (docs/v12/FEED-TEST.md, the Lenovo 11-27): feeds saved through a RADIO's process reach the FRONT DOOR
#    that owns them (its config.json watch) within ~2 s; an unrelated save re-applies nothing. ~25 s.
printf '\n\033[1m── aircraft feeds saved through a radio reach the front door (test-feeds-door-watch) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-feeds-door-watch.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: the aircraft table as deltas + the standalone map\x27s tickets, real binary (test-server-aircraft-delta, ADSB-FEED.md) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" npx tsx scripts/test-server-aircraft-delta.mts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 THE STANDALONE AIR & SEA MAP (openAirSeaViewer): the landing's button, no radio and no WebSocket, live updates from
#    a feed, CLOSE MAP back to the landing, no button with nothing to show. Headless Edge, --mute-audio. ~40 s.
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-web-airsea-standalone.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta96 THE ADS-B DELTA PROTOCOL through the real server + its MEASUREMENT: an older client and a delta client side by
#    side on a synthetic 150-aircraft Beast sky (KB/s per message type), the resync, 100 delta listeners' CPU. ~4 min.
printf '\n\033[1m── ADS-B deltas: older vs delta listener, bytes and CPU (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 --no-warnings scripts/test-server-adsb-delta.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta95 THE FRONT DOOR'S ONE MERGED COMMUNITY FEED: a layer any radio may share is at the door, the same DSC call /
#    APRS station from two radios is one row, a save through the door follows. Door only, no radio. ~10 s.
printf '\n\033[1m── community map: the front door answers for every radio (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-community-door.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE WHOLE MACHINE HOLDS STILL FOR A BENCHMARK (Stuart, 2026-11-20 — the Pi 2's quick run measured beside the radio
#     process's DX scanner): a quick run from the FRONT DOOR holds every radio process still (no DX hop inside the run) and
#     brings them back; a listener on any radio refuses it by name (forced, it runs); a radio that does not answer stops it;
#     a wide run in a radio's process holds the others; a held radio's lease brings it back. Door + 2 radio processes on
#     fake rtl_tcp, the measurement stubbed to 8 s. Silent. ~2 min.
printf '\n\033[1m── the benchmark holds the whole machine still (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bench-quiet.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE RADIO PORTS ARE THIS MACHINE ONLY (security pass, 2026-11-12): behind the front door each radio's own port
#     (base+1+n) accepts on 127.0.0.1 and refuses on the LAN address; the door still serves /r/<id> in full (json,
#     waterfall, audio) from loopback and the LAN, and the radio sees the CLIENT's address; a radio with no usable
#     hand-off socket keeps its port on the network and says so. Door + three radio processes on fake rtl_tcp. ~25 s.
printf '\n\033[1m── radio ports behind the front door answer this machine only (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-radio-ports-local.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 USRP (2026-11-02): a clone's FPGA image uploaded through /r/<serial>/… while that radio is NOT running — the
#    front door takes it (the tester's "Refused (503)"); every other path of the down radio still 503s.
printf '\n\033[1m── a USRP image upload through the front door, the radio down (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-usrp-images-frontdoor.mjs; rc=$?
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
# ★★ V12 beta92 THE GALLERY'S TIME RANGE (Stuart 11-23): gallery.json?since= (page + total narrow, count/kinds whole, old
#    requests unchanged); the REAL built page: 1 H · 24 H · 7 DAYS · 30 DAYS · ALL, page 1 on a change, the footer names
#    the range, remembered across a reload, the keyboard, 390 px. ~25 s, silent.
printf '\n\033[1m── the gallery time range (end to end, headless) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-gallery-range.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 beta91 TELEMETRY CHARTS in the gallery: a pass .csv served as text/csv with its download name; the REAL built page:
#    the FUNCUBE filter, a pass opened as its charts + the passes up to it, the day log as text, 390 px. ~20 s, silent.
printf '\n\033[1m── FUNcube telemetry in the gallery (end to end, headless) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-web-fctlm-gallery.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12 METEOR LRPT's MAP OUTLINE in the gallery through a real vibeserver and the built page: the outline item served and
#    never listed, ?outline=1 a burnt-in copy, MAP OUTLINE: OFF by default / ON draws the SVG on the picture, remembered, the
#    reason line, an older picture plain, 390 px. ~25 s, silent (headless Edge --mute-audio).
printf '\n\033[1m── METEOR map outline in the gallery (end to end, headless) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-web-lrpt-outline.mjs; rc=$?
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
# ★★★ V12 THE OWNER'S NAMES FOR THE RADIO'S OWN PORTS (2026-11-03): from config.json cleaned and bounded, back out of the
#     config API escaped, none in hwinfo for a one-input dongle, a port it lacks changes nothing, and a save renames LIVE.
printf '\n\033[1m── antenna port names (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-antenna-port-names.mjs; rc=$?
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
# ★★★ V12: THE THIRD BOOKMARK ORIGIN, SCANNED (2026-10-26): a year's expiry not 30 days, evicted only after every learned
#     row, an old file loads as before, the file round-trips byte-identical, "Clear learned" takes scanned too. ~20 s.
printf '\n\033[1m── scanned bookmarks: expiry, eviction, round trip (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bookmark-scanned.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ V12: EDIT A SERVER BOOKMARK (GitHub discussion #59): POST /bookmarks?was= — admin-only, in place and moved (one save),
#    every refusal in words, learned → the owner's and NOT renamed by a later RDS learn (with a learning control). ~90 s.
printf '\n\033[1m── server bookmark edit (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-bookmark-edit.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12: THE DAB BAND SCAN (M7EIW, 2026-10-26) on the real server and the bench multiplex (played on 11D only): finds it,
#     bookmarks every service, cancel/tune, shared radio refused in words, the owner's setup scan, and the re-learn-on-visit
#     reconcile (two visits 6 h apart, a weak day deletes nothing). Needs the bench clip. ~12 min — the longest suite.
printf '\n\033[1m── the DAB band scan (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-bandscan.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12: THE LAST DAB SCAN SURVIVES A RESTART (2026-11-12, Stuart on the R36S: power off and on, "the DAB multiplexes are
#     forgotten"). Scan, SIGKILL, start again: the same list to a fresh client; a cancelled rescan keeps the blocks it never
#     reached; another radio's list is its own; a damaged file is no list; a scan that finds nothing removes it; the
#     handheld's file sits with the radio's memory and Start+Select clears it. Needs the bench clip. ~7 min.
printf '\n\033[1m── the DAB scan survives a restart (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-scan-persist.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12: AUTOMATIC FREQUENCY CORRECTION (auto-PPM, vibe_afc.h, 2026-11-12) on the real server and the bench multiplex from
#     a dongle whose crystal runs 4 ppm fast (fake-rtl-tcp --clock-ppm 4): off = nothing changes; an upgrade (no key) = on,
#     nothing for 100 s, then ~4 ppm learned, glided in, checked, handed to the RTL's crystal correction (the dial unmoved),
#     DAB's offset ~0; a restart restores it; a manual ppm is the base it adds to; Reset / Turn off = the manual exactly.
#     Needs the bench clip. Silent. ~12 min.
printf '\n\033[1m── auto-PPM (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-afc.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ V12: THE DAB SCAN AND THE GAIN (2026-10-27, Stuart: "the AGC can work quick enough to allow the multiplex to be found
#     but without taking forever"). An RTL front end that OBEYS the gain (fake-rtl-tcp --dab-mux): strong→weak locks in ~3 s
#     (was 20 s, the recovering jump refused by its own gate), a typical aerial's five ensembles all found in < 2.5 min, a +3 dB
#     ensemble found, the gain handed back, each level locks < 2 s; the impaired clip too. Needs bench-clip/mux.cf32. ~11 min.
printf '\n\033[1m── the DAB scan and the gain (end to end, RTL front-end model) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-scan-agc.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★ AN IDLE DECODER SOCKET IS PINGED (2026-10-10): behind Cloudflare's ~100 s idle cut-off the /ws/dxcluster socket
#    reconnected for ever ("an error log as long as my arm"). ~30 s. Same VIBESERVER_BIN rule.
# ★★★ …and end to end on the real server and the bench multiplex: a legacy socket unchanged, a dab=2 socket's
#     reassembled report EQUAL to it, a fraction of the bytes, scopes on/off, resync, a new session. Needs the bench clip
#     (make-clip.sh) as well as VIBESERVER_BIN — NOT RUN without either.
printf '\n\033[1m── the DAB report in pieces (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-dab-report-pieces.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ WHAT PLAYS AFTER A LISTENER CHANGES MULTIPLEX (2026-11-12): silent until the list, then the first; the same
#     station by SId on another block; a named one wins; the last one here across a restart. VIBESERVER_BIN + the bench
#     clip (VIBE_DAB_CLIP=), NOT RUN without either. ~40 s.
printf '\n\033[1m── the DAB station choice (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-station-choice.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ WHICH BLOCK DAB OPENS ON WITH NONE NAMED (2026-11-12, vibe_dab_landing.h): the last block (across an exit, a scan
#     and a restart), else the saved scan's strongest, else the country's national mux (Germany → 5C), else 12B with
#     `unscanned`. Same rule: VIBESERVER_BIN + the bench clip, NOT RUN without either. ~60 s.
printf '\n\033[1m── the DAB landing block (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-dab-landing-block.mjs; rc=$?
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

# ★★★ BACKGROUND DECODING TAKES THE BACK SEAT (beta92, the Pi 500's V4L: a busy in-range background profile + raw IQ out at
#     250 kHz broke the listener's audio, overran the DSP and starved the IQ consumer to 11 %). Same VIBESERVER_BIN rule. ~70 s.
printf '\n\033[1m── raw IQ out + saturating background decoders: the listener and the stream keep real time (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-iq-priority.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ beta94 THE RTL-TCP FRONT DOOR (rtltcp-fix-v12, Sloper 11-25: "connected", "Good", then nothing on a non-RTL radio):
#     RAW IQ OUT's full-rate stream served as rtl_tcp by the engine (--rtltcp-port) — header only with IQ, the client's rate,
#     SET_FREQUENCY, every command, VibeAGC / manual gain, one client, a non-RTL rate list resampled, LAN only (8.8.8.8
#     refused, 169.254 link-local served). Loopback, silent. ~60 s.
printf '\n\033[1m── RTL-TCP front door (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-rtltcp.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ beta96 VIBESERVER SIMPLE: HOME NETWORK ONLY, enforced by the engine (lanOnly — the RTL-TCP door's isPrivateIp rule on
#     every request; --lan-only on the daemon): 8.8.8.8 refused (page and JSON, logged), 192.168 / 169.254 / 10 / fd00:: served,
#     without the switch as before, a request through the tunnel refused. Loopback, silent. ~20 s.
printf '\n\033[1m── VibeServer Simple: home network only (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-lanonly.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ beta94 A CLEAN STOP WITH BACKGROUND DECODING EXITS CLEANLY: ~BgRunner published to the shim's BgRegistry after it
#     was destroyed — SIGABRT on every apt upgrade / restart once background decoding had run (beta93 too). Running, and
#     stopped by a listener first; exit 0, no std::terminate. Same VIBESERVER_BIN rule. ~40 s.
printf '\n\033[1m── a clean stop with background decoding on: exit 0, no abort (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-bg-stop.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ RAW IQ OUT IS THE REAL IQ (V12, docs/v12/IQOUT-WIDE.md): a carrier 100 kHz off a USB dial is IN a 250 kHz stream, at its
#     exact rate, nothing dropped — one-pipeline radio and a listener's own VFO. Same VIBESERVER_BIN rule. ~80 s.
printf '\n\033[1m── raw IQ out is the real IQ, not the listener'"'"'s passband (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-iqout-wide.mjs; rc=$?
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
# ★★ V12: the DAB band scan on the served page — SCAN only on a single-user radio, progress, the result list (11D, 12
#    stations, quality, "added to bookmarks"), expand, 390 px fit, a tap tunes; a shared radio has no key. Edge, muted. ~3 min.
printf '\n\033[1m── the DAB band scan on the web page (Edge, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-dab-bandscan.mjs; rc=$?
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
# ★★★ THE OWNER'S BROADCAST FM DEFAULTS (2026-11-03): the setup card, a fresh server (NR off), owner-set defaults, RETURN and
#     REMEMBER on a shared dial and a locked range, and the served page NOT re-asserting a remembered CEQ OFF (why Stuart
#     kept finding servers with CEQ off). Same VIBESERVER_BIN rule; the page half wants Edge (muted). ~60 s.
printf '\n\033[1m── Broadcast FM defaults: remember / return (server + setup card + served page, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-fm-defaults.mjs; rc=$?
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
# ★★ V12 EDIT A BOOKMARK in the web client (discussion #59): yours always, the receiver's only as admin (no EDIT / ✕ for
#    a listener), prefill, "use what I'm listening to now", a rule refused in words, a learned row moved on the server,
#    the server's own refusal shown, Enter on EDIT and in a field. Edge, muted, the page just built. ~40 s.
printf '\n\033[1m── bookmark edit on the served page (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-bookmark-edit.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ V12 THE TUNE AND ZOOM KEYS TUCK AWAY while a pad, keyboard or remote drives (Stuart, 2026-11-04): pad press / driving
#     key → faded and gone, a click / mouse move / wheel → back; typing is not driving; the option off; pad-first start
#     (no touch, no mouse; ?input=pad); reduced motion; no clipping at 640×480, 480×640, 390 px and desktop.
printf '\n\033[1m── the tune and zoom keys tuck away while a pad drives (simulated pad, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-web-pad-tuck.mjs; rc=$?
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

# ★★★ V12 ISM BACKGROUNDED ON AN ADS-B-IDLE RADIO (2026-11-12, Stuart's iPhone on the Pi 500's V4L: back to "ISM" at
#     1089.590 MHz): kept ADS-B waits for ISM's own leave; the same session back past the hold is put back in ISM on its
#     band before its first config; with somebody else on, it is told ism_off (and why); an exact ISM frequency outside
#     every allocation is refused. Fake rtl_tcp noise only — needs VIBESERVER_BIN (exit 3 = not run). ~3 minutes.
printf '\n\033[1m── ISM backgrounded, ADS-B the idle default (end to end) ──\033[0m\n'
node scripts/test-server-ism-bg-return.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ V12 ISM MODE (docs/v12/ISM-DECODER.md): the real server on a fake rtl_tcp replaying a synthetic 433 MHz clip made by
#     test-ism --write-clip — the capability, the device list and its readable lines, the privacy rule ON THE WIRE, band
#     steps (clamped), an exact frequency, EXIT giving the dial back; then the web client in headless Edge (--mute-audio).
#     Needs VIBESERVER_BIN (exit 3 = not run); the web half also needs Edge.
# ★★ V12 NDB mode end to end (2026-10-26): an RTL without direct sampling offers no NDB; with automatic direct sampling
#     the capability, entering, the radio on the beacon band, WOD and CAM identified and named, a tap moving only the VFO,
#     a tune outside refused, Find NDB, the stats, and the receiver given back exactly (test-server-ndb). ~1 min.
printf '\n\033[1m── NDB mode (end to end: server, then the web client) ──\033[0m\n'
if [ -x "$OUT/test-ndb" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-ndb" --write-clip "$OUT/ndb-clip.u8" 362750 2400000 10 >/dev/null
  VIBE_NDB_CLIP="$OUT/ndb-clip.u8" node scripts/test-server-ndb.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★ …then the web client in headless Edge (--mute-audio): the box, ✓ WOD Woodley, a tap, the tabs, Find, the stats, 390 px
  VIBE_NDB_CLIP="$OUT/ndb-clip.u8" node scripts/test-web-ndb.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/ndb-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-ndb build'; notrun=$((notrun+1))
fi
printf '\n\033[1m── ISM mode (end to end: server, then the web client) ──\033[0m\n'
if [ -x "$OUT/test-ism" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-ism" --write-clip "$OUT/ism-clip.u8" 434130000 2400000 >/dev/null
  # ★ V12 wM-Bus: a meter clip on the 868 band (a Sensus T1, encrypted; a Kamstrup C1-A with readings)
  # ★ 868.8: where ISM puts the radio for the 868 band since 2026-11-12 (vibe_ism_bands.h)
  [ -x "$OUT/test-wmbus" ] && "$OUT/test-wmbus" --write-clip "$OUT/wmbus-clip.u8" 868800000 2400000 >/dev/null
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" VIBE_WMBUS_CLIP="$OUT/wmbus-clip.u8" node scripts/test-server-ism.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ beta94 UNIDENTIFIED BURSTS + the ISM AUTO chip: described on the wire, the carrier listed as steady (not counted),
  #    gain/peak on the report, and NO IQ download for anybody (removed before beta94 shipped — no keyless-entry capture)
  if [ -x "$OUT/test-ism-unknown" ]; then
    "$OUT/test-ism-unknown" --write-clip "$OUT/ism-unk-clip.u8" 434130000 2400000 >/dev/null
    VIBE_ISM_UNK_CLIP="$OUT/ism-unk-clip.u8" node scripts/test-server-ism-unknown.mjs; rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
    rm -f "$OUT/ism-unk-clip.u8"
  else
    echo '   not run — needs the test-ism-unknown build'; notrun=$((notrun+1))
  fi
  # ★ V12 "Use 2.56 MS/s sample rate for ISM" (ism256): the rate up on entry, back on leaving, MeshCom 433.175 then fits
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-server-ism256.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" VIBE_WMBUS_CLIP="$OUT/wmbus-clip.u8" node scripts/test-web-ism.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 ISM IN THE GALLERY (2026-10-25): a listener's session writes the day's log + card (nothing while it ran, the
  #    list's own lines, no ID anywhere), then an ISM background profile runs with nobody here and gives way to a listener;
  #    then the web gallery's ISM chip in headless Edge at desktop and 390 px.
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-server-ism-gallery.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_ISM_CLIP="$OUT/ism-clip.u8" node scripts/test-web-ism-gallery.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/ism-clip.u8" "$OUT/wmbus-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-ism build'; notrun=$((notrun+1))
fi
# ★★ V12 LoRa (docs/v12/LORA-MESH.md): the 868 band through the real server — Meshtastic, MeshCore and LoRaWAN rows, the
#    public messages, positions for the map, and the privacy rule on the wire; then the web client's chips, rows and MAP.
printf '\n\033[1m── LoRa in ISM mode (end to end: server, then the web client) ──\033[0m\n'
if [ -x "$OUT/test-lora" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-lora" --write-clip "$OUT/lora-clip.u8" 868800000 2400000 >/dev/null
  VIBE_LORA_CLIP="$OUT/lora-clip.u8" node scripts/test-server-lora.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 LoRa FOUND ON AIR (2026-11-12): a repeater on no plan's channel, found, decoded, labelled, remembered over a restart
  "$OUT/test-lora" --write-clip-found "$OUT/lora-found-clip.u8" 915250000 2400000
  VIBE_LORA_FOUND_CLIP="$OUT/lora-found-clip.u8" node scripts/test-server-lora-found.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/lora-found-clip.u8"
  VIBE_LORA_CLIP="$OUT/lora-clip.u8" node scripts/test-web-lora.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 the ISM map (2026-11-01): icons by type, approximate people, position links, the time filter, a live push,
  #    the gallery's "stations received" MAP — fake nodes added to the real reports in the page, a seeded gallery.
  VIBE_LORA_CLIP="$OUT/lora-clip.u8" VIBE_LORA_NODES_BIN="$OUT/test-lora-nodes" node scripts/test-web-ism-map.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 the NAVTEX map (2026-11-01): the stations received, the warnings (icons, shapes, clusters, the card with its
  #    text and TRANSLATE, keyboard focus, live pulse) and the met labels, from a gallery of real NAVTEX (scripts/navtex-fixtures)
  node scripts/test-web-navtex-map.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ beta96 NAVTEX decode quality + SNR as the web client draws them: the strip follows wrapping at two widths (each
  #    rendered line = the worst slice of its characters, the faded lines red), the timeline, the quality bar, an old server
  node scripts/test-web-navtex-snr.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ beta96 THE MESSAGE SNR CHIP as the web draws it: ADS-B (no bars), AIS (snrv 2 only), LoRa messages per SF — the
  #    band colours per mode, dark text, inside the row, no sideways push, at 900 / 390 px on a dark and a light page
  node scripts/test-web-msg-snr.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 2026-11-02 THE SHIPPING MAP: the NAVTEX fixtures + a ship store (test-ais-store --write) + a DSC distress call —
  #    the layers present, ships / DSC / arrows on the map, the KEY, a ship's card, DSC first from the gallery's DSC row
  VIBE_AIS_STORE_BIN="$OUT/test-ais-store" node scripts/test-web-shipping-map.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 2026-11-20 THE SHIPPING MAP KEPT BUILT: a 2 000-message gallery indexed by the background warm-up with no map
  #    request (the first answers whole, the stations map from cache); the gate held shut → the on-request build exactly
  #    as before, then both together with every message indexed once; the warm-up off.
  node scripts/test-navtex-warm.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/lora-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-lora build'; notrun=$((notrun+1))
fi

# ★★★ V12 AIRCRAFT DATALINK (docs/v12/AVIATION.md): the real server on a fake rtl_tcp replaying a synthetic VDL2 clip
#     (test-vdl2 --write-clip): the mode's capability and bands, the aircraft table, message bodies to the owner only (and
#     only with the opt-in), the band step, the receiver given back, the owner's switch, /addon/hfdl/*. Needs VIBESERVER_BIN.
printf '\n\033[1m── Aircraft datalink mode + the HFDL endpoints (end to end: the server) ──\033[0m\n'
if [ -x "$OUT/test-vdl2" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-vdl2" --write-clip "$OUT/vdl2-clip.cu8" 2400000 >/dev/null 2>&1
  VIBE_VDL2_CLIP="$OUT/vdl2-clip.cu8" node scripts/test-server-avdata.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 2026-11-16 THE SHARED AIRCRAFT TABLE: two radio processes on one runtime dir — ADS-B (test-adsb --write-clip) on
  #    one, the VDL2 clip on the other — each aircraft once in /vibeserver/aircraft.json with both sources
  "$OUT/test-adsb" --write-clip "$OUT/adsb-syn-clip.u8" 4 >/dev/null 2>&1
  VIBE_VDL2_CLIP="$OUT/vdl2-clip.cu8" VIBE_ADSB_SYN_CLIP="$OUT/adsb-syn-clip.u8" node scripts/test-server-avmap.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★ …and the web client's view of it (headless Edge, --mute-audio) — needs the page built into VIBESERVER_BIN
  VIBE_VDL2_CLIP="$OUT/vdl2-clip.cu8" node scripts/test-web-avdata.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 2026-11-16 the AVIATION MAP in the web client: two radio processes (ADS-B + DATALINK), one table, headless Edge
  VIBE_VDL2_CLIP="$OUT/vdl2-clip.cu8" VIBE_ADSB_SYN_CLIP="$OUT/adsb-syn-clip.u8" node scripts/test-web-avmap.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★★ beta96 the AIRCRAFT list sizes by its OWN width (Stuart 11-26: "the signal hides the aircraft details") — the real
  #     page, the panel at 232 / 300 / 400 / 600 px and two phones: the callsign always shows >= 7 characters
  VIBE_ADSB_SYN_CLIP="$OUT/adsb-syn-clip.u8" node scripts/test-web-adsb-list-width.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/vdl2-clip.cu8" "$OUT/adsb-syn-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-vdl2 build'; notrun=$((notrun+1))
fi

# ★★★ beta96 THE MARITIME COMBINATION (AIS + DSC ch 70) through the server: a fake rtl_tcp at 8 MS/s with an RSP's rate list
#     replays `test-maritime-combo --write-clip` (16 MB, deleted after): not offered on an RTL; offered, entered, 4 ships + the
#     DSC call, the gallery's pin + day log, AIS alone in place, leave; kept at start; single mode. Needs VIBESERVER_BIN.
printf '\n\033[1m── Maritime: AIS + DSC together (end to end: the server) ──\033[0m\n'
if [ -x "$OUT/test-maritime-combo" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-maritime-combo" --write-clip "$OUT/maritime-clip.cu8" 8000000 >/dev/null 2>&1
  VIBE_MARITIME_CLIP="$OUT/maritime-clip.cu8" node --max-old-space-size=512 scripts/test-server-maritime.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/maritime-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-maritime-combo build'; notrun=$((notrun+1))
fi

# ★★★ V12 SONDES (docs/v12/RS41.md): the real server on a fake rtl_tcp replaying a synthetic 403.005 MHz capture
#     (test-rs41 --write-clip: three RS41s from our own transmitter): the capability and bands, the sonde table, no
#     temperature before the calibration is whole, the band step, the receiver given back, the owner's switch; then the web
#     client (headless Edge, --mute-audio). Needs VIBESERVER_BIN (exit 3 = not run). ★ Nothing here was heard on air.
printf '\n\033[1m── Sondes mode (RS41 radiosondes, end to end: the server, then the web client) ──\033[0m\n'
# ★ the clip writer must be today's source (a stale $OUT/test-rs41 from an earlier run wrote its third sonde outside the
#   402–404 MHz band, 2026-11-16): rebuilt here whenever a source it reads is newer
if [ -n "${VIBESERVER_BIN:-}" ] && { [ ! -x "$OUT/test-rs41" ] || [ -n "$(find vibeserver/test-rs41.cpp android/app/src/main/cpp/vibe_rs41*.h -newer "$OUT/test-rs41" 2>/dev/null | head -1)" ]; }; then
  cobj="$(cobjs_for test-rs41)" && g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for test-rs41) -o "$OUT/test-rs41" vibeserver/test-rs41.cpp $(deps_for test-rs41) $cobj
fi
if [ -x "$OUT/test-rs41" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-rs41" --write-clip "$OUT/rs41-clip.cu8" 2400000 >/dev/null 2>&1
  VIBE_RS41_CLIP="$OUT/rs41-clip.cu8" node scripts/test-server-sondes.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_RS41_CLIP="$OUT/rs41-clip.cu8" node scripts/test-web-sondes.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/rs41-clip.cu8"
  # ★★ V12 the other types (docs/v12/SONDES-TYPES.md): a DFM-09, an M10 and an M20 in one band — AUTO type per sonde
  if [ -x "$OUT/test-sondes-types" ] && [ -z "$(find vibeserver/test-sondes-types.cpp android/app/src/main/cpp/vibe_sonde_*.h android/app/src/main/cpp/vibe_rs41*.h -newer "$OUT/test-sondes-types" 2>/dev/null | head -1)" ]; then
    "$OUT/test-sondes-types" --write-clip "$OUT/sondes-types-clip.cu8" 2400000 >/dev/null 2>&1
    VIBE_RS41_CLIP="$OUT/sondes-types-clip.cu8" VIBE_SONDES_TYPES=1 node scripts/test-server-sondes.mjs; rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
    rm -f "$OUT/sondes-types-clip.cu8"
  else
    echo '   not run — the test-sondes-types build is missing or older than its sources'; notrun=$((notrun+1))
  fi
else
  echo '   not run — needs VIBESERVER_BIN and the test-rs41 build'; notrun=$((notrun+1))
fi

# ★★★ V12 SATELLITE ENGINE (docs/v12/SATELLITE-ENGINE.md): the real server on a fake rtl_tcp with CelesTrak's 2026-11-19
#     groups in its orbit cache (epochs moved to an hour ago — always fresh, no network): the picker list, the Doppler lock
#     on the shared dial and on a locked window (built once, never rebuilt by the tracker), nudge / tune away / unlock and
#     the freeze, the greyed state with no orbit data. Needs VIBESERVER_BIN (exit 3 = not run). ★ No satellite on air here.
printf '\n\033[1m── Satellite engine (the Doppler lock, end to end) ──\033[0m\n'
node --max-old-space-size=512 scripts/test-server-sat.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 SINGLE MODE: SATELLITES end to end (fake rtl_tcp + VIBESERVER_SAT_TEST_CLOCK): watch-only before AOS, the
#     linear-transponder HYBRID at AOS (own VFO + AUTO DOPPLER each, independent tuning, outside refused, a joiner), LOS back.
printf '\n\033[1m── Single mode: SATELLITES (watch only + the transponder hybrid) ──\033[0m\n'
node --max-old-space-size=512 scripts/test-server-sat-single.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ b96-satgrace (Mac Ha): unattended passes vs a kept AIS map — a 2 s socket drop never loses the radio, a pass waits the
#     grace (or the held session), and a listener back mid-pass gets AIS back at once with nothing to explain.
printf '\n\033[1m── Satellite passes: the grace, and the kept mode given back ──\033[0m\n'
node --max-old-space-size=512 scripts/test-server-sat-grace.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta94 ORBCOMM: another ORBCOMM pick KEEPS the whole-band decoder (no second attach, its counts run on), a joiner who
#     opens SATELLITE is told the room's pick and joins the same run; then the web box following the decoder after the pick's
#     LOS (headless Edge, --mute-audio). The follow on a real synthetic pass is test-orbcomm-run §1b.
printf '\n\033[1m── ORBCOMM: the pick keeps its decoder, the box follows it ──\033[0m\n'
node --max-old-space-size=512 scripts/test-server-orbcomm-pick.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
node --max-old-space-size=512 scripts/test-web-orbcomm-follow.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta94 THE TRANSPONDER SIGNAL FINDER end to end (docs/v12/TRANSPONDER-FINDER.md): a synthetic RS-44 clip (test-xpdr
#     --write-clip: its beacon, G4ABC's CQ, an SSB voice) through the real server — rows, the beacon's pick opening the
#     transponder, the slice turned by the lock's Doppler, LISTEN → VFO A follows, a manual tune ends it; then the web page.
printf '\n\033[1m── Transponder finder (end to end: server, then the web client) ──\033[0m\n'
#     ★★★ + TRANSPONDER + TELEMETRY (--write-clip-tlm: AO-73's FUNcube BPSK 700 Hz off its listing beside an SSB voice).
if [ -x "$OUT/test-xpdr" ] && [ -n "${VIBESERVER_BIN:-}" ] && "$OUT/test-xpdr" --write-clip "$OUT/xpdr-clip.u8" 2400000 435640000 12 \
   && "$OUT/test-xpdr" --write-clip-tlm "$OUT/xpdr-tlm-clip.u8" 2400000 145950000 145935700 145958000; then
  VIBE_XPDR_CLIP="$OUT/xpdr-clip.u8" VIBE_XPDR_TLM_CLIP="$OUT/xpdr-tlm-clip.u8" node --max-old-space-size=512 scripts/test-server-xpdr.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_XPDR_CLIP="$OUT/xpdr-clip.u8" VIBE_XPDR_TLM_CLIP="$OUT/xpdr-tlm-clip.u8" node --max-old-space-size=512 scripts/test-web-xpdr.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/xpdr-clip.u8" "$OUT/xpdr-tlm-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-xpdr build'; notrun=$((notrun+1))
fi
# ★★★ beta94 FUNCUBE TELEMETRY ON IQ end to end (Stuart 11-25: "or again use iq"): a synthetic FUNcube clip (test-fctlm
#     --write-clip: two frames 0.7 kHz above AO-73's listing) through the real server — the pick's fctlm reads the IQ slice.
printf '\n\033[1m── FUNcube telemetry on IQ (end to end) ──\033[0m\n'
if [ -x "$OUT/test-fctlm" ] && [ -n "${VIBESERVER_BIN:-}" ] && "$OUT/test-fctlm" --write-clip "$OUT/fctlm-clip.u8" 2400000 145935000 145935700; then
  VIBE_FCTLM_CLIP="$OUT/fctlm-clip.u8" node --max-old-space-size=512 scripts/test-server-fctlm-iq.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/fctlm-clip.u8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-fctlm build'; notrun=$((notrun+1))
fi
# ★ …then the web client (headless Edge, --mute-audio): the SATELLITE entry, the timetable, a pick, the readout beside the digits, 390 px
node --max-old-space-size=512 scripts/test-web-sat.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta91 SATELLITE LABELS on the waterfall, end to end: a server under the ISS on 2 m (not SATELLITE mode) sends sat_labels
#    (Doppler-shifted, its capture only, moving), the page draws "ISS Voice/SSTV" in sky blue (headless Edge, --mute-audio),
#    ORBCOMM in sat_list as regular picks (decoder orbcomm, 2026-11-24), the owner's "satlabels" switch and no location send
#    nothing named. Needs VIBESERVER_BIN.
node --max-old-space-size=512 scripts/test-web-sat-labels.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ WHOLE-RADIO MODES HAND BACK THE SERVER'S GEOMETRY (Stuart 11-21, beta89: blank spectrum + the old readout after ADS-B →
#     SATELLITE, 1090000 kHz in NFM, 216928 in SATELLITE): the app's REAL client against a real server through ADS-B / DAB /
#     AIS / ISM / SATELLITE and back, shared dial and one-listener radio — readout = the server's dial, every row drawn in a
#     view at the server's centre; plus the web client's tune-then-mode race. Needs VIBESERVER_BIN for the server half.
printf '\n\033[1m── Whole-radio modes: the client re-syncs to the server ──\033[0m\n'
node --max-old-space-size=512 --no-warnings scripts/test_whole_mode_resync.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ SATELLITE MODE's web window alone against a fake sat_list (no server): order, greying, the synced line, the pick box sizes
#     (compact / big with the decoder canvas lent / ISS SSTV growing), APRS lines, AUTO DOPPLER, EXIT → drop; 390 px. Needs Edge + web/dist.
node --max-old-space-size=512 scripts/test-web-satellite-mode.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ beta96 the web window's WATCH view (single mode: satellites): no EXIT / list, refusals, LIVE, the hybrid, the admin; 390 px
node --max-old-space-size=512 scripts/test-web-sat-single.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★ beta94 SHOW on the web window (fake sat_list): a tick hides the others' passes, a group shortcut, kept across a reload,
#    the pick stays, reminders follow it; default/silver/black, 390 px. Needs Edge + web/dist.
node --max-old-space-size=512 scripts/test-web-sat-filter.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

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
  # ★★ 2026-11-22 APRS AUTO on a listener's own VFO with the squelch closed, and the developer decoder capture
  #    (VIBE_DEC_AUDIO_DUMP) written by the real server and replayed by tool-dec-replay (docs/v12/APRS-ON-AIR-2.md)
  cobj="$(cobjs_for test-dec-dump)" && g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for test-dec-dump) -o "$OUT/tool-dec-replay" \
      "$SRC/tool-dec-replay.cpp" $(deps_for test-dec-dump) $cobj 2>"$OUT/tool-dec-replay.buildlog" || echo '   (tool-dec-replay did not build — the replay half is not run)'
  VIBE_APRS_CLIP="$OUT/aprs-clip.cu8" VIBE_DEC_REPLAY_BIN="$OUT/tool-dec-replay" node scripts/test-server-aprs-capture.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ 2026-11-23 APRS on the ONE-PIPELINE receiver (users 1, a shared dial with a mirror, a strong neighbour 60 kHz up,
  #    the app's silent mute with the DX scanner due) and a listener's own VFO, keyed off-air packets, the clients' AUTO
  #    attach — the clips written by test-aprs-onair --write-clip
  if [ -x "$OUT/test-aprs-onair" ]; then
    VIBE_APRS_ONAIR_BIN="$OUT/test-aprs-onair" node --max-old-space-size=512 scripts/test-server-aprs-shared.mjs; rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
    # ★★ 2026-11-24 the same rows at 960 kS/s — an Airspy HF+'s class of capture rate (the XCover's radio; docs/v12/APRS-WEAK.md)
    printf '   (again at 960 kS/s)\n'
    VIBE_APRS_RATE=960000 VIBE_APRS_ONAIR_BIN="$OUT/test-aprs-onair" node --max-old-space-size=512 scripts/test-server-aprs-shared.mjs; rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  else notrun=$((notrun+1)); echo '   (test-server-aprs-shared not run — test-aprs-onair did not build)'; fi
  # ★★★ 2026-11-24 SHARED DECODERS (Stuart): on a shared dial the room's decoder is told to every arrival, joined with the
  #     server's parameters, closed for ONE listener only and stopped when the last lets go; spots too; a SATELLITE pick's
  #     decoder not listed and not stopped by a box; a per-VFO radio never told (vibe_shared_decoders.h)
  if [ -x "$OUT/test-aprs-onair" ]; then
    VIBE_APRS_ONAIR_BIN="$OUT/test-aprs-onair" node --max-old-space-size=512 scripts/test-server-shared-decoders.mjs; rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
    # …and the web page following it (headless Edge, --mute-audio): opens on arrival, joins, a close stays closed, follows a new one
    VIBE_APRS_ONAIR_BIN="$OUT/test-aprs-onair" node --max-old-space-size=512 scripts/test-web-shared-decoders.mjs; rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  else notrun=$((notrun+1)); echo '   (test-server-shared-decoders not run — test-aprs-onair did not build)'; fi
  # ★★ V12 2026-11-10: a listener's manual gain / AUTO reaches EVERY listener's readout (hwinfo agc + gainNow)
  VIBE_APRS_CLIP="$OUT/aprs-clip.cu8" node scripts/test-server-gain-readout.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 2026-11-11: every audio switch a listener presses is ANSWERED with their hwinfo (no flip-back in a menu)
  VIBE_APRS_CLIP="$OUT/aprs-clip.cu8" node scripts/test-server-switch-reports.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/ais-clip.cu8" "$OUT/aprs-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-ais / test-aprs builds'; notrun=$((notrun+1))
fi

# ★★ V12 9600 PACKET + FX.25 + IL2P (docs/v12/AX25-9600-FX25-IL2P.md): test-ax25hs --write-clip (9600 bursts on 435 MHz:
#     plain AX.25, an FX.25-only telemetry frame, IL2P), the real server on a fake rtl_tcp, a listener in NFM with the
#     packet decoder in AUTO (the channel IQ reaches the 9600 receiver), the frame log, the satellite name, FX.25 off = the
#     FX.25 frame gone, the gallery. Needs VIBESERVER_BIN (exit 3 = not run). Silent.
printf '\n\033[1m── 9600 packet, FX.25, IL2P (end to end: the server) ──\033[0m\n'
if [ -x "$OUT/test-ax25hs" ] && [ -n "${VIBESERVER_BIN:-}" ]; then
  "$OUT/test-ax25hs" --write-clip "$OUT/pkt9600-clip.cu8" 2400000 2>/dev/null
  VIBE_PKT9600_CLIP="$OUT/pkt9600-clip.cu8" node --max-old-space-size=512 scripts/test-server-packet9600.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_PKT9600_CLIP="$OUT/pkt9600-clip.cu8" node --max-old-space-size=512 scripts/test-web-packet9600.mjs; rc=$?   # ★ headless Edge, --mute-audio
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/pkt9600-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN and the test-ax25hs build'; notrun=$((notrun+1))
fi

# ★★★ V12 DECODER REACH + REGION DEFAULTS (vibe_decoder_reach.h): the real server on a fake rtl_tcp — a locked range greys
#     every fixed-frequency decoder WITH its reason and refuses an attach; SAME / DAB off by their region default (never
#     greyed), the owner's "+same" back on, no position = no default. Needs VIBESERVER_BIN (exit 3 = not run). Silent.
printf '\n\033[1m── decoder reach + region defaults (end to end: the server) ──\033[0m\n'
node scripts/test-server-decoder-reach.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
node scripts/test-web-decoder-reach.mjs; rc=$?   # ★ …and the web client's greyed keys (headless Edge, --mute-audio)
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
npx tsx scripts/test_decoder_reach_clients.ts; rc=$?   # ★ …and the clients' gating (the app's keys, the Setup page's real list functions)
if [ $rc -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
npx tsx scripts/test_new_modes_clients.ts; rc=$?   # ★★★ beta96 new modes off: the page's and the app's rule agree, Paul's shape, satCapFrom
if [ $rc -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
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
  # ★★ V12 beta92 THE BAND SKIMMER on 40 m over the same clip: "waiting for callsign", EXPAND's CW text, EXPLAIN, the
  #    waterfall marks and the VFO drawn to the CW window (test-web-band-skim)
  VIBE_CW_CLIP="$OUT/cw-clip.cu8" node scripts/test-web-band-skim.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/cw-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and the bench to build: scripts/test-cw.sh)'; notrun=$((notrun+1))
fi

# ★★★ V12 FreeDV AS A MODE (vibe_freedv.h, docs/v12/FREEDV.md): the real server on a fake rtl_tcp replaying test-freedv's
#     synthetic clip (700D, 700E and 1600 stations made by codec2's own transmitter): AUTO names each, the report, the text,
#     decoded speech on the audio socket, a manual sub-mode, USB again after, a one-pipeline and a range-locked radio, and no
#     mode without libcodec2 (test-server-freedv). NOT RUN without VIBESERVER_BIN or libcodec2 (the clip writer exits 3).
# ★★ A CLIP WRITER IS TODAY'S SOURCE (2026-11-16: a stale $OUT/test-rs41 wrote yesterday's clip). The loop above builds every
#    test-*.cpp, but a build that FAILS leaves the old binary behind — and the end-to-end suites below would then replay a
#    clip from code that is no longer in the tree. So each clip writer is rebuilt here when a source it reads is newer, and
#    removed when that rebuild fails (its suite then reports NOT RUN, never a pass on old code).
fresh_clip_writer() {   # name  source…
  local name="$1"; shift
  local bin="$OUT/$name"
  if [ ! -x "$bin" ] || [ -n "$(find "$SRC/$name.cpp" "$@" -newer "$bin" 2>/dev/null | head -1)" ]; then
    local cobj; cobj="$(cobjs_for "$name")" && g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for "$name") -o "$bin" "$SRC/$name.cpp" $(deps_for "$name") $cobj 2>"$OUT/$name.clipbuildlog" \
      || { rm -f "$bin"; echo "   ($name did not build — its clip is not made: $OUT/$name.clipbuildlog)"; }
  fi
}
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-freedv android/app/src/main/cpp/vibe_freedv.h $RADE_LIBS
printf '\n\033[1m── FreeDV (end to end: the server, then the web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-freedv" ] && "$OUT/test-freedv" clip "$OUT/freedv-clip.cu8" 288000 20 12 >/dev/null \
   && "$OUT/test-freedv" clip "$OUT/freedv-clip-lsb.cu8" 288000 20 12 lsb >/dev/null; then
  VIBE_FREEDV_CLIP="$OUT/freedv-clip.cu8" VIBE_FREEDV_CLIP_LSB="$OUT/freedv-clip-lsb.cu8" node scripts/test-server-freedv.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★ …then the web page (headless Edge, --mute-audio): the key, the VTS line, the FREEDV row, 390 px, LSB on 40 m —
  #   served from this tree's web/dist (node scripts/build-web.mjs), exit 3 without it.
  VIBE_FREEDV_CLIP="$OUT/freedv-clip.cu8" VIBE_FREEDV_CLIP_LSB="$OUT/freedv-clip-lsb.cu8" node scripts/test-web-freedv.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/freedv-clip.cu8" "$OUT/freedv-clip-lsb.cu8"
else
  echo '   not run — needs VIBESERVER_BIN and libcodec2 (apt install libcodec2-1.2 / brew install codec2) or RADE (build-rade.sh)'; notrun=$((notrun+1))
fi

# ★★★ V12 M17 AS A MODE (vibe_m17.h, docs/v12/M17.md): the real server on a fake rtl_tcp replaying test-m17's synthetic clip
#     (our own M17 transmitter from the spec on 433.425 MHz: a voice over with META text, then an SMS, on a 10 s loop): the
#     offer, the report (callsigns, kind, text, the SMS), Codec 2 speech on the audio socket, silence between overs, a
#     one-pipeline and a range-locked radio, NFM again after, and no mode without libcodec2 (test-server-m17); then the web
#     page (test-web-m17, headless Edge --mute-audio). NOT RUN without VIBESERVER_BIN or libcodec2.
printf '\n\033[1m── M17 (end to end: the server, then the web client) ──\033[0m\n'
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-m17 android/app/src/main/cpp/vibe_m17.h android/app/src/main/cpp/vibe_m17_fec.h android/app/src/main/cpp/vibe_freedv.h
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-m17" ] && [ "$("$OUT/test-m17" --probe 2>/dev/null)" = 1 ] && "$OUT/test-m17" clip "$OUT/m17-clip.cu8" 288000 10 20 >/dev/null; then
  VIBE_M17_CLIP="$OUT/m17-clip.cu8" node scripts/test-server-m17.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_M17_CLIP="$OUT/m17-clip.cu8" node scripts/test-web-m17.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/m17-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN and libcodec2 (apt install libcodec2-1.2 / brew install codec2) or RADE (build-rade.sh)'; notrun=$((notrun+1))
fi

# ★★★ V12 THE DRM CONSTELLATION + RECEPTION METER (2026-11-12): the same shape on a SYNTHETIC DRM signal (the test
#     transmitter, vibeserver/drm-synth-clip.cpp) — so it needs no recording: drm_iq only to a socket that asked, ~one per
#     super frame, within budget, clustered on the QAM grid, off again, never to another listener (test-server-drm-
#     constellation); then the web page: the constellation in the Signal pane, the meter in the signal bar's place with
#     the decoder's SNR (test-web-drm-constellation, headless Edge --mute-audio).
printf '\n\033[1m── DRM constellation + meter (synthetic signal: server, then the web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && g++ -std=c++17 -O2 -I android/app/src/main/cpp -I android/app/src/main/cpp/vibedsp vibeserver/drm-synth-clip.cpp \
     -o "$OUT/drm-synth-clip" 2>"$OUT/drm-synth-clip.buildlog" && "$OUT/drm-synth-clip" "$OUT/drm-synth.cu8" 30 24 16 B >/dev/null; then
  VIBE_DRM_SYNTH="$OUT/drm-synth.cu8" node scripts/test-server-drm-constellation.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_DRM_SYNTH="$OUT/drm-synth.cu8" node scripts/test-web-drm-constellation.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/drm-synth.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and the synthetic clip to build: vibeserver/drm-synth-clip.cpp)'; notrun=$((notrun+1))
fi

# ★★★ V12 PSK31/63 (2026-10-26): the same shape — the real server on a fake rtl_tcp replaying the bench's synthetic clip
#     (five PSK stations around 14.071 MHz, one PSK63): the skimmer on a one-pipeline radio and a range-locked one, a
#     second listener's own segment, ONE SIGNAL's text and its spot's RF (test-server-psk); then the web client in
#     headless Edge (--mute-audio) — the PSK button, the list and spots, a tap to tune, ONE SIGNAL, 390 px (test-web-psk).
#     The engine's numbers are vibeserver/test-psk.cpp above; the measuring bench is scripts/test-psk.sh.
printf '\n\033[1m── PSK (end to end: server, then the web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && scripts/test-psk.sh clip "$OUT/psk-clip.cu8" >/dev/null; then
  VIBE_PSK_CLIP="$OUT/psk-clip.cu8" node scripts/test-server-psk.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_PSK_CLIP="$OUT/psk-clip.cu8" node scripts/test-web-psk.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/psk-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and the bench to build: scripts/test-psk.sh)'; notrun=$((notrun+1))
fi

# ★★★ V12 BALLOONS (docs/v12/BALLOONS.md): the real server on a fake rtl_tcp replaying test-balloon's synthetic clip (our own
#     transmitter: a payload on 434.200 MHz USB sending UKHAS at 300 Bd, an SSDV picture at 600 Bd and Horus v2): every report
#     as sent, the picture complete, the manual pins, a one-pipeline and a range-locked radio, the owner's switch
#     (test-server-balloon); then the web client in headless Edge (--mute-audio) — the BALLOONS key, the payload rows, the
#     picture drawn, the pins, 390 px (test-web-balloon). NOT RUN without VIBESERVER_BIN.
printf '\n\033[1m── Balloons (end to end: the server, then the web client) ──\033[0m\n'
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-balloon android/app/src/main/cpp/decoders/balloon_rx.h android/app/src/main/cpp/decoders/balloon_tx.h \
  android/app/src/main/cpp/decoders/balloon_horus.h android/app/src/main/cpp/decoders/balloon_ssdv.h android/app/src/main/cpp/decoders/balloon_ukhas.h
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-balloon" ] && "$OUT/test-balloon" clip "$OUT/balloon-clip.cu8" 288000 60 >/dev/null; then
  VIBE_BALLOON_CLIP="$OUT/balloon-clip.cu8" node scripts/test-server-balloon.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_BALLOON_CLIP="$OUT/balloon-clip.cu8" node scripts/test-web-balloon.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/balloon-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and test-balloon built above)'; notrun=$((notrun+1))
fi

# ★★★ V12 DTMF + 5-TONE (docs/v12/TONES.md): the real server on a fake rtl_tcp replaying test-tones' synthetic NFM clip (our own
#     transmitter: DTMF "*73#1234", a ZVEI-1 call and a CCIR call): on 2 m the digits, on PMR446 only "heard" and the digits in
#     nothing any socket was sent, the owner's 5-tone opt-in reaching the signed-in owner and never a listener's mirror, the
#     pins, a range-locked radio, the owner's switch (test-server-tones); then the web client in headless Edge (--mute-audio) —
#     the key, its options, the lines (test-web-tones). NOT RUN without VIBESERVER_BIN.
printf '\n\033[1m── DTMF + 5-tone (end to end: the server, then the web client) ──\033[0m\n'
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-tones android/app/src/main/cpp/decoders/tones_tx.h android/app/src/main/cpp/decoders/tones_fivetone.h \
  android/app/src/main/cpp/decoders/tones_dtmf.h
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-tones" ] && "$OUT/test-tones" clip "$OUT/tones-clip.cu8" 288000 30 >/dev/null; then
  VIBE_TONES_CLIP="$OUT/tones-clip.cu8" node scripts/test-server-tones.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_TONES_CLIP="$OUT/tones-clip.cu8" node scripts/test-web-tones.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/tones-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and test-tones built above)'; notrun=$((notrun+1))
fi

# ★★★ V12 VOR / ILS and 406 MHz BEACONS (docs/v12/VOR-ILS.md, SARSAT.md): the real server on a fake rtl_tcp replaying our own
#     clips — a VOR "LON" on radial 247.5° (113.60 MHz AM) and three 406 MHz bursts (406.037 MHz): the radial, ident and
#     station, one pipeline and per-listener; the beacon's identity and position to the signed-in owner only (a listener's
#     mirror checked against everything it was sent); the owner's switches (test-server-vorsar). NOT RUN without VIBESERVER_BIN.
printf '\n\033[1m── VOR / ILS and 406 MHz beacons (end to end: the server) ──\033[0m\n'
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-vor android/app/src/main/cpp/decoders/nav_tx.h
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-sarsat android/app/src/main/cpp/decoders/sarsat_tx.h android/app/src/main/cpp/decoders/sarsat_msg.h
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-vor" ] && [ -x "$OUT/test-sarsat" ] \
   && "$OUT/test-vor" clip "$OUT/vor-clip.cu8" 288000 40 25000 247.5 >/dev/null && "$OUT/test-sarsat" clip "$OUT/sarsat-clip.cu8" 288000 25000 >/dev/null; then
  VIBE_VOR_CLIP="$OUT/vor-clip.cu8" VIBE_SARSAT_CLIP="$OUT/sarsat-clip.cu8" node scripts/test-server-vorsar.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/vor-clip.cu8" "$OUT/sarsat-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and test-vor / test-sarsat built above)'; notrun=$((notrun+1))
fi

# ★★★ V12 THE VOR / ILS MAP (docs/v12/VOR-MAP.md): the real server on a fake rtl_tcp synthesising two VORs at their own
#     frequencies (--vor: HON 113.65 in window 3, DTY 116.40 in window 5) at 2.4 MS/s — a radio held off VHF offers none;
#     the capability (5 windows); entering, the sweep stepping window by window, both found in their own windows with their
#     radials, idents and stations; a tune ignored; 1 fps; leaving gives the receiver back exactly; ask before tuning refuses a
#     second listener; the only mode lands a listener and refuses their "off" (test-server-vormap). ~3 min. NOT RUN without
#     VIBESERVER_BIN.
printf '\n\033[1m── VOR / ILS map (end to end: the server) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-server-vormap.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ V12 DV ID — DMR, D-STAR, YSF, NXDN, dPMR signalling, never voice (docs/v12/DV-SIGNALLING.md): the real server on a fake
#     rtl_tcp replaying test-dvsig's synthetic clip (our own transmitter for each system in turn): on 70 cm the identities copy
#     (DMR source, TG, alias and GPS; YSF and D-STAR callsigns; NXDN and dPMR IDs), status and pins, NFM after; on PMR446 the
#     public copy with no identity in anything sent; the owner's opt-in reaching the signed-in owner and never a listener; the
#     owner's switch (test-server-dvsig). NOT RUN without VIBESERVER_BIN.
printf '\n\033[1m── DV ID (end to end: the server) ──\033[0m\n'
[ -n "${VIBESERVER_BIN:-}" ] && fresh_clip_writer test-dvsig android/app/src/main/cpp/vibe_dvsig.h android/app/src/main/cpp/vibe_dvsig_fec.h \
  android/app/src/main/cpp/vibe_dvsig_front.h android/app/src/main/cpp/vibe_dvsig_tx.h
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-dvsig" ] && "$OUT/test-dvsig" clip "$OUT/dvsig-clip.cu8" 288000 20 20 >/dev/null; then
  VIBE_DVSIG_CLIP="$OUT/dvsig-clip.cu8" node scripts/test-server-dvsig.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/dvsig-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and test-dvsig built above)'; notrun=$((notrun+1))
fi

# ★★★ V12 THE RTTY SKIMMER (2026-11-14): the real server on a fake rtl_tcp replaying test-rtty-skimmer's synthetic clip (four
#     RTTY stations of different settings around 14.080 MHz): every station listed with its own settings, listed before
#     set, spotted at its RF, on a one-pipeline and a range-locked radio (test-server-rtty-skim); then the web client in
#     headless Edge (--mute-audio) — RTTY's VIEW › SKIMMER, the rows, the card, 390 px (test-web-rtty-skim).
printf '\n\033[1m── RTTY skimmer (end to end: server, then the web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-rtty-skimmer" ] && "$OUT/test-rtty-skimmer" clip "$OUT/rtty-clip.cu8" 40 >/dev/null; then
  VIBE_RTTY_CLIP="$OUT/rtty-clip.cu8" node scripts/test-server-rtty-skim.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_RTTY_CLIP="$OUT/rtty-clip.cu8" node scripts/test-web-rtty-skim.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★★ V12 beta90: the skimmers' BAND picker in the decoder box (RTTY + CW skimmer, the dial at segment bottom + width/2)
  VIBE_RTTY_CLIP="$OUT/rtty-clip.cu8" node scripts/test-web-skim-band.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/rtty-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and test-rtty-skimmer built above)'; notrun=$((notrun+1))
fi

# ★★ …and a REAL capture, opt-in: VIBE_RTTY_WAV=<stereo int16 I/Q .wav> VIBE_RTTY_WAV_CENTRE=<Hz> replays it through the
#    engine and prints what it listed, the settings, text, calls and CPU (a report, never a pass/fail; the recording is
#    never committed). Not run without it.
if [ -n "${VIBE_RTTY_WAV:-}" ] && [ -x "$OUT/test-rtty-skimmer" ]; then
  printf '\n\033[1m── RTTY skimmer: a real capture (%s) ──\033[0m\n' "$VIBE_RTTY_WAV"
  "$OUT/test-rtty-skimmer" wav "$VIBE_RTTY_WAV" "${VIBE_RTTY_WAV_CENTRE:-0}"; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi
fi

# ★★★ V12 DSC (2026-10-26): the real server on a fake rtl_tcp replaying the bench's synthetic clips — a distress alert and an
#     all-ships safety call on 2187.5 kHz, and the same two as an FM subcarrier on channel 70: a listener's MF/HF and VHF
#     decoder, the gallery's day log and pinned distress item, and background decoding on a DSC VFO (test-server-dsc); then
#     the web client in headless Edge (--mute-audio) — the DSC button, its channel keys, the coloured rows, MAP, a channel
#     key's tune, the gallery's DSC chip, 390 px (test-web-dsc). The engine's numbers are vibeserver/test-dsc.cpp above;
#     the measuring bench is scripts/test-dsc.sh. Needs VIBESERVER_BIN.
printf '\n\033[1m── DSC (end to end: server, then the web client) ──\033[0m\n'
# ★ beta88 the web half runs on the `pos` clip (+ a routine position reply): the gallery's SHIPPING MAP row and its map.
if [ -n "${VIBESERVER_BIN:-}" ] && scripts/test-dsc.sh clip "$OUT/dsc-hf.cu8" >/dev/null && scripts/test-dsc.sh clip "$OUT/dsc-vhf.cu8" vhf >/dev/null \
   && scripts/test-dsc.sh clip "$OUT/dsc-pos.cu8" pos >/dev/null; then
  VIBE_DSC_CLIP="$OUT/dsc-hf.cu8" VIBE_DSC_CLIP_VHF="$OUT/dsc-vhf.cu8" node scripts/test-server-dsc.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_DSC_CLIP_POS="$OUT/dsc-pos.cu8" node scripts/test-web-dsc.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/dsc-hf.cu8" "$OUT/dsc-vhf.cu8" "$OUT/dsc-pos.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and the bench to build: scripts/test-dsc.sh)'; notrun=$((notrun+1))
fi

# ★★★ V12 SITOR-A (2026-10-29): the real server on a fake rtl_tcp replaying a synthetic link on 4209.5 kHz (tools/sitor-bench/
#     sitor_clip — call, identification, traffic both ways, the end), and the web client in headless Edge (--mute-audio): the
#     key in the HF Maritime group, both stations' labelled passages, the status line, every repeat printed once, 390 px.
#     The engine's numbers are vibeserver/test-sitora.cpp; the measuring bench is tools/sitor-bench/sitor_bench.sh.
# ★★ V12 EAS / SAME and SELCAL in the WEB client (2026-10-31): a real server fed tools/alert-bench/alert_clip, headless Edge.
printf '\n\033[1m── SAME + SELCAL (end to end: the web client on a real server) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && c++ -std=c++17 -O2 -I android/app/src/main/cpp -o "$OUT/alert_clip" tools/alert-bench/alert_clip.cpp android/app/src/main/cpp/decoders/selcal_decoder.cpp \
   && "$OUT/alert_clip" "$OUT/alerts.cu8" >/dev/null; then
  VIBE_ALERT_CLIP="$OUT/alerts.cu8" node scripts/test-web-alerts.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else notrun=$((notrun+1)); fi
# ★★ V12 MFSK / OLIVIA / THOR in the WEB client (2026-10-31): a real server fed tools/textmode-bench/tm_clip, headless Edge.
printf '\n\033[1m── MFSK + OLIVIA + THOR (end to end: the web client on a real server) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && cc -O2 -c -o "$OUT/pffft_tm.o" android/app/src/main/cpp/vibedsp/third_party/pffft/pffft.c \
   && c++ -std=c++17 -O2 -I android/app/src/main/cpp -o "$OUT/tm_clip" tools/textmode-bench/tm_clip.cpp android/app/src/main/cpp/decoders/textmodes.cpp android/app/src/main/cpp/decoders/mt63.cpp android/app/src/main/cpp/decoders/ale_decoder.cpp "$OUT/pffft_tm.o" \
   && "$OUT/tm_clip" "$OUT/tm.cu8" >/dev/null; then
  VIBE_TM_CLIP="$OUT/tm.cu8" node scripts/test-web-textmodes.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else notrun=$((notrun+1)); fi
# ★★ V12 FELD HELL in the WEB client (2026-10-31): a real server fed tools/hell-bench/hell_clip, headless Edge.
printf '\n\033[1m── FELD HELL (end to end: the web client on a real server) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && cc -O2 -c -o "$OUT/pffft_hell.o" android/app/src/main/cpp/vibedsp/third_party/pffft/pffft.c \
   && c++ -std=c++17 -O2 -I android/app/src/main/cpp -o "$OUT/hell_clip" tools/hell-bench/hell_clip.cpp android/app/src/main/cpp/decoders/feld_hell.cpp "$OUT/pffft_hell.o" \
   && "$OUT/hell_clip" "$OUT/hell.cu8" >/dev/null; then
  VIBE_HELL_CLIP="$OUT/hell.cu8" node scripts/test-web-hell.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/hell.cu8"
else notrun=$((notrun+1)); fi
printf '\n\033[1m── SITOR-A (end to end: the web client on a real server) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && c++ -std=c++17 -O2 -I android/app/src/main/cpp -I tools/sitor-bench -o "$OUT/sitor_clip" tools/sitor-bench/sitor_clip.cpp android/app/src/main/cpp/decoders/sitor_a.cpp android/app/src/main/cpp/decoders/same_decoder.cpp android/app/src/main/cpp/decoders/selcal_decoder.cpp \
   && "$OUT/sitor_clip" "$OUT/sitora.cu8" >/dev/null; then
  VIBE_SITORA_CLIP="$OUT/sitora.cu8" node scripts/test-web-sitora.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/sitora.cu8"
else
  echo '   not run — needs VIBESERVER_BIN'; notrun=$((notrun+1))
fi

# ★★★ V12 JTTY end to end (2026-10-26): the real server on a fake rtl_tcp replaying the bench's 20 m clip (two JTTY
#     stations, a CQ and an exchange, overlapping) — vibeserver.json "jtty", the JTTY button and band keys, both stations
#     as rows lowest frequency first with their calls and confidence colours, the SPOTS, a tap and a band key tuning,
#     390 px (test-web-jtty, headless Edge, --mute-audio). The engine's own numbers are vibeserver/test-jtty.cpp above;
#     the measuring bench is scripts/test-jtty.sh. Needs VIBESERVER_BIN.
printf '\n\033[1m── JTTY (end to end: server and web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && scripts/test-jtty.sh clip "$OUT/jtty-clip.cu8" >/dev/null; then
  VIBE_JTTY_CLIP="$OUT/jtty-clip.cu8" node scripts/test-web-jtty.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  rm -f "$OUT/jtty-clip.cu8"
else
  echo '   not run — needs VIBESERVER_BIN (and the bench to build: scripts/test-jtty.sh)'; notrun=$((notrun+1))
fi

# ★★★ V12 BAND SPOTS AND THE TUNER'S DC (2026-10-27, v12-band-dc): a fake rtl_tcp with the dongle's 15 kHz offset tuning
#     (VIBE_TCP_DC_OFFSET_HZ), a dirty DC at its tuned centre and six FT8 stations round 40 m's 7.0756 — BEFORE
#     (VIBE_BAND_DC=0) the DC sits on FT8 and Band decodes nothing; AFTER the tuner moves the DC outside the slices, all six
#     are spotted at their RF, a second listener's USB tone stays within 1 Hz through Band on and off, and Band off puts
#     the tuner back. Needs VIBESERVER_BIN.
printf '\n\033[1m── Band spots and the tuner DC (end to end) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-digi-spots" ] && [ -x "$OUT/test-band-dc" ]; then
  VIBE_DIGI_BIN="$OUT/test-digi-spots" VIBE_DC_BIN="$OUT/test-band-dc" node --max-old-space-size=512 scripts/test-server-band-dc.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else
  echo '   not run — needs VIBESERVER_BIN (and test-digi-spots / test-band-dc built above)'; notrun=$((notrun+1))
fi

# ★★★ V12 DIGITAL SPOTS end to end (2026-10-26): a UTC-anchored 20 m clip (test-digi-spots clip) through the real server —
#     Band on a one-pipeline and a range-locked radio (every FT8 / FT4 station with its mode at its RF, the lower sideband
#     never), Single on the listener's audio, the owner's "spotsband" switch (test-server-spots); then the web client in
#     headless Edge (--mute-audio) — the two buttons, band stepping, the band readout and drawn span, the Mode column and
#     the map coloured by mode with its legend, desktop and 390 px (test-web-spots). Needs VIBESERVER_BIN.
printf '\n\033[1m── digital spots (end to end: server, then the web client) ──\033[0m\n'
if [ -n "${VIBESERVER_BIN:-}" ] && [ -x "$OUT/test-digi-spots" ]; then
  VIBE_DIGI_BIN="$OUT/test-digi-spots" node scripts/test-server-spots.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  VIBE_DIGI_BIN="$OUT/test-digi-spots" node scripts/test-web-spots.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
  # ★ Band chosen while in DAB leaves DAB and tunes the band; the arrows then step bands, not multiplexes (2026-10-27)
  VIBE_DIGI_BIN="$OUT/test-digi-spots" node --max-old-space-size=512 scripts/test-web-spots-from-dab.mjs; rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
else
  echo '   not run — needs VIBESERVER_BIN (and test-digi-spots built above)'; notrun=$((notrun+1))
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

# ★★★ beta92 THE 8-SECOND SHUTDOWN HANG (docs/v12/SHUTDOWN-HANG.md; the Lenovo, 12 times in 3 days): SIGTERM 0.3–2.5 s after
#     start, after running with a listener, and with a static destructor that never returns (VIBE_TEST_EXIT_WEDGE) — each
#     exits in well under 3 s without the watchdog. Same VIBESERVER_BIN rule. ~40 s.
printf '\n\033[1m── a quick stop, however soon after start (end to end) ──\033[0m\n'
node --max-old-space-size=512 scripts/test-server-quick-stop.mjs 1; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ A RADIO THAT FALLS OVER AT A RATE A MODE BOOSTED IT TO (vibe_rate_guard.h, 2026-10-24): a fake rtl_tcp that stops
#     at 2.4 MS/s — ADS-B left, the radio back at its base rate, the switch persisted off; then a planted crash marker
#     found at start, recorded, cleared. Same VIBESERVER_BIN rule. ~30 s.
printf '\n\033[1m── boosted-rate stall and crash (end to end) ──\033[0m\n'
node scripts/test-rate-guard.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
# ★★★ THE DAB+ DECODER CHOICE IS GONE (V12, 2026-10-29 — FAAD2 is the only one): the real setup page in headless MUTED Edge,
#     a front door + the radio's own process (as systemd runs them), started from an OLD config carrying dabAacDecoder /
#     dabFaad2: both start, no choice is drawn, hwinfo has no old fields, and through a save and the radio persisting its
#     own state the keys are kept as written (unknown keys are preserved) and never acted on. ★ Bounded: 512 MB heap, its own 1 GB / 2 GB-per-child memory watch and a 180 s
#     deadline. ~40 s.
printf '\n\033[1m── an old config with the DAB+ decoder choice loads; the choice is gone (setup page, end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-setup-dab-faad2.mjs; rc=$?
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

# ★★★ AN AUDIO SOCKET WITH NO WATERFALL KEEPS ITS LISTENER'S TURN (2026-10-28, V12 two servers at once: the background
#     server's audio-only socket was invisible to the shared dial's limit — Stuart's "sneaky time limit bypass"). Shared
#     dial, hard 1-minute limit: audio-only listener warned + ended at the end of the ORIGINAL turn; a waterfall back
#     after time in the background gets what is LEFT; another listener id behind the same address gets its own turn.
#     ~80 s, so only with VIBESERVER_SLOW=1.
printf '\n\033[1m── session turn: an audio-only listener is present (end to end) ──\033[0m\n'
if [ "${VIBESERVER_SLOW:-0}" = "1" ]; then
  node scripts/test-session-audio-presence.mjs; rc=$?
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
# ★★★ THE TUNNEL ADDRESS, ANDROID (VibeQuickTunnelUrl.kt, 2026-10-27): cloudflared's failure line is never an address —
#     Kiko's TV box read api.trycloudflare.com out of it and recycled every 15 s. Same lines as test-tunnel-url.cpp.
printf '\n\033[1m── tunnel address, Android (VibeQuickTunnelUrl) ──\033[0m\n'
bash scripts/test-tunnel-url-android.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ beta96 SUITES THE BRANCHES' DOCS NAME BUT NOBODY REGISTERED (beta96 prep, 11-26) — each runs alone as its doc says;
#     exit 3 = not run (no VIBESERVER_BIN / no Edge / no CMake target built).
printf '\n\033[1m── beta96: the standalone AIR & SEA map + aircraft deltas, clients (test_airsea_viewer, ADSB-FEED.md) ──\033[0m\n'
npx tsx scripts/test_airsea_viewer.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
npx tsx scripts/test_airsea_standalone.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
npx tsx scripts/test_aircraft_feeds_app.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96 follow-up: the phone\x27s feeds card — FIND, ADD tests first, ADD ANYWAY, no dead buttons (FEED-TEST.md) ──\033[0m\n'
node scripts/test_aircraft_feeds_setup.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: the two live maps\x27 rules (test_live_maps, ONE-MAP.md) ──\033[0m\n'
npx tsx scripts/test_live_maps.ts; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: Setup order (test-setup-order, SETUP-UX.md) ──\033[0m\n'
node scripts/test-setup-order.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: map packs read straight out of the APK (test-apk-pack, VibeApkPack.kt) ──\033[0m\n'
bash scripts/test-apk-pack.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: ADS-B kept running survives an in-process restart (test-adsb-restart, ADSB-RESTART-RATE.md) ──\033[0m\n'
BUILD="$(dirname "${VIBESERVER_BIN:-vibeserver/build/x}")" bash scripts/test-adsb-restart.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: the Linux daemon enters ADS-B on every start (test-adsb-restart-linux) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" bash scripts/test-adsb-restart-linux.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: kept-running modes survive a restart (test-keep-restart, KEEP-RUNNING.md) ──\033[0m\n'
BUILD="$(dirname "${VIBESERVER_BIN:-vibeserver/build/x}")" bash scripts/test-keep-restart.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: the satellite tracker stops with the server across in-process restarts (test-sattrack-restart) ──\033[0m\n'
BUILD="$(dirname "${VIBESERVER_BIN:-vibeserver/build/x}")" bash scripts/test-sattrack-restart.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: keep running a whole-radio mode (test-server-keep-run) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-keep-run.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: background decoding split from the landing (test-server-bg-split) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-bg-split.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96: the Netherlands\x27 water + rivers on a server\x27s GPU map (test-server-map-water-nl, BASIC-MAP-WATER.md) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node --max-old-space-size=512 scripts/test-server-map-water-nl.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── ADS-B RANGE outline on the web map (test-web-adsb-range) ──\033[0m\n'
node --max-old-space-size=512 scripts/test-web-adsb-range.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi
printf '\n\033[1m── beta96 the right GLYPH for every aircraft, any source (test-web-ac-icons) ──\033[0m\n'
node --max-old-space-size=512 scripts/test-web-ac-icons.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

# ★★★ ONE VERSION, EVERYWHERE IT IS WRITTEN DOWN. app.json does NOT reach the iOS build — the
#     pbxproj owns MARKETING_VERSION and only `expo prebuild` would copy it across, which this
#     project deliberately never runs — so the App Store shipped 10.2 while the app's own About
#     overlay, its User-Agent and the Android build all said 10.3. Caught only because a build was
#     inspected by hand before submitting.
if node scripts/check-versions.mjs; then pass=$((pass+1)); else fail=$((fail+1)); echo "!!!! SUITE FAILED (the one whose output is just above)"; fi

printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build, %d not run\033[0m\n' "$pass" "$fail" "$broke" "$notrun"
[ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]
