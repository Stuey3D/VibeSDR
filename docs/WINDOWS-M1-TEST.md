# VibeServer for Windows — Milestone 1 test sheet (test build 2)

*For Stuart's Windows 11 PC, with the RTL-SDR Blog **V3**, **V4** and **V4L** (the V4L has the R828S tuner; our
librtlsdr, osmocom 797f814, already drives it on Linux). Written 2026-10-26 with test build 2
(`vibeserver-windows-x64-test2.zip`). The plan is `docs/WINDOWS-PORT-PLAN.md`; section numbers refer to it.*

**What this test decides.** Stuart, 2026-10-26: *"no manual Zadig messing around, ONE admin prompt to cleanly replace
the driver, and ONLY show a stick that is showing as a TV stick"* — and *"If VibeServer doesn't work without
workarounds and messing then Windows is not happening."* So the one question is:

> Plug in a stick Windows uses as a TV tuner (or has no driver for). Does VibeServer offer it, does **SWITCH** need
> **exactly one** Windows permission prompt, and does the dongle then work — for the V3, the V4 and the V4L — and does
> **Give back** return it to the TV driver?

What SWITCH does: Windows' own **WinUSB** driver (`C:\Windows\INF\winusb.inf`, Microsoft's, already on every Windows
PC) is installed on **that one dongle**. No driver of ours, no certificate (Zadig makes and trusts one — we do not),
and nothing about Windows' security is changed. The dongle's TV driver stays installed, which is how Give back works.

**Time:** about 1½ hours. Parts **B** and **C** are the important ones. If time is short: A, B1–B7, C1–C4.

---

## 0. Before you start (10 minutes)

| # | Do this | Why |
|---|---|---|
| 0.1 | *(Optional)* **Create a restore point**: Start → *Create a restore point* → **Create…** → `before VibeServer M1`. | A belt-and-braces way back. Nothing in the main test should need it. |
| 0.2 | Unzip `vibeserver-windows-x64-test2.zip` to **`C:\VibeServer-test`** — not Downloads or the Desktop. | Part C runs VibeServer at boot as Windows' background account, which cannot read your personal folders. (The page says so if you try.) |
| 0.3 | Right-click each `.exe` → **Properties** → if there is an **Unblock** tick-box, tick it → OK. | Downloaded files are marked; unblocking avoids a SmartScreen prompt each run. |
| 0.4 | Open **Terminal** **as a normal user** (not administrator) and `cd C:\VibeServer-test`. | VibeServer never runs as administrator. It asks Windows for permission itself, once, when a driver changes. |
| 0.5 | If an earlier test left settings, delete `C:\ProgramData\VibeServer` so this is a **first run**. | Part B starts from a new server. |
| 0.6 | Note **Memory Integrity** (Windows Security → Device security → Core isolation). Leave it as it is. | Every test runs with Windows' security as users have it. |

> **If Windows shows "Windows protected your PC"**: **More info → Run anyway**. The test build is unsigned (user
> builds are signed, §4.7). If there is **no** *Run anyway*, Smart App Control is on: note it and stop — that is
> itself an answer. **Never switch Smart App Control off.**
>
> ★★★ **No step may ever ask a user to lower their security** (Stuart, 2026-10-26) — no Memory Integrity / Smart App
> Control / Secure Boot / signature enforcement off, no test-signing, no certificates, no antivirus exclusions, no
> firewall off. If anything below seems to need one, **stop and report it**: that is the no-go.

**Logs to send back at the end** (all in `C:\ProgramData\VibeServer\` unless said):
`vibeserver-helper.log` (every step the permission helper took), `vibeserver.log` (part C), and
`C:\VibeServer-test\vibe-usbprobe.log` (part A). Also copy the terminal window's text from part B.

---

## A. What each dongle is now (5 minutes per dongle — diagnostic)

Plug in **one** dongle at a time, wait 10 seconds, run `vibe-usbprobe report`. Record:

| Dongle | `driver:` line (copy the **service name in quotes**) | Problem code | `VibeServer:` line |
|---|---|---|---|
| V3 | | | |
| V4 | | | |
| V4L | | | |

- **TV tuner (DVB-T/BDA)** — Windows (or a CD) installed a Realtek TV driver: state **a2**. This is the main case.
- **no driver**, usually `problem: code 28` — state **a1**. Offered the same way.
- **WinUSB / libusbK / libusb0** — Zadig or SDR# set it up before. VibeServer uses it silently (part D). To test
  the main flow on such a dongle, return it to stock first: Device Manager → *Universal Serial Bus devices* → the
  dongle (*Bulk-In, Interface*) → **Uninstall device** → tick **Attempt to remove the driver** → Uninstall →
  unplug and replug → `report` again.

---

## B. THE MAIN TEST — switch a TV stick to SDR from VibeServer (40 minutes)

Do B1–B9 with the **V3**, then repeat B1–B7 with the **V4** and the **V4L** (from B3; B1 only once). Each dongle on its
own the first time.

### B1. First start

With the dongle (state a2 or a1) plugged in:

```
vibeserver --serve --admin-pass <a password you choose>
```

| Check | Pass when | Result |
|---|---|---|
| It starts — no `no SDR found` | `starting as a new server; set it up at http://localhost:48000/setup` | |
| The offer, in the terminal | `Windows is using this dongle as a TV tuner (Generic RTL2832U OEM, serial …). Switch it to SDR so VibeServer can use it? You can switch it back any time.` (or *has no driver* for a1) | |
| No mention of Zadig anywhere | | |
| Windows Firewall asks about network access | tick **Private networks** → **Allow** (once; part C relies on this rule) | |

### B2. The offer in the setup page

On **this PC**, open `http://localhost:48000/setup` and sign in with that password.

| Check | Pass when | Result |
|---|---|---|
| Above the tabs: the same sentence, with **Switch** and **Not now** | | |
| Nothing is shown for a dongle that already works (if you have one plugged in too) | | |

### B3. SWITCH — exactly one prompt

Press **Switch**. The page says *waiting for Windows — look for the permission prompt on this PC's own screen*.

| Check | Pass when | Result |
|---|---|---|
| **How many** Windows permission (UAC) prompts appeared | **exactly 1**, for *VibeServer* (publisher *Unknown* — this test build is unsigned; user builds are signed) | |
| You pressed **Yes**; time until the page answered | | |
| The page's answer | `… is now an SDR. Added as a radio — set it up in its tab below.` and a **tab** for it appears | |
| Did Windows ask for a **restart**? (the page would say *Restart Windows to finish*) | ideally **no** — record either way | |
| `vibeserver-helper.log`: which way worked — copy the lines starting `A:` / `B:` / `C:` and `DiInstallDevice` | **A** expected for a1, **B** for a2 | |
| The terminal shows the dongle being found | | |

★★ **If it fails** with *Windows offered no way to pick its own WinUsb Device for this dongle*, or `DiInstallDevice`
returns an error: copy `vibeserver-helper.log`, then do part **M** (the manual check) for that dongle. Do **not** try
Zadig or anything that changes a security setting. That result is the finding.

### B4. Use it

In the dongle's tab: set it up (an FM frequency is enough), leave **Start VibeServer when Windows starts** ticked on
the Server tab, press **Save and start**.

| Check | Pass when | Result |
|---|---|---|
| Save asks Windows for permission **once more** (to start at boot) — the page said it would | 1 prompt → Yes | |
| It restarts and the radio plays: from a phone or another computer, `http://<this PC's IP>:48000/` (`ipconfig` → IPv4) | clean FM, no stutter for 5 minutes | |
| Retune, change the sample rate once in the client | | |
| Task Manager: CPU of `vibeserver.exe` (the radio's process) | | |
| **V4 only**: tune HF (7.100 MHz) with an antenna (a bit of wire) | signals | |

### B5. Clean data (stop the server first: Ctrl+C)

```
vibe-usbprobe stream --seconds 60                 (V4 and V4L: add  --hf --bias-t)
vibe-usbprobe cycle --cycles 20
```

**Pass:** `jumps 0`, `99–100 %`, `CLEAN`; `cycle RESULT: 20/20 clean` with retries 0 (WinUSB has no hand-back race).
V4: `tuner type 6 (R828D)`; **V4L: record the tuner type line** — R828S is new, the V4L may report as an R828D.

### B6. Replug — Windows remembers, VibeServer does not ask again

Start `vibeserver --serve` again (no password needed now). Unplug the dongle, plug it into the **same** port.

| Check | Pass when | Result |
|---|---|---|
| No offer appears for it (it already works) | | |
| The radio plays again (it may need **Check again** on the setup page, or a restart of VibeServer) | | |
| A **different** USB port: is it still WinUSB? (`vibe-usbprobe list`) — Windows remembers per port on some dongles | record | |

### B7. Give back — one prompt

In the setup page, under the offers: *… switched to SDR by VibeServer.* → **Give it back to Windows (TV tuner)**.

| Check | Pass when | Result |
|---|---|---|
| Exactly **1** permission prompt | | |
| The page: `… is back on Windows' own driver. Its settings are kept, paused…` | | |
| The radio stopped first (its tab shows paused) | | |
| `vibe-usbprobe list` | back on **TV tuner** (a2) or **no driver** (a1) — whatever part A recorded | |
| Within a few seconds the **offer** appears again (terminal, and on the page after **Check again**) | | |
| Restart asked for? | ideally no | |

To carry on, **Switch** it again (one prompt): its settings come back and it goes on the air without a new setup.

### B8. Plugging in while it runs

With VibeServer running, plug in another dongle that is still a TV stick.

**Pass:** within ~5 seconds the terminal prints the offer for it; the setup page shows it after **Check again**.

### B9. Saying no

- Press **Switch** and answer **No** to the permission prompt. **Pass:** *Windows' permission prompt was declined —
  nothing was changed*; the dongle is unchanged.
- Press **Not now**. **Pass:** the offer goes; it comes back after the dongle is replugged or VibeServer restarts
  (nothing is remembered).

| Dongle | from a2 (TV driver) | from a1 (no driver) | prompts per switch | restart needed? | stream CLEAN | give back OK |
|---|---|---|---|---|---|---|
| V3 | | | | | | |
| V4 | | | | | | |
| V4L | | | | | | |

---

## C. Start VibeServer when Windows starts — the BOOT CHECK (25 minutes)

After B4 the Server tab says **On: starts at boot — before anyone signs in, as Windows' restricted background account
(LOCAL SERVICE).**

| # | Check | Pass when | Result |
|---|---|---|---|
| C1 | Task Scheduler (Start → *Task Scheduler*) → Task Scheduler Library → **VibeServer** | trigger *At system startup*; *When running the task, use the following user account:* **LOCAL SERVICE**; *Run whether user is logged on or not* | |
| C2 | Stop the terminal copy (Ctrl+C). **Restart the PC. Do NOT sign in.** Wait 2 minutes. From a phone: `http://<PC IP>:48000/` | the radio list loads and the radio **plays** — with nobody signed in | |
| C3 | Sign in. Task Manager → Details → `vibeserver.exe` | **User name: LOCAL SERVICE** (never SYSTEM, never you); 3 processes for 1 radio (keeper, front door, radio) | |
| C4 | `C:\ProgramData\VibeServer\vibeserver.log` | the server's log, starting `keeper (boot): starting VibeServer` | |
| C5 | Task Manager → end the `vibeserver.exe` that is the **front door** (the one using port 48000; *Resource Monitor → Network → Listening Ports* shows its PID) | within ~20 s it is back (log: `keeper: VibeServer stopped … starting it again`), and the phone can listen again | |
| C6 | In the setup page (from the phone or this PC), change a setting and **Apply and Restart** | it comes back by itself (the log shows the restart) | |
| C7 | The setup page of this boot-started copy, Server tab | says it was started at boot and **cannot ask for permission**; the switch cannot be changed from here; a TV stick plugged in now is offered with the same explanation | |
| C8 | **Turn it off.** Task Scheduler → VibeServer → **End**. In the terminal: `vibeserver --serve`. Setup page → untick *Start VibeServer when Windows starts* | **1** permission prompt; the task is gone from Task Scheduler; the page says *Off* | |
| C9 | **Sign-in mode.** Tick it again and answer **No** to the prompt, then press **Start when I sign in instead** | no prompt; the page says *On: starts when you sign in to Windows (not at boot…)* | |
| C10 | Restart the PC: before signing in, the phone gets **nothing** (expected). Sign in | within ~1 minute the phone can listen. Task Manager: `vibeserver.exe` runs as **you**. Did a black window flash at sign-in? (record) | |
| C11 | Untick it | no prompt; *Off*; Start → *Startup apps* no longer lists VibeServer | |

> ★ If C2 fails because the phone cannot connect but C3 shows it running: the firewall. The rule Windows asked for in
> B1 is for the program, for any account — record whether it covered the background run. **Do not turn the firewall
> off**; that would be a no-go answer, not a fix.

---

## D. A dongle Zadig or SDR# already set up (5 minutes, optional)

If you have one on WinUSB already (part A said WinUSB / libusbK / libusb0): plug it in and start `vibeserver --serve`.
**Pass:** no offer at all — not in the terminal, not on the page — and it can be added as a radio from the *new radio*
banner and plays. There is **no** Give back for it (VibeServer did not switch it, so it will not undo it).

---

## E. Two radios on one port (15 minutes, after B)

Both dongles switched (B3). With both plugged in, `vibeserver --serve`, set both radios up and save.

| Check | Result |
|---|---|
| The terminal shows `front door on port 48000` and `started radio <serial>` twice | |
| From another computer, `http://<PC IP>:48000/` lists both; both play at the same time | |
| Two stock dongles share serial `00000001`: the page asks you to give one a new serial — note how that went | |
| **Ctrl+C**: every `vibeserver.exe` gone within ~5 s (Task Manager) | |
| Close the terminal window instead (start again first): all gone | |

---

## F. (removed)

★★★ **Nothing we ship may ever need Memory Integrity — or any other Windows security feature — switched off** (Stuart,
2026-10-26). So there is no such test.

---

## M. DIAGNOSTIC ONLY — does Windows itself offer "WinUsb Device" for this dongle?

★★ **This is not a user flow and never will be.** It is only for when B3 fails, to tell "VibeServer's code missed a
way" from "Windows offers no way at all". Record the answers; undo with `vibe-usbprobe giveback` afterwards.

Device Manager → the dongle's entry (the TV-tuner entry, or *Bulk-In, Interface*) → **Update driver** → *Browse my
computer* → *Let me pick from a list* → untick *Show compatible hardware* if it is ticked →

| Question | Answer |
|---|---|
| Is **Universal Serial Bus devices** in the list of device types? | |
| Inside it, manufacturer **WinUsb Device** → model **WinUsb Device**? | |
| If you pick it (accept the warning): does `vibe-usbprobe stream --seconds 30` say CLEAN? | |
| Did Windows ask for a restart? | |

`vibe-usbprobe bind` runs the same code as SWITCH and prints each way it tries — run it too if B3 failed, and send its
output (`vibe-usbprobe.log`).

---

## U. UsbDk "borrow" — OPTIONAL, no longer the default (30 minutes)

WinUSB is the route (Stuart, 2026-10-26). UsbDk is kept in the code, **off** unless `VIBE_USB_BACKEND=usbdk` is set.
Only test it if there is time, with the V3, and only with Memory Integrity as it is.

1. Install **UsbDk 1.0.22 x64** (<https://www.spice-space.org/download/windows/usbdk/UsbDk_1.0.22_x64.msi>), accept
   the UAC prompt, **reboot**. **Pass:** the PC boots normally, keyboard and mouse work. (If not: Shift+Restart →
   Troubleshoot → System Restore → the restore point from 0.1, and stop here.)
2. `vibe-usbprobe usbdk` → `service installed`, `UsbDkHelper.dll C:\Windows\system32\UsbDkHelper.dll`.
3. A dongle on its **TV driver** (give it back first, B7): `vibe-usbprobe stream --usbdk --seconds 60` → `CLEAN`;
   `vibe-usbprobe cycle --usbdk --cycles 50` → record clean / failed / retries.
4. `set VIBE_USB_BACKEND=usbdk` then `vibeserver --device 0 --mode wfm --freq 98800000` → first lines say
   `USB path = UsbDk (borrow)`; listen; Ctrl+C; `vibe-usbprobe list` → back on the TV driver.
5. Uninstall UsbDk (G3) and **reboot**; check keyboard and mouse.

---

## G. Undo everything

| # | Undo | How |
|---|---|---|
| G1 | A switched dongle | **Give it back** in the setup page (B7), or `vibe-usbprobe giveback`. By hand: Device Manager → *Universal Serial Bus devices* → the dongle → Uninstall device (do **not** tick *Attempt to remove the driver* — winusb.inf is Windows' own) → Action → Scan for hardware changes. |
| G2 | Start with Windows | Untick it in the setup page (C8/C11). By hand: Task Scheduler → **VibeServer** → Delete; and Start → *Startup apps* → VibeServer off (or `reg delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v VibeServer`). |
| G3 | UsbDk (part U) | Settings → Apps → **UsbDk Runtime Libraries** → Uninstall → **reboot** → check keyboard and mouse. |
| G4 | VibeServer's settings and logs | Delete `C:\ProgramData\VibeServer`. |
| G5 | The firewall rule | Windows Security → Firewall & network protection → Allow an app through firewall → remove `vibeserver.exe`. |
| G6 | The test folder | Delete `C:\VibeServer-test`. |
| G7 | Last resort | System Restore → `before VibeServer M1`. |

Nothing else is changed: no service is installed, no certificate, no driver of ours, no security setting.

---

## Results to send back

1. `C:\ProgramData\VibeServer\vibeserver-helper.log`, `C:\ProgramData\VibeServer\vibeserver.log`,
   `C:\VibeServer-test\vibe-usbprobe.log`, and the terminal text from part B.
2. The tables, filled in (a photo of a printed copy is fine) — above all the B matrix and C2/C3.
3. Anything odd: a second prompt you did not expect, a restart Windows asked for, a message that did not make sense.

### The go / no-go these results decide

- **GO** — B3 switches the V3, V4 and V4L from the TV driver (a2) with **one** prompt each, they stream clean (B5),
  Give back works (B7), and C2 plays with nobody signed in. Windows proceeds to M2 on the WinUSB route.
- **GO, with a note** — it works but Windows asks for a restart after the switch: acceptable if said plainly; record
  which dongles and states.
- **NO-GO** — Windows offers no way to put its own WinUsb Device on a stick on the TV driver without a driver or
  certificate of ours (B3 fails with way A, B and C, and part M shows Windows itself does not offer it). By Stuart's
  rule that ends the Windows port: no workaround, no Zadig, nothing that lowers security.
