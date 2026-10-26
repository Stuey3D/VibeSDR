VibeServer for Windows — TEST BUILD 2 (Milestone 1)
====================================================

This is a test build for checking how VibeServer talks to an RTL-SDR on Windows. It is not a release:
there is no installer, no tray icon, no tunnel and no DAB+ audio yet. It is unsigned, so Windows may say
"Windows protected your PC" — click More info, then Run anyway.

What is in this folder
  vibeserver.exe       the server (RTL-SDR in this build). Run from a terminal:
                         vibeserver --serve --admin-pass <a password you choose>
                       then open http://localhost:48000/setup . A dongle Windows is using as a TV tuner is
                       offered there: SWITCH (Windows asks once for permission) or NOT NOW. Ctrl+C stops it.
  vibe-usbprobe.exe    a DIAGNOSTIC: shows which driver Windows has on each dongle and streams a clean-data
                       test. Not needed for the normal flow. Start with:  vibe-usbprobe report
  WINDOWS-M1-TEST.md   the step-by-step test sheet (open it in Notepad or any browser). Follow it in order.
  LICENSE.txt          VibeServer's licence (GPL-3.0)
  licences\            the licences of the libraries built into these programs

Nothing is installed. The only things these programs change are:
  - C:\ProgramData\VibeServer\   (settings and logs)
  - a dongle's driver, ONLY when you press SWITCH (undo: Give back), or run "vibe-usbprobe bind"
    (undo: "vibe-usbprobe giveback") — always Windows' own WinUSB, never a driver or certificate of ours
  - "Start VibeServer when Windows starts", ONLY if left on at the first Save and start (or switched on):
    a Scheduled Task "VibeServer" (starts at boot) or a sign-in entry. Switch it off in the setup page.
The test sheet's part G lists how to undo everything.

Source: the VibeSDR repository, branch windows-m1 (vibeserver/, android/app/src/main/cpp/ and
vibeserver/windows/). The libraries are built from pinned upstream sources by vibeserver/windows/build-deps.sh:
libusb 1.0.30, librtlsdr (osmocom rtl-sdr 797f814 = 2.0.3), libopus 1.6.1.
