# VibeSDR — Privacy Policy

_Last updated: 29 July 2026_

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

## Information the app uses

### Location (optional)
If you grant location permission, VibeSDR uses your device location **only** to
sort and filter the list of available SDR instances by distance (nearest first).

- Your location is sent **only** to the public instance directory
  (`instances.ubersdr.org`) as latitude/longitude **at the moment you refresh the
  list**, so it can return instances ordered by distance. It is not stored by the
  app or by the developer.
- Location is **entirely optional**. If you deny or disable it, every other feature
  of the app continues to work normally — you can still browse and use every
  instance; the list simply won't be sorted by distance.
- VibeSDR never accesses your location in the background.

### Connections to SDR receivers
When you select an SDR instance, the app connects directly from your device to
that third-party receiver to stream audio and spectrum data. Your device's IP
address is necessarily visible to the receiver you connect to, as with any network
connection. These receivers are operated by independent third parties and are not
controlled by the developer; their own logging and privacy practices are their
responsibility.

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

## Permissions

- **Location** (optional) — sort/filter servers by distance, as described above.
  Requested at approximate ("coarse") accuracy only, and never in the background.
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

The VibeSDR apps are not directed at children and does not knowingly collect any data from
anyone.

## Changes

If this policy changes, the updated version will be published at this URL with a new
"last updated" date.

## Contact

Questions about privacy: **stuey3dttb@icloud.com**

Source code: <https://github.com/Stuey3D/VibeSDR>
