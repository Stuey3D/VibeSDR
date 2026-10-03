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
    # ★ The Advanced RDS instrument (MpxMeasure) against a signal of known deviation, at several
    #   capture rates and passbands — the real pipeline end to end, so the same deps again.
    test-mpx-measure)      echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ WFM DSP cost vs passband — a narrow width once cost ~5x the whole chain (the Sony's
    #   161-276 % of real time). Optimised like the server, or the ratio is not the server's.
    test-wfm-narrow-cost)  echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ Every mode's DSP cost vs passband, both directions and past the ceiling — the audit that
    #   followed the WFM cliff (2026-09-29). Optimised for the same reason.
    test-passband-cost)    echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ The real decoders on real signals (RTTY, WEFAX, an encoded FT8 slot) — optimised so FT8's
    #   slot decode finishes in a second rather than ten.
    test-decoder-hosts)    echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ The benchmark's decoder rows — optimised like the server, or the costs are not the server's.
    test-bench-decoders)   echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ Hostile attach messages through the real decoders (audit 2026-10-03) — same deps as the hosts.
    test-decoder-hardening) echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
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
    test-multipath-meter) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-mpx-measure)     echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-wfm-narrow-cost) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-passband-cost)   echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    # ★★ proc.cpp goes with anything that SHELLS OUT (curl, mostly): geoip, asndb and radiodns all
    #    call vibeproc::run. It was missing, so those three "did not build" — and a suite that does
    #    not build is not a suite that passes, it is one nobody is running. Caught 2026-09-21.
    test-config-radios) echo "$SRC/vibeserver_config.cpp" ;;
    test-converter)     echo "$SRC/vibeserver_config.cpp" ;;
    test-rtl-eeprom)    echo "$SRC/rtl_eeprom.cpp" ;;
    test-fd-passing)    echo "android/app/src/main/cpp/fd_passing.cpp" ;;
    test-parent-watch)  echo "$SRC/parent_watch.cpp" ;;
    test-connlog)       echo "" ;;
    # ★★★ An admin read must never hold the connection log's lock while it works (Pi 2 IQ overruns
    #     with the admin page open, 2026-10-01). Header-only; slow resolvers make it CPU-independent.
    test-connlog-lock)  echo "" ;;
    # ★★ The map's shared byte budget (vibe_bulk_pace.h), on a synthetic clock.
    test-bulk-pace)     echo "" ;;
    test-time-decoder)  echo "android/app/src/main/cpp/decoders/time_decoder.cpp" ;;
    test-radiodns-ecc)  echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-radiodns-name) echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-geoip)         echo "$SRC/geoip.cpp $SRC/proc.cpp" ;;
    test-asndb)         echo "$SRC/asndb.cpp $SRC/proc.cpp" ;;
    test-admin-banlist) echo "" ;;
    test-decoder-hardening) echo "android/app/src/main/cpp/spyserver/spyserver_messages.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-decoder-hosts|test-bench-decoders) echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    *)                  echo "" ;;
  esac
}

# test name -> C sources it needs. ★ Compiled as C, separately: g++ would compile a .c file as C++,
#   and ft8_lib is C that C++ refuses (malloc without a cast). Prints the object files built.
FT8C=android/app/src/main/cpp/ft8_lib
cobjs_for() {
  case "$1" in
    test-decoder-hosts|test-bench-decoders|test-decoder-hardening)
      local d="$OUT/cobj-ft8"; mkdir -p "$d"
      for f in $FT8C/ft8/*.c $FT8C/fft/kiss_fft.c $FT8C/fft/kiss_fftr.c $FT8C/common/monitor.c; do
        local o="$d/$(basename "$f" .c).o"
        [ "$o" -nt "$f" ] || cc -O2 -c -I "$FT8C" -o "$o" "$f" || return 1
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
  case "$name" in
    test-radio-api) printf '\n\033[1m── %s ──\033[0m\n   \033[2mskipped — a CMake target (needs the whole core); build it with cmake\033[0m\n' "$name"; continue ;;
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
  if "$OUT/$name"; then pass=$((pass+1)); else fail=$((fail+1)); fi
done

# ★ The setup page is a C++ raw string, so no compiler ever looks at its JavaScript. A syntax
#   error there ships silently and the page simply does nothing — the worst failure this tree has,
#   because that page is what a new owner meets first.
printf '\n\033[1m── setup page ──\033[0m\n'
if node scripts/check-setup-page.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ Fork labelling (directory forkOf): a fork's name is shown, checked; official names refused as fork names.
if node --no-warnings directory/scripts/test-fork.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ Behaviour, not syntax: the page's own functions driven against a stubbed DOM, because the bug
#   this catches — one radio's sample rate landing in another's config during a tab switch — is
#   perfectly valid JavaScript and perfectly well-formed HTML.
if node scripts/test-setup-tabswitch.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The admin log folds a visit's per-radio rows into one. Pure logic, so it is testable without a
#   browser — and the cases that matter are the ones it must NOT fold (session-less refusals).
if node scripts/test-visit-grouping.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ A DATA MIGRATION, so it is tested: bookmarks move from being keyed on the URL you arrived by
#    to the server's own identity. The failure modes are somebody's bookmarks going invisible on
#    upgrade, or one server adopting another's — neither of which looks wrong in a screenshot.
if node scripts/test-bookmark-scope.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE NR SLIDER'S TWO CONVERSIONS MUST BE EXACT INVERSES. The server echoes the strength it is
#     using and the client renders it back onto the slider, then dispatches `input` — which sends
#     it again. In that loop a units mismatch is not an off-by-a-bit, it is a RATCHET: the setting
#     climbed 30 -> 100% on its own, on air (2026-08-26). Neither half looks wrong on its own.
if node scripts/test-nr-roundtrip.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The two ends of the advanced-RDS message must agree on field NAMES. A stray "R." prefix meant
#   the deviation readout never populated at all, and neither half looked wrong on its own.
if node scripts/check-rdsx-wire.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The web playout (worklet AND the main-thread fallback) must come back clean by itself after the
#   server has delivered audio in bursts — the Sony's narrow-WFM overload, 2026-09-29. Silent.
if node scripts/test-web-playout-burst.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The web decoder socket says whose it is (user_session_id) and takes a refusal — shown in the
#   server's words and FORGOTTEN, so the 3-s reconnect cannot turn one "no" into a loop (B6).
if node scripts/test-web-decoder-refusal.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE WEB CONTROL COLOURS (controlColours.ts): the default look sets no variable (today, to the
#    pixel), every swatch combination stays ≥ 3:1, SOLID is opaque with no backdrop-filter anywhere, the
#    keys travel as VIEW_KEYS, and an older browser starts solid unless a stored choice says otherwise.
if node --no-warnings scripts/test-web-control-colours.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE DESKTOP BAR STAYS GONE. The compact card (#mcard) is the web layout at every width; the old
#     bar shipped hidden for months, still wired, and fixes landed on it that reached nobody. Checks the
#     BUILT page (web/dist — run node scripts/build-web.mjs): no removed id as an element, a selector or
#     a script literal, and the bar's real leftovers (#linkStats, search, bandwidth row) where they live.
if node scripts/test-web-no-desktop-bar.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE FACEPLATE RULES (app). Nixie One is neon in every chassis × display × colour, dot/seg can
#     never reach white by any route (picker, migration, stored prefs), and the DEFAULT settings
#     resolve to today's literal colours — the pixel-identical promise, checked as data.
#  ★ Node runs the .ts directly (type stripping), so this needs nothing installed.
if node --no-warnings scripts/test_faceplate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ WHAT A VFD CAN SHOW (faceplates §7). Every printable ASCII character is exactly one DSEG14
#    cell (its space is 200 wide and its ! is the BLANK cell), accents fold like a display ROM,
#    units keep their case, and a string that folds to nothing shows the frequency, never tofu.
if node --no-warnings scripts/test_faceplate_text.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ NIXIE: real hardware never adds or removes a tube. Units change only the lit bulb, leading
#     zeros are switched off, and the TUBE shrinks to its window (smallest window first) — domes
#     are never clipped.
if node --no-warnings scripts/test_faceplate_nixie.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE METERS (faceplates §4): one deck height across meter × shared, the squelch ring on the SAME
#     table the LEDs light from, Φ((μ − T)/σ) with its σ floor, the eye filter as the only easing, the
#     steady-LED hysteresis, and the needle's 300 ms / 1 % ballistics — the maths, checked as data.
if node --no-warnings scripts/test_faceplate_meters.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ WORKLETS AS THE UI THREAD RUNS THEM (the 11 B7 LED VU crash): a default parameter that names a
#     module constant THROWS on the UI thread (the plugin unpacks captures inside the body), so every
#     worklet in src/ is compiled with the app's plugin and checked, and the meters' frame callbacks are
#     rebuilt from the plugin's output with no module scope and run — the test above cannot see it.
if node --no-warnings scripts/test_worklet_defaults.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ No photo leaves this repo carrying EXIF — GPS above all (2026-10-03: twelve did, two of them on the live website).
if node --no-warnings scripts/test_safe_text.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ Untrusted URLs (deep links, directories, a server's landing link) and the map page's literal/path guards.
if node --no-warnings scripts/test_safe_url.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ A gzipped spectrum-socket frame cannot inflate past its ceiling (gzip bomb).
if node --no-warnings scripts/test_bounded_inflate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ The map WebView page parses, for all three kinds (a template-literal page tsc cannot check).
if node scripts/check-map-overlay.mjs >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ node scripts/check-map-overlay.mjs"; fi
echo "── image metadata (EXIF / GPS / XMP) ──"
if python3 scripts/strip-image-metadata.py --check; then pass=$((pass+1)); echo "  none"; else fail=$((fail+1)); echo "  ✗ run: python3 scripts/strip-image-metadata.py"; fi
# ★★★ A FACEPLATE CANNOT LOCK YOU OUT (faceplate.ts CRASH SAFETY): the mark is armed before a risky
#     faceplate draws and cleared after 5 s / on leaving the foreground; a launch that finds it comes up
#     on HYPER / BAR / DEFAULT with the choice kept aside — a crash, a clean exit, a swipe-away, a torn
#     mark and a refusing disk, across fake launches.
if node --no-warnings scripts/test_faceplate_safety.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE DRUM WELLS (faceplates §6): the default well is today's drum as data, the needle is gone on
#    every chassis, the aluminium notch pair inverts (and its draw order is a token), the LED pool's
#    geometry, the controls colour at the brief's brightness, and the tuner keys' 31 % / 34 % layout.
if node --no-warnings scripts/test_faceplate_wells.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE LANDSCAPE DECK (faceplates §9): never taller than today's band at 568 → 1366 pt for every
#     chassis × meter × shared, four equal keys, no negative or overlapping column, the drums' 80 pt,
#     real Nixie tubes that fit the window, and the SE's fall-backs (no labels; the bar).
if node --no-warnings scripts/test_faceplate_landscape.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE STEREO LIGHT IN DAB is the playing service's (its audio headers), never the FM pilot's — the
#    server's `stereo` and, for an older server, its codec line; app and web share the one function.
if node --no-warnings scripts/test_dab_stereo.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ An unchanged DAB service list keeps its array and objects across the once-a-second reports.
if node --no-warnings scripts/test_dab_share_services.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_sprite_sizing.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_sprite_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The waterfall jitter buffer's pooled frame copies (framePool.ts): independent copies, reuse, bounded.
if node --no-warnings scripts/test_frame_pool.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_platformCopy.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_grid.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node scripts/test_derived_values_pure.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE DECODER BOXES (faceplates §10.2): every text role ≥ 4.5:1 (WCAG luminance, composited over
#    what it sits on) in every chassis × controls colour × text colour × Transparency on/off; the
#    meaning colours never move, the text colour never reaches a box, no blur on silver / black or OFF.
if node --no-warnings scripts/test_decoder_contrast.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ TRANSPARENCY EFFECTS: each low-end signal alone turns the default OFF, a stored choice always
#     wins and the auto default is never saved as chosen, decoderBg migrates (solid = chosen OFF,
#     transparent = NOT chosen), OFF is alpha 1.0 exactly with no scrim or drop shadow, no darker/lighter than the glass on a
#     dark waterfall, and every file that draws a BlurView reads the switch.
if node --no-warnings scripts/test_transparency.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ FRAME RATE (power audit 2026-10-01): the 60 Hz row is hidden on a 60 Hz panel and when the binary
#     cannot say, labelled with the panel's real rate, defaults to no cap, survives the crash reset,
#     and both native modules export the two methods JS calls.
if node --no-warnings scripts/test_frame_rate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ MAC VOLUME + MUTE (2026-10-01): shown only on iOS-on-a-Mac, unity gain everywhere else, MUTE is gain 0,
#     every native audio ingress plays through the one mixer that carries the gain, recordings are taken
#     before it, and the deck's speaker key shows a prohibition sign in the legend colour (never red).
if node --no-warnings scripts/test_mac_audio.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ POPUPS TAKE THE CHASSIS (faceplates §10.3): the default chassis is today's gold, value for value;
#     no popup holds a hard-coded gold; engraved text clears 4.5:1 on silver and black at the plate's
#     worst lighting; the tune entry follows the Display (Nixie One only ever neon); and with
#     Transparency OFF no popup dims the waterfall or blurs it.
if node --no-warnings scripts/test_popup.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE CANNED CHAT PAD (chatPad.ts, capSheen.ts): the phrase pad scrolls inside a cap that never
#     overflows the drawer on any phone (the SE lost phrases 7–14 below its edge), and a dome key's
#     sheen is flex shares, never percentage heights (Yoga drew ghost slabs over a multi-line wrap).
if node --no-warnings scripts/test_chat_pad.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ SHARE A STATION (canned chat, app + web): a bookmark's LABEL never reaches the payload, the server's
#     relayed line is what is drawn, and TUNE asks first on a shared dial somebody else is on. The server half
#     (validation, naming, the line itself) is vibeserver/test-chat-share.cpp, run with the C++ tests above.
if node --no-warnings scripts/test_chat_share.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE SMALL-SCREEN STATUS ROW (faceplates §8.2): dropped strictly in order (IF first, the recording
#     timer last), SHARED TUNER shortens before it goes, the connection meter never goes, portrait never
#     drops, the row packs before anything drops, and a 1 pt wobble cannot flap (hysteresis).
if node --no-warnings scripts/test_faceplate_status.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ FACEPLATE LIGHTING + VFD GLASS (briefs/BRIEF-lighting-and-vfd-glass.md): the filament wires (count by
#    height, device-pixel snapped), the one light angle (LEFT = today exactly), MOTION EFFECTS, LIGHT ANGLE.
if node --no-warnings scripts/test_faceplate_lighting.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE BLIND TUNES OBEY THE SHARED DIAL (blindTuneGate.ts): lock-screen / headset / car ⏮⏭, a car
#     pick and Siri are refused on a shared dial with others listening (and on spectator / listen-only),
#     as FM-DX's always were; alone or exclusive they work; the native switch and a press agree.
if node --no-warnings scripts/test_blindTuneGate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ "IS THIS ACTUALLY NEWS?" (renderChurn.ts): a repeated RDS label / bookmark list keeps the old
#    object so the radio screen does not re-render for it, any drawn change still gets through, and
#    the controls-bar clock re-renders on the minute, not every second (power audit, 2026-10-01).
if node --no-warnings scripts/test_renderChurn.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ CONNECTION REFRESH (connectionRefresh.ts): every backend the radio screen hosts gets one, the
#    audio it restarts matches the screen's own mount gates (native Opus after re-registration, the
#    VibeServer/dongle pump, the OWRX/Kiwi adapter), and the row is hidden wherever it is a no-op.
if node --no-warnings scripts/test_connectionRefresh.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE VTS STATION LINE (vtsLine.ts), app AND web: "PI: C363 / Name: RadioText" with no dangling
#    punctuation for a missing part, DAB's SId never called a PI, the coloured runs identical to the
#    plain line, and the web pill dropping band → RDS mark → flag only as far as its measured widths
#    demand (2026-10-01).
if node --no-warnings scripts/test_vtsLine.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE SHARED CHUNK CACHE (chunkCache.ts / chunkVerify.ts / directory store.html): a stored map or
#     admin chunk runs ONLY if it hashes to this server's own page; the build's embedded hash is the
#     served file's (web/dist AND vibe_web_page.h); server B loads from the store with zero fetches; a
#     tampered entry is refused and refetched; the store answers only *.vibeserver.vibesdr.net. Needs
#     web/dist (node scripts/build-web.mjs).
if node --no-warnings scripts/test_chunk_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi

# ★★★ THE REAL SERVER, END TO END (B6): per-listener decoders on a locked range, the decoder limit's
#     refusal, Advanced RDS only to whoever asked on a shared dial, and an audio socket that opens
#     first keeping its codec — through the same WebSockets the clients use, against fake-rtl-tcp.
#  ★ It needs a vibeserver BUILT FROM THIS TREE (VIBESERVER_BIN=…). Without one it is reported as
#    NOT RUN — counted separately, never as a pass: a stale binary would test yesterday's server.
printf '\n\033[1m── server decoders (end to end) ──\033[0m\n'
node scripts/test-server-decoders.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ SHARE A STATION THROUGH THE REAL SERVER: two listeners on a shared dial; the line the room receives is
#     named from the receiver's own store, a smuggled label is not relayed, out-of-range / closed-mode shares
#     are refused with a reason, flood control covers shares. Same VIBESERVER_BIN rule: not run without one.
printf '\n\033[1m── chat share (end to end) ──\033[0m\n'
node scripts/test-server-chat-share.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ THE WEB CLIENT, AS THE SERVER HANDS IT OUT: the page and every /vs/ script, in every encoding,
#     byte for byte against web/dist, `immutable` on the scripts and no-store on the page, through a
#     /r/<id>/ prefix as well. Same VIBESERVER_BIN rule as above: not run without one.
printf '\n\033[1m── web client serving (end to end) ──\033[0m\n'
node scripts/test-web-serving.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A PERMANENT SCROLLBAR NEVER COVERS OR SQUEEZES A PANEL'S CONTROLS (Stuart, 2026-10-01: Windows /
#     Edge keeps the bar on screen). Every panel at 390 and 1280 px with a classic 15 px bar forced on:
#     the gutter is reserved, nothing scrolls sideways, every control ends left of the bar. Headless
#     Edge, --mute-audio, throwaway profile. Same VIBESERVER_BIN rule.
printf '\n\033[1m── web panels: the scrollbar lane (real browser) ──\033[0m\n'
node scripts/test-web-scrollbar-lane.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ THE SHARED CHUNK CACHE IN A REAL BROWSER, ACROSS REAL SERVERS: two vibeservers behind a local
#     https front as a/b/c.vibeserver.vibesdr.net, this tree's store.html as the directory's. Server B
#     opens the map and the admin panel with ZERO requests for either; a tampered IndexedDB entry is
#     not run on C. Headless Edge, --mute-audio, throwaway profile. Same VIBESERVER_BIN rule.
printf '\n\033[1m── shared chunk cache (real browser) ──\033[0m\n'
node scripts/test-web-chunk-cache.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

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
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ MODERN ROOTS FOR OLD PHONES (B10: a Lite on Android 5.1 could not fetch RIPE/APNIC, so Europe had
#     no flags). VibeTls.kt itself, on the desktop JVM, against the real chains, with an old store
#     simulated — and still refusing expired / self-signed / untrusted / wrong-host. Needs the network.
printf '\n\033[1m── TLS roots for old Android (VibeTls) ──\033[0m\n'
bash scripts/test-tls-roots.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ ONE VERSION, EVERYWHERE IT IS WRITTEN DOWN. app.json does NOT reach the iOS build — the
#     pbxproj owns MARKETING_VERSION and only `expo prebuild` would copy it across, which this
#     project deliberately never runs — so the App Store shipped 10.2 while the app's own About
#     overlay, its User-Agent and the Android build all said 10.3. Caught only because a build was
#     inspected by hand before submitting.
if node scripts/check-versions.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi

printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build, %d not run\033[0m\n' "$pass" "$fail" "$broke" "$notrun"
[ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]
