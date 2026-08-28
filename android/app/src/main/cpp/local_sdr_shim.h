// VibeSDR V4 — local-SDR shim: RTL-SDR → FFT + demod → localhost UberSDR.
// Stage 3 added the spectrum WebSocket; Stage 4 adds the demodulated-audio
// WebSocket (int16 PCM, mono or stereo for WFM) plus tune/mode/bandwidth
// control, so the existing VibeSDR audio engine plays local hardware.
#pragma once
// ★ <cstdint> EXPLICITLY. Clang (Android NDK, Apple) pulls the fixed-width types in transitively
// through <string>/<vector>; GCC 14 on Debian 13 does NOT, so uint32_t/uint64_t here failed to name
// a type and the whole shim refused to compile on the Raspberry Pi (2026-07-31). Include what you
// use — the header is shared by three toolchains and can only rely on what the standard promises.
#include <cstdint>
#include <string>
#include <vector>

#include <functional>

namespace vibe {

class LocalSdrShim {
public:
    static LocalSdrShim& instance();

    // Open the RTL-SDR on `fd`, start FFT + demod pipelines and the localhost
    // server (spectrum + audio WebSockets). Returns the chosen TCP port (>0) on
    // success, or -1 with `err` set. `fd` stays owned by the caller (Kotlin).
    int start(int fd, int vid, int pid,
              double centerFreq, double sampleRate, int gainTenthDb,
              int fftSize, double fftRate, const std::string& mode, std::string& err);

    // RTL-TCP source (rtl_tcp protocol over the network — no USB/librtlsdr, so it
    // works on iOS too). Same pipeline as start(), IQ from a TCP socket.
    // ── VibeServer (share this device's radio, server-side DSP) ──────────────
    //
    // The shim ALREADY is an UberSDR-compatible server: it owns the dongle, runs
    // the FFT and the demodulator, and serves SPEC frames + PCM audio over a
    // WebSocket. It has simply never listened anywhere but loopback. Bind it to
    // 0.0.0.0 and a remote VibeSDR connects to it exactly as it would to any
    // UberSDR instance — no new client code, and the wire carries pictures and
    // sound (tens of KB/s) instead of raw IQ (4.8 MB/s).
    //
    // Call BEFORE start(). Off by default: this WebSocket carries tuning control
    // and has no authentication, so it must never leave loopback by accident.
    static void setServeOnLan(bool on);
    static bool serveOnLan();

    // VibeServer PIN auth. When a non-empty secret is set, incoming LAN clients
    // must pass an HMAC-SHA256(secret, nonce) challenge-response before the
    // spectrum/audio WebSockets upgrade — the secret itself never crosses the
    // wire. Empty secret (default) = open access (no PIN). Set BEFORE start().
    /** Listen on THIS port. 0 (default) = scan 48000..48049 and take the first free one. An
     *  explicit port is used or the start FAILS — never silently moved, or a port-forward or a
     *  saved client bookmark would break with no visible cause. Set BEFORE start(). */
    static void setVibeServerPort(int port);
    static void setVibeServerAuth(const std::string& secret);
    // Server-side compatibility limits, for low-end hosts / slow networks. A
    // maxBandwidthHz <= 0 means "no cap"; maxFftRate <= 0 means "server default".
    // The client still interpolates the waterfall, so a throttled fps stays
    // smooth. Set BEFORE start() (honoured on the serving path only).
    static void setVibeServerLimits(double maxBandwidthHz, double maxFftRate);
    /** Require listeners to keep their idle power-saving on (they normally choose). For a host on
     *  solar/cellular where saving power outranks a listener's preference. Set BEFORE start(). */
    static void setVibeServerForceIdleSaver(bool on);
    /** Hand the SDR to another program while nobody is listening. Off by default. */
    static void setVibeServerReleaseWhenIdle(bool on);
    /** Tell the connected client the host is looking for it — the browser tab flashes and tries to
     *  focus itself. Uses the socket we already have, so no browser automation and no permission
     *  prompt. No-op when nobody is listening. */
    void summonClient();
    // Compressed (IMA-ADPCM) audio on the /ws/audio path (default on). A client
    // that hits a decode issue can ask the server to fall back to raw int16 PCM.
    static void setVibeServerCompressAudio(bool on);
    /// Owner policy: may a client that cannot decode Opus be served RAW audio
    /// (~187 KB/s of the owner's uplink each)? Default OFF.
    /** VsUncompressedAudio: 0 = off, 1 = listener's choice, 2 = compatibility fallback only.
     *  Loopback clients are outside this setting entirely and always get raw PCM. */
    static void setVibeServerUncompressedAudio(int mode);
    /** ★ ADMIN PASSWORD — a second secret, gating CONTROL rather than ACCESS. The PIN decides
     *  who may listen; this decides who may touch bias-T, direct sampling and calibration.
     *  Independent of the PIN on purpose: a public receiver may be open to all listeners and
     *  still refuse a visitor putting DC on the feedline. Empty = nothing is protected. */
    static void setVibeServerAdminSecret(const std::string& secret);

    // ── ★★★ WHAT A LISTENER MAY DO TO THE FRONT END ────────────────────────────────────────
    //
    // ★★★ A CEILING, NOT A LOCK. The admin gate takes the gain away entirely on a shared receiver;
    //     these leave the control in the listener's hands and simply stop it going too far. An
    //     owner capping FM wants the front end protected and the listener left alone, not to field
    //     gain requests all evening (Stuart, 2026-08-12). See briefs/BRIEF-admin-gain-limits.md.
    // ★★ VALUES ARE IN THE RADIO'S OWN UNITS — tenths of a dB on an RTL, an RF slider POSITION on
    //    an RSP. The three radios do not share a gain model and nothing here pretends they do.

    /** Per-band ceilings, "fm:250, 0-30M:400". Empty = no limit anywhere (today's behaviour). */
    static void setGainLimits(const std::string& csv);
    /** The gain to return to when everybody has left, in the radio's units. -1 = leave it be. */
    static void setRestGain(int gain);
    /** Force the AGC on and refuse to let a listener turn it off (RSP and Airspy HF+). */
    static void setAgcLock(bool on);
    /** RTL only. Protection: come DOWN from the owner's gain when the ADC rails, and return to it.
     *  AGC: the same loop with the ceiling raised to the tuner's maximum, the owner's figure
     *  becoming the starting point. See the notes by g_ovlProtect. */
    static void setOverloadProtect(bool on);
    void        setRtlAgc(bool on);
    /** ★ The tuner's IF filter follows the zoom (RTL only). Persisted in the server's config —
     *  see RadioConfig::tunerBwAuto — so it survives a restart and is not re-asserted by a client. */
    void        setTunerBwAuto(bool on);
    /** ★ What is HOSTING this server ("VibeSDR 10.5 for Android"), reported beside — never instead
     *  of — the engine version. See g_srvHost for why the two must not share a field. */
    static void setServerHost(const std::string& label);
    /** The ceiling in force at a frequency, or -1 for none. Public so the client can be told. */
    static int  gainCapAt(double hz);
    /** Where the listener actually is — the VFO, falling back to the capture centre. This is the
     *  frequency a gain ceiling is judged against: on the radios where a cap applies at all (the
     *  ones a listener can still move the gain on) the hardware follows the VFO anyway, and it is
     *  what the owner means by "when I tune into FM". */
    double listenFrequency() const;
    static bool agcLocked();
    /* ★★★ THE CEILING AS A SETTING, NOT ONLY AS A LIMIT (Stuart, 2026-08-28). With the lock on, a
     *   band that HAS a ceiling is fixed there and no listener may move the gain — the same
     *   figures, read a different way. A band with no ceiling is untouched: the lock changes what
     *   a rule MEANS, it never invents one.
     * ★★ ifGainCapAt is the SDRplay's second stage (Saber's RSP1 clone: the RF stage alone does
     *   not do it), and gainSplitAt is the HackRF's LNA share of the total, 0-100 — a total does
     *   not determine two stages, so a ceiling is enough to limit with and not enough to SET
     *   with. Both are read only where they mean something; see the setters. */
    /** The sample rate is PINNED rather than capped — see the definition. */
    static void setVibeServerRateLock(bool on);
    static void setGainLock(bool on);
    /** ★★★ WHICH BANDS ARE FIXED — per band, because a receiver can want FM held at one figure
     *  while HF stays adjustable under a ceiling (Stuart, 2026-08-28). Same band syntax as the
     *  ceilings; an "all" rule covers the whole radio, which is how a radio-wide lock is spelled.
     *  ★ setGainLock is the LEGACY radio-wide flag from 4.1.47/48 and applies only while this list
     *    is empty, so an owner who has since locked one band is not locked on all of them. */
    static void setGainLocks(const std::string& csv);
    /** Is the gain FIXED at this frequency? Always false where there is no ceiling to be fixed at. */
    static bool gainLockedAt(double hz);
    /** ★★★ THE LEAST IF GAIN REDUCTION A LISTENER MAY USE, in dB — the units the client's own
     *  slider shows and the wire field `ifgr` carries (20 = maximum gain, 59 = minimum). A BIGGER
     *  number is a TIGHTER limit, which is why it is named for the reduction rather than dressed up
     *  as a ceiling. -1 = none. */
    static void setIfGrFloors(const std::string& csv);
    static int  ifGrFloorAt(double hz);
    static void setGainSplits(const std::string& csv);
    static int  gainSplitAt(double hz);
    /** ★★★ RTL OVERLOAD PROTECTION — one call per second, FROM THE DSP THREAD ONLY. It takes the
     *  hardware lock and touches the tuner, so the libusb callback must never call it (see the
     *  note on enqueueIq). Non-static: it works on the live device. */
    void overloadTick();
    /** The gain the radio is ACTUALLY set to, in its own units (tenths of a dB on an RTL);
     *  -1 = auto/AGC. Sent in hwinfo so a remote client can SHOW the truth instead of imposing
     *  its own remembered value on a radio it does not own. */
    int currentGainTenthDb() const;
    /** ★★★ Reverse proxies whose X-Forwarded-For we believe, comma separated (addresses/CIDRs).
     *  EMPTY = trust nobody and read no headers, which is the default: the header is
     *  client-supplied, so believing it from any peer lets a stranger forge an address and walk
     *  through the ban list. See vibe_proxy.h. */
    static void setTrustedProxies(const std::string& csv);
    /** The directory's shared secret, for answering its address challenge. */
    static void setDirectoryKey(const std::string& key);
    /** ★ Per-listener time limit in MINUTES; 0 = unlimited (default). For a PUBLIC receiver,
     *  where one client per radio makes the server a queue of one. Loopback and admin sessions
     *  are exempt. On expiry the listener is told, disconnected, and their address held on a
     *  short cooldown — without that they would simply reconnect and carry on. */
    static void setVibeServerSessionLimit(int minutes);
    /** Is a listener currently holding the radio? Used by the identity endpoint. */
    bool isBusy() const;
    /** How many spectrum listeners are attached. ★ `isBusy` answers a one-at-a-time question; on
     *  a shared receiver the owner wants the NUMBER, and someone deciding whether to connect
     *  wants to know there is room. */
    int  listenerCount() const;
    /** How many are waiting in the queue for a slot. 0 when nobody is. */
    int  waitingCount() const;
    /** The captured span in Hz (the sample rate). 0 when nothing is running. */
    double captureSpanHz() const;
    /** ★★ SPECTROGRAM PERSISTENCE. Setting a path also LOADS whatever is there, so a restart keeps
     *  the history the landing page exists to show. Empty path (the default, and every phone) =
     *  memory only, exactly as before.
     *  ★ The daemon drives the writing: `saveSpectrogramIfDue()` from its own loop, and
     *    `saveSpectrogram()` on the way out. A 3 MB write must never land on the DSP thread. */
    void setSpectrogramPath(const std::string& path);
    void saveSpectrogram();
    void saveSpectrogramIfDue();
    /** Seconds until the current listener's limit expires; -1 when there is no limit,
     *  nobody is listening, or the listener is exempt (loopback / admin). */
    /** Seconds left on the listening limit, or -1 for none.
     *  @param adminOverride 1 = the ASKING listener is admin (exempt), 0 = it is not,
     *         -1 = no particular listener, fall back to the radio-wide flag. The exemption belongs
     *         to a listener, so on a shared receiver answering it radio-wide made one person's
     *         countdown depend on whether somebody else was unlocked. */
    int  occupantSecsLeft(int adminOverride = -1) const;

    // ── ★★★ THE ADMIN API — monitoring, listeners, bans, maintenance ─────────────────────────
    //
    // Serves /vibeserver/admin/*, reached from the SERVER ADMIN button at the bottom of the
    // client's menu once the admin session is unlocked. Gated on the SAME nonce + HMAC challenge
    // as the config API — see the routing comment in local_sdr_shim.cpp for why there is no
    // fourth credential mechanism, and no web terminal.

    /** Where the ban list is persisted (one JSON object per line). Empty = memory only, which is
     *  every phone: an app has no /var/lib and nobody to ban. Setting a path also LOADS it. */
    void setBanListPath(const std::string& path);
    /** ★★ Where the owner's listener-facing notice lives. A FILE, so a multi-radio machine's
     *  front door and its radios all show the same one — they are separate processes. */
    void setNoticePath(const std::string& path);
    /** The notice to show right now, or "". */
    static std::string noticeText();
    /** Post one. minutes <= 0 = until it is cleared; empty text clears it. */
    static bool setNotice(const std::string& text, int minutes, std::string& err);
    /** Push the current notice to every connected listener. */
    void broadcastNotice();

    /** ★★ Where the connection log is kept. It used to be memory-only, which meant every update
     *  wiped the history — and an update is the most common reason this server restarts. Empty
     *  path = memory only (every phone). */
    void setConnLogPath(const std::string& path);
    /** ★★★ ONE LISTENER, ONE RADIO — across the whole machine, not per process. Occupancy is
     *  enforced per radio and every radio is its own process, so nothing had a view across them:
     *  one address held two of the three single-user radios for twenty minutes and each process
     *  was individually right to admit it (Stuart, 2026-08-17). `dir` is the shared runtime
     *  directory the handover sockets already live in; `serial` and `label` identify this radio
     *  in the registry, so a refusal can name the radio the visitor is ALREADY on. */
    /** The aerial bolted to THIS radio, and the machine's standing landing-screen message.
     *  ★ The URL is scheme-checked here and stored empty if it fails, so nothing downstream has to
     *    wonder whether it was. See vsconfig::safeLinkUrl. */
    /** Is the session limit a GUARANTEE (soft) rather than a deadline? See sessionLimitSoft. */
    void setSessionLimitSoft(bool soft);
    /** ONE RADIO PER ADDRESS — see ServerConfig::oneRadioPerIp. Default true; the owner may allow
     *  several, which is reasonable privately and unwise on a public server. */
    void setOneRadioPerIp(bool on);
    /** How many of this machine's radios one address may hold at once. 1 = the old rule (default),
     *  0 = no limit, 2 = the A/B case. setOneRadioPerIp() is now a wrapper for 1/0. */
    void setMaxRadiosPerIp(int cap);
    /** Minutes of asking for nothing before a listener is prompted, then released. 0 = off. */
    void setIdleKickMinutes(int minutes);
    /** True when somebody is listening but is past their guarantee, so an arriving listener may
     *  take the radio. ★ For the PUBLIC card only — the admin views report the truth. */
    bool claimableNow() const;
    /** Which aerial picture this radio shows — a key, see RadioConfig::antennaIcon. */
    void setAntennaIcon(const std::string& key);
    void setLandingInfo(const std::string& antenna, const std::string& message,
                        const std::string& linkUrl, const std::string& linkLabel);
    void setOccupancyRegistry(const std::string& dir, const std::string& serial,
                              const std::string& label);
    /** Refresh this radio's registry entry. Called from the daemon's 1 Hz loop — a heartbeat, so a
     *  radio that dies cannot hold a slot for ever. */
    void refreshOccupancy();
    /** Append anything that closed since the last call. Driven from the daemon's 1 Hz loop so
     *  file I/O never lands on a connection thread — see ConnLog::saveIfDue. */
    void saveConnLogIfDue();

    /** ★★ Minutes of no interaction after which an admin session's CONTROLS re-lock — the
     *  session, its audio and any running decode continue. 0 = never. Default 30.
     *  The case this defends against is an unattended tab left logged in as admin, not a stolen
     *  password (Stuart, 2026-08-06). */
    static void setAdminIdleMinutes(int minutes);

    /** ★★ Is this receiver shared with strangers? Drives WHICH admin panels the page draws —
     *  listeners, blocking and connection history are about managing people you do not know, and
     *  are noise on a household receiver.
     *  ★★★ IT GATES THE DISPLAY ONLY. The log is still kept and bans are still enforced, so
     *  turning it on later arrives with history already there rather than starting from zero.
     *  ★ Held HERE rather than read by each client, so a browser and the app cannot disagree
     *    about what this server is. */
    static void setPublicSharing(bool on);

    /** Scheduled updates, for DISPLAY on the admin page. The daemon owns the firing — this is a
     *  readout so the page shows what is set rather than what it last sent. hour -1 = off. */
    static void setUpdateSchedule(int srvHour, int srvDay, int allHour, int allDay);

    /** Is this address on the ban list right now? Expired entries are pruned as they are found.
     *  Called on the connection path, so it is cheap when the list is empty (the normal case). */
    static bool isBanned(const std::string& ip, std::string* reason = nullptr);

    /** Connection log bookkeeping. `reason` is the field that makes the log worth keeping —
     *  "closed" | "kicked" | "banned" | "busy" | "timeout" | "queue-full". */
    static void noteConnectionOpened(const std::string& ip, const std::string& session,
                                     const std::string& agent, const std::string& cc = "");
    static void noteConnectionClosed(const std::string& ip, const std::string& session,
                                     const char* reason, uint64_t bytes = 0, uint64_t drops = 0);

    /** One consistent snapshot of the whole machine: load, temperature, memory, uptime, the
     *  radio, listeners, uplink rate, and the ban list. One request rather than five, so the
     *  page's panels cannot disagree about what a moment looked like. */
    std::string adminStatusJson();
    /** The live listeners: address, session, frequency, mode, codec, drops, and who holds the
     *  slot. */
    std::string adminSessionsJson();
    /** Close a listener's sockets, telling them why. Empty `session` matches on address.
     *  @return how many sessions were closed. */
    int adminKick(const std::string& session, const std::string& ip);
    /** Kick everyone a ban rule matches — a ban that leaves the banned person connected is not
     *  a ban, it is a note about future connections. */
    int adminKickMatching(const std::string& cidr);

    /** ★★★ THE FOUR BUTTONS THAT EXIST INSTEAD OF A TERMINAL: reboot, restart, update-check,
     *  update (plus shutdown). A FIXED list, deliberately — the need is bounded, so the
     *  mechanism should be too. Performed by the DAEMON via the handler below; unregistered on a
     *  phone, where the honest answer is "not supported on this server". */
    bool adminAction(const std::string& action, std::string& err);
    using AdminActionFn = std::function<bool(const std::string& action, std::string& err)>;
    static void setAdminActionHandler(AdminActionFn fn);

    /** ★★★ WHICH maintenance actions this platform actually offers, comma-separated. The admin
     *  page draws only these — a button for something the server cannot do is the "drawn,
     *  enabled and inert" failure this project keeps re-learning, and here it would be worse
     *  than inert:
     *    • macOS: a reboot stops at the FileVault login and needs someone PHYSICALLY THERE to
     *      continue, so a remote reboot takes the receiver off the air until somebody walks to it.
     *    • Android: after a reboot the USB radio is not re-detected until it is unplugged and
     *      plugged back in — again, a person, in the room.
     *    • Neither updates through apt at all; they update through their app store.
     *  ★ Empty (the default) = no maintenance section at all, which is right for a phone.
     *  ★★ Advertised by the SERVER rather than sniffed by the client, so there is no per-platform
     *     branching in the page — it draws what it is told, and a new platform needs no JS. */
    static void setMaintenanceActions(const std::string& csv);

    /** ★★ WHAT THE LAST MAINTENANCE ACTION IS PRINTING, so the admin page can show it instead of
     *  a button that goes quiet for two minutes. Returns the captured output; `running` is false
     *  once the action has finished (successfully or not).
     *  ★ Supplied by the DAEMON because only it knows where the helper writes, and because this
     *    file does not exist on a phone. */
    using AdminLogFn = std::function<void(std::string& text, bool& running, int& exitCode)>;
    static void setAdminLogHandler(AdminLogFn fn);

    /** ★★ IP -> ISO-3166 country, for the flags beside listeners and the top-countries tally.
     *  Registered by the DAEMON, which owns the dataset (see vibeserver/geoip.cpp for why it is
     *  the RIRs' own published statistics and NOT a geolocation API — nobody's address is sent
     *  anywhere at runtime). Unregistered on a phone, and on a server that has not downloaded
     *  the data yet; both cases return empty, which the client must render as NO FLAG rather
     *  than as a guess. */
    using GeoIpFn = std::function<std::string(const std::string& ip)>;
    static void setGeoIpHandler(GeoIpFn fn);

    /** ★★ IP -> ASN and the network's name, for the listener list and for ASN BANS. Registered by
     *  the daemon (vibeserver/asndb.cpp — iptoasn.com's public-domain BGP table, downloaded once
     *  and queried locally, so again nothing is asked about a visitor at runtime).
     *  ★ Unregistered = ASN bans are inert rather than fatal. A server with no data must let
     *    people in, not refuse everyone it cannot identify. */
    using AsnFn = std::function<bool(const std::string& ip, uint32_t& asn, std::string& name)>;
    static void setAsnHandler(AsnFn fn);
    /** Serve the browser client at GET /. Off = app-only (a browser gets 403). */
    static void setVibeServerWebEnabled(bool on);
    /** Pin the capture rate (Hz). 0 = client-controlled. */
    static void setVibeServerLockedRate(double rate);
    /** ★★★ PIN THE HARDWARE CENTRE (Hz). 0 = follows the VFO, as it always has.
     *
     *  The radio has ONE centre frequency, so on a SHARED receiver whoever tunes past the edge
     *  of the captured band moves it FOR EVERYBODY — the band slides under every other listener
     *  mid-sentence. Locking it makes the captured window the fixed thing and lets listeners
     *  tune freely INSIDE it, which at 8 MSPS is a genuinely useful window (2.5-10.5 MHz covers
     *  80m through 30m). Tuning past the edge is then clamped rather than obeyed.
     *
     *  ★ It is also the PRECONDITION for the shared-channel (fast convolution) DSP method: every
     *  channel is a slice of one FFT of one captured band, which only means anything while that
     *  band stays put. --channels shared therefore requires this. */
    static void setVibeServerLockedCentre(double hz);
    /** Channel extraction method — see ZoomSpectrum. Set once at startup, never live. */
    static void setVibeServerSharedChannels(bool shared);
    /** Zoom spectrum on/off. It SUPPRESSES the wide path while active, so a fault in it takes
     *  the waterfall with it — hence a switch that restores the previous behaviour outright. */
    static void setVibeServerZoomSpectrum(bool on);
    /** ★★ RSP front-end notches, declared by the OPERATOR and applied by the server at open.
     *  Never defaulted on: the RF notch covers broadcast FM, so assuming it would silently gut
     *  an FM receiver. A listener cannot set these on a locked receiver, so the server must. */
    /** ★★ Seconds to wait after the LAST listener leaves before idle-parking the radio.
     *  Default 300. Protects the hardware from park/wake churn (the RSP re-enumeration stall,
     *  the libusb crash on resume, RDS dying on resume) and guarantees the AGC settle finishes.
     *  0 parks immediately, i.e. the old behaviour. */
    static void setVibeServerIdleGrace(double sec);
    static void setVibeServerRfNotch(bool on);
    /** ★★★ WHERE LISTENERS MAY TUNE. Two owner-written lists (see vibe_bands.h) collapsed against
     *  the hardware's own coverage into ONE permitted set. Published to clients so the dial can
     *  bounce and jump exactly as it does at the Airspy's tuning hole, and enforced HERE as well:
     *  a permitted set that lives only in the browser is decoration, since anyone can send a raw
     *  tune. Same split as the locked window. */
    static void setVibeServerTuneLimits(const std::string& allowCsv, const std::string& blockCsv);
    /** ★ True when THIS radio is the one drawing the landing page's spectrogram and measuring the
     *  band conditions. It must keep capturing with nobody listening — that is the entire point of
     *  a 24-hour picture — so it is exempt from the idle PAUSE. It is not exempt from RELEASE,
     *  because letting the device go already means giving those up. */
    static void setProvidesSpectrogram(bool on);
    /** ITU region (1/2/3) for the named band presets — derived from where the owner says the
     *  receiver is. See vibe_bands.h: the allocations genuinely differ between regions. */
    static void setBandRegion(int region);
    /** ★★★ SAVE A LIVE SETTING THE ADMIN JUST CHANGED, without restarting.
     *  Distinct from the config SET handler on purpose: that one is the setup page pressing Save,
     *  and it restarts to apply — correct there, absurd for someone nudging the RF gain while
     *  listening. `patch` is a JSON fragment merged over the running config and written out.
     *  ★ Registered only by the daemon. On a phone there is no /etc and nothing to persist to, so
     *    it stays unset and every call is a no-op. */
    /** Fetch/report the EiBi schedule. @param refresh download now. Returns entry count; sets
     *  `err` on failure and `updated` to the cache date. Registered by the DAEMON only — a phone
     *  has no /var/lib and no business downloading a shortwave schedule. */
    /** Space weather + our predicted verdicts, as a JSON object body. The DAEMON supplies it (it
     *  owns the network); on a phone nothing registers one and the endpoint says so. */
    using SolarFn = std::function<std::string()>;
    static void setSolarHandler(SolarFn fn);

    /** ★★ EVERY RADIO ON THIS MACHINE, for the landing page — served by whichever process owns the
     *  main port. It answers for its siblings, which it can do because they all read one config
     *  file. The shim knows nothing of the schema; the daemon supplies the JSON. */
    using RadiosFn = std::function<std::string()>;
    static void setRadiosHandler(RadiosFn fn);

    /** ★★ ONE FORWARDED PORT. Given a request path, return the unix socket of the process that
     *  should answer it, or "" to answer here. The whole connection is then handed over
     *  (SCM_RIGHTS), so nothing is proxied and this process is not in the data path.
     *  ★ The shim knows nothing about radios or serials; the daemon owns that mapping. */
    using HandoffFn = std::function<std::string(const std::string& path)>;
    static void setHandoffRouter(HandoffFn fn);

    /** Our own "/r/<serial>" prefix, stripped from arriving requests so every route below
     *  keeps matching bare paths. "" (the default) means no prefix, i.e. a single-radio server. */
    static void setPathPrefix(const std::string& prefix, const std::string& alt = "");

    /** ★★ Run a server that owns NO radio: the front door. It lists the radios, serves setup and
     *  admin, and hands connections on. It stays up when every radio has failed, which is exactly
     *  when an admin needs to get in. Returns the port, or -1 with `err`. */
    static int startFrontDoor(int port, std::string& err);

    /** Accept connections handed to us by the process holding the public port. */
    static bool listenForHandoff(const std::string& socketPath, std::string& err);
    using EibiFn = std::function<int(bool refresh, std::string& err, std::string& updated)>;
    static void setEibiHandler(EibiFn fn);
    /** ★★★ THE BROADCASTER'S OWN STATION LOGO, from RadioDNS (radiodns.org) — looked up by PI
     *  code, ECC and frequency rather than by NAME. The name is what RDS is worst at: eight
     *  characters, so "BBC Radio 2" arrives as "BBC R2" and no database matches it.
     *  ★★ THE DAEMON DOES IT, not the client: a browser cannot make a DNS SRV query, and the
     *     broadcaster's SPI host sends no CORS headers. Doing it here also fixes it once for every
     *     client — browser, phone and watch — instead of three implementations.
     *  ★ Empty on a phone, where no handler is registered; the caller falls back to the name
     *    search, which is what already works for stations outside RadioDNS. */
    using StationLogoFn = std::function<std::string(const std::string& piHex,
                                                    const std::string& ecc, double freqHz)>;
    static void setStationLogoHandler(StationLogoFn fn);
    /** ★★ EMPTY THE SERVER'S STATION-LOGO CACHE. A logo is looked up once and remembered — hits
     *  for a day, misses for an hour — so a WRONG one is remembered exactly as confidently as a
     *  right one, and the owner has no way to say "that is not the station" (Stuart, 2026-08-15:
     *  "a station that previously had a correct logo flashes up an incorrect one then never shows
     *  a logo again"). Injected like the lookup itself so this file stays free of RadioDNS. */
    using LogoCacheClearFn = std::function<void()>;
    static void setLogoCacheClearHandler(LogoCacheClearFn fn);
    static void clearLogoCache();

    /** ★★★ REWRITE AN RTL DONGLE'S SERIAL. The daemon owns this because it owns the filesystem
     *  (the mandatory backup) and the device; the shim only exposes it, exactly like the config
     *  and EiBi handlers. On a phone no handler is registered and the endpoint says so rather
     *  than pretending.
     *  ★★ It is in the WEB UI because the alternative is a shell command, and the owner most
     *  likely to need it — somebody who has just plugged in four identical dongles — is the least
     *  likely to want to type one (Stuart, 2026-08-08: "I would be nervous to use CLI to do it").
     *  @return true on success; `msg` always carries something worth showing either way. */
    using RtlSerialFn = std::function<bool(const std::string& newSerial, std::string& msg)>;
    static void setRtlSerialHandler(RtlSerialFn fn);
    /** JSON: what is pending, what the bus reports, and whether the change has taken. The page
     *  uses it to VERIFY after the reboot rather than assuming a verified write was enough — a
     *  write is confirmed against the chip, not against what USB is still announcing. */
    using RtlSerialStatusFn = std::function<std::string()>;
    static void setRtlSerialStatusHandler(RtlSerialStatusFn fn);
    using ConfigPersistFn = std::function<void(const std::string& patch)>;
    static void setConfigPersistHandler(ConfigPersistFn fn);
    /** The RSP front end as the owner last left it, applied AFTER the start-up AGC sequence.
     *  -1 on any of them = never set; leave the radio's own default. */
    static void setVibeServerSavedFrontEnd(int lnaState, int ifGr, int ifAgc);
    static void setVibeServerDabNotch(bool on);
    /** ★★★ How many spectrum listeners may attach at once (default 1 = the old single-occupant
     *  server). The DSP cost of an extra listener is ~nothing now the channelizer shares one
     *  forward FFT (+0.02% of a Pi core each, measured); the real ceiling is UPLINK. */
    static void setVibeServerMaxUsers(int n);

    // ── ★★★ THE CONFIG API — the browser setup page is a CLIENT of this, not a special case ──
    //
    // WHY HANDLERS AND NOT CODE IN HERE. This shim compiles into the Android and iOS apps as well
    // as the daemon, and a phone has no /etc and no business writing one. The DAEMON registers
    // these; on a phone they are simply never registered and the endpoints report as unavailable.
    //
    // ★★★ AND WHY AN API AT ALL, rather than form handling wired into the request router: the
    // roadmap is a Pi Zero 2 W up a tree with no keyboard, no screen and no SSH, set up entirely
    // from the VibeSDR app over a captive portal. If the browser page is one CLIENT of a
    // documented endpoint, the app later becomes a second client with NO SERVER-SIDE WORK. If the
    // validation lives in the page's JavaScript instead, app setup is a from-scratch rebuild of
    // every rule — and a SECOND implementation of them, which is the same drift this whole config
    // rework exists to kill, moved from settings to setup.
    // ★ Corollary: every question the first-run wizard asks must also be answerable through here,
    //   the admin password included, or the appliance story is blocked on the terminal.
    using ConfigGetFn = std::function<std::string()>;                             // -> JSON
    using ConfigSetFn = std::function<bool(const std::string&, std::string&)>;    // JSON, &err
    static void setConfigHandlers(ConfigGetFn get, ConfigSetFn set);
    /** Has the owner finished browser setup? Distinct from "an admin password is set" — the
     *  wizard makes that mandatory, so it can no longer stand in for this. Drives the
     *  unconfigured landing page, and gates mDNS so an unconfigured server is never discovered. */
    static void setConfigured(bool on);

    /** ★★★ THIS PLATFORM SETS ITSELF UP IN ITS OWN WINDOW, so the browser setup wizard must not
     *  be served at all — not at GET / and not at GET /setup.
     *  ★★ The wizard exists for a HEADLESS Linux box, where a browser is the only way in. macOS
     *     and Android have a settings pane that IS the setup (Stuart, 2026-08-07: "this wizard
     *     page shouldn't need to exist for either android or mac"). Leaving it reachable would
     *     leave a SECOND way to configure the same server — and two configuration surfaces drift,
     *     which is the failure this project keeps paying for.
     *  ★ Default false: the daemon is the one that needs it, and a new host should have to say
     *    it has its own UI rather than inherit an assumption. */
    static void setNativeSetup(bool on);
    /** ★★ Where a NEW SESSION starts: the owner's landing frequency (Hz) and demodulator.
     *  Applied when the listener count goes 0 -> 1, never on every connect — the shared receiver
     *  is one radio and one VFO, so landing each joiner would yank the group already listening. */
    static void setVibeServerLanding(double hz, const std::string& mode);
    static bool isConfigured();

    /** Learned RDS station bookmarks. The APP persists them; the shim learns them. */
    static void setBookmarksJson(const std::string& json);
    /** File the shim persists bookmarks to. It owns them, so it saves them — the app's
     *  JS timer could not, being suspended whenever the server was actually serving. */
    static void setBookmarksPath(const std::string& path);
    /** mDNS hostname responder: answer "<host>.local" with this IPv4. Renames itself
     *  (vibesdr-2, …) if the name is already taken on the network — RFC 6762 probing. */
    static void startMdns(const std::string& host, const std::string& ipv4);
    /** Advertise the hostname AND the _vibesdr._tcp service (Linux/macOS daemons). */
    static void startMdnsService(const std::string& host, const std::string& ipv4,
                                 int port, bool pinRequired);
    static void stopMdns();
    /** The name actually taken — may differ from the one requested. */
    static std::string mdnsHostname();
    static std::string getBookmarksJson();
    /** Empty the learned + saved bookmark list (and the file). */
    static void clearBookmarks();
    /** Station list (JSON array) served at GET /stations for the web client's
     *  search. Supplied by the app, which already downloads + caches EiBi — a
     *  browser can't fetch eibispace.de itself (it sends no CORS headers). */
    static void setStationsJson(const std::string& json);
    /** RECEIVER coarse location, served at GET /location: {"lat":..,"lon":..,"label":".."}.
     *  It's the SERVER's position — distances, map centring and the ITU region are
     *  properties of the antenna, not of whoever happens to be listening. */
    static void setLocationJson(const std::string& json);
    /** Is the running source an SDRplay? The client's controls differ materially. */
    bool isSdrplay() const;
    /** RF gain positions on an RSP (0 = not an RSP / no radio). */
    int rfGainPositions() const;
    /** `,"radio":{…}` describing the running receiver's real controls, for hwinfo. */
    std::string radioCapsJson() const;
    /** RSP-only controls. No-ops on any other source. */
    void setLnaState(int state);
    void setIfGainReduction(int gRdB);
    void setIfAgc(bool on);
    void setIfAgcSetPoint(int dBfs);
    void setIfAgcDynamics(int attackMs, int decayMs, int delayMs, int threshDb);
    void setRfNotch(bool on);
    void setDabNotch(bool on);
    void setBiasT(bool on);
    /** Start on an Airspy HF+ (Discovery / Dual Port). Returns the port, or -1 with err set.
     *  ★ The requested sample rate is a HINT — the radio's own list wins, so read back
     *  getVibeServerStatus().sampleRate rather than assuming what you asked for. */
    /** ★ HackRF One — EXPERIMENTAL, all three servers, never run against a radio by its author.
     *  See hackrf_source.h. No Fd twin: this driver is not offered on Android. */
    /** ★ Android: open from a UsbManager descriptor. libusb takes ownership of the fd. */
    int startHackRfFd(int fd, double centerFreq, double sampleRate, int gainTenthDb,
                      int fftSize, double fftRate, const std::string& mode, std::string& err);
    /** Shared body — exactly one of index/fd is valid. See the note in the .cpp. */
    int startHackRfCommon(int index, int fd, double centerFreq, double sampleRate, int gainTenthDb,
                          int fftSize, double fftRate, const std::string& mode, std::string& err);
    int startHackRf(int index, double centerFreq, double sampleRate, int gainTenthDb,
                    int fftSize, double fftRate, const std::string& mode, std::string& err);
    int startAirspyHf(int index, double centerFreq, double sampleRate, int gainTenthDb,
                      int fftSize, double fftRate, const std::string& mode, std::string& err);
    /** ★ Start an Airspy HF+ from a USB file descriptor — Android's only route in. Reached
     *  from start() by VID/PID, so callers do not need to know which driver a device wants. */
    int startAirspyHfFd(int fd, double centerFreq, double sampleRate, int gainTenthDb,
                        int fftSize, double fftRate, const std::string& mode, std::string& err);
private:
    int startAirspyHfCommon(int index, int fd, double centerFreq, double sampleRate,
                            int gainTenthDb, int fftSize, double fftRate,
                            const std::string& mode, std::string& err);
public:
    /** Airspy HF+ only controls. No-ops on any other source. */
    void setAhfAgc(bool on);
    void setAhfAgcThreshold(bool high);
    void setAhfAttenuation(int steps);
    void setAhfLna(bool on);
    /** ★ HackRF stages. setHackRfAmp is OWNER-ONLY at the message layer — it can destroy the
     *  radio, unlike every other gain control here. See the hackrf_control handler. */
    void setHackRfAmp(bool on);
    void setHackRfLna(int db);
    void setHackRfVga(int db);
    void setHackRfBiasTee(bool on);
    /** ★ What the stages are set to now. Read by the owner's gain limit, which caps LNA + VGA
     *  TOGETHER — both sit after the mixer, so the total is what drives the 8-bit converter. */
    int  hackRfLna() const;
    int  hackRfVga() const;
    void setAhfCalibrationPpb(int ppb);

    /** Start on an SDRplay RSP (14-bit). Returns the port, or -1 with err set. */
    int startSdrplay(int index, double centerFreq, double sampleRate, int gainTenthDb,
                     int fftSize, double fftRate, const std::string& mode, std::string& err);

    // SpyServer-compatible backend. Mirrors startTcp(): network IQ into the same
    // DSP pipeline, so demod/decoders/NR/audio all work unchanged — and, like
    // startTcp, it has no USB dependency and therefore works on iOS too.
    int startSpyServer(const std::string& host, int port,
                       double centerFreq, double sampleRate, int gainTenthDb,
                       int fftSize, double fftRate, const std::string& mode,
                       std::string& err);

    int startTcp(const std::string& host, int port,
                 double centerFreq, double sampleRate, int gainTenthDb,
                 int fftSize, double fftRate, const std::string& mode, std::string& err);

    void stop();
    bool isRunning() const;

    // Decoder-only "sidecar" mode for network backends (Kiwi/OWRX): starts just
    // the localhost /ws/dxcluster server + the decoder modules, NO RTL/FFT/demod.
    // The app feeds it the backend's decoded audio via feedDecoderPcm(); the
    // existing DecoderClient connects to the returned port and the decoder UI
    // works unchanged. Returns the TCP port (>0) or -1 with `err` set.
    int startDecoderService(std::string& err);
    // Feed mono int16 PCM at `rate` Hz (upsampled to the decoders' 48 kHz).
    void feedDecoderPcm(const int16_t* pcm, int n, int rate);
    // Tell the sidecar the backend's dial frequency (Hz) so FT8 spot RF freq +
    // band are correct (otherwise they're computed against a 100 MHz default →
    // empty band / wrong tune freq). Network backends (Kiwi) call this on tune.
    void setDecoderFreq(double hz);

    // Hardware controls (no-ops if not running). gainTenthDb < 0 = auto gain.
    /** Let the radio go so another program can open it, keeping the server up. See the .cpp. */
    bool releaseRadio();
    /** Take it back. False (with a reason) when something else now holds it — a normal outcome. */
    bool reacquireRadio(std::string& err);
    bool radioIsReleased() const;

    void setGain(int gainTenthDb);
    /** ★ The R820T's own IF filter, in Hz. 0 = librtlsdr's automatic choice (today's behaviour).
     *  See pendingTunerBw — the only selectivity we have AHEAD of the mixer. */
    void setTunerBandwidth(int hz);
    /** ★ Recompute the IF filter from the current view (Auto mode only). */
    void applyAutoIf();
    /** ★ TEF6686-style automatic demodulator bandwidth, FM broadcast only. See g_autoBwOn. */
    void autoBandwidthTick();
    /** ★ Where the tuner is centred (NOT the view, NOT the VFO) — the IF filter is centred here. */
    double rfCentreHz() const;
    /** ★ Re-send the hardware description to every spectrum listener. */
    void broadcastHwInfo();
    /** ★ Re-decide the tuner's centre and move it if the answer changed (VibeClarity). */
    void applyCentreNow();
    void setPpm(int ppm);
    void setBiasTee(bool on);
    void setAgc(bool on);                 // RTL2832 digital AGC
    void setDirectSampling(int mode);     // 0=off, 1=I, 2=Q (not needed on Blog V4)
    void setSampleRate(double rate);
    /** ★ True when the serving radio is an Airspy HF+, whose sample rate is PINNED at open —
     *  changing it on a live stream is a path no other SDR client takes and ours could leave the
     *  device needing a power cycle. See the lockedRate note in hwinfo. */
    bool isAirspyHf() const;      // cancels + restarts the IQ stream (auto FFT size)
    bool isHackRf() const;        // EXPERIMENTAL — three manual gain stages, and no AGC at all
    void setFftRate(double fps);          // LIVE spectrum frame rate (power saving); audio unaffected
    void setDeemphasis(double tau);       // FM de-emphasis time constant (0=off, 50e-6, 75e-6)
    void setSquelch(bool on, float db);   // power-based audio squelch (dBFS)
    void setNR(bool on);                  // audio noise reduction on/off
    void setNrStrength(float s);          // NR aggressiveness 0..1.4 (>1 = over-subtraction)
    void setNotch(bool on);               // automatic notch (adaptive line enhancer)
    // ★ Weak-signal processing: FM stereo high-blend + the audio high-cut, together. One switch,
    //   because they are one treatment — and a DXer A/B-ing a marginal catch wants BOTH out of the
    //   way, not half of it (asked for by Saber via the FM-DX community, 2026-08-14).
    void setWeakProc(bool on);
    // ★ IMS is SEPARATE from the noise treatment on purpose: NR works on noise, this works on a
    //   neighbour, and measured they want opposite actions. One button for both would leave a
    //   listener unable to tell which of the two was helping.
    void setIms(bool on);
    void setCeq(bool on);
    void setNoiseBlanker(bool on);        // impulse noise                 // blind channel equaliser (multipath)
    void setStereoEnabled(bool on);       // WFM: allow stereo (true) vs force mono
    float getNrCpu();                     // NR CPU% (rolling) for the UI readout
    // Returns supported tuner gains (tenths of dB); empty if not running.
    std::vector<int> getTunerGains();

    // Network (rtl_tcp client) link health. `tcp` is false on the USB path, where
    // none of this applies. Counters are cumulative for the session.
    struct NetStatus {
        bool     tcp        = false;
        uint64_t stalls     = 0;   // socket delivered nothing for >120ms
        uint64_t droppedSamples = 0;
        uint32_t bufferedMs = 0;   // current standing backlog
        // SpyServer only. `spy` distinguishes the backend; `canControl` is false
        // when another client owns the tuner (a read-only server), and `closed`
        // means the server hung up — session time limit, or it handed the tuner
        // to someone else. Both must be surfaced, not reported as a generic
        // "connection lost".
        bool     spy        = false;
        bool     canControl = true;
        bool     closed     = false;
    };
    NetStatus getNetStatus();

    // VibeServer live status for the sharing screen: whether a remote client is
    // connected, and SEPARATE real-time byte rates for the spectrum vs the audio
    // stream (so the user sees exactly what the server is pushing).
    struct VibeServerStatus {
        bool     running          = false;
        bool     clientConnected  = false;
        std::string clientAddr;
        double   specBytesPerSec  = 0.0;
        double   audioBytesPerSec = 0.0;
        bool     compressed       = true;
        bool     pinEnabled       = false;
        double   fftRate          = 0.0;
        double   bandwidthHz      = 0.0;
        /** Capture sample rate the CLIENT has asked for (the shim honours it
         *  live). Surfaced on the sharing screen so the host can see the server
         *  responding to the client. */
        double   sampleRate       = 0.0;
        /** The radio has stopped delivering IQ — unplugged or failed. The server is still up and
         *  still serving; it simply has nothing to serve. */
        bool     deviceLost       = false;
        /** ★★★ THE SAME COUNT THE ADMIN PAGE AND EVERY PICKER USE (specListenerCount). The host's
         *  own screen used to derive "is anybody on" from the spectrum socket alone, which is a
         *  SECOND definition of the same state — and the two disagreed exactly when it mattered.
         *  See the note on clientConnected in getVibeServerStatus(). */
        int      listeners        = 0;
        int      maxUsers         = 1;
        /** The port it actually bound. The status is how a screen ADOPTS a server it did not
         *  start, and without this it could only ever show `ip:0`. */
        int      port             = 0;
    };
    VibeServerStatus getVibeServerStatus();

private:
    LocalSdrShim() = default;
    void stopLocked();      // teardown; caller must hold g_lifecycle
    struct Impl;
    /** ★ Replay the listener's DSP choices (de-emphasis, squelch, NR, notch, stereo) onto a
     *  freshly built Impl. Every start path replaces `p` with a `new Impl`, which would
     *  otherwise revert those choices to constructor defaults while the client's UI carried on
     *  showing what the user had picked. Call at EVERY `p = impl` site. */
    static void applyDesiredDsp(Impl* impl);
    Impl* p = nullptr;
};

} // namespace vibe
