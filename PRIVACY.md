# VibeSDR — Privacy Policy

_Last updated: 5 October 2026_

This policy covers the whole VibeSDR family:

- **VibeSDR** — the receiver app for iPhone, iPad, Android and Mac.
- **VibeSDR Jr** — the standalone receiver for Apple Watch.
- **VibeSDR Buddy** — the Apple Watch remote for the phone app.
- **VibeServer** — the server built into the Android app and available for macOS,
  which turns your own radio hardware into a receiver on your own network.

They are client apps for listening to Software-Defined Radio (SDR) receivers. This
policy explains what they do and do not do with your data.

## Summary

**VibeSDR does not collect, store, or transmit any personal information to the
developer.** There are no analytics, no advertising, no tracking, and no developer
servers that receive your data. There are no accounts and nothing to sign in to.
Everything the app stores stays on your device, or in your own iCloud account.

## Our position

This is deliberate, and it is permanent.

- **No data-collecting SDKs.** The apps contain no analytics, advertising,
  attribution, crash-reporting or "monetisation" libraries from anyone. The
  third-party code they do contain is the kind a radio needs — open-source
  drivers, decoders and codecs (librtlsdr, libusb, Opus and the like), and, in
  VibeServer, Cloudflare's tunnel client so a server can be reached from outside
  your network. All of it is open source or published, does one job, and reports
  nothing to anyone.
- **No bandwidth-sharing or proxy SDKs — ever.** Developers of apps like this one are
  regularly offered money to embed SDKs that use the phones of people running the app
  as proxy exit nodes ("proxyware"): other people's internet traffic, of unknown
  origin and purpose, leaves through your connection, using your data allowance and
  your IP address, usually behind a consent screen nobody reads. VibeSDR has been
  approached with exactly such an offer and refused it. It will refuse every future
  one, regardless of the payment. It would also be the exact inverse of what the
  tunnel exists to do. Your device and your connection are not for sale, and not by
  us.
- **No accounts, no identifiers.** There is nothing to sign up for and no device
  identifier is generated, stored or sent.
- **Privacy first in the design, not only in the policy.** VibeServer reaches the
  internet through a Cloudflare tunnel rather than a forwarded port. That is slower
  and less efficient than the port-forwarded servers other receiver software uses,
  and it is chosen deliberately: nothing inbound ever touches the owner's home
  address, nothing is exposed to internet scanners, and the address appears in no
  listing. It protects the owner from exposure and inbound attack; it is not
  end-to-end encryption — the tunnel terminates at Cloudflare's edge, as for any
  site behind Cloudflare. An owner who prefers the speed of a direct, port-forwarded
  server and is happy to manage their own exposure can run one; the tunnel is the
  default, not the only option.
- **Verifiable.** The full source of every released build is public at
  <https://github.com/Stuey3D/VibeSDR>, the dependency list with it, and anyone can
  capture the app's network traffic and see exactly which endpoints it talks to.
  What this page says is checkable in minutes, and with this app's audience it will be.

If any of this ever changes, this page will say so, at the top, before the change ships.

## Information the app uses

### Location
VibeSDR asks for **coarse** location only, on iOS and Android only, and never in
the background. What it is for depends on whether you are listening or serving.

**Listening.** Location is entirely optional. Grant it and the app can sort and
filter the server directories by distance, and, when you plug an SDR into the
device itself, place your own receiver on the map so the digital-mode spots it
decodes can be shown with a distance and bearing. Deny it and you lose only those:
the directories are unsorted, and spots still appear on the map but with no
distance or bearing, because the app has no position of its own to measure from
(it holds 0° latitude, 0° longitude — "Null Island" — as the marker for
"unknown", draws no receiver on the map, and offers a *Set location* button). You
can fix that without sharing your real location at all: enter the nearest city or
a Maidenhead grid reference by hand and the app uses that instead.

**Serving.** Listing a server publicly is the one thing that needs a position,
because the directory exists to sort receivers by distance. With the default
option — the tunnel and a listing on vibesdr.net — VibeSDR and VibeServer will
not start the server without one. It can come from the device's coarse location
(not on Linux, where there is none), reduced to a Maidenhead grid reference, or
you can enter a city or grid reference yourself. Serving on your local network
only, or through your own port forwarding, needs no location at all; listeners on
such a server simply see spots and transmitters without a distance, as described
above, unless you set a city or grid reference.

**Where it goes.** All directory sorting and every map view is handled on the
device: no directory, and no receiver, is ever told where a listener is. A
position leaves a device only when a server's owner gives the server one, and then
it goes to three places:

- to the vibesdr.net directory, to show the server's location when its owner lists
  it there;
- to the clients connected to that server, for the decoders that show
  location-based data on a map;
- to OpenStreetMap's Nominatim service (nominatim.openstreetmap.org), to turn the
  position into the name of the nearest town and its country, for display. The
  answer is remembered on the device, so this happens once per position. If the
  owner types a city instead, that name is sent to Nominatim to find where it is.
  Nominatim's own privacy policy applies to those requests.

In every case it is the centre of the Maidenhead grid square only, never a fix: a
square a few kilometres on a side (about 5.6 km by 4.6 km in the UK), so it says
which town, not which street. The directory and Nominatim are always reached over
an encrypted (HTTPS) connection. Unlisting the server, or turning its location off,
removes the position from the directory.

**A word of advice if you serve from somewhere remote.** A grid square hides a
person well in a town, a suburb or a village, where it covers thousands of
homes and is still accurate enough to sort by and to measure spots from. It does
not hide a person whose square holds one farmhouse and nothing else: for them the
square *is* the address, and a public listing would point straight at the front
door. If that is you, do not use the device's location when you list a server.
Enter the nearest town or city instead — the directory sorts just as well from
twenty kilometres away, and nobody browsing it can tell the difference. The
choice is always yours; the app never insists on the device's position, and it
would rather you were listed a town over than listed at home.

### Connections to SDR receivers
When you select an SDR instance, the app connects directly from your device to
that third-party receiver to stream audio and spectrum data. Your device's IP
address is necessarily visible to the receiver you connect to, as with any network
connection. These receivers are operated by independent third parties and are not
controlled by the developer; their own logging and privacy practices are their
responsibility.

A VibeServer's owner can see, on their own admin page, the IP address, country and
network of whoever is connected, and the frequency they are listening to. That
view is held in the server's memory on the owner's hardware, is not sent to the
developer or anywhere else, and lets the owner block abuse of their radio. Servers
reached through the public directory pass through Cloudflare's network on the way,
as any website does.

### Chat
Some receivers — OpenWebRX, UberSDR and FM-DX servers — have a chat room of their
own, and the app shows it. Chat belongs to the receiver: what you type goes
straight from your device to that receiver, exactly as it would from its own web
page, and the receiver's owner runs and moderates it. The developer never receives,
stores or relays any chat. Many receivers are plain `http://` sites, so chat on them
is not encrypted, as with any unencrypted website. You can hide any user's messages
from your own view for the rest of your visit (long-press a message); that choice
stays on your device and nobody is told. VibeServer has no free-text chat.

### Public sharing, the tunnel and the directory
A VibeServer is private to your own network until you switch public sharing on.
When you do, this is what happens:

1. **The tunnel.** VibeServer opens an outbound Cloudflare tunnel with a random
   hostname. It is outbound only: no port is opened on your router, nothing inbound
   ever reaches your home IP address, and that address is never published or
   listed anywhere. The random hostname is different on every connection. Because
   the tunnel is outbound, it does not care what your address is: it works on
   Wi-Fi or cellular, and when your connection changes address — a home router
   reconnecting with a new one, or a phone moving from Wi-Fi to cellular — the
   tunnel reconnects and the listing carries on under the same friendly name.
2. **Registration.** Through that tunnel VibeServer tells the directory three things:
   the name you gave the server, its **coarse** position — a Maidenhead grid
   locator, which names a square a few kilometres across, or a city-level position —
   and a unique identifier for that server. The identifier is random, generated on
   your machine, and identifies the server, not you. VibeServer asks for the coarse
   position before it will start, because the directory exists to sort receivers by
   distance; it is never an exact location and the app never derives one.
3. **The listing.** In return the directory lists the server, pins its rough
   position on the map, and assigns it a friendly name of its own
   (`<name>.vibeserver.vibesdr.net`). That name is linked to the identifier, so it
   survives reconnections and tunnel changes, and it is yours to give to friends,
   family or a radio club.
4. **Listening.** A listener who opens the friendly name reaches the server through
   Cloudflare and the tunnel. The directory is not involved in the connection and
   learns nothing about who listens; browsing the list is an ordinary web request
   that is not logged by the developer. The tunnel terminates at Cloudflare's edge,
   as for any site behind Cloudflare, so it is protection from exposure rather than
   end-to-end encryption.

The directory records the IP address each registration came from, to limit abuse,
and nothing else about the owner.

**Leaving is automatic.** A listing is only ever as current as the server behind
it: VibeServer checks in while it runs, and a server that stops checking in simply
drops off the list. There is nobody to contact and nothing to remember; switch
public sharing off, or turn the machine off, and the listing goes with it. An owner
who wants to be sure can also delist with one press, effective immediately. The
friendly name is held for that server for a week, so a reboot, an outage or a
holiday does not cost you it; after a week without the server it is released, to
you on new hardware or to anybody else. The name belongs to the server's own
identifier, which lives in its configuration: keep that configuration across a
reinstall and the name comes back with it; lose it and the old name frees itself a
week later, with no person to ask.

**No email address, no account, no contact details.** Listing a server never asks
who you are. Some directories collect an email address at sign-up; this one holds
nothing that could identify the owner, which is also why there is nothing to
recover, nothing to leak and nothing to make a data-protection request about.

The tunnel and the directory were built this way on purpose: the tunnel so that
serving a radio never exposes your home connection, and the directory so that
listing one never asks for more than a name and a rough position, and never keeps
a record of you that outlives the server.

### On-device data
The following are stored **only on your device** and are never transmitted to the
developer:

- Your saved bookmarks, favourite servers, and a default server.
- App settings and preferences.
- Audio recordings you choose to make (saved to your device; shared only when you
  explicitly use the share button).

You can remove all of this by deleting the app.

### iCloud sync (optional)
If you are signed in to iCloud, your bookmarks and favourite servers sync between
your own devices — for example, between VibeSDR on your iPhone and VibeSDR Jr on
your Apple Watch.

- This uses **your own iCloud account**, through Apple's iCloud key-value storage.
  The data goes from your device to your iCloud and back to your other devices.
- **The developer has no access to it.** There is no developer server involved and
  no copy is kept anywhere else.
- It syncs bookmarks and preferences only — never recordings, and never anything
  about what you have been listening to.
- Turning off iCloud for VibeSDR in your device settings stops it, and the app
  carries on working normally with everything stored locally.

### VibeSDR Jr on Apple Watch
Jr is a standalone app: it makes its own network connection and does not send your
data through the paired iPhone. Everything above applies to it in the same way — no
accounts, no analytics, nothing sent to the developer.

### VibeServer
If you run VibeServer, it serves your own radio to devices you point at it. It runs
on your hardware, on your network. The developer has no visibility of it, receives
nothing from it, and it phones home to nobody. If you choose to make it reachable
from the internet, anyone you give the address to can connect and listen, and the
connection logs it keeps are yours alone — so set a PIN if it is not meant to be
public.

### Diagnostics
If something breaks you can build a diagnostics report from the app and share it
yourself, through the system share sheet. It is assembled on demand, shown to you
first, and never sent by the app. It contains no PIN, password, callsign or precise
location.

## Permissions

- **Location** (optional for listening; a grid square is needed to list a server)
  — sort/filter servers by distance and place your own receiver on the map, all on
  the device, as described above. Requested at approximate ("coarse") accuracy
  only and never in the background.
- **Local network** (iOS and watchOS) — to discover and connect to SDR receivers on
  your local network.
- **Notifications / media controls** — to show now-playing controls and run audio
  in the background while you listen.
- **USB** (Android) — only to talk to an SDR dongle you plug in yourself, when you
  use the app as a server.

VibeSDR does **not** use the microphone, camera, contacts, or any other personal
data. (Audio "recording" records the radio stream you are listening to, not your
microphone.)

## Children

The VibeSDR apps are not directed at children and do not knowingly collect any data
from anyone.

## Changes

If this policy changes, the updated version will be published at this URL with a new
"last updated" date.

## Contact

Questions about privacy: **stuey3dttb@icloud.com**

Source code: <https://github.com/Stuey3D/VibeSDR>
