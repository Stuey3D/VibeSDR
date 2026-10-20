# BRIEF — VibeServer Portable: realtime audio over a phone hotspot

**Status:** check + small hardening (Phase 0), then new work (Phases 1–2)
**Applies to:** `vibeserver/linux/pocket/vibeserver-pocket`, `vibeserver/main.cpp` (front door), the per-radio server process
**Reported by:** Stuart, 2026-10-20

## The problem

*(Corrected 2026-10-20 — the first draft described an iPhone hotspot. What actually happened is below.)*

Stuart's iPhone had no signal, so the Portable (Pi 3A+ + RTL-SDR V4) **and** the iPhone running VibeSDR both joined a
Personal Hotspot on his **Android work phone**. That hotspot was in 2.4 GHz compatibility mode, because the Pi never
found it on 5 GHz (Phase 3). Audio was fine until he put VibeSDR in the background and browsed on the iPhone: the
iPhone's own internet traffic, going through the same hotspot, made the Portable's audio stutter and drop out.

```
  Pi 3A+ ──(hop 1: Pi → hotspot)──► Android hotspot ──(hop 2: hotspot → iPhone)──► iPhone (VibeSDR in background)
                                         ▲    │
                                         │    └── the iPhone's browsing: hotspot → iPhone downlink, same channel
                                         └─────── mobile data
  Both hops, and the browsing, share ONE 2.4 GHz channel (one radio, half-duplex airtime).
```

Every audio packet crosses the air **twice** on that one channel, and the browsing competes with it on the second hop —
in the hotspot's own transmit queue for the iPhone. The phones can't be configured from here, so this brief covers what
the Pi controls, and notes what the app could do later:

1. how its own radio behaves (power save) — Phase 0;
2. which WMM queue its frames go out in, and whether that tag can carry over hop 2 — Phase 1;
3. what it sends first when airtime runs short — Phase 2;
4. why it couldn't use the 5 GHz hotspot in the first place — Phase 3 (findings only).

The bottleneck is **2.4 GHz airtime and latency spikes**, not raw throughput. Opus audio is tiny; the waterfall isn't.

### The second hop — why the Pi's tag may matter on the phone too
An Android hotspot is a Linux **mac80211 AP** (the phone's Wi-Fi driver plus hostapd). When it forwards a packet from the
Pi to the iPhone, the kernel picks the WMM access category from the packet's **DSCP** (`cfg80211_classify8021d`: the
RFC 8325 table on kernels ≥ 5.x, plain `DSCP >> 3` before that). So an EF (`0xB8`) tag the Pi puts on its audio packets
**may carry through to the hotspot → iPhone downlink**, which is exactly where the iPhone's browsing competes. It's not
guaranteed: some vendor offload paths (tethering offload in the modem/Wi-Fi firmware) re-mark or ignore DSCP, and a
NAT/forward path could re-mark, though Linux forwarding preserves DSCP by default. Hence the sniffer check in Phase 1.3
looks at **both** hops.

### The client's uplink matters too
The iPhone's TCP ACKs for the audio socket, and the app's own requests, also cross the air (iPhone → hotspot). See
**Follow-up A** at the end: whether the app can tag its audio WebSocket. Not done here — app changes ride a later release.

---

## Phase 0 — Verify Wi-Fi power save is off (check, don't assume)

What a read of the source shows (verify each point):

- `802-11-wireless.powersave 2` (NM "disable") is set on every ranked client network when the setup page saves it (~line 1083), and on the setup/fallback hotspots in `ap_args()` (~line 763).
- That is the **only** place it is set. Nothing checks it at runtime, and no test asserts it.

### 0.1 Existing connections — **TRAP**
Connections saved by an older build, before this line existed, keep NM's default (`0`, which means use the global default, which means **on** for brcmfmac) until they are re-saved.

- Check whether the converge/update path rewrites existing `vibe-net-*` connections.
- If it doesn't, add an idempotent migration at service start: for every connection we own with `802-11-wireless.powersave` ≠ `2`, `nmcli connection modify … 802-11-wireless.powersave 2`.
- The migration must not bounce a live link. A modify on the active connection takes effect at next activation, which is acceptable. Do not `down`/`up` it.

### 0.2 Belt and braces at runtime
After each successful join, run `iw dev <dev> get power_save`.

- If it reports `on`, log it once and apply `iw dev <dev> set power_save off`.
- Surface the state in the existing link health that `iw … link` / `station dump` already feeds, so it is visible in the setup page.

**TRAP:** brcmfmac can re-enable power save after a reassociation or firmware recovery. The check belongs on every join, not just at boot.

### 0.3 A test that guards it
In `test_vibeserver_pocket.py`, add tests that:

- assert the args for every saved client network and both APs contain `802-11-wireless.powersave`, `2`;
- assert the migration in 0.1 modifies a stale connection and leaves a correct one alone.

### 0.4 On-device check (Stuart)
On a running Portable joined to the hotspot, `iw dev wlan0 get power_save` must say `off`. Report what a box upgraded from an older image says *before* the fix.

### Phase 0 — done (2026-10-20)
**Verified against the source:**
- ✓ `802-11-wireless.powersave 2` is in `apply()` for every ranked network (add *and* modify-in-place) and in `ap_args()`
  for both hotspots. The line numbers have drifted since the brief (they are now ~1148 and ~828).
- ✓ That was the only place. `vibeserver-pocket-converge` never touches NetworkManager profiles (it writes NM's
  connectivity conf, nothing under `system-connections`), so **an Update did not fix an older box's profiles**. No test
  asserted it, and nothing checked the radio at runtime.
- ★ The profiles persist. `/etc/NetworkManager/system-connections` is bind-mounted from `/data/nm` (vibeserver-pocket-data),
  so a `connection modify` on the read-only-root Portable survives a reboot.

**Changed (`vibeserver/linux/pocket/vibeserver-pocket`):**
- `Box.migrate_powersave()` runs from `setup_once()` at every service start. Every profile we own (`vibe-net-N`, `vibe-ap`,
  `vibe-setup-ap`) whose powersave doesn't read as disable gets `nmcli connection modify <name> 802-11-wireless.powersave 2`.
  It accepts all three spellings nmcli prints (`2`, `2 (disable)`, `disable`) and works by name, so it needs no radio.
  It never runs down/up, and it leaves a profile that isn't ours (e.g. Raspberry Pi Imager's `preconfigured`) alone.
- `Box.powersave_check()` runs on every new join and again every 60 s while joined. It calls `iw dev <dev> get power_save`;
  if that says `on`, it applies `set power_save off`, re-reads, and logs once per join. The 60 s re-check catches brcmfmac
  turning it back on after a reassociation or firmware recovery that NM never reports.
- The result is `link.powerSave` (`"off"` / `"on"` / null) in `state.json`. The setup page's Wi-Fi line now shows signal,
  retries and power saving, and warns if the radio refused to switch it off. The app's `/vibeserver/pocket/connection`
  answer is **unchanged** (V11 lock).

**Tests (`test_vibeserver_pocket.py`, `powersave_phase0`):** they cover 0.3 as specified, plus:
- the migration's idempotence, a no-bounce check, and leaving foreign profiles alone;
- that it runs at start with no radio;
- that the runtime switch-off happens and is logged once;
- that the mid-join re-enable is caught;
- that a radio which refuses is reported as `on`, never `off`;
- that a hotspot is never asked.

Run against the old script, the suite fails.

---

## Phase 1 — Tag audio for WMM voice

Currently there are **no** `SO_PRIORITY`, `IP_TOS`, `IPV6_TCLASS` or `TCP_NODELAY` calls in `vibeserver/`.

### 1.1 Where to set it
The front door (`main.cpp` ~3839) routes by path *before* handing the fd to the radio process (fd passing). Socket options belong to the socket, so they survive the hand-off. Set them **at the door, once the path is known**:

| Path | `SO_PRIORITY` | DSCP (`IP_TOS` / `IPV6_TCLASS`) | Notes |
|---|---|---|---|
| `/ws/audio` | 6 (AC_VO) | EF, `0xB8` | plus `TCP_NODELAY` |
| `/ws/user-spectrum` (waterfall) | 0 or 1 | CS0, `0x00` (or CS1, `0x20`) | must yield to audio |
| control / `/connection` | 0 | CS0 | small, leave default |
| map tiles / bulk | 1 (background) | CS1, `0x20` | already paced by `vibe_bulk_pace.h` |

**TRAP:** Use `IPV6_TCLASS` for v6 sockets. `IP_TOS` silently does nothing on an IPv6 socket, including a v4-mapped one, depending on kernel.

**TRAP:** DSCP EF alone maps to AC_VI on older kernels (DSCP>>3). Newer kernels use the RFC 8325 table. Setting `SO_PRIORITY` 6 explicitly makes the Pi's choice independent of the kernel version.

**TRAP:** Never tag the waterfall or anything bulk as EF/priority 6. A high-volume flow in AC_VO starves the channel, including our own ACKs.

### 1.2 Scope
- Apply this on **all** Linux VibeServers, not only the Portable. It is harmless on Ethernet.
- Android VibeServer/Lite: `Socket.setTrafficClass(0xB8)` on the audio socket only, if the Kotlin layer owns the socket. Otherwise, note it and skip.
- Carriers strip DSCP, so expect no effect over the tunnel or the internet. This is for the local Wi-Fi hop.

### 1.3 Verify (Stuart)
Use a Mac in Wireless Diagnostics → Sniffer on the hotspot's channel, and filter in Wireshark with `wlan.qos.tid`:

- Pi → phone audio frames should show **TID 6**.
- Also note what the phone → client hop shows. This tells us whether iOS honours the tag when it forwards packets. That would be a bonus, not a requirement.

**Updated for the real incident (Android hotspot, two hops on one channel):** see *Checks for Stuart* at the end for the
exact sniffer recipe. In short, capture **both** hops:
- **Hop 1, Pi → hotspot:** audio = TID 6, waterfall = TID 0, map = TID 1.
- **Hop 2, hotspot → iPhone:** what TID the hotspot gives the *forwarded* audio (EF should mean TID 6 or 5), and the
  iPhone's browsing (TID 0). If the audio rides TID 5/6 there, the Pi's mark protects the hop where the browsing competes.

### Phase 1 — done (2026-10-20)
**Verified against the source, with corrections:**
- ✓ No `SO_PRIORITY`, `IP_TOS` or `IPV6_TCLASS` anywhere — not in `vibeserver/`, and not in
  `android/app/src/main/cpp/`, where the server's socket code actually lives (`net_shim.cpp`, `local_sdr_shim.cpp`).
- ✗ **`TCP_NODELAY` was already set**, on every accepted socket (`net::Listener::accept`, `net_shim.cpp`) and every outgoing
  one. That includes the audio socket, since the door's accepted fd carries it through the hand-off.
- ✓ The hand-off works as the brief says, but the code lives in the shim. `main.cpp` (~3802–3839 now) only supplies the
  *router* lambda. `LocalSdrShim::routeOrHandle()` reads the request line with `MSG_PEEK`, asks the router, and
  `vibe::sendFdTo()` passes the fd over `SCM_RIGHTS`. The radio process adopts it in `handoffLoop()`.
- ★ **Marked in the radio process, not at the door** (`acceptWs()`, once the path is known). Socket options travel with
  the socket, so the effect is the same for a handed-over connection. But this one line also covers a single-radio
  server with no door, a client on a radio's own port, and **Android**, which has no door at all. Marking at the door would
  have missed all three.
- ★ The server only listens on IPv4 today (`net::listen` binds `AF_INET`). The IPv6 branch is there for when that changes:
  an `AF_INET6` socket gets both `IPV6_TCLASS` and `IP_TOS`, the latter for v4-mapped traffic.

**Two traps found that the brief didn't list:**
- ★★ **On Linux, setting `IP_TOS` overwrites `SO_PRIORITY`** (`rt_tos2priority`: EF → 4). Measured on the Lenovo
  (kernel 6.8): setting priority 6 and *then* TOS reads back **4**. `apply()` sets TOS first and priority second, and the
  test reads 6 back on Linux.
- ★★ **Which mark a driver honours depends on the driver:**
  - **brcmfmac** (the Pi's onboard Wi-Fi, FullMAC) uses skb->priority 1–7 *as* the 802.1d tag, and classifies by DSCP
    only when it is 0. So SO_PRIORITY 6 is TID 6 on a Pi, whatever its kernel's DSCP table.
  - **mac80211** drivers (most USB dongles and PCIe cards on other Linux servers) ignore priorities below 256 and use
    DSCP. The 256+ override would need CAP_NET_ADMIN, which the daemon doesn't have.
  - So both marks go on every time. The next hop (the phone's hotspot) only ever sees the DSCP anyway.
  - Many Android phones use vendor FullMAC drivers (Qualcomm, Broadcom) rather than mac80211. Their DSCP → TID map is
    their own, typically `DSCP >> 3`, which gives TID 5 (AC_VI) for EF. That is still above the browsing.

**Changed:**
- New `android/app/src/main/cpp/vibe_wifi_qos.h`: `marksFor()` holds the table, `apply()` sets the options in the right
  order and is best effort per option.
- `local_sdr_shim.cpp`: `acceptWs()` marks `/ws/audio` as Audio (priority 6, EF, NODELAY) and `/ws/user-spectrum` as
  Waterfall (priority 0, CS0), and logs once per class.
- `/mapdata/` and `/mapgl/` are marked as Bulk (priority 1, CS1), alongside the existing bulk thread priority and pacing.
- Control sockets are left at the default.
- I chose CS0 rather than CS1 for the waterfall. AC_BK can be starved almost to nothing on a busy hotspot, which would
  read as a frozen waterfall. Phase 2 does the yielding instead, under our control.

**Scope:**
- Every Linux VibeServer gets this. So do the Mac app, through the same shim: IP_TOS only, since macOS has no SO_PRIORITY.
- **Android VibeServer/Lite needed no Kotlin change**: the native shim owns the socket. An app process may set
  SO_PRIORITY 0–6 and IP_TOS without privileges.
- No wire change, and a client sees nothing.

**Test:** `vibeserver/test-wifi-qos.cpp` is picked up by `scripts/run-tests.sh`. It checks the table, then marks a real
loopback TCP pair (IPv4 and IPv6) and reads back IP_TOS / IPV6_TCLASS / TCP_NODELAY, plus SO_PRIORITY on Linux. It also
checks that re-marking a socket as waterfall really takes it out of voice. It passes on the Mac (SO_PRIORITY skipped,
reported as not applied) and on the Lenovo, where SO_PRIORITY reads 6.

---

## Phase 2 — Audio-first sending (the real QoS we control)

Audio and waterfall are already separate WebSockets. What's missing is a rule for when they compete.

### 2.1 Waterfall yields to audio
In the radio process, when a client's **audio** socket has unsent data queued (`SIOCOUTQ`/`TIOCOUTQ` above a small threshold, or the WS send queue non-empty), **drop or decimate waterfall frames for that client** until audio drains.

- Drop whole frames; never send a partial one.
- Resume at full rate once audio has been clear for ~1 s, so it doesn't flap.

### 2.2 Keep audio from queueing behind itself
On the audio socket, use `TCP_NODELAY` and a modest `SO_SNDBUF` so that stale audio is dropped server-side rather than delivered late.

- Coordinate with the client jitter buffer, which is the thing that already absorbs hiccups. Do not shrink the buffer so far that a normal Wi-Fi retry burst causes a gap.
- Measure on the Pi 3A+ first.

### 2.3 Optional: step Opus down under pressure
If audio itself backs up for more than ~2 s, step the Opus bitrate down one notch and recover slowly.

- Only if the existing encoder path makes this cheap; otherwise defer it.
- Old apps must be unaffected, per V11 core lock: no protocol change.

---

## Out of scope
- Anything on the phone or hotspot side.
- 5 GHz: the 3A+ supports it, but the iPhone's non-compatibility hotspot often sits on ch 149+. Investigate `iw reg get` / `iw list` with `GB` separately. **Do not** fake the country code.
- Bluetooth on the hotspot phone (2.4 GHz coexistence). Mention it in the Portable docs as a user tip only.

## Done when
- Phase 0: the tests pass, an upgraded box reports `power_save off`, and the stale-connection migration is proven.
- Phase 1: sniffer shows TID 6 on the Pi's audio frames, and the waterfall is not in AC_VO.
- Phase 2: with the hotspot phone streaming video, audio holds and the waterfall visibly slows instead.
