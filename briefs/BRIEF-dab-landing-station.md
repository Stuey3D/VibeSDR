# BRIEF — a DAB station as a radio's landing station

Stuart, 2026-09-28: *"the ability to set a DAB station as the landing station as I am planning to add a
25-2000MHz antenna to the TV and that has an FM stop filter so i would have nowhere other than the
Volmet at 128.5925MHz to drop a listener on to upon connection, however DAB works."*

## THE UI (Stuart's words — build exactly this)
*"in the current setup page/section (android) where you choose a landing frequency just above the entry
for frequency and demod selector have a use DAB station as landing station? tap yes and then a block
selector pops up and you choose a block so for me i'd maybe choose 12B, then next to that a quick
station scan and then a selection box for the station so then BBC Radio 1"*

- A **"Use a DAB station as the landing station?"** toggle, placed just ABOVE the existing landing
  frequency + demod selector, in EVERY place a landing is set:
  - the server setup page `android/app/src/main/cpp/vibe_setup_page.h` ("Where new listeners start"
    card, ~:668-679; filled ~:3490, saved ~:3817/:3847) — served by every Linux/Mac server;
  - the main Android app's server settings `src/screens/ServerModeScreen.tsx` (~:395-396, :1195, the mode
    picker ~:2219) — how the XCover sets its landing;
  - VibeServer Lite's setup (the Sony TV): FIND how Lite sets its landing (lite/android, lite/app —
    possibly the served setup page, possibly its own screen). Stuart calls it "the current setup
    page/section (android)".
- Yes → a **block selector** (Band III blocks from `vibedab::kBandIII`, e.g. 5A…13F), beside it a
  **quick station scan** button (tune that block briefly, read the ensemble's services), then a
  **station selector** (e.g. "BBC Radio 1"). While DAB landing is on, the frequency + demod entries are
  hidden or visibly superseded (not both active).
- ★ **Only on radios that can do DAB** (`vsDabCapable` / `radioCanDab`; e.g. an Airspy HF+ cannot —
  its max rate is below DAB's 2.048 MS/s). No toggle at all otherwise (AGENTS.md: never draw a control
  that cannot act).
- ★ **Blocking DAB removes it**: if `dab` is added to the radio's blocked modes, the DAB landing is
  cleared (config and UI), and the page says so in a sentence ("DAB is blocked on this radio, so its DAB
  landing station has been removed — listeners will start on <freq/mode> instead."). Enforce it on the
  SERVER too (a config that says both must never land on DAB), not only in the UI.

## SERVER (android/app/src/main/cpp/local_sdr_shim.cpp — line numbers approximate, find by content)
- Config (per radio): `landingDab` = `{ "channel": <index into kBandIII>, "label": "12B", "sid": <uint>, "service": "BBC Radio 1" }`
  or equivalent fields in `vibeserver/vibeserver_config.h/.cpp` (RadioConfig ~:376, Config ~:175, load/save
  ~:201/276/423/480/560/872). Absent = today's behaviour.
- Applying it: the landing is applied on the 0→1 listener transition (spectrum-socket accept ~:17631-17780).
  A DAB landing must use the EXISTING DAB entry by synthesising
  `{"type":"dab","on":1,"channel":X,"sid":Y}` through `handleControl` on the arriving socket — exactly as the
  remembered-block resume does (~:17757-17763) — NOT `mode=lm; buildAudio(); retune()` (that treats "dab" as
  a demodulator — the bug noted ~:12298).
- Precedence with the remembered block (`g_dabWantChannel`, persisted as `dabChannel`): think it through and
  document it; suggested: the configured landing applies when the radio is idle/fresh; a remembered DAB
  session from the last listener still resumes as today. Persist the SID alongside `dabChannel` too, so a
  resume brings back the station, not just the block.
- Shared dial: the landing never overrides a radio that already has listeners (today's rule — keep it).
- A scan endpoint for the setup page's "quick station scan": if one exists reuse it (search the DAB code for
  ensemble/service listing, `bmLearnDab` ~:1737, `/bookmarks`); otherwise add a guarded one that is refused
  while listeners are on the radio (it would move the shared dial) and says so.
- ★ Android: the Kotlin/JNI boot never calls `setVibeServerLanding` / `setVibeServerDabChannel` / a persist
  handler (grep VibeServerBoot.kt, VibeLocalSDR.kt, vibe_localsdr_jni.cpp). So on Android the landing only
  reaches the server as the start frequency/mode, and a remembered DAB block does not survive a restart.
  Wire it properly for both Android apps (main + Lite).

## CLIENTS
- Web client and app already follow a server that is in DAB (`onDab` in web/client/src/main.ts ~:1299;
  SDRScreen ~:4424). Verify, don't rebuild. Jr (watch) is out of scope.

## RULES
UK English; "servers" not "instances". No silent catches. Don't push/deploy/touch real servers or devices.
Commit your own files only. Keep the Mac's disk in mind.
