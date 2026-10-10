# Pocket VibeServer on a Raspberry Pi 3 A+

_Written 10 October 2026, the day the first pocket hardware (a Pi 3 A+) arrived. It builds on
`docs/POCKET-VIBESERVER-BRIEF.md` (16 September) and turns its steps 1–3 and 6 into code, plus the
image. Nothing here has run on a Pi 3 A+ yet. The checklist at the end is how to prove it._

## What Stuart asked for

1. **First boot:** an OPEN hotspot called **VibeServer**. An iPhone that joins it gets the setup
   page automatically, through the captive-network sheet. It is the EXISTING setup page, not a new
   one.
2. **Up to three Wi-Fi networks, in STRICT order.** If 1 and 2 are both in range, the box joins 1,
   whatever the signal strength and whichever it used last. Network 1 (home) is picked from a scan.
   Network 2 (an iPhone Personal Hotspot) is typed in, because it is usually not advertising while
   the phone is busy being the setup client. Network 3 is optional. The owner can reorder them.
3. **The box's own fallback hotspot:** the owner picks its name and password (WPA2, at least 8
   characters). At boot, and whenever every saved network is lost, the box joins a saved network
   if it can see one. Otherwise it raises this hotspot. The hotspot still has the captive redirect.
4. **Radios without the TUI.** A fresh box with no radio, and a dongle plugged in later, must be
   set up entirely from the browser.
5. Raspberry Pi OS **Lite**, with a minimal footprint. Build **both** a 32-bit and a 64-bit image
   from one stage, and let the device decide which one we recommend.

## Status

| Piece | Where | Built | Tested |
|---|---|---|---|
| Captive answers, first-run password claim, Wi-Fi request validation | `vibeserver/pocket.{h,cpp}` | yes | `test-pocket` (65 checks) |
| Shim hook: `/vibeserver/pocket/…` routes and the captive portal | `local_sdr_shim.{h,cpp}` | yes | real binary, curl + browser |
| Daemon wiring: claim, inbox, `:80` port file, hotspot-state poll, setup gate | `vibeserver/main.cpp` | yes | real binary (Mac) |
| Setup page: choose the admin password, Wi-Fi card, the "leave the hotspot" step | `vibe_setup_page.h` | yes | `scripts/test-pocket-e2e.mjs` (headless Edge, muted) |
| Root Wi-Fi state machine | `vibeserver/linux/pocket/vibeserver-pocket` | yes | fake-NetworkManager test (38 checks) |
| Its unit (dormant everywhere but the image) | `vibeserver-pocket.service` | yes | **not on a device** |
| Image customisation (one script, both arches) | `image/pocket/stage-pocket/00-pocket/files/customise.sh` | yes | see the build log in the report |
| pi-gen stage + official-image route | `image/pocket/` | yes | see `image/pocket/README.md` |
| Clock from DAB/RDS (brief step 5) | — | **no** | — |
| "Use this phone's location" button (brief step 4) | — | **no** | — |

**Untested until the Pi 3 A+ runs it:** everything NetworkManager, `iw` and `nft` actually do; the
iOS captive sheet itself; brcmfmac's AP-mode scanning; the real RAM and CPU numbers.

## The hardware, and what 512 MB means

The Pi 3 A+ has 4× Cortex-A53 at 1.4 GHz, **512 MB RAM** and one CYW43455 radio (2.4 + 5 GHz,
802.11ac). The radio does **one thing at a time**: it is either a hotspot or a client. So the box
switches modes, and never runs AP and client together.

512 MB is **below the confirmed minimum** (Pi 2, 1 GB). The last real measurement is from the
Pi 2, on DAB+ (2026-09-19): the radio process used 60 MB RSS (64 MB peak), ffmpeg 37 MB, and the
whole Pi 176 MB of 920. From that, the **expected** use on the Pi 3 A+ is below. These are
estimates, not measurements.

| State | Expected used RAM (armhf) | arm64 (≈ +15–25 %) |
|---|---|---|
| Idle: OS + NetworkManager + avahi + front door + radio open, no listener + pocket service | **~140–160 MB** | ~165–195 MB |
| + 1 WFM listener with Advanced RDS | ~180–200 MB | ~210–240 MB |
| DAB+ (radio ~65 MB + ffmpeg ~37 MB) | ~230–260 MB | ~270–310 MB |

The image recovers RAM by running headless: `gpu_mem=16`, the KMS display driver off (it reserves
CMA for a framebuffer nobody sees), and Bluetooth off. That should leave roughly 450–470 MB for
Linux, so every row fits with room to spare **if the estimates hold**. Raspberry Pi OS Trixie's
`rpi-swap` (zram) stays on as a cushion.

**RAM risks to watch:**
- **DAB+ plus a second listener plus Advanced RDS.** This is the combination most likely to push
  the box into swap. The first-start benchmark should decide it, as it does on Lite.
- **Page size.** The web client is ~490 KB and the setup page is compiled in. Each request copies
  the page into a string (a few hundred KB, briefly), which is harmless at hotspot scale.
- **The pocket service is Python** (~10 MB resident). It is cheap on CPU on purpose: a quiet tick is
  one `nmcli` call every 5 s, and the connection list is cached. If 10 MB matters on the device, the
  same logic ports to a shell loop. Measure it before deciding.
- **apt.** The daily apt timers are off. A manual "Install updates" from the admin page still needs
  RAM for apt, so do not run it during DAB+.

### 32-bit or 64-bit? The device decides

Both images come from the same stage. The hypothesis, untested:
- **arm64** should be faster for the DSP. AArch64 has twice the NEON registers and double-precision
  SIMD, and it is the same build the Pi 500 runs and the one most tested.
- **armhf** should be lighter on RAM (4-byte pointers), and it is the build that proved DAB+ on the
  Pi 2.

The measurement plan below settles it. Do not pick by argument.

## Design

```
  iPhone ──Wi-Fi──▶ [ wlan0 ]  NetworkManager  ◀── nmcli / iw / nft ── vibeserver-pocket (root, Python)
                                   │ shared mode: DHCP + dnsmasq                 ▲            │
                                   │ (dnsmasq-shared.d: every name → 10.42.0.1)  │ reads      │ writes
                                   ▼                                             │            ▼
                  :80 ──nft redirect──▶ :48000  vibeserver front door  ──writes──┘  /run/vibeserver-pocket/state.json
                                        (unprivileged, NoNewPrivileges)   /var/lib/vibeserver/pocket-wifi.request
                                                                          /var/lib/vibeserver/pocket-kick.request
```

**The privilege boundary is the same as `vibeserver-maintenance`, one layer over.** The daemon stays
unprivileged. It writes a **request** into its own state directory, and a root service re-validates
every field and drives NetworkManager:

- `pocket-wifi.request` holds the networks in order and the hotspot, with passwords. It is
  written 0600 and deleted the moment root reads it.
- `pocket-kick.request` holds one word: `switch`, `scan` or `scan-force`.
- Root reads a request only if it is a regular file, not a link, owned by the `vibeserver` user,
  and at most 16 KB. `nmcli` gets **argument lists**, never a shell, so an SSID like `"; reboot`
  is just a strange name.
- `state.json` goes back the other way: mode, saved SSIDs (never passwords), scan list, client
  count and last error. It is 0640 `root:vibeserver`.

Why not just add actions to `vibeserver-maintenance`? That helper is a one-shot whose log the admin
page tails. The Wi-Fi needs a **long-running** owner of the radio, and a second request written
within a second would overwrite the first.

**Off everywhere else.** Pocket mode needs `/etc/vibeserver-pocket/enabled`. That directory is
root's, so the daemon cannot switch pocket mode on for itself. Without the marker:
- the daemon registers no pocket routes and no captive portal (checked in `test-pocket`);
- the unit has `ConditionPathExists=` on the marker and is not enabled by `postinst`;
- NetworkManager's dnsmasq rule is written by the pocket service, not by the package.

A desktop, an ordinary Pi or the Mac sees no change.

### Boot state machine (`vibeserver-pocket`, one tick every 5 s)

```
                         ┌───────────────────────────────────────────────┐
  boot ──▶ any saved     │ no ──▶ scan (radio is still a station) ──▶ OPEN "VibeServer" hotspot   [setup-ap]
           network? ─────┤                                           │ idle ≥ IDLE_SEC and a secured
                         │                                           │ hotspot has been saved
                         │                                           ▼
                         └ yes ─▶ NetworkManager gets BOOT_GRACE_SEC (35 s) to join by itself
                                   │ (autoconnect-priority 30 / 20 / 10 = network 1 / 2 / 3)
                                   ▼
                         connected? ── yes ──▶ [client]  on network 2 or 3: every UPGRADE_SEC (120 s)
                                   │                     scan; if a HIGHER one is in range, move up.
                                   │                     A network that refuses us rests 15 min.
                                   no
                                   ▼
                         try 1, then 2, then 3, then any other (only those seen in a scan,
                         plus those marked "not visible", which NM probes by name)
                                   │ none joined
                                   ▼
                         SECURED fallback hotspot                                   [fallback-ap]
                           every RESCAN_SEC (120 s): look for a saved network
                             · "iw scan ap-force" if the firmware allows it (no drop);
                             · leave ONLY if nobody has been connected for IDLE_SEC (120 s)
                               — a hotspot with a phone on it is never pulled from under the phone;
                             · if ap-force is refused, a short disruptive scan, still only when idle.
```

- **Strict order.** NetworkManager's `autoconnect-priority` makes its own boot-time choice prefer
  1, then 2, then 3, among the networks it can see. But NM never leaves a working connection for a
  better one, so this service does the move-up itself. The fake-NM test checks that "1 and 3 in
  range, 3 much stronger" ends on 1. **On the device:** confirm that NM honours the priority at
  boot (checklist item 6).
- **The open hotspot exists only until a secured one does.** Once setup has saved the secured
  hotspot, the open "VibeServer" never comes back. The exception is a Wi-Fi reset.
- **The box may not finish setup without its secured hotspot.** "Save and start" is refused by the
  server while none is saved. Otherwise a box out of range of every network would fall back to an
  open hotspot serving a configured receiver.

### Captive portal

- **DNS:** `/etc/NetworkManager/dnsmasq-shared.d/vibeserver-captive.conf` holds
  `address=/#/10.42.0.1`. Only the dnsmasq that NetworkManager starts for a hotspot reads it, so the
  box's own lookups as a client are untouched.
- **HTTP:** our own nft table redirects `tcp dport 80` on `wlan0` to the front door's port, which
  the daemon writes to `pocket-port`. The table exists only while a hotspot is up. NM's NAT rules
  are left alone.
- **Answers:** while a hotspot is up, any request whose Host is not ours gets a `302`. "Ours" means
  an IP literal, `.local`, `localhost` or the hostname.
  - An unconfigured box redirects to `/`, which is the setup page itself.
  - A configured box redirects to `/vibeserver/pocket/welcome`, a small page that says where the
    receiver is.
  - Its **Stay connected** button marks that phone as accepted. From then on, the phone's own probe
    gets the exact answer it expects: Apple's `Success` page, Android's `204`, Windows' and
    Firefox's strings. The iOS sheet then shows **Done**, not **Cancel**, and Cancel would drop the
    network. The accepted list resets whenever the hotspot restarts.
- **The iOS sheet is a limited web view.** It has no `confirm()`, no storage that lasts and no
  geolocation, and it **closes when the network goes away**. So the page never relies on a dialog.
  The "leave the hotspot" step shows a **screenshot-this** panel **before** the box switches. The
  panel names network 1, then 2, the `<name>.local` and `vibepocket.local` addresses, and the
  fallback hotspot with `http://10.42.0.1:<port>/`. The panel cannot stop the sheet closing. It
  makes sure the closing loses nothing.

### Radios without the TUI

The TUI's first run does three things: tick radios, set the admin password, and set an optional
server PIN. On the pocket box:
- **The admin password and PIN** are chosen in the page (`/vibeserver/pocket/claim`). The claim
  works **once**, only while no password exists, only from a private address, and never through
  the tunnel (loopback). It writes the config and applies the secret live, and the page signs in at
  once.
- **Radios** were already browser-only: the "new radio" cards from `sdr-change` (2026-10-04) add a
  dongle that is plugged in at any time. They then go on air when their tab is saved. That path is
  unchanged.
- **A box set up before its dongle arrives** now counts as set up on the pocket image. Before this
  change, a front door with zero ready radios stayed "not set up" for ever. That meant no
  `<name>.local` advert, so the owner could not find the box on the home Wi-Fi to add the dongle.
- **Services in a chroot.** `postinst` only enables units when systemd is running, and an image
  build is a chroot. `customise.sh` enables `vibeserver`, `vibeserver-radios`,
  `vibeserver-maintenance.path` and `vibeserver-pocket` explicitly.
- **The first-boot config** (`files/config.json`) is a Full-mode front door with no radio and no
  password. Without it, the daemon would start in single-radio mode and look for a dongle that is
  not there.

### Security notes

- The open setup hotspot is **plaintext**: no TLS exists on a fresh box. The admin password chosen
  there crosses it in clear. So do the Wi-Fi passwords, if they are saved while still on the open
  hotspot. The window is the few minutes of setup, but say it plainly. Once a secured hotspot or the
  home network is in use, the traffic is WPA2-protected like any LAN admin page today.
- Wi-Fi passwords are never logged (both tests assert it). They are never returned by any route. On
  disk they live only in NetworkManager's keyfiles (`/etc/NetworkManager/system-connections`, root
  0600). They appear in `nmcli`'s argv for the instant it runs, and only root's processes can see
  that.
- No default login. User `vibe` exists with a **locked** password, and SSH is off. Raspberry Pi
  Imager's settings can add a password and enable SSH.

## Building and flashing

`image/pocket/README.md` has the exact commands. In short:

```
image/pocket/build-deb.sh arm64        # then armhf — one at a time
image/pocket/build-image.sh arm64      # then armhf — one at a time
```

Each image is a `.img.xz` in `image/pocket/out/`.

**Flashing, for Stuart:**
1. Raspberry Pi Imager → **Choose OS → Use custom** → pick
   `vibeserver-pocket-arm64-<date>.img.xz` (or armhf).
2. **Choose storage** → the microSD card.
3. When Imager offers OS customisation, the first test should use **No**: it tests the first-boot
   hotspot. A later test can use **Edit settings** to set Wi-Fi and SSH. The box then skips the
   setup hotspot and joins that network (the "other network" path).
4. Put the card in the Pi 3 A+, plug the RTL-SDR in (or not — test both), and power on.

## On-device checklist (Pi 3 A+)

Run with the phone that matters: an iPhone. `journalctl -u vibeserver-pocket -f` is the log, if
SSH is on.

1. **First boot, no dongle.** About 40–60 s after power, "VibeServer" (open) appears. The iPhone
   joins it, and the captive sheet opens on **Choose an admin password**. _If no sheet appears,
   open `http://10.42.0.1:48000/` in Safari and record which step failed: DNS (`nslookup
   captive.apple.com` from a laptop on the hotspot) or the `:80` redirect (`curl -v
   http://10.42.0.1/hotspot-detect.html`)._
2. **In the sheet:** choose the password. The page signs in, and the Wi-Fi card lists the networks
   the box scanned at boot.
3. **Wi-Fi card:** network 1 = home, picked from the list. Network 2 = the iPhone hotspot, typed,
   with **not visible** ticked. Set the hotspot name and password (try a 7-character one first; it
   must be refused). Save. Expect "Saved" and two saved rows after a reload.
4. **"Save and start"** is refused before the hotspot is saved (try it first), then works. The
   sheet survives the restart.
5. **Plug the dongle in**, press **Check again**: a "new radio" card appears. **Add**, set its tab,
   save, then "Save and start". It goes on air. (If the dongle was in from boot, the card is there
   at once.)
6. **Leave the hotspot:** the panel lists what happens. Press **Leave the hotspot now**. Within a
   minute the box is on home Wi-Fi. Rejoin home on the iPhone. The VibeSDR app finds the box, and
   `http://<name>.local:48000/` works in Safari. **Strict order:** with home AND the iPhone hotspot
   both on, reboot the box. It must come back on **home** (check `nmcli -t -f NAME,DEVICE connection
   show --active`).
7. **Away from home:** with home off (unplug the router or walk away) and the iPhone hotspot on, the
   box joins the iPhone within ~1–2 minutes. Confirm the iPhone shows a connected device.
8. **Back home:** turn the router on. Within `UPGRADE_SEC` (120 s) the box moves **up** to home by
   itself.
9. **Nothing in range:** both off. The box raises **your** secured hotspot (never the open one).
   Join it with the password. The sheet opens the welcome page. **Stay connected** → the sheet says
   **Done** → the receiver plays in Safari at `http://10.42.0.1:48000/`.
10. **Rejoin from the hotspot:** with the phone still on the box's hotspot, turn home back on. The
    box must **not** switch while the phone is connected. Disconnect the phone. Within ~4 minutes
    (IDLE + RESCAN) the box is back on home. Record whether `iw dev wlan0 scan ap-force` works on
    this firmware (`journalctl` shows a disruptive scan if not).
11. **Wrong password:** change network 1's password on the router. The box ends on network 2 (or
    the hotspot) and does not retry network 1 every two minutes (15-minute rest). The page shows
    "Last problem: could not join …".
12. **Reorder** in the Wi-Fi card (2 above 1) without retyping passwords. Save. The order holds
    after a reboot.
13. **Factory Wi-Fi reset:** put an empty file `vibeserver-reset-wifi` on the card's boot partition
    and boot. The open "VibeServer" is back, and the admin password is unchanged.
14. **Ordinary install untouched:** on the Pi 500 or any non-pocket box, after installing the same
    package: `systemctl status vibeserver-pocket` → "condition failed", and
    `/vibeserver/pocket/hello` → `{"pocket":false}`.

## Measurement plan: 32-bit or 64-bit for the Pi 3 A+

Flash each image in turn on the **same card brand, same dongle, same aerial, same room**, and run
the same script. Take every figure **three times**, once settled, and use the median.
`cpu.sh` = utime+stime of all vibeserver pids over 60 s (the method from the armhf work).

| # | Measurement | How | Pass |
|---|---|---|---|
| M1 | Idle RAM, no dongle | 5 min after boot, setup done: `free -m` "used", `ps -o rss,comm -C vibeserver,python3,NetworkManager,dnsmasq` | record |
| M2 | Idle RAM, dongle in, no listener | as M1 | record |
| M3 | 1 WFM listener + Advanced RDS | iPhone on the receiver, 96–105 MHz station, Adv RDS open, 10 min: `free -m`, `vmstat 5 12` (si/so), `cpu.sh` | no swap-in, CPU < 70 % of a core |
| M4 | DAB+ | 1 listener on a DAB+ service, 10 min: RAM, ffmpeg RSS, `cpu.sh`, the audio's bad-frame count | no swap-in, 0 bad frames |
| M5 | DAB+ + a 2nd listener (WFM on another device) | as M4 | record — decides the default listener cap |
| M6 | Swap | `cat /proc/swaps`, `vmstat` si/so during M3–M5; `/sys/block/zram0/mm_stat` | any swap-in = flag it |
| M7 | Thermal | `vcgencmd measure_temp; vcgencmd get_throttled` at the end of M4, in the case it will live in | `throttled=0x0` |
| M8 | Hotspot mode cost | M3 repeated with the iPhone on the box's own hotspot | audio clean |
| M9 | The first-start benchmark | the setup page's "Run benchmark" on each image | compare the green/amber/red rows |

**Decide:** pick arm64 unless armhf avoids swap in M4/M5 where arm64 does not, or the CPU
difference is inside the ±7 % noise. In those cases the lighter build wins on a 512 MB board.
Record the result in this doc and in `minimum_confirmed_spec`.

## Open questions for Stuart

1. **Rescan interval on the fallback hotspot.** It is 120 s, and only when nobody has been
   connected for 120 s. Shorter returns home faster but drops a hotspot more often if `ap-force`
   is unsupported. All the timers are in `/etc/vibeserver-pocket/pocket.conf`.
2. **Switch home while a phone is on the hotspot?** Today it never does: the phone keeps the
   hotspot. Would you rather it moved home anyway, because the phone will follow?
3. **Factory reset.** Today a file on the boot partition forgets the Wi-Fi only, and the admin
   password stays. Should it also clear the password, or the whole config? Should there also be a
   button (GPIO) reset? The Pi 3 A+ has no button, but a pocket case could have one.
4. **The setup hotspot name.** It is plain "VibeServer". With two new boxes in one room, a suffix
   (VibeServer-ab12) tells them apart. Keep it plain?
5. **Wi-Fi passwords over the open setup hotspot.** Acceptable for the setup window? The other
   option is to save Wi-Fi only after joining the secured hotspot, which adds a step.
6. **When to ship it.** The pocket code must be in a published package before a box can update
   safely. The images use a local build at revision **900**, so "Install updates" cannot swap it for
   an rc35 without pocket code. The first release with a higher version replaces it.
7. **The open hotspot after a fresh flash with Imager Wi-Fi settings.** If Imager pre-sets a network,
   the box joins it and never shows the setup hotspot. Setup is then at `http://vibepocket.local:48000/`
   on that network, and the password can be claimed from the LAN (private addresses only). Is that
   the path you want, or should Imager-configured boxes still raise the setup hotspot once?

## Not done here (from the brief)

- **Clock from DAB/RDS** (step 5). A box that boots with no network keeps the time
  `systemd-timesyncd` saved at the last shutdown. It is wrong after time off, but monotonic.
- **"Use this phone's location"** (step 4). It would not work inside the captive sheet anyway (no
  geolocation), so it belongs in Safari.
- **Offline audit** (EiBi, logos, listing fail quietly without internet). This exists mostly, but
  was not re-audited for the hotspot case.
- **AP + client together.** Not on one radio (by design). A second USB Wi-Fi adapter would make it
  possible.
