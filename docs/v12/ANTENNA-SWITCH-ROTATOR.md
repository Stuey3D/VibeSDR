# V12 — antenna switching and rotator control

Asked for by a listener on Discord (2026-10-09): "can you add an antenna switcher for those SDRs with multiple antennas for better
DX?" Stuart: *"they are talking about external switching"*, *"a lot of FM-DX servers have an external switch"*, *"UberSDR has an
antenna switch and rotator control too"*.

Three parts, smallest first. None needs hardware Stuart owns: every backend has a way to test against a fake (§4).

## 1. UberSDR client — show and drive the server's switch and rotator

UberSDR (MadPsy, M9PSY) already exposes both. **Read live from `https://m9psy.tunnel.ubersdr.org` on 2026-10-09 — GET status only,
nothing switched or moved.** (`m9psy-1` and Stuart's own `.11:8080` have both disabled; they answer 503 "not enabled".)

**Antenna switch**
- `GET /api/ant-switch/status` →
  `{"enabled":true,"active":"Long Wire","antenna_labels":["Long Wire","Sky Loop"],"num_antennas":2,"selected":[1],
    "allow_mixing":false,"grounded":false,"thunderstorm":false,"last_update":"…"}`
  - `selected` is a LIST: `allow_mixing` lets more than one antenna be combined.
  - `grounded` / `thunderstorm`: the owner has grounded the antennas (lightning). Show it plainly and do not offer switching.
- `POST /api/ant-switch/command` `{ password, command: 'select', antenna: <n> }` or `{ password, command: 'ground' }`.
  401 = wrong/missing password (the page clears and re-prompts). History: `GET /api/ant-switch/history`
  (actions: select, ground, add, remove, default, thunderstorm_on/off).
- Pushed on the socket as `ant_switch_status`.

**Rotator** (Hamlib `rotctld` behind it)
- `GET /api/rotctl/status` →
  `{"connected":true,"enabled":true,"moving":false,"position":{"azimuth":216,"elevation":0},"read_only":false,
    "connected_duration_seconds":184807,"last_update":"…"}`
  - `read_only`: listeners may see the bearing but not move it.
- `POST /api/rotctl/position` `{ password, azimuth }`; socket `rotator_set_bearing { bearing }`, pushed `rotator_status`.
- `/api/description` carries `rotator: { azimuth, connected, enabled }` — enough to decide whether to draw anything at all.
- `/api/rotctl/countries` — the page's beam-heading-to-country helper.

**Client work (app, web, Jr; Buddy via the phone)**
- Draw nothing unless `enabled` (AGENTS.md: no inert controls). Read-only state (bearing, active antenna) for everyone;
  controls only where the server allows (`read_only:false`, password accepted).
- The password is the owner's; treat it like our admin credential (never in a URL, never logged).
- **Etiquette ([[third_party_receiver_etiquette]])**: moving someone's rotator or switching their antenna changes what every
  listener hears and can strain hardware. Only on an explicit user action, never on reconnect/restore, never in a test.

## 2. FM-DX — done

The app and Jr already show an FM-DX server's antenna list and switch through the webserver (`ant`, `antennas`). On FM-DX the
switch is usually driven by the receiver itself (a TEF668x board's firmware outputs; the webserver sends it an antenna
command). Keep the VibeServer picker looking the same so the two are one lesson.

## 3. VibeServer — its own external switch (and rotator)

RTL, Airspy and SDRplay radios have no antenna outputs (SDRplay's internal A/B/C/Hi-Z ports are already handled —
`SdrplaySource::setAntenna`), so the switch is driven from the machine. Owner setup (setup page → radio → Antennas):

- **Antennas:** a name and a coverage (the existing antenna-bands field) each — so the "outside this antenna's range" notice
  follows the SELECTED antenna.
- **Backend**, one of:
  | Backend | Covers | Notes |
  |---|---|---|
  | Pi GPIO pins (one-hot or binary) | most DIY coax-relay boxes, switches with a control-voltage input | `/dev/gpiochip*` (libgpiod) — not sysfs |
  | HTTP request per antenna | Tasmota/ESP8266 relays, KMTronic, web switches | URL template; GET or POST |
  | MQTT publish per antenna | Tasmota and most ESP32 DIY switches (they already speak it) | broker host, topic, payload per antenna; read the state topic back |
  | N1KDO AntennaSwitchControl | an open-source IoT switch (github.com/n1kdo/AntennaSwitchControl) | read its API from the source — a named target after generic HTTP |
  | USB serial relay (CH340 "LCUS") | the cheap relay boards | documented byte protocol (A0 nn ss sum) |
  | Owner command per antenna | Antenna Genius, homebrew, anything else | run as the server user, no shell interpolation of listener input |
- **Who may switch:** listeners / admin only / nobody. On a SHARED dial a switch counts as a tune — the shared-dial contract
  applies (client transmits only on a user action; the banner/"ask before tuning" rules).
- **Rotator:** point at a `rotctld` (Hamlib) host:port — the standard every rotator controller speaks. Same who-may rule.
- **Ground / thunderstorm:** an owner "ground all" state, as UberSDR has, honoured by every client.
- **Busy / interlock (hot-switch protection):** an owner who shares an antenna with their own TRANSMITTER relies on the switch
  controller's interlock (PTT/RF sensing). Never switch while the backend reports busy/locked; show why. Contest controllers
  (MOAS 2 by K1XM, the "Remote Switch Controller") and proprietary IP wrappers (Remoterig) are later backends, on request.

Precedent worth reading before designing the backends: KiwiSDR's `ant_switch` extension (GPIO and several web/serial
switches) — what owners already expect.

## 3a. Stuart's design (2026-10-09) — the shape to build

Chicopee runs **VibeServer Lite on Android**: no GPIO, so **MQTT (and HTTP) first** — network-only, and the C++ shim that
would hold the client runs in Lite and on Linux alike. Setup lives in BOTH Lite's server screen and the Linux web setup page.

**Setup (owner) — above the existing antenna details**
1. "Set up an antenna switch?" — **Yes / No**. No = today's single antenna, untouched.
2. **"Search for switch…"** → the result says what and where, e.g. *"Switch found on 192.168.86.77:12345 — Generic Antenna
   Switch Example"* (mDNS `_mqtt._tcp` → the broker → Tasmota / Home-Assistant discovery topics; HTTP switches by their own
   discovery where they have one; or typed in). Pick from the list — a TV remote cannot comfortably type topics.
3. **"How many antennas are connected?"** — a number.
4. That many **antenna blocks**, each the existing antenna + filter block repeated: *Antenna 1 (Name) (Details — coverage,
   filters)*, *Antenna 2 (Name) (Details)*… plus which relay/output it is. A **Test** key switches each in turn and reads the
   state back.

**Client (app, web, Jr) — its own menu entry, just UNDER the SDR settings**
- An **Antenna** selector listing the owner's names, e.g. *VHF Vertical* / *WideBand Loop*, the selected one marked, the
  state read back from the switch (never assumed from what we asked).
- Absent entirely when the radio has no switch (AGENTS.md: no inert controls). Greyed with the reason when locked (below).

**Per-band limits — a new antenna column**
- In the existing per-band limits, **choose an antenna per band** and optionally **lock** it there: tuning into 40 m selects
  *WideBand Loop*, into 2 m *VHF Vertical*.
- ★ Band defaults apply ONLY on a person's tune (`userTuneSeq` — [[band_defaults_user_only]]), never on reconnect/restore —
  the same rule as mode/step defaults, or a reconnect would flip someone else's antenna.

**Admin lock**
- **"Lock antenna controls behind the admin password"**: listeners get no selector (or a read-only name of what is in use),
  and the per-band presets drive the switch. The admin, signed in, can still switch by hand.
- On a SHARED dial an antenna change is a tune: the shared-dial contract and "ask before tuning" apply.

## 4. Testing without the hardware

- **Rotator:** Hamlib's dummy rotator `rotctld -m 1` accepts bearings and reports position with nothing attached. Point a
  VibeServer (or, with Stuart's OK, his UberSDR's config) at it for a full end-to-end test.
- **GPIO:** drive the pins on the Pi 500 and read them back (`pinctrl get`); an LED is enough to see it.
- **HTTP:** a fake switch on loopback that records requests — the same shape as `scripts/test-tunnel-selfcheck.mjs`'s fake
  directory.
- **USB relay:** the protocol is public; one board is ~£5 if a real one is wanted.
- **UberSDR client:** read-only against M9PSY's status endpoints; control only against a dummy-backed UberSDR we run.

## 5. Order

1. UberSDR client read-only (bearing + active antenna) — smallest, real data exists today.
2. UberSDR controls (password, read_only, grounded).
3. VibeServer switch: GPIO + HTTP first, then USB relay and command.
4. VibeServer rotator via rotctld.
