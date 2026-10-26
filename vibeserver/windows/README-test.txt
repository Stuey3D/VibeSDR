VibeServer for Windows — TEST BUILD 1 (Milestone 1)
====================================================

This is a test build for checking how VibeServer talks to an RTL-SDR on Windows. It is not a release:
there is no installer, no tray icon, no tunnel and no DAB+ audio yet. It is unsigned, so Windows may say
"Windows protected your PC" — click More info, then Run anyway.

What is in this folder
  vibeserver.exe       the server (RTL-SDR in this build). Run from a terminal, e.g.
                         vibeserver --device 0 --mode wfm --freq 98800000
                       then open http://localhost:48000/ (or http://<this PC's IP>:48000/ from another device).
                       Ctrl+C stops it.
  vibe-usbprobe.exe    the driver test: shows which driver Windows has on each dongle, and tests the
                       UsbDk "borrow" and Windows' own WinUSB. Start with:  vibe-usbprobe report
  WINDOWS-M1-TEST.md   the step-by-step test sheet (open it in Notepad or any browser). Follow it in order.
  LICENSE.txt          VibeServer's licence (GPL-3.0)
  licences\            the licences of the libraries built into these programs

Nothing is installed. The only things these programs change are:
  - C:\ProgramData\VibeServer\   (settings, created when you save from the setup page)
  - a dongle's driver, ONLY when you run "vibe-usbprobe bind" (undo: "vibe-usbprobe giveback")
The test sheet's part G lists how to undo everything.

Source: the VibeSDR repository, branch windows-m1 (vibeserver/, android/app/src/main/cpp/ and
vibeserver/windows/). The libraries are built from pinned upstream sources by vibeserver/windows/build-deps.sh:
libusb 1.0.30, librtlsdr (osmocom rtl-sdr 797f814 = 2.0.3), libopus 1.6.1.
