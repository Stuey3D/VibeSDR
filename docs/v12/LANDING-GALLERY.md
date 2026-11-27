# The gallery on the landing, and a server PIN over the landing's viewers (V12 beta96, 2026-11-27)

Stuart, 11-27: *"along with the air/sea maps on the landing screen we could put the servers gallery there too so a user who
just wants to look at meteor pics doesnt tie up the entire radio"* — and: *"if a server has a pin on the entire server not
just specific radios then the pin applies to gallery and maps on the landing screen too"*.

## RESUME HERE

Branch `b96-landgallery` (off b96-2-integrate e6fa8e866). Built and tested; not merged, not pushed.

- **Web** — `#splashGallery` beside `#splashAirSea` (in `#splashViewers`). `splashGalleryOffer` → `openGalleryViewer`
  opens the existing `#galleryPanel` over the landing (`galStandalone`): no session, no listener slot, no WebSocket.
  CLOSE / back / Escape / a click away → `closeGalleryViewer` (hooked into `closePanels`). In the viewer the MAP keys and the
  NAVTEX / SPOTS / DSC map rows are not drawn (each opens a live map behind a radio). The sky plot row, RANK card,
  pictures, text, charts, TRANSLATE, MAP OUTLINE and DOWNLOAD all work (they are HTTP reads). `initGallery` was split:
  `initGalleryPanel` wires the panel once, whoever opens it first.
- **Shared** — `src/services/galleryOffer.ts`: ONE read when the landing draws, `gallery.json?offset=0&limit=1&since=86400`,
  never polled. Caption: "12 today · newest METEOR 3 h ago", or "340 kept · nothing new today". No key when the gallery is
  empty, off, private (403) or refused (401).
- **App** — the radio chooser (SDRScreen, `pickItems` 'gallery', `radioChooserNav.ts`): GALLERY under AIR & SEA MAP; it
  opens `GalleryOverlay` with no radio chosen (`galleryViewer` → the MAP callbacks are null; the chooser's list stops
  listening while it is over it). GalleryOverlay needed no change: it only ever read `base` + `readAuth`.
- **Server PIN (web)** — `landingPinLocked()`: `/vibeserver/auth` says required (the master) and the page holds no proof →
  GALLERY and AIR & SEA MAP are drawn with NO count and nothing is read; a press shows `#splashViewPin`; the typed PIN is
  checked with one gated read (401 = "That PIN does not open anything here."), kept with `setReadAuth`, the offers run,
  then the viewer opens. A directory proof for this page (`pinProof()`) is adopted.
- **Server PIN (app)** — the server list already asks for the server PIN before the chooser (`openVibeServer` needsPin);
  the chooser's offers go through `withReadAuth(…, authSuffix)`, so no proof → 401 → no key, no count.
- **Server** — `vsServerPinHttpOk` (local_sdr_shim.cpp): `/vibeserver/gallery*` (list, files, the ship store),
  `/vibeserver/aircraft.json` and `/vibeserver/airsea/*` are gated by the SERVER PIN only. A per-radio PIN keeps guarding
  its radio (spectrogram, DX log, sockets: `vsPinHttpOk`) but no longer the server-level reads.

## Was there a hole in the shipped AIR & SEA MAP?

No. With a server PIN, every read it makes (aircraft.json, the ship store, gallery.json for sondes/VOR, the viewer ticket)
was already refused without the PIN, on a single-radio server and on a multi-radio door (`test-server-landing-pin` A and C
pass on the b96-2-integrate build). On the landing the reads simply failed, so the button was hidden and no count leaked.
What was wrong was the other way round: a PER-RADIO PIN made the radio's process — and the door — refuse the server-level
reads, so on such a server the landing had no AIR & SEA MAP at all. Fixed (B and C-per-radio: 10 FAIL before → pass).

## Viewer ticket: none for the gallery

The AIR & SEA "Map viewers" pool (vibe_airsea_viewers.h) exists for the map's 3-second aircraft poll. The gallery polls
nothing: a page is one read of the server's compact cached index (vibe_gallery.h — at most 100 rows a page, 1000 for an
old client), a picture is one static file, and the server's connection ceiling bounds a crowd. A ticket would add a
join/leave round trip and a way to be refused for no saving.

## Community map

Unchanged (the owner's explicit opt-in). A server with ANY PIN (server or radio) shares nothing to it:
`communityWhyNot()` → "pin".

## Not done / follow-ups

- **DX LOG on the landing** — not added. It is one per RADIO (the server keeps one per radio), the door does not serve
  `/vibeserver/dxlog` (it would need each radio's `/r/<id>/` and a radio choice), it is gated by that radio's PIN, and its
  rows TUNE. Worth doing as its own small piece: a DX LOG key per radio card, read-only, TUNE hidden.
- On a single-radio PIN server the landing now shows two PIN boxes when GALLERY is pressed (the connect form's and the
  viewers'). They do the same job; merging them would touch the connect form.
- The sky plot row does not appear in the landing's gallery on a multi-radio door (`/vibeserver/skyplot.json` is a
  radio's route, not the door's); inside a radio it does.
- Pre-existing, noticed: a hand-written config with a radio `"pin"` and no top-level `"pin"` reads the radio's PIN as the
  server's (the config reader's plain key search). The server's own saves always write the top-level key first.

## Tests

- `scripts/test-server-landing-pin.mjs` — real vibeserver, asked from the LAN address: server PIN refuses / opens each
  read (single radio + door); per-radio PIN keeps them open and still guards the radio. 41/0 (31/10 before).
- `scripts/test-web-landing-gallery.mjs` — real vibeserver + built page, headless Edge `--mute-audio`: the key and its
  caption, one read, opens with no WebSocket and 0 listeners, no MAP keys, a text item and a picture open, BACK, CLOSE and
  Escape return; empty → no key; server PIN → keys with no count and no reads, asks, wrong PIN refused, right PIN opens
  and the caption appears. 28/0. Screenshots with `SHOTS=<dir>`.
- `scripts/test_gallery_offer.ts` — the offer (12) + the app chooser's wiring (12). 24/0.
- `scripts/test_radio_chooser_nav.ts` — the pad order with GALLERY.
- Re-run: `test_nav_coverage`, `test_gamepad_nav`, `test-web-gamepad`, `test-web-gamepad-admin`, `test-web-airsea-standalone`,
  `test-web-gallery-range`.
