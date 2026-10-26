# VibeServer for Windows — Milestone 1 test sheet

*For Stuart's Windows 11 PC, with the RTL-SDR Blog V3 and V4. Written 2026-10-26 with test build 1
(`vibeserver-windows-x64-test1.zip`). The plan is `docs/WINDOWS-PORT-PLAN.md`; section numbers below refer to it.*

**What this test decides.** UsbDk "borrow" becomes the **default** for Windows if, on this PC:

1. UsbDk installs and the PC **reboots normally** (the Windows 11 24H2 report in §2.3).
2. It borrows a **V3 and a V4** from the stock DVB-T driver (or from no driver).
3. 2.4 MS/s streams **clean** (no lost data).
4. Hand-back and re-borrow are **reliable** (the close/reopen race in §2.3).

If any of these fails, binding Windows' own WinUSB to the dongle becomes the default instead (part D). Part D is
also tested on its own, because it is the fallback either way. Nothing in this sheet needs Zadig.

**Time:** about 1½ hours for everything. Parts A, B and E are the important ones. If time is short, do A, B1–B4 and E1.

---

## 0. Before you start (10 minutes)

| # | Do this | Why |
|---|---|---|
| 0.1 | **Create a restore point.** Start → type *Create a restore point* → **Create…** → name it `before VibeServer M1`. | UsbDk is a USB filter on every hub. If the PC misbehaves after installing it, this restore point is the way back (§2.3). |
| 0.2 | Unzip `vibeserver-windows-x64-test1.zip` to a folder, e.g. `C:\VibeServer-test`. | Everything runs from that folder. Nothing is installed. |
| 0.3 | Right-click each `.exe` → **Properties** → if there is an **Unblock** tick-box at the bottom, tick it → OK. | Downloaded files are marked. Unblocking avoids a SmartScreen prompt on every run. |
| 0.4 | Open **Terminal** (or Command Prompt) **as a normal user** (not administrator) and `cd C:\VibeServer-test`. | Every command below is typed here. The two driver commands ask for admin themselves. |
| 0.5 | Check the dongles' current state: run `vibe-usbprobe report` with **both dongles unplugged** and keep the output. | A baseline. |
| 0.6 | Note **Memory Integrity**: Windows Security → Device security → Core isolation. `report` prints it too. | Part B is run with Memory Integrity **on** first; part F turns it off and repeats the short version. |

> **If Windows shows "Windows protected your PC"**: click **More info → Run anyway**. The test build is unsigned
> (signing comes later, through SignPath — §4.7). If there is **no** *Run anyway* button, Smart App Control is on:
> note it in the results and stop; that is itself an answer the plan needs.

**Everything the probe prints is also appended to `vibe-usbprobe.log`** in the same folder. At the end, send that
file back — it holds the whole session.

---

## 1. How to read the probe's output

`vibe-usbprobe report` prints, for each radio node:

```
[1] Generic RTL2832U OEM 0bda:2838  (interface 0)  ← the radio's node
    instance:  USB\VID_0BDA&PID_2838&MI_00\7&...
    driver:    TV tuner (DVB-T/BDA)  [service "RTL2832UBDA", class "MEDIA"]
    VibeServer: RTL-SDR found — Windows is using it as a TV tuner (...)
```

- **"← the radio's node"** marks the one that matters (interface 0 of the dongle). A V3/V4 normally shows a
  *composite parent* and two interfaces; interface 1 (the IR receiver) is left alone.
- **driver** is what Windows has bound right now: `no driver`, `TV tuner (DVB-T/BDA)`, `WinUSB`, `libusbK`,
  `libusb0`, or `UsbDk (borrowed)`. Please copy the **service name in quotes** exactly — the plan does not know the
  Realtek driver's real service name yet.
- **libusb (WinUSB) / libusb (UsbDk)** lines say whether the radio library could open the dongle each way:
  `open OK` is a pass for that route.
- **VibeServer would use** is the route the server will pick on its own.

`stream` and `cycle` put the RTL2832 into its **counter test mode**: instead of radio samples it sends a running
count, so every lost byte shows as a **jump**. `jumps 0` is the only clean result. `% of 2.40 MS/s` should be
99–100 %.

---

## A. The states a dongle can be in (5 minutes per dongle)

Plug in **the V3 only**, wait 10 seconds, run `vibe-usbprobe report`. Then unplug it, plug in **the V4 only**, repeat.

Record for each:

| Dongle | State seen (driver line, service name in quotes) | Problem code (if any) | What "VibeServer would use" says |
|---|---|---|---|
| V3 | | | |
| V4 | | | |

The possible states (§2, "what stock Windows means"):

- **(a1) no driver** — `driver: no driver`, usually `problem: code 28`. Windows found no TV driver.
- **(a2) TV tuner** — a Realtek BDA driver from Windows Update or the dongle's CD.
- **(b) Zadig** — `WinUSB` (or `libusbK` / `libusb0`) because Zadig or SDR# set it up earlier.

If a dongle is already in state (b) from an earlier Zadig, that is fine — part C covers it — but for parts B and D
we need a dongle in (a1) or (a2). To put a Zadig'd dongle back to stock: Device Manager → *Universal Serial Bus
devices* → the dongle (*Bulk-In, Interface*) → right-click → **Uninstall device** → tick **Attempt to remove the
driver for this device** → Uninstall → unplug and replug. Then run `report` again.

**Pass:** each dongle is listed, its interface 0 is marked, and the service name is shown.

---

## B. UsbDk "borrow" — the intended default (40 minutes)

### B1. Install UsbDk and REBOOT (the #1 risk)

1. Download **UsbDk 1.0.22 x64**: <https://www.spice-space.org/download/windows/usbdk/UsbDk_1.0.22_x64.msi>
   (it is the newest installer; Red Hat's, Apache-2.0).
2. Run it. Accept the UAC prompt. Note whether it asks for a reboot.
3. **Reboot the PC now, whether asked or not.**

| Check | Result |
|---|---|
| Installer finished without error | |
| Asked for a reboot? | |
| **PC booted normally to the desktop** | |
| Keyboard and mouse work after the reboot | |
| Windows version (`report` prints it, e.g. 24H2 26100.x) | |
| Memory Integrity during this install | |

**Fail → stop part B here.** If the PC does not boot, or the keyboard/mouse are dead: boot into recovery
(hold **Shift** while clicking Restart → Troubleshoot → Advanced options → **System Restore**) and restore
`before VibeServer M1`. Then go to part D. That outcome alone answers the question: UsbDk cannot be the default.

### B2. Is UsbDk usable by libusb?

Run `vibe-usbprobe usbdk`.

**Pass:** `service installed`, `hub filter registered`, and **`UsbDkHelper.dll C:\Windows\system32\UsbDkHelper.dll`**.
If it says *not in System32* but *also at C:\Program Files\UsbDk Runtime Library\…*, record that — libusb only
looks in System32, so the server would need to load it from there itself (a small change for M2).

### B3. Borrow the V3 from its stock driver and stream 60 s at 2.4 MS/s

Plug in the V3 (state a1 or a2 — see part A). Then:

```
vibe-usbprobe report
vibe-usbprobe stream --usbdk --seconds 60
```

`report` should now end with `VibeServer would use: UsbDk (borrow)` and show `libusb (UsbDk): 0bda:2838 … open OK`.

`stream` prints one line a second and ends with `RESULT:`.

| Check | Pass when | Result |
|---|---|---|
| Opens | `opened in … ms` | |
| Counter run | `jumps 0`, `99–100 %`, `CLEAN` | |
| Tuned check (100 MHz) | `samples arriving` | |
| During the stream, Device Manager (View → Devices by connection) shows the dongle under **UsbDk** | yes/no | |
| **After** it finishes: run `vibe-usbprobe list` | the dongle is back on its **original** driver (a1 or a2) | |

### B4. Same with the V4, plus HF and the bias-T

Unplug the V3, plug in the V4:

```
vibe-usbprobe stream --usbdk --seconds 60 --hf --bias-t
```

Pass as B3, plus: `tuner type 6 (R828D — a V4)`, the **HF 7.100 MHz** line says `samples arriving` (an antenna helps;
even a bit of wire), and `bias-T on=0 off=0`. If you have an LNA or a meter, the bias-T is on for 3 seconds — check
it powers.

### B5. Hand-back and re-borrow — 50 cycles (the race)

With the V4 still plugged in:

```
vibe-usbprobe cycle --usbdk --cycles 50
```

Each cycle opens (borrows), streams 5 s in counter mode, closes (hands back), waits ½ s, and goes again. It takes
about 5 minutes.

**Pass:** `cycle RESULT: 50/50 clean, 0 failed to open`. `needed a retry` above zero is not a failure, but **record
the number** — it is the re-bind race, and it tells M2 whether the server must hold the borrow for its whole life.
Then run `vibe-usbprobe list`: the dongle must be back on its original driver.

Repeat with the V3: `vibe-usbprobe cycle --usbdk --cycles 50`.

### B6. The real server, borrowing

```
vibeserver --device 0 --mode wfm --freq 98800000
```

(any strong local FM station). The first lines say which USB route it took — expect
`VibeServer: USB path = UsbDk (borrow)`. Windows Firewall will ask about network access the first time: tick
**Private networks**, click **Allow**.

Then, from a browser **on another computer or phone on the same network**, open `http://<this PC's IP>:48000/`
(the IP: `ipconfig` → IPv4 Address). Also try `http://localhost:48000/` on the PC itself.

| Check | Result |
|---|---|
| Web client loads; waterfall moves | |
| FM audio plays, clean, no stutter for 5 minutes | |
| Retune to another station and back; change the sample rate in the client once | |
| Task Manager: CPU of `vibeserver.exe` | |
| **Ctrl+C** in the terminal stops it within a few seconds | |
| After stopping, `vibe-usbprobe list`: the dongle is back on its original driver | |

### B7. Leave UsbDk installed for part C if you are continuing, otherwise uninstall it (G2).

---

## C. A dongle Zadig already set up (10 minutes)

Use Zadig only if a dongle is not already in state (b) — it is the state many users arrive in, not a step we
ask anyone to take. If you would rather not, skip C: it is the best-understood route.

1. Zadig: Options → List All Devices → **Bulk-In, Interface (Interface 0)** → **WinUSB** → Replace Driver.
2. `vibe-usbprobe report` → the dongle shows `WinUSB`, `VibeServer would use: WinUSB — every RTL-SDR is already on a
   libusb driver`.
3. `vibe-usbprobe stream --seconds 30` (no `--usbdk`) → `CLEAN`.
4. `vibeserver --device 0 --mode wfm --freq 98800000` → first line says `USB path = WinUSB`; listen briefly.

To undo: the Device Manager *Uninstall device* steps at the end of part A.

---

## D. The fallback: Windows' own WinUSB on one dongle (20 minutes)

This is the "Give this dongle to VibeServer" button of §3, done by the probe. It uses Microsoft's own
`winusb.inf` — no driver of ours, no certificate — on **that one dongle only**. Run it with UsbDk **uninstalled**
(G2) so the result is not mixed up with the borrow.

1. Dongle in state (a1) or (a2) (part A). `vibe-usbprobe list` → note its driver.
2. `vibe-usbprobe bind` → **one UAC prompt** → Yes. Its output is shown in the same window when it finishes.

   It tries three ways to find "WinUsb Device" and says what each one found, e.g.
   `winusb.inf as a class list ("Have Disk"): found "WinUsb Device" by … in C:\Windows\INF\winusb.inf`.
   **Please copy all of those lines** — which way works from (a1) and from (a2) is exactly what §2.4 marks unverified.

| Check | Pass when | Result (a1) | Result (a2) |
|---|---|---|---|
| A way to "WinUsb Device" was found | one of the three lines says `found` | | |
| `DiInstallDevice: OK` | no error; note if it asks for a REBOOT | | |
| `now: WinUSB [service "WinUSB" …]` | | | |
| `libusb (WinUSB): … open OK` | | | |

3. `vibe-usbprobe stream --seconds 60` → `CLEAN`. With the V4 add `--hf --bias-t`.
4. `vibe-usbprobe cycle --cycles 50` → `50/50 clean`. (With WinUSB there is no hand-back, so retries should be 0.)
5. `vibeserver --device 0 --mode wfm --freq 98800000` → `USB path = WinUSB`; listen from another computer as in B6.
6. **Give it back:** `vibe-usbprobe giveback` → UAC → Yes. **Pass:** the dongle returns to what it had before step 2
   (the TV driver, or no driver), and `DiUninstallDevice: OK`.
7. Replug the dongle and `vibe-usbprobe list` once more: it must still be on its original driver (Windows must not
   have remembered WinUSB).

**If `bind` reports `FAIL — Windows offered no way to pick WinUsb Device`**, try it by hand, and record whether the
choice exists: Device Manager → the dongle's **Bulk-In, Interface** (or the TV-tuner entry) → Update driver →
*Browse my computer* → *Let me pick from a list* → is **Universal Serial Bus devices** in the list, and inside it
**WinUsb Device** (manufacturer *WinUsb Device*)? Pick it, accept the warning, then run `vibe-usbprobe report` and
`stream`. Undo with `vibe-usbprobe giveback`.

---

## E. Front door with two radios (15 minutes, after B or D)

Both dongles must be usable by the route being tested (both borrowed via UsbDk, or both bound in part D).

1. With both dongles plugged in: `vibeserver --serve --admin-pass <a password you choose>` (type your own; it is
   the admin password of this test server). With no settings file yet it runs the first dongle directly and
   serves the **setup page** on port 48000. Open `http://localhost:48000/`, sign in with that password, enable both
   dongles and save. (Settings live in `C:\ProgramData\VibeServer\config.json`.)
   ★ Test build 1 has no first-run screen of its own: started with no dongle and no settings it stops with
   `no SDR found`. The first-run flow (tray app, choose-a-password page) is Milestone 3.
2. After saving, it restarts itself. The terminal should show `front door on port 48000` and `started radio <serial>`
   twice (each radio is its own process — Task Manager shows three `vibeserver.exe`).
3. From another computer: `http://<PC IP>:48000/` lists both radios; open each and listen.

| Check | Result |
|---|---|
| Both radios listed and playing at the same time | |
| A connection to each radio works through port 48000 (the hand-off, §4.2) | |
| **Ctrl+C** in the terminal: all three `vibeserver.exe` gone within ~5 s (Task Manager) | |
| Close the terminal window instead (start again first): all three gone | |
| After that, `vibe-usbprobe list`: dongles back on their original drivers (UsbDk route) | |

---

## F. Memory Integrity OFF (10 minutes, only if B passed)

Windows Security → Device security → Core isolation → Memory integrity **Off** → reboot. Then repeat **B3** and
**B5 with 20 cycles** for one dongle. Turn Memory Integrity back **On** afterwards and reboot.

---

## G. Undo everything

| # | Undo | How |
|---|---|---|
| G1 | A `bind` | `vibe-usbprobe giveback` (UAC). Or Device Manager → *Universal Serial Bus devices* → the dongle → Uninstall device (tick *Attempt to remove the driver* is **not** needed — winusb.inf is Windows' own) → Action → Scan for hardware changes. |
| G2 | UsbDk | Settings → Apps → Installed apps → **UsbDk Runtime Libraries** → Uninstall. Then **reboot**, and check the keyboard and mouse work (the #120 report in §2.3). If they do not: Shift+Restart → System Restore → `before VibeServer M1`. |
| G3 | VibeServer's settings | Delete `C:\ProgramData\VibeServer` (config, `usbprobe-bound.txt`, and `run\`). |
| G4 | The firewall rule | Windows Security → Firewall & network protection → Allow an app through firewall → remove `vibeserver.exe`. |
| G5 | The test folder | Delete `C:\VibeServer-test`. |
| G6 | Zadig (if part C was done) | Device Manager uninstall, as at the end of part A. |
| G7 | Last resort | System Restore → `before VibeServer M1`. |

Nothing else is changed by these programs: no service is installed, nothing is added to start-up, nothing runs
after the terminal is closed.

---

## Results to send back

1. `vibe-usbprobe.log` from the test folder (all the probe's output, in order).
2. The tables above, filled in (a photo of a printed copy is fine).
3. Anything odd: a stutter, a beep from Windows when a dongle is borrowed, a Device Manager entry that looked
   wrong, any message that did not make sense.

### The go / no-go these results decide (from §5)

- **UsbDk becomes the default** if: B1 boots normally with Memory Integrity on, B3/B4 are clean for both dongles,
  B5 is 50/50 for both, B6 plays, and G2 leaves keyboard and mouse working.
- **Windows' WinUSB bind becomes the default instead** if UsbDk fails any of those but D passes from (a1) and (a2).
- **Neither** → the plan's §2.5/§2.6 questions come back (a signed INF), and we talk before M2.
