# BRIEF — V10 remaining work (ready-to-go punch list)

Snapshot 2026-07-24. The actionable V10 list + the near-term roadmap that came out of the 2026-07-23
design session. This is the index/starting point; detailed design lives in the linked briefs/memories.
Ordering is Stuart's call.

## Shipped this session (2026-07-23, done)
- Jr↔VibeServer audio hardening: Opus is the compressed codec, speaker **mono-request** (fullband),
  wrist-up **cushion** (no stutter), **RDS** station name, cold-start audio recovery, settle-deadline,
  same-session **takeover** + liveness **ping** (no phantom connections), bounded resume auth.
- **Edit custom servers** (phone pencil + Jr long-press) with a **CUSTOM SERVERS vs FAVOURITES**
  subheading split. (memory: edit_custom_servers)
- VibeServer macOS app: menu-flicker + hang fixes; being released as **0.2.0 alpha** (Opus + tonight's
  fixes).

## V10 deliverables (remaining)
1. **iCloud KVS sync** (one entitlement across VibeSDR iPhone/iPad/Mac + Jr). Syncs:
   - Favourite servers (Jr `FavStore` ↔ phone `Favourite`s — shapes already match).
   - Spectrum colour + VFO colour + waterfall/display settings.
   - Bookmarks — only the "available on watch" subset (KVS ~1 MB cap). Phone gets a `savedFromWatch`
     flag + per-bookmark "Make available on watch" toggle.
   - ★ **Last-tuned station PER SERVER, not per device** — one KVS key = JSON map
     `serverKey → {freq, mode, ts}`; write on tune (debounced), read on CONNECT only; last-write-wins
     by ts; fall back to local. serverKey = normalised host:port / url, same both apps. Works all
     backends. (memory: jr_bookmarks)
2. **Tune buttons / input control system** — the big one from tonight. HiFi tuner button mode
   (committed), drums-by-scroll, pointer support, full keyboard scheme + cheat sheet + first-detect
   pills. Full spec: **repo `BRIEF-inputs-shack-mode-mac.md`** (memory: buttons_hifi_tuner_design).
3. **Menu redesign** remaining items (memory: menu_relocation_progress) — DAB decoder box + §6 keyboard
   rework.

## Post-V10 candidates (designed tonight, NOT V10 unless pulled in)
- **Shack mode / external display** — VibeSDR full-screen on a TV (AirPlay + USB-C→HDMI), video-app
  model (TV = RF panorama, phone = controls + audio analysis), independent per-surface zoom.
  `BRIEF-inputs-shack-mode-mac.md` §5 (memory: external_display_shack_mode).
- **Audio visualiser panel** — client-side scope + audio spectrogram off the decoded audio; the phone's
  shack-mode panel; all backends, zero server cost. (memory: audio_visualiser_panel)
- **The free Mac app** — iOS-on-Mac (Designed-for-iPad) + drums-by-scroll + input work + VibeServer
  "this device only" mode auto-detected as an "On this device" radio. No Catalyst needed for USB.
  `BRIEF-inputs-shack-mode-mac.md` §7 (memories: mac_native_build, vibeserver_mac_standalone).

## Marketing (from tonight)
- KEEP "mobile first" in the tagline (SEO/discovery asset): *"VibeSDR — an easy-to-use, mobile-first,
  fully scalable SDR client. Works from a 1" watch screen all the way up to a giant TV, and everything
  in between."*
- Second angle: *"designed to bridge the gap between real, easy-to-use hardware and complex, highly
  technical SDR software"* + the 80s-boombox-drums / **90s-CAR-STEREO-keys** body copy (updated
  2026-07-25 — the keys read as car stereo, not separates, and both references are then PORTABLE like
  the app; see BRIEF-inputs-shack-mode-mac.md). ★ Terminology: hold-button = "sweep"/"fast-tune",
  NEVER "scan" — reserve scan/scanner for the V11 auto-seek feature. (memory:
  positioning_and_readme)

★★ **"Mobile-first, fully scalable" is now EVIDENCED, and it is a stronger claim than it sounds**
(Stuart, 2026-07-25). Most "scalable" clients mean a LAYOUT that reflows to the screen. VibeSDR
adapts to the **INPUT available**, which is rarer and harder:

| surface | input | control surface |
|---|---|---|
| 1" watch (Jr) | crown | its own dial + waterfall |
| phone | touch | drums or tuner keys |
| tablet / Mac | pointer + keyboard | hover-scoped drums, full shortcuts |
| TV | keyboard only | the pill + flanks, and NO touch controls at all |

Same app, same engine, and the controls do not merely resize — they CHANGE IDENTITY to match what the
user is holding. The tagline's "from a 1 inch watch screen all the way up to a giant TV" stops being a
size claim and becomes a demonstrable one. ★ Worth leading with this rather than the size range: the
size range is the symptom, the input adaptation is the actual engineering.

## V11 (not now)
Scanner (auto-seek), ADS-B own decoder, remote-control mesh.
</content>
