# Pocket VibeServer on a Raspberry Pi 3 A+

_Written 10 October 2026, the day the first pocket hardware (a Pi 3 A+) arrived. It builds on
`docs/POCKET-VIBESERVER-BRIEF.md` (16 September) and turns its steps 1–3 and 6 into code and an
image, then adds the requirements Stuart set during the day:
- the tunnel switch;
- power-cut hardening;
- the High Detail Maps baked into the image;
- the card layout.

Nothing here has run on a Pi 3 A+ yet. The checklist at the end is how to prove it._

## What Stuart asked for

1. **First boot:** an OPEN hotspot called **VibeServer**. An iPhone that joins it gets the setup
   page automatically, through the captive-network sheet. It is the EXISTING setup page.
2. **Up to three Wi-Fi networks, in STRICT order.** If 1 and 2 are both in range, the box joins 1,
   whatever the signal and whatever it used last. Network 1 (home) is picked from a scan. Network 2
   (an iPhone Personal Hotspot) is typed in, because it is not advertising while the phone is the
   setup client. Network 3 is optional. The owner can reorder them.
3. **The box's own fallback hotspot:** the owner picks its name and password (WPA2, at least 8
   characters). The box uses it whenever no saved network is in range. It still has the captive
   redirect.
4. **Radios without the TUI.** An iPhone has no terminal.
5. **Raspberry Pi OS Lite, minimal**, with **both** a 32-bit and a 64-bit image from one stage. The
   device decides which one we recommend.
6. **The tunnel:**
   - **Enable the tunnel** is off on a fresh box.
   - **Resume the tunnel after a restart** is a sub-toggle, also off by default.
   - The tunnel never tries to start without real internet, and never in hotspot mode.
7. **The power WILL be pulled at any moment.**
   - The root filesystem must survive a cut.
   - SD writes are kept to a minimum.
   - Saves are atomic.
   - If the settings cannot be read, the box falls back to the OPEN setup hotspot.
8. **High Detail Maps in the image**, on the read-only root, and seen as installed.
9. **Cards:** 8 GB minimum, 32 GB typical. The spare space is **left unallocated** for a future
   gallery partition.

## Status

| Piece | Where | Built | Tested |
|---|---|---|---|
| Captive answers, first-run password claim, Wi-Fi request validation | `vibeserver/pocket.{h,cpp}` | yes | `test-pocket` (65) |
| Shim hook: `/vibeserver/pocket/…` and the captive portal | `local_sdr_shim.{h,cpp}` | yes | real binary, curl + browser |
| Daemon wiring: claim, inbox, `:80` port, hotspot poll, setup gate, tunnel gate, resume-on-boot | `vibeserver/main.cpp` | yes | real binary (Mac) |
| Setup page: choose the password, Wi-Fi card, leave-the-hotspot step, tunnel switches | `vibe_setup_page.h` | yes | `scripts/test-pocket-e2e.mjs` (headless Edge, muted) |
| Tunnel waits for internet (no spawn, no retry while offline) | `vibeserver/directory.{h,cpp}` | yes | e2e (no tunnel attempted on the hotspot) |
| Root Wi-Fi state machine + connectivity by `nmcli monitor` | `vibeserver/linux/pocket/vibeserver-pocket` | yes | fake-NM test |
| Atomic config with a last-good `.bak`, refuses a truncated file | `vibeserver_config.cpp` | yes | `test-config-durable` |
| Settings partition, failsafe, binds, RAM caches, baked-map link | `vibeserver-pocket-data` | yes | **device only** |
| Read-only root seal (first boot) | `vibeserver-pocket-seal` | yes | **device only** |
| Updates under a read-only root | `vibeserver-maintenance` (`apt_do`) | yes | **device only** |
| Image customisation (one script, both arches, both routes) | `image/pocket/stage-pocket/00-pocket/files/customise.sh` | yes | images built (see the report) |
| Clock from DAB/RDS (brief step 5) | — | **no** | — |
| "Use this phone's location" (brief step 4) | — | **no** | — |

**Untested until the Pi 3 A+ runs it:**
- everything NetworkManager, `iw`, `nft`, overlayroot and the initramfs actually do;
- the iOS captive sheet;
- brcmfmac's AP-mode scanning;
- the real RAM, CPU and SD-write numbers.

## The hardware

The Pi 3 A+ has:
- 4× Cortex-A53 at 1.4 GHz;
- **512 MB RAM** (below the confirmed minimum, a 1 GB Pi 2);
- one CYW43455 radio, 2.4 + 5 GHz.

That radio is a hotspot **or** a client, never both at once, so the box switches modes.

### RAM: the baseline is Stuart's Pi 2

Stuart measured the Pi 2 on 2026-10-10: Raspbian Lite 32-bit, rc36, idle, no listeners.
- **225 MB "used"**, of which about 20 MB is his own ssh session plus curl.
- So about **205 MB at rest**, with swap untouched.

| On the Pi 2 | RSS |
|---|---|
| vibeserver ×2 (front door 42 + one radio 34) | 76 MB |
| cloudflared ×2 (the tunnel) | 63 MB |
| NetworkManager | 17 MB |
| systemd / journald / udevd | ~30 MB |
| everything else (avahi, cron, dbus, gettys, logind, timesyncd, wpa_supplicant, ssh) | the rest |

**What the pocket image changes, item by item (estimates — the device decides):**

| Change | RAM | Why |
|---|---|---|
| Tunnel OFF (Personal, the default) | **−63 MB** | no cloudflared at all |
| Tunnel ON | −32 MB vs the Pi 2 | **one** cloudflared: a pocket box lists from the front door only (the Pi 2's radio process runs a second tunnel) |
| NetworkManager | 0 | already on Lite; NM's hotspot mode is built in |
| dnsmasq (NM's hotspot DHCP + captive DNS) | +1–2 MB | only while a hotspot is up |
| nftables (`:80` redirect) | 0 | kernel rules, no daemon; its service stays off |
| `iw` | 0 | run for a moment, nothing resident |
| vibeserver-pocket (Python) | **+10 MB** | the Wi-Fi state machine |
| its `nmcli monitor` (connectivity events) | +3–4 MB | one long-lived nmcli |
| gettys (tty1 + serial) off | −2–3 MB | headless, and the only login is locked |
| Bluetooth off | −2–3 MB | it also shares the antenna and UART |
| journald volatile, 16 MB cap | ≤ +16 MB | logs in RAM instead of on the card |
| overlay root (tmpfs upper) | small | only stray writes to `/` land in RAM; settings and caches are bound elsewhere |
| Headless boot config (`gpu_mem=16`, no KMS) | **+** ~64 MB usable | CMA no longer reserved for a display |

**Expected on the 32-bit image, idle, tunnel off:** 205 − 63 − 3 − 3 + 10 + 4 + 1 ≈ **150 MB**
used. With the tunnel on it is ≈ 180 MB. Of ~460 MB usable that is **~300 MB of headroom**,
Stuart's figure.

DAB+ adds about 100 MB on top (Pi 2: radio 60–65 MB + ffmpeg 37 MB). That still fits. The 64-bit
image is expected to use more (8-byte pointers), and **the gap measured against these numbers
decides the recommendation** (measurement plan below).

**RAM risks:**
- DAB+ plus a second listener plus Advanced RDS. Measure it.
- The Python service's 10 MB. It could become a shell loop if the device says it matters.
- A tunnel left on in DAB mode.

## Design

```
 iPhone ─Wi-Fi─▶ wlan0 ─ NetworkManager ◀── nmcli / iw / nft ── vibeserver-pocket (root)
                           │ hotspot: DHCP + dnsmasq (every name → 10.42.0.1)   ▲       │
                           │ connectivity check ("full" = real internet) ───────┘       ▼
          :80 ─nft redirect─▶ :48000 vibeserver front door ◀── /run/vibeserver-pocket/state.json
                                (unprivileged)  ──writes──▶ /var/lib/vibeserver/pocket-*.request
```

**The privilege boundary is the same as `vibeserver-maintenance`, one layer over.** The daemon stays
unprivileged. It writes a **request** into its own state directory:
- `pocket-wifi.request` holds the networks in order and the hotspot (0600, deleted the moment root
  reads it);
- `pocket-kick.request` holds one word: `switch`, `scan` or `scan-force`.

A root service reads them. It accepts only a regular, non-link file from the vibeserver user, at
most 16 KB, re-validates every field, and calls `nmcli` with argument lists, never a shell.
`state.json` comes back the other way (0640 `root:vibeserver`, no passwords).

**Off everywhere else.** Everything keys on `/etc/vibeserver-pocket/enabled`, which lives in root's
directory, so the daemon cannot create it. All the pocket units are dormant without it, and
`postinst` enables none of them. `test-pocket` checks that a probe, the claim and the routes all do
nothing on an ordinary install.

### The boot state machine (`vibeserver-pocket`, one tick every 5 s)

```
  boot ─▶ saved network? ─ no ─▶ scan (still a station) ─▶ OPEN "VibeServer" hotspot     [setup-ap]
                │                                         │ (a secured one saved, nobody on it for IDLE_SEC)
                │                                         ▼
                └ yes ─▶ NM's own autoconnect gets BOOT_GRACE_SEC (35 s); priorities 30/20/10
                          │
                          ├ connected ─▶ [client]  on 2 or 3: every UPGRADE_SEC scan; a HIGHER one in range → move up.
                          │                         A network that refuses us rests 15 minutes.
                          └ not ─▶ try 1, then 2, then 3, then any other (seen in a scan, or marked "not visible")
                                    │ none
                                    ▼
                                  SECURED fallback hotspot                                [fallback-ap]
                                    every RESCAN_SEC: `iw scan ap-force` if the firmware allows (no drop);
                                    leave ONLY when nobody has been on it for IDLE_SEC (a phone on the
                                    hotspot is never pulled off); otherwise a short disruptive scan, also only when idle.
```

- **Strict order.** NetworkManager's `autoconnect-priority` makes its own boot-time choice prefer
  1, then 2, then 3 among the networks it can see. NM never leaves a working connection for a
  better one, so this service moves up itself. The fake-NM test checks the "1 and a much stronger 3
  in range → 1" case. It must be confirmed on the device (checklist).
- **The open hotspot exists only until a secured one does.**
- **Setup cannot finish without the secured hotspot.** "Save and start" is refused by the server.

### The captive portal

- **DNS:** `/etc/NetworkManager/dnsmasq-shared.d/vibeserver-captive.conf` holds
  `address=/#/10.42.0.1`. Only the hotspot's dnsmasq reads it.
- **HTTP:** our own nft table redirects `tcp dport 80` on `wlan0` to the front door's port. The
  table exists only while a hotspot is up.
- **Answers:** a request for a foreign host gets a `302`.
  - An unconfigured box redirects to the setup page itself.
  - A configured box redirects to a small welcome page. Its **Stay connected** button makes that
    phone's probe get the exact "online" answer (Apple `Success`, Android `204`, Windows, Firefox,
    GNOME), so the iOS sheet shows **Done**, not **Cancel**. Cancel would drop the network.
- **The iOS sheet** is a limited web view: no `confirm()`, no storage that lasts, no geolocation,
  and it **closes when the network goes**. So the leave-the-hotspot step shows a **screenshot-this**
  panel (network 1 then 2, the `.local` addresses, the fallback hotspot and `http://10.42.0.1:<port>/`)
  **before** anything switches.

### Radios without the TUI

The TUI's first run does three things: tick radios, set the admin password, set an optional PIN.
- **Password + PIN** are set in the page (`/vibeserver/pocket/claim`). It works once, only while
  no password exists, only from a private address, and never through the tunnel. The secret is
  applied live.
- **Radios** were already browser-only: the "new radio" cards (`sdr-change`, 2026-10-04) can add a
  dongle plugged in at any time.
- **A box set up before its dongle arrives** counts as set up on the pocket image. Otherwise it
  had no `.local` advert, and the owner could not find it again on the home Wi-Fi.
- **Services in a chroot.** `postinst` enables nothing in a chroot, so the image enables them
  itself.

### The tunnel: Personal or shared

**Enable the tunnel** is the existing "Advertise on VibeSDR.net" switch, posted on its own so it
acts at once with no restart. On, it starts `cloudflared` and lists the box. Off, it stops the
`cloudflared` **process** (the RAM really comes back) and delists the box.

**Resume the tunnel after a restart** (config `dirResume`) sits under it. It is shown only while
the tunnel is enabled, and is off by default. The wording matches VibeServer Lite's ADVERTISE ON
VIBESDR.NET card where the two overlap.

| Enable the tunnel | Resume after restart | Box state | What happens |
|---|---|---|---|
| off | — | any | Personal. No cloudflared. Reachable on its own hotspot or your Wi-Fi only. |
| on | — | own hotspot (setup or fallback) | **"Tunnel waiting for an internet connection"** — no cloudflared, no retries, no log lines; one line logged when it starts waiting. |
| on | — | joined a network, NM connectivity not `full` (captive Wi-Fi, no upstream) | Same: waiting. |
| on | — | joined, connectivity `full` | Tunnel starts, the box is listed; the page shows the address. |
| on | — | internet lost (fell back to its hotspot, or the router went offline) | Tunnel stopped (paused), listing ages out; it restarts by itself when `full` returns. |
| on | off | after a power cut / reboot | Comes back **Personal** (Enable is switched off on a new boot — the kernel boot id, so a settings save's restart does NOT switch it off). |
| on | on | after a power cut / reboot | Tunnel enabled, and waits for `full` connectivity as above. |

"Internet" means **NetworkManager's own connectivity verdict**:
- The image gives NM a check URI (`nmcheck.gnome.org`, every 5 minutes, and at every connect).
- `vibeserver-pocket` follows it by event (`nmcli monitor` → "Connectivity is now 'full'"), with
  no polling of the internet.
- The daemon reads the result from `state.json` and gates the directory worker
  (`vibedir::setInternet`).

A non-pocket server never calls the gate, so it is unchanged.

## Power cuts: the box will be unplugged mid-anything

### The choice: a read-only root plus one small journalled settings partition

| | Read-only root (overlay) + settings partition | Writable root, careful writes |
|---|---|---|
| A cut during normal use | **cannot** damage the OS: `/` is never written | every write anywhere (apt, journald, NM leases, caches) is a chance |
| SD wear | settings + rare events only | constant |
| Updates | an explicit window (below) | as now |
| Effort | raspi-config's own "Overlay File System" (overlayroot) | an audit that is never finished |

**Chosen: the read-only root.**

- **What is read-only.** The root (`overlayroot=tmpfs`: writes to `/` go to RAM and vanish at
  power-off) and the boot partition (`ro` in fstab).
- **What persists.** It lives on partition 3, `LABEL=vibedata`, 256 MB, ext4 mounted
  `noatime,data=journal,commit=5`. With data journalling, a file is either the old one or the new
  one after a cut. `vibeserver-pocket-data` binds it into place before NetworkManager and
  VibeServer start:

  | On the settings partition | Bound onto | Holds |
  |---|---|---|
  | `vibeserver/etc` | `/etc/vibeserver` | `config.json` (+ `.bak`): admin password, PIN, radios, **both tunnel switches**, benchmark |
  | `vibeserver/lib` | `/var/lib/vibeserver` | directory id/key/slug (the tunnel's identity), bans, bookmarks, connection log, DAB learned tables, GeoIP/ASN/EiBi caches |
  | `nm` | `/etc/NetworkManager/system-connections` | saved Wi-Fi + the hotspot (root 0600) |
  | `pocket` | `/var/lib/vibeserver-pocket` | the Wi-Fi country chosen in setup |
  | `timesync` | `/var/lib/systemd/timesync` | the last known time (no RTC on a Pi) |

  The Quick Tunnel needs no credential file: its identity is the directory key in `directory.json`.

- **First boot is the only read-write boot.** Pi OS writes its one-time identity (machine-id, SSH
  host keys). Then `vibeserver-pocket-seal` prepends `overlayroot=tmpfs` to `cmdline.txt` (temp file
  + fsync + rename) and reboots. A cut before the rename means the seal runs again next boot; a cut
  after it means the box is sealed. **Only that first boot, about a minute, is unprotected. Leave
  it powered.**
- **The image is built with overlayroot and its initramfs already in place**, so the seal only
  changes one line. The ` resize` token is removed from `cmdline.txt`: Pi OS would otherwise try
  to grow root into the settings partition.

**Updates under a read-only root.** The admin page's "Install updates" (and the scheduled update)
runs apt **in the real root** through overlayroot's own `overlayroot-chroot`:
1. The boot partition is remounted writable for a kernel or `config.txt` change.
2. apt runs.
3. The box **reboots**, because an overlay whose lower layer changed underneath it is not safe to
   keep running.

That window is the one time the root is written. Do not pull the power during an update. An
update-check also writes apt's lists to the real root. To change anything else in the OS by hand:
`sudo overlayroot-chroot`, or `sudo raspi-config` → Performance → Overlay File System → off, then
reboot.

**The failsafe: the box is always recoverable from a phone.**
1. The settings partition is `fsck`'d before mounting.
2. If it still will not mount, or its label and filesystem are gone, it is made afresh. It held only
   settings, and settings that cannot be read are already lost. If even that fails, nothing is
   bound and the read-only root's defaults apply.
3. In both cases there is **no saved Wi-Fi**, so `vibeserver-pocket` raises the **OPEN setup
   hotspot**.
4. A missing, empty, garbage or truncated `config.json` falls back to `config.json.bak` (the last
   good copy `saveServer` keeps, by hard link). If that is unreadable too, the pocket box starts as
   **a new box**: the setup page offers "Choose an admin password" again.
5. Wi-Fi saves **modify each network in place** (NetworkManager rewrites that one keyfile
   atomically). Dropped ranks are deleted last, so a cut mid-save never leaves the box with no
   network to join.

### Volatile by default

| Item | Where now |
|---|---|
| journald | RAM only (`Storage=volatile`, 16 MB) |
| `/tmp`, `/var/log`, `/var/tmp`, apt and dpkg state, NM leases | the overlay (RAM) — nothing on the card |
| swap | zram only (`/etc/rpi/swap.conf.d/60-vibeserver-pocket.conf`); `dphys-swapfile` off |
| clock | timesyncd's clock file on the settings partition, written on a sync, not on a timer; `fake-hwclock` off |
| `noatime` | the settings partition; root and boot are read-only |

### VibeServer's own writes (the audit, and what was done)

| Write | Trigger | Before | Now |
|---|---|---|---|
| `config.json` | a save | temp+fsync+rename | **+ directory fsync, + last-good `.bak`**, and the loader refuses empty/garbage/truncated files (all installs) |
| `directory.json` (tunnel identity) | every successful ping (~15 min) | temp+rename, no fsync; delist was remove-then-write | **only on a real change, fsync + directory fsync; one atomic replace on delist** (all installs) |
| directory POST body temp | every ping | `/var/lib/vibeserver` | **`/run/vibeserver`** (all installs) |
| `spectrogram.bin` (~3 MB) | every 15 min | `/var/lib/vibeserver` | **pocket: `/run/vibeserver`** (RAM; history is per session) |
| `radio-status-<serial>.json` | every 5 s while a radio is missing | `/var/lib/vibeserver` | **pocket: `/run/vibeserver`** |
| `dab-slides/`, `dab-carousel/`, `dab-logos/` | each new image on DAB | settings dir | **pocket: tmpfs bound over them** |
| connection log | a connection closes (1 Hz flush when dirty) | append | unchanged (event-driven, small) |
| bans, bookmarks, notice, DAB gains/ensembles/AAC ratio | owner action / learned once | atomic (rename) | unchanged; `data=journal` makes rename-after-write safe on the pocket box |
| GeoIP/ASN | at start, if > 7 days old | atomic | unchanged (≈ weekly) |
| EiBi | hourly check, downloads if > 20 h | atomic | unchanged (≈ daily) |
| benchmark + DAB clip | the owner runs the benchmark | atomic | unchanged (once) |
| `pocket-*.request`, `pocket-port` | setup / start | temp+fsync+rename | unchanged |
| occupancy, sockets, locks | every second | `/run` | already RAM |
| crash reports | a fatal signal | the journal (RAM) | — |

## Cards, partitions, and the image

| Partition | Size | Contents |
|---|---|---|
| 1 boot (FAT) | 512 MB (Pi OS) | firmware, kernel, initramfs (overlayroot); read-only |
| 2 root (ext4) | **used + 1 GB** — measured in the build log | Pi OS Lite + VibeServer + the High Detail Maps; read-only. The 1 GB is room for an update in the real root |
| 3 settings (ext4, `vibedata`) | **256 MB, never grown** | the table above |
| — | **the rest of the card, UNALLOCATED** | kept for a future gallery |

- **The `.img`** is cut right after partition 3, so it stays small. Raw size and the `.xz` size are
  printed by the build (about 3.7 GB raw; see the report).
- **Minimum card: 8 GB. Typical: 32 GB**, which is the cheap, common size now.
- **Why the spare stays unallocated** (Stuart's decision):
  - the settings are kilobytes;
  - unwritten space is spare area for the card's own wear-levelling;
  - a small settings partition fscks in a second;
  - a future **gallery** partition (V12 decoder images, recordings — `docs/v12/`) can go there
    without touching root or settings.
  - On a **reused** card the old blocks still count as written. Erase it once (SD Card Formatter)
    before flashing to give the space back to the card.
- **Adding the gallery later, safely on a box that loses power at any moment.** Each step is
  idempotent, and the next boot simply repeats whatever did not finish:
  1. If no partition 4 exists, append one from the end of partition 3 to the end of the card
     (`sfdisk --append`). The MBR is a single 512-byte sector, so a cut leaves the old table or the
     new one.
  2. If partition 4 has **no** filesystem signature (`blkid -p`), `mkfs.ext4 -L vibegallery`. Never
     format one that has a filesystem.
  3. Mount with `noatime` (and `data=journal`, or `data=ordered` plus atomic writes for big media),
     and write a marker last.

  It would be its own oneshot unit, like `vibeserver-pocket-data`. The MBR allows four primary
  partitions, and this uses the fourth.

### The High Detail Maps, baked in

- **Fetch and check at image build time.** The URL and exact size are read from the server's own
  `vibe_mapgl.h` (`DETAIL_URL`, `DETAIL_BYTES` = 177,024,426). The file is verified exactly as the
  server's installer does (`verifyPart`): curl `-f`, the exact byte count, and the `PMTiles` magic.
  (The server's installer has no hash check, so neither does this.) It is cached for the second
  image.
- **Where it lives.** Baked into the read-only root at
  `/usr/lib/vibeserver/mapgl-detail/vibemap-detail.pmtiles`. That costs no SD writes and survives
  any cut.
- **Seen as installed.** The server checks
  `/var/lib/vibeserver/mapgl/vibemap-detail.pmtiles` (`vibe_mapgl.h` `statusJson`: `stat` → a
  regular file, size > 0; serving uses `fopen`). Both follow a symbolic link, so
  `vibeserver-pocket-data` links that path to the baked file at every boot. The page shows the
  maps installed, offers no download, and nothing re-downloads over a phone's data.
- **Cost:** about +170 MB in the image, raw and compressed alike (PMTiles is already compressed).
- **A newer map set** comes the same way as a VibeServer update: a new image, or a future package
  installed into the real root by the update window above. The admin page's "install" would
  download into the 256 MB settings partition, which does not have room for 177 MB. It fails with
  its own error message rather than filling the partition. "Remove" only deletes the link, and the
  next boot puts it back.

## Building and flashing

`image/pocket/README.md` has the exact commands. Run them one at a time:

```
image/pocket/build-deb.sh arm64 ; image/pocket/build-deb.sh armhf
image/pocket/build-image.sh arm64 ; image/pocket/build-image.sh armhf
```

**Flashing:**
1. Raspberry Pi Imager → **Choose OS → Use custom** → `vibeserver-pocket-<arch>-<date>.img.xz` →
   the card.
2. For the first test, answer **No** to OS customisation, to test the first-boot hotspot.
3. Put the card in the Pi 3 A+ and power on. **Leave it powered for the first ~3 minutes**: it
   boots, seals its root read-only, and reboots once.

## On-device checklist (Pi 3 A+)

Use an iPhone. If SSH was enabled with Imager, the logs are:
`journalctl -u vibeserver-pocket -u vibeserver-pocket-data -u vibeserver-pocket-seal -u vibeserver -f`.

1. **First boot, sealing:** the box reboots once by itself, about a minute in. Afterwards
   `findmnt /` shows `overlayroot`, `findmnt /data` shows `data=journal`, and `grep overlayroot
   /proc/cmdline` matches.
2. **The setup hotspot:** "VibeServer" (open) appears. The iPhone joins it, and the captive sheet
   opens on **Choose an admin password**. _If no sheet appears, open `http://10.42.0.1:48000/` in
   Safari and find out which step failed: DNS (`nslookup captive.apple.com` from a laptop) or `:80`
   (`curl -v http://10.42.0.1/hotspot-detect.html`)._
3. **Wi-Fi card:** 1 = home (from the list), 2 = the iPhone hotspot (typed, **not visible**). Set
   the hotspot (try a 7-character password first: it must be refused). Save.
4. **"Save and start"** is refused before the hotspot is saved, then works.
5. **Plug the dongle in** → **Check again** → a "new radio" card → **Add** → its tab → save → on air.
6. **The maps:** the admin/setup page shows High Detail Maps **installed**, with no download offered,
   even on the hotspot.
7. **Leave the hotspot:** read the panel, then **Leave the hotspot now**. The box is on home within
   a minute, and the VibeSDR app finds it. **Strict order:** with home and the iPhone hotspot both
   on, reboot → it must come back on **home**.
8. **Away:** home off, iPhone hotspot on → it joins the iPhone within about 2 minutes.
   **Back home:** within 120 s it moves **up** to home.
9. **Nothing in range:** it raises **your** secured hotspot. Join it; the sheet shows the welcome
   page. **Stay connected** → **Done** → the receiver plays in Safari at `http://10.42.0.1:48000/`.
10. **Rejoin from the hotspot:** with the phone still on it and home back, the box must **not**
    switch. Disconnect the phone; within about 4 minutes it is home. Record whether
    `iw dev wlan0 scan ap-force` works on this firmware.
11. **Wrong password** on network 1: it ends on 2 and does not retry 1 every two minutes. The page
    shows the problem.
12. **The tunnel:**
    - On the hotspot, **Enable the tunnel** → "Tunnel waiting for an internet connection".
      `pgrep cloudflared` → nothing.
    - Join home → the tunnel starts; listed.
    - Pull home's internet (not the Wi-Fi) → paused.
    - Reboot with **Resume** off → Personal. With **Resume** on → it comes back once internet is
      `full`.
    - `free -m` with the tunnel on and off (≈ 30 MB difference).
13. **Factory Wi-Fi reset:** an empty `vibeserver-reset-wifi` on the boot partition → the open
    "VibeServer" is back, and the admin password is unchanged.
14. **Ordinary installs untouched:** on the Pi 500 with the same package:
    - `systemctl status vibeserver-pocket vibeserver-pocket-data vibeserver-pocket-seal` → all three
      report "condition failed";
    - `/vibeserver/pocket/hello` → `{"pocket":false}`.
15. **★★★ POWER-PULL TEST — 20 times, at random moments.** Pull the cable:
    - mid-boot (×4, including twice during the very first, unsealed boot);
    - mid-setup-save (×4: Wi-Fi save, "Save and start", the admin password claim);
    - while listening to WFM (×4);
    - during DAB+ (×4);
    - on the hotspot with a phone connected (×2);
    - mid-tunnel-start (×2).

    Every time, it must boot clean, with every setting intact (or, after a cut mid-save, the old
    ones). It is never unreachable. Afterwards (SSH on):
    - `sudo fsck.ext4 -fn /dev/disk/by-label/vibedata` → clean;
    - `journalctl -b -u vibeserver-pocket-data` shows no "made afresh";
    - `cat /data/vibeserver/etc/config.json` shows a whole object;
    - `nmcli -t -f NAME connection show` shows all saved networks;
    - `sudo overlayroot-chroot dmesg | grep -i ext4` (or `dmesg`) shows no errors;
    - for the root, after unsealing: `sudo fsck.ext4 -fn` on partition 2 → clean.
16. **SD writes:** after 30 minutes listening, `cat /sys/block/mmcblk0/stat`, twice, 10 minutes
    apart. Field 7 (sectors written) should barely move: only the connection log, a GeoIP/EiBi
    refresh, or a setting the owner changed.

## Measurement plan: 32-bit or 64-bit for the Pi 3 A+

Flash each image in turn: **same card, dongle, aerial and room, same script**. Take three settled
samples of each and use the median. The baseline is **Stuart's Pi 2: ≈ 205 MB at rest (32-bit)**,
so the 32-bit pocket box should sit around **150 MB used / ~300 MB headroom**. The 64-bit image is
measured against that.

| # | Measure | How | Pass |
|---|---|---|---|
| M1 | Idle, no dongle, tunnel **off** | 5 min after boot: `free -m` "used"; `ps -o rss,comm -C vibeserver,python3,nmcli,NetworkManager,dnsmasq,cloudflared` | record — compare with ~150 MB |
| M2 | Idle, tunnel **on** (home Wi-Fi) | as M1 | record the cloudflared share (~30 MB) |
| M3 | Idle, dongle in, no listener | as M1 | record |
| M4 | 1 WFM listener + Advanced RDS | 10 min: `free -m`, `vmstat 5 12` (si/so), `cpu.sh` | no swap-in; CPU < 70 % of a core |
| M5 | DAB+ | 1 listener, 10 min: RAM, ffmpeg RSS, `cpu.sh`, bad frames | no swap-in; 0 bad frames |
| M6 | DAB+ + a second (WFM) listener + tunnel on | as M5 | record — sets the default listener cap |
| M7 | Swap | `/proc/swaps`, `vmstat` si/so during M4–M6, `/sys/block/zram0/mm_stat` | any swap-in is a flag |
| M8 | Overlay RAM | `df -h /media/root-rw` after M4–M6 | a few MB at most |
| M9 | Thermal | `vcgencmd measure_temp; vcgencmd get_throttled` after M5, in its case | `0x0` |
| M10 | On its own hotspot | M4 with the iPhone on the box's hotspot | audio clean |
| M11 | First-start benchmark | "Run benchmark" on each image | compare the rows |

**Decide:** arm64 unless armhf avoids swap where arm64 does not (M5/M6), or the CPU difference is
inside the ±7 % noise. Then the lighter build wins on 512 MB. Record the result here and in
`minimum_confirmed_spec`.

## Open questions for Stuart

1. **Hotspot rescan:** every 120 s, and only after the hotspot has been idle for 120 s
   (`/etc/vibeserver-pocket/pocket.conf`). Is that right?
2. **Leave the hotspot while a phone is on it?** Today it never does.
3. **Factory reset:** the boot-partition file forgets the Wi-Fi only. Should it also clear the
   password or the whole config? Should a pocket case have a reset button (GPIO)?
4. **The setup hotspot name:** plain "VibeServer", or a suffix so two new boxes can be told apart?
5. **Wi-Fi passwords sent over the open setup hotspot** (plaintext, during setup only): acceptable?
6. **Shipping:** the images use a local build at Debian revision **900**, so "Install updates"
   cannot swap it for an rc35 without pocket code. The first release with a higher version
   replaces it.
7. **One tunnel per pocket box:** the pocket box lists only from the front door. The Pi 2 runs two
   cloudflared (door + radio). Is the radio's own listing needed for anything? If it is, a pocket
   box would need it back, at ~30 MB.
8. **The first boot is unprotected for about a minute** (it writes its one-time identity, then
   seals). Acceptable, with "leave it powered for 3 minutes" on the card?
9. **Lite's tunnel restores after a reboot by default** (`VibeTunnel.restoreIfWanted`). The pocket
   box now asks first, through **Resume the tunnel after a restart**. Should Lite get the same
   switch, so the two portable servers behave alike?

## Not done here (from the brief)

- **Clock from DAB/RDS** (step 5). The box keeps the last synced time on the settings partition
  instead: wrong after time off, but monotonic.
- **"Use this phone's location"** (step 4). It cannot work in the captive sheet anyway; it belongs
  in Safari.
- **Offline audit** of EiBi, logos and listing in hotspot mode. These mostly fail quietly today;
  not re-audited.
- **AP + client together** (one radio). A second USB Wi-Fi adapter would allow it.
