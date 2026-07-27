// VibeSDR V4 — local-SDR shim: RTL-SDR → FFT + demod → localhost UberSDR.
// Stage 3 added the spectrum WebSocket; Stage 4 adds the demodulated-audio
// WebSocket (int16 PCM, mono or stereo for WFM) plus tune/mode/bandwidth
// control, so the existing VibeSDR audio engine plays local hardware.
#pragma once
#include <string>
#include <vector>

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
    /** Serve the browser client at GET /. Off = app-only (a browser gets 403). */
    static void setVibeServerWebEnabled(bool on);
    /** Pin the capture rate (Hz). 0 = client-controlled. */
    static void setVibeServerLockedRate(double rate);
    /** Learned RDS station bookmarks. The APP persists them; the shim learns them. */
    static void setBookmarksJson(const std::string& json);
    /** File the shim persists bookmarks to. It owns them, so it saves them — the app's
     *  JS timer could not, being suspended whenever the server was actually serving. */
    static void setBookmarksPath(const std::string& path);
    /** mDNS hostname responder: answer "<host>.local" with this IPv4. Renames itself
     *  (vibesdr-2, …) if the name is already taken on the network — RFC 6762 probing. */
    static void startMdns(const std::string& host, const std::string& ipv4);
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
    int startAirspyHf(int index, double centerFreq, double sampleRate, int gainTenthDb,
                      int fftSize, double fftRate, const std::string& mode, std::string& err);
    /** Airspy HF+ only controls. No-ops on any other source. */
    void setAhfAgc(bool on);
    void setAhfAgcThreshold(bool high);
    void setAhfAttenuation(int steps);
    void setAhfLna(bool on);
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
    void setGain(int gainTenthDb);
    void setPpm(int ppm);
    void setBiasTee(bool on);
    void setAgc(bool on);                 // RTL2832 digital AGC
    void setDirectSampling(int mode);     // 0=off, 1=I, 2=Q (not needed on Blog V4)
    void setSampleRate(double rate);      // cancels + restarts the IQ stream (auto FFT size)
    void setFftRate(double fps);          // LIVE spectrum frame rate (power saving); audio unaffected
    void setDeemphasis(double tau);       // FM de-emphasis time constant (0=off, 50e-6, 75e-6)
    void setSquelch(bool on, float db);   // power-based audio squelch (dBFS)
    void setNR(bool on);                  // audio noise reduction on/off
    void setNrStrength(float s);          // NR aggressiveness 0..1
    void setNotch(bool on);               // automatic notch (adaptive line enhancer)
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
