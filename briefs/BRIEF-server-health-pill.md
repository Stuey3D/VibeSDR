# BRIEF — Server health pill

**Status:** ready for implementation
**Scope:** VibeServer (status payload) · VibeSDR app (React Native/Expo) · web client
**Design reference:** https://claude.ai/artifact/DdGXVP96PsjNfo4HyqaHEA (boards: *label beside*, *label above*, *States*)

## 1. Goal

Users have asked for server CPU and other stats. The raw figures stay on the admin page. For the public, we replace the existing battery pill (top right, over the frequency scale) with a **health pill**: small pictograms for CPU, RAM and TEMP, plus the battery with its %, each coloured by status. The pill's border takes the colour of the worst component, so the overall state reads at a glance.

Public clients receive **levels only, never raw values**.

## 2. Layout

Build both variants. Ship **A** as the default and keep **B** behind a dev/debug toggle so they can be compared on real devices.

**A — label beside (single row, ~28 px tall)**
`server │ [CPU] [RAM] [TEMP] │ [BAT 100% ⚡]`

**B — label above (two rows, ~44 px tall)**
```
SERVER
[CPU] [RAM] [TEMP] │ [BAT 100% ⚡]
```

- Pill background `rgba(8,12,8,0.86)`, border 1.5 px, radius 14 (A) / 12 (B).
- Icons 16 × 16, stroke 1.4, round caps, `currentColor`; 8 px gaps; 1 px dividers at `rgba(255,255,255,0.16)`.
- Label: A = `server`, 12 px/600; B = `SERVER`, 9 px/700, letter-spacing 0.16em, `#B9C0B4`.
- The pill is **content-width and right-anchored**, so it grows leftwards when a slot appears or disappears.
- Battery: 40 × 16 outline with a 2 × 6 nub. The inner fill width equals the charge %, in the level colour at 22% alpha. The % text (10 px bold mono) is always shown. A bolt appears only while charging.
- Hit target ≥ 44 px (A needs invisible vertical padding; B already meets it).

> **TRAP — B's 9 px caption** is marginal on low-density Android hosts and web. If it proves unreadable, drop the caption rather than growing the pill; the icons and accessibility label carry the meaning.

## 3. Levels and colours

| Level | Token | Colour | Behaviour |
|---|---|---|---|
| 0 OK | `health.ok` | `#5BE36B` | static |
| 1 Elevated | `health.warm` | `#E8C547` | static |
| 2 High | `health.high` | `#FF8A3D` | static |
| 3 Critical | `health.critical` | `#FF4B4B` | **breathes** |

- **Border:** worst level of the visible slots; alpha 0.45 when OK, 0.75 otherwise.
- **Breathing (critical only):** the icon's opacity goes 1 → 0.3 → 1 over 1.4 s, ease-in-out, with a 3 px drop-shadow glow at the peak. The pill border pulses a `0 0 9px` red glow in sync.
- **Reduced motion** (iOS/Android system setting, `prefers-reduced-motion` on the web): no animation. Critical stays solid red **and** shows a non-motion cue: a 4 px red dot at the icon's top-right.

> **TRAP — colour-blind users.** Elevated and High differ only by hue. The reduced-motion dot is the minimum; consider showing the dot from High upwards for everyone.

## 4. Slot rules and thresholds

Server-side, computed from smoothed values (see §6).

| Slot | Metric | Elevated / High / Critical |
|---|---|---|
| CPU | machine-total % = per-core sum ÷ core count | 50 / 75 / 90 % |
| RAM | used % | 70 / 85 / 95 % |
| TEMP | headroom = throttle limit − current °C | ≤ 20 / ≤ 10 / ≤ 5 °C |
| BAT | charge %, **only while discharging** | ≤ 50 / ≤ 20 / ≤ 10 % |

- Battery while charging or on mains is always OK.
- Hosts without a battery (Pi, x86) do not show the battery slot at all.

> **TRAP — CPU scale.** The admin card reports a per-core sum (e.g. 83% / 800%). Always normalise by core count, or a single busy core would show red on an 8-core phone.

## 5. The TEMP slot: sensor, throttle inference, or nothing

The TEMP slot has three states:

1. **Thermometer icon** — a trusted temperature source exists. Levels come from headroom (§4). If throttling is *also* detected (§5.2), escalate TEMP to at least High.
2. **Snail icon** — no usable temperature, but throttling is detected. The snail appears **only while throttling**, in High, or in Critical if throttling persists past 60 s. It is hidden again once throttling has been clear for 30 s.
3. **Nothing** — no temperature and no throttle signal, or throttling not detectable. The slot is omitted and the pill shrinks.

**Why a snail with its shell on fire:** there's no universal "throttled" icon. A snail is the most widely understood "slow" metaphor, and the flames say *why* it's slow (heat), so the glyph reads as "throttled" rather than just "slow connection". The shell is shrunk to make room for three small flame tongues on top, keeping it single-colour (`currentColor`) so it tints by level like the other slots. Legibility at 16 px has been checked on the *Throttle icon* board in the design reference. The accessibility label spells it out ("CPU throttling").

Snail-on-fire glyph (16 × 16, same stroke style; flames at stroke 1.2):
```svg
<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="6.3" cy="9.6" r="3.3"/>
  <path d="M6.3 9.6a1.2 1.2 0 1 1 1.2-1.2"/>
  <path d="M1.5 13.4h9.8a2.3 2.3 0 0 0 2.3-2.3V9.4"/>
  <path d="M13.6 9.4l-.8-2M13.6 9.4l1-1.8"/>
  <g stroke-width="1.2">
    <path d="M4.1 5.6c-.8-.8-.4-1.8.2-2.5.1.7.6 1 .5 1.9"/>
    <path d="M6.3 5.2c-1-1.1-.4-2.5.4-3.6.2 1.1.9 1.6.6 3"/>
    <path d="M8.5 5.7c-.7-.7-.3-1.6.3-2.2.1.7.6.9.4 1.8"/>
  </g>
</svg>
```

> **TRAP — flames imply heat.** A frequency cap can also come from a power or battery limit, not temperature. On a Pi, show the snail on fire only when `get_throttled` reports a thermal cause (bit 3).

★★★ **TWO SNAILS, AND THE DIFFERENCE IS THE CAUSE** (Stuart, 2026-09-25, amending this brief):
> "snail on fire = thermal throttle, snail with a lightning bolt = power limit throttled."

A plain snail (the brief's original fallback) says only "slow" and leaves the owner guessing — and
the two causes want opposite fixes: a thermal cap means cool it or slow it down, a power cap means
a better supply or cable. On a Pi that distinction is already in `vcgencmd get_throttled`: bit 3 is
the soft TEMPERATURE limit, bits 0/2 are under-voltage and current-limit. So:

| cause | glyph | when |
|---|---|---|
| thermal | snail **on fire** | `get_throttled` bit 3, or a temperature source already at/over its trip |
| power / under-voltage | snail with a **lightning bolt** | `get_throttled` bit 0 or 2, or a cap with no thermal cause |
| cap, cause unknown | plain snail (no flames, no bolt) | a cap is observed but nothing says why |
| **no cap observed** | **nothing** | even if `get_throttled` or the under-voltage alarm is asserting — see the measurement below |

★ The bolt is drawn in the shell's place at the same 16x16 and stroke, single-colour `currentColor`
  like every other slot, so it tints by level. ✗ Do NOT reuse the battery's charging bolt shape at a
  different size — one glyph, one meaning.
★★ Under-voltage on a Pi was previously logged for admins only and deliberately NOT treated as
  throttling ([[pi500_undervoltage_reboot]] — "undervoltage REAL, box STABLE, do not raise it").
  That still stands for the TEMP slot's LEVEL; the bolt snail appears only when the voltage problem
  has actually caused a CAP (bit 2 / bit 0 with a frequency cap present), not merely a warning bit.

### 5.1 Temperature sources, in order of preference

- **Linux (Pi, x86, Armbian):** `/sys/class/thermal/thermal_zone*/`. Pick the zone whose `type` matches CPU/SoC (`cpu-thermal`, `soc_thermal`, `x86_pkg_temp`, `cpu*`). `temp` is in millidegrees. For the limit, use the lowest `trip_point_N_temp` whose `trip_point_N_type` is `passive`; fall back to 80 °C.
- **Raspberry Pi:** as above. Treat `vcgencmd get_throttled` bit 3 (soft temperature limit active) as throttling.
- **Android:** use `PowerManager.getCurrentThermalStatus()` (API 29+) and map NONE/LIGHT → OK, MODERATE → Elevated, SEVERE → High, CRITICAL and above → Critical. On API 30+, `getThermalHeadroom()` can refine this. Only fall back to sysfs thermal zones if they're readable and plausible. On API < 29, use no temperature and infer throttling instead (§5.2).
- **macOS:** `ProcessInfo.thermalState`, mapped as nominal → OK, fair → Elevated, serious → High, critical → Critical. No sensor reads.

> **TRAP — Android thermal zones.** Names and units vary by vendor. Many zones are the battery, PMIC or skin rather than the SoC, and SELinux often blocks them. Reject values outside 10–110 °C, and never guess a millidegree/degree conversion from magnitude alone.

> **TRAP — the Android thermal status API** reports the *device's* thermal pressure, not a temperature. That's a good fit here, but show it with the thermometer only when it comes from a real reading. When the status is the only source, use it to drive the snail/throttle logic rather than presenting it as a temperature.

### 5.2 Throttle inference (hosts without a temperature source)

Read per-core `/sys/devices/system/cpu/cpu*/cpufreq/{scaling_cur_freq, scaling_max_freq, cpuinfo_max_freq}`.

Throttling is detected when **either** of these holds for ≥ 10 s:

- `scaling_max_freq < 0.9 × cpuinfo_max_freq` on any online core (a cap has been imposed); **or**
- machine-total CPU ≥ 60% **and** the mean `scaling_cur_freq` of busy cores is < 60% of `cpuinfo_max_freq`.

★★★ **MEASURED ON THE PI 500, 2026-09-25 — `get_throttled` IS NOT AUTHORITATIVE AND MUST NOT DRIVE
THE ICON.** Stuart: *"My Pi500 always reports under voltage but from what I can see it is hitting the
2.4GHz it should all the time and isnt throttled."* Three samples, four seconds apart, on an idle-ish
box at 43.9 °C:

```
throttled=0x50005   cur 2400 MHz   scaling_max 2400   cpuinfo_max 2400
throttled=0x50000   cur 2400 MHz
throttled=0x50000   cur 2400 MHz
```

`0x50005` sets bit 0 (under-voltage NOW) **and bit 2 (THROTTLED NOW)** — while the clock sits at its
full 2400 MHz and never moves. Four seconds later the live bits have cleared, leaving only the sticky
bits 16/18 ("has occurred since boot"), which on this machine are set permanently.

**So the rule is inverted from the paragraph this replaces:**
- The snail appears ONLY on an **observed cap** — `scaling_max_freq < 0.9 x cpuinfo_max_freq`, or low
  clock under load (§5.2). That is a measurement, not a claim.
- `get_throttled` (and the `rpi_volt` under-voltage alarm the server actually reads — `readSys()` uses
  `in0_lcrit_alarm`, NOT vcgencmd, because the service user cannot open `/dev/vcio`) may then be used
  ONLY to choose WHICH snail: flames for a thermal cause, bolt for a power one.
- A box reporting under-voltage, or even "throttled now", with its clock at maximum shows **no snail
  at all**. Anything else would put a permanent warning on Stuart's Pi 500, which is stable and fast
  ([[pi500_undervoltage_reboot]]: "undervoltage REAL, box STABLE, do not raise it").

> **TRAP — the sticky bits are useless for a live icon.** Bits 16-19 mean "since boot" and, once set,
> never clear. An icon driven by them is permanent furniture.

> **TRAP — low clock alone means nothing.** Governors (e.g. `interactive`, `schedutil`) idle at the minimum clock: the XCover 4S sits at 800 MHz at ~10% load, which is healthy. Only low clock **under load**, or an imposed cap, counts.

> **TRAP — big.LITTLE.** On heterogeneous SoCs, compare each core to its **own** `cpuinfo_max_freq`, not a global maximum, or the little cluster will always look throttled.

## 6. Server: sampling and payload

- Sample every 1 s. Smooth CPU and RAM with an EWMA (α ≈ 0.3).
- **Hysteresis:** a level rises as soon as the smoothed value crosses its threshold, and falls only once the value is 5 points (5 °C for TEMP headroom) back below it. **Critical** needs 3 s sustained before it's entered.
- Add an additive `health` object to the existing public status message that already carries the battery %. Send it only when a level changes, plus in the connect snapshot:

```json
"health": {
  "v": 1,
  "cpu": 0, "ram": 0,
  "temp": { "kind": "sensor|throttle|none", "level": 0 },
  "bat": { "present": true, "pct": 100, "charging": true, "level": 0 }
}
```

- Levels are 0–3. No raw CPU, RAM, °C or clock values in the public payload; those stay on the admin page.
- **Compatibility (V11 core lock):** this is additive. Older apps ignore `health` and keep showing the existing battery pill from the current field, which must continue to be sent unchanged.

## 7. Clients

**App (RN/Expo):**
- A `HealthPill` component with a `variant: 'beside' | 'above'` prop (dev toggle).
- Animate on the native driver (opacity/shadow only; no layout animation).
- Stop animating when the screen isn't visible.
- Read Reduce Motion via `AccessibilityInfo`.

**Web client:** the same component in CSS/SVG. Keyframes as in the mockup, with `@media (prefers-reduced-motion: reduce)` as in §3.

**Both:**
- If `health` is absent (older server), fall back to the current battery pill exactly as it is today.
- The accessibility label is built from the levels, e.g. *"Server health: CPU OK, RAM elevated, CPU throttling, battery 8%, not charging."*
- **Optional:** tapping the pill shows a short popover with the same wording. No numbers.

## 8. Acceptance

Use the mockup's scenarios as fixtures (a server-side fake source behind a debug flag):

| Fixture | Expected |
|---|---|
| This server (83%/800%, RAM 60%, 50/80 °C, 100% charging) | all green, thermometer shown |
| No temp sensor, not throttling | TEMP slot absent, pill narrower |
| No temp sensor, 70% load at 45% clock for 15 s | snail appears in High |
| Critical (95% CPU, 77/80 °C) | CPU and TEMP breathe, border pulses |
| On battery, 8%, not charging | battery breathes, no bolt |
| Reduce Motion on + Critical | no animation, solid red and dot |
| CPU oscillating 88–92% | no flicker between High and Critical |
| Old app ↔ new server, new app ↔ old server | old pill in both cases, no errors |
