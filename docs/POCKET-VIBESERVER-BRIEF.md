# Pocket VibeServer — build brief

_Drafted 16 September 2026 from Stuart's goal and tgcfabian's access-point suggestion. Build when the
hardware arrives; nothing here needs the radio code to change._

## The goal
A box small enough for a pocket that a phone connects to over Wi-Fi and uses almost as if the SDR
were plugged into the phone. iPhone users cannot plug an SDR in at all; this gives them the same
"own hardware, anywhere" experience Android users have with a dongle on a cable. The box is a host
exactly as the Pi 500 is today: it runs the same VibeServer package, the same web client, the same
app connection, raw IQ out, DAB, everything.

## What it does on the day
1. Power on. The box looks for a known Wi-Fi network (home). If it finds one it joins it and
   behaves as any VibeServer: tunnel, directory listing, the lot.
2. If nothing known is in range within about 30 s, it raises its own access point
   ("VibeServer-xxxx", WPA2, password on the label or set in the wizard) on a fixed address.
3. The phone joins that network. A captive-portal check opens the box's page on its own, the way a
   hotel login page does. No address to type.
4. First run: the existing setup wizard lists the radios found on USB, takes a name, and offers to
   take the location from the phone's browser (coarse, grid square only). Save and start.
5. Every run after that: the receiver page opens directly, or the app finds "vibeserver.local" in
   its discovered list.

## Pieces and where they live
| Piece | Where | Status |
|---|---|---|
| Setup wizard in the browser (radios found, name, location) | `vibe_setup_page.h` | exists |
| mDNS advert (`vibeserver.local`) | `vibeserver/mdns` | exists |
| Root helper for privileged actions | `vibeserver/linux/vibeserver-maintenance` | exists — add `hotspot-on` / `hotspot-off` / `wifi-join` |
| Hotspot config (SSID, password, home network) | new fields in `vibeserver.conf` + setup page section | to build |
| Access point with fallback | NetworkManager: `nmcli device wifi hotspot`; a first-boot unit tries known networks then falls back | to build |
| Captive-portal redirect | answer the OS connectivity-check URLs (`captive.apple.com`, `connectivitycheck.gstatic.com`, `msftconnecttest.com`) with a redirect to the box's page; dnsmasq address wildcard while the AP is up | to build |
| Location from the phone | "Use this phone's location" button in the wizard: browser geolocation → Maidenhead square | to build |
| Offline-clean behaviour | EiBi / transmitter DB / logo fetches fail quietly; listing disabled with a reason; no internet is not an error | mostly exists; audit |
| Clock without NTP | set the system clock from DAB (FIG 0/10) or RDS CT once decoded; a box that boots with no network has no idea what day it is | to build (helper action `settime`) |
| The image | Raspberry Pi OS Lite + the apt package + the first-boot unit, built with pi-gen, flashed with Pi Imager | to build, last |

## Caveats to state on the page
- While the access point is up the box has no internet: no tunnel, no public listing, no EiBi
  refresh. The page says so and offers "join home Wi-Fi" to switch back.
- Single Wi-Fi radio: access point or client, not both, unless a second adapter is fitted.
- A Zero 2 W has roughly a third of the Pi 500's CPU. One RTL with WFM, RDS and the spectrum is
  comfortable. DAB / DAB+ at 2.048 MS/s must be measured on the board before it is promised. Two
  radios at once is not a Zero 2 W job. A Pi 4 / 5 in a small case removes the question.
- Power: an RTL plus the board is under 1 A at 5 V; a small power bank runs it for hours. The RSP
  draws more and wants a proper supply.

## Build order (each step usable on its own)
1. Helper actions + config fields + setup-page "Portable hotspot" section. Test on the Pi 500 with
   the tunnel off: phone joins, `vibeserver.local` resolves, receiver page and raw IQ work.
2. Fallback logic on boot (known network first, hotspot otherwise) and "join home Wi-Fi" from the page.
3. Captive-portal redirect.
4. Phone-location button in the wizard.
5. Clock from the air.
6. The pi-gen image and a one-page "flash, power, join, listen" guide.

## Not in scope
Custom router firmware; any change to how the app talks to a server; a second Wi-Fi adapter by default.
