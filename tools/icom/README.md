# Icom capture kit — for Saber

Thank you for testing this. VibeServer is learning to **listen through** an Icom (starting with your
IC-7300). It will never transmit. Before anything of ours talks to your radio for real, we need to
see exactly what the radio sends, and how fast. This kit records that. It changes nothing on the radio.

## Before you start (please do all four)

1. **Dummy load on the antenna socket.** Not because this kit transmits (it cannot), but because every
   test of this work is done into a dummy load first, as a rule.
2. **Close wfview / wfweb, WSJT-X rig control and any logger.** Only one program can use the radio's USB
   port at a time.
3. **On the radio, check these are OFF** — `MENU > SET > Connectors > USB SEND/Keying`:
   - **USB SEND**: OFF
   - **USB Keying (CW)**: OFF
   - **USB Keying (RTTY)**: OFF
   - and **VOX** is off.

   *Why:* on Linux and macOS, opening a serial port raises its DTR and RTS lines for an instant. If USB
   SEND or USB Keying is set to DTR or RTS, that alone would key the transmitter. (VibeServer itself does
   not open the port this way — it drives the radio's USB chip directly with those lines held low — but
   this little script uses an ordinary serial port.) On Windows the lines are never raised.
   If you normally use RTS for WSJT-X, write down your settings so you can put them back afterwards.
4. **CI-V settings** — `MENU > SET > Connectors > CI-V`: note what they are (don't change them yet):
   CI-V Baud Rate, CI-V Address, CI-V Transceive, CI-V USB Port, CI-V USB Baud Rate, CI-V USB Echo Back.

## Run it

```
pip install pyserial
python3 civ-capture.py --port /dev/ttyUSB0 --baud 115200
```

(`--port` is `/dev/ttyUSB0` on Linux, `/dev/cu.SLAB_USBtoUART` or `/dev/cu.usbserial-…` on a Mac, `COM3`
or similar on Windows. `--baud` must be the radio's **CI-V USB Baud Rate**; if the radio is on Auto,
115200 is fine.)

It records for 5 minutes. **While it runs, please:** turn the main dial a few times, change band once,
change the scope span (SPAN) through a few steps, switch the scope between centre (CENT) and fixed (FIX),
and change mode a couple of times (USB, CW, USB-D). Press Ctrl-C to stop early.

What it sends, and nothing else: "who are you" (19 00), "what frequency" (03), "what mode" (04), the
S-meter (15 02), "scope on" (27 10 01), "scope data to the USB port on" (27 11 01), and on exit "scope data
off" (27 11 00). Every frame is checked against VibeServer's own allow-list before it is written.

### Optional: the menu scan (helps us a lot)

```
python3 civ-capture.py --port /dev/ttyUSB0 --menu-scan --seconds 60
```

This additionally **reads** (never writes) the radio's menu items 1–110. We need it because VibeServer
refuses to start if USB SEND / USB Keying / VOX could key the radio, and we must be certain which menu
numbers those are on your firmware. Afterwards, please note what your radio's menu shows for
USB SEND, USB Keying (CW), USB Keying (RTTY) and DATA MOD, so we can match them to the readings.

## What to send back

The whole output folder (`icom-capture-<date>-<time>/`: `capture.civhex`, `capture.bin`, `summary.txt`),
plus:

- `lsusb -v` for the radio (Linux), or System Information > USB (Mac), or Device Manager's details (Windows)
- the radio's **firmware version** (MENU > SET > Others > Information > Version)
- the CI-V settings from step 4, and the USB SEND / USB Keying / VOX / DATA MOD values from step 3
- **ACC/USB Output Select** (AF or IF) — `MENU > SET > Connectors`
- the baud rate **wfweb** is set to, and roughly how many waterfall lines a second wfweb shows you

That is everything Phase 0 needs. The recording goes into VibeServer's tests (`test/fixtures/icom/`), so
every future build is checked against your real radio's bytes.
