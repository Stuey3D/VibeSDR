// icom_source.h — an Icom transceiver as a VibeServer RECEIVER (driver "icom"). Receive only.
//
// ★★★ A NEW KIND OF SOURCE (plan §2.2). Every other *_source pushes IQ into the engine. An Icom
//     has no IQ at all (the 7300 has none of any kind): it gives us
//       • CI-V — tuning, mode, filter, AGC, ATT, preamp, RF gain — over its USB serial port;
//       • its SCOPE (27 00): 475 points a sweep, which becomes our waterfall (icom_scope.h);
//       • demodulated AUDIO on its USB codec, which goes in AFTER our demodulator (icom_audio.h).
//     So the shim runs no FFT and no demodulator for it, and the dial is always SHARED: there is one
//     physical receiver, and its front panel is another "user" (CI-V transceive announces the knob).
//
// ★★★ RECEIVE ONLY, PROVABLY (plan §3). This class has no PTT, no TX audio, no transmit command:
//       1. every frame comes from civ::build (there is no builder for a transmit command);
//       2. sendFrame() — the ONE write path — asks civ::mayTransmit() about the whole frame first
//          and drops anything off the table, counting it as a fault (refusedFrames());
//       3. the transport never asserts DTR/RTS (icom_transport.h);
//       4. open() refuses to start if the radio's own menu could key it from a modem line or from
//          audio (USB SEND, USB Keying, VOX), naming the menu item to change. We never change it.
//     While the OWNER transmits (1C 00 polled), audio is muted and the scope is frozen: listeners
//     must not hear the owner's monitor, and a TX sweep is not the band.
//
// ★★ THREADS: one CI-V thread ("vibe-civ", spectrum priority — the scope IS the spectrum) that owns
//    the transport, and the audio capture thread (audio priority). No others. Control calls from the
//    socket threads only queue; tuning jumps every poll and scope command (thread priority rule:
//    a person turning the dial comes before a meter reading).
#pragma once
#include "civ_codec.h"
#include "icom_audio.h"
#include "icom_models.h"
#include "icom_scope.h"
#include "icom_transport.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace icom {

struct IcomConfig {
    std::string port  = "usb";     ///< see makeTransport
    int  baud         = 115200;    ///< must match the radio's CI-V USB Baud Rate (or the radio is on Auto)
    int  civAddr      = 0;         ///< 0 = the model's default (from the USB serial), else forced
    std::string audio = "auto";    ///< see makeAudio
    int  scopeSpeed   = 0;         ///< 27 1A: 0 fast, 1 mid, 2 slow
    bool ttyOk        = false;     ///< the owner allows a /dev serial device (DTR/RTS glitch on open)
    std::string modelHint;         ///< "IC-7300" from the USB serial, when known
};

struct PreflightItem {
    std::string name, where;   ///< "USB SEND", "MENU > SET > Connectors > …"
    std::string value;         ///< "OFF", "DTR", "RTS", "ON", or "not readable"
    bool gate = true;          ///< a non-OFF value refuses the start
    bool ok = false;
};
struct Preflight {
    bool ran = false, passed = false;
    bool verified = false;     ///< the model's menu numbers have been confirmed on a real radio
    std::vector<PreflightItem> items;
    std::string summary;       ///< plain words for the setup page and the error
};

/** What the radio is doing, as last read or announced. */
struct RadioState {
    uint64_t freq = 0;
    std::string mode;          ///< our name: "usb", "usbd", "cw", "rtty", …
    int fil = 1;
    int att = 0;               ///< dB
    int preamp = 0;            ///< 0 off, 1 P.AMP1, 2 P.AMP2
    int agc = 2;               ///< 1 fast, 2 mid, 3 slow
    int rfGain = -1, squelch = -1;
    bool nb = false, nr = false, anf = false;
    int sRaw = -1;             ///< 15 02, 0-255
    bool tx = false;
    int scopeMode = 0;         ///< 0 centre, 1 fixed
    uint32_t spanHalfHz = 0;
    double sweepsPerSec = 0;
    bool linkUp = false;
};

class IcomSource {
public:
    using SweepFn = std::function<void(const Sweep&)>;
    /** `what` is a mask of the Changed bits — so the shim re-sends config only for a real move. */
    using StateFn = std::function<void(unsigned what)>;
    using LostFn  = std::function<void(const std::string& why)>;
    enum Changed : unsigned { kFreq = 1, kMode = 2, kRf = 4, kScope = 8, kTx = 16, kMeter = 32, kLink = 64 };

    IcomSource() = default;
    ~IcomSource();

    void setSweepSink(SweepFn f) { onSweep_ = std::move(f); }
    void setStateSink(StateFn f) { onState_ = std::move(f); }
    void setPcmSink(PcmFn f)     { onPcm_ = std::move(f); }
    void setLostSink(LostFn f)   { onLost_ = std::move(f); }

    /** Open, identify (19 00), PREFLIGHT, read the state, switch the scope on, start the threads.
     *  ★ Fails — with plain words in `err` — when the preflight does not pass. `injected` replaces
     *  the transport (tests, the fake radio). */
    bool open(const IcomConfig& cfg, std::string& err, std::unique_ptr<CivTransport> injected = nullptr);
    /** Scope data output OFF (we leave the radio as we found it as far as we can), then close. */
    void close();

    // ── Controls. Each queues an allow-listed frame; `why` says what was refused, in plain words.
    bool tune(uint64_t hz, std::string& why);
    bool setMode(const std::string& ourMode, int fil, std::string& why);   ///< fil 0 = keep
    bool setFilter(int fil, std::string& why);
    bool setAtt(int db, std::string& why);
    bool setPreamp(int v, std::string& why);
    bool setAgc(int v, std::string& why);         ///< 1 fast, 2 mid, 3 slow
    bool setRfGain(int v, std::string& why);      ///< 0-255
    bool setNb(bool on, std::string& why);
    bool setNr(bool on, std::string& why);
    bool setAnf(bool on, std::string& why);
    bool setSpanTotal(uint32_t totalHz, std::string& why);   ///< one of model().spans ×2
    bool setScopeMode(int m, std::string& why);   ///< 0 centre, 1 fixed
    bool setScopeSpeed(int s, std::string& why);

    RadioState state() const;
    const Model& model() const { return *model_; }
    const Preflight& preflight() const { return pre_; }
    uint8_t civAddr() const { return policy_.radio; }
    int baud() const { return cfg_.baud; }
    const char* transportKind() const { return transport_ ? transport_->kind() : "none"; }
    bool linesHeldLow() const { return transport_ && transport_->linesHeldLow(); }
    std::string audioDescription() const;
    /** The open-time failure, else what the live source says is wrong now (the Android push source's state). */
    std::string audioProblem() const;
    /** ★ ANDROID: the radio left the bus — its usbfs descriptor is dead and only the app can open a new one. */
    bool usbGone() const { return usbGone_.load(); }
    /** True while usbGone() and no fresh descriptor is waiting (the app polls this, as for every radio). */
    bool wantsFreshFd() const { return usbGone_.load() && cfg_.port.rfind("fd:", 0) == 0 && freshFd_.load() < 0; }
    /** Hand in that fresh descriptor (ownership passes); the next reconnect adopts it. Any thread. */
    bool offerFd(int fd);
    uint64_t refusedFrames() const { return refused_.load(); }
    uint64_t sweepsDropped() const { return scope_.dropped; }
    /** S-meter reading as dB relative to S9 (Hamlib's published 7300 calibration points). */
    static double sRawToDbOverS9(int raw);

    /** Build the CI-V allow-list policy for a model (also used by the fake radio and the tests). */
    static civ::Policy policyFor(const Model& m, uint8_t addr);

private:
    IcomConfig cfg_;
    const Model* model_ = nullptr;
    civ::Policy policy_;
    std::unique_ptr<CivTransport> transport_;
    std::unique_ptr<AudioCapture> audio_;
    std::string audioErr_;
    Preflight pre_;
    civ::Parser parser_;
    ScopeAssembler scope_;

    mutable std::mutex stMtx_;
    RadioState st_;

    SweepFn onSweep_; StateFn onState_; PcmFn onPcm_; LostFn onLost_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> refused_{0};
    std::thread io_;
    std::atomic<bool> tx_{false};
    std::atomic<bool> usbGone_{false};   ///< read() said the device is GONE (not merely quiet)
    std::atomic<int>  freshFd_{-1};      ///< Android: a descriptor waiting for reconnect() (ownership is ours)
    std::atomic<double> txHoldUntil_{0.0};

    // ── The request queue: a fixed ring per lane, coalesced by (cmd, sub). ★ No allocation.
    struct Req { civ::Out out; uint8_t cmd = 0; int sub = -1; bool used = false; };
    static constexpr int kLane = 16;
    std::mutex qMtx_;
    Req hi_[kLane];          ///< a person's controls: tune, mode, gain — always first
    Req lo_[kLane];          ///< polls and reads
    Req inflight_;
    double inflightAt_ = 0;

    bool enqueue(const civ::Out& o, bool high, std::string& why);
    bool takeNext(Req& r);
    bool sendFrame(const civ::Out& o);
    bool transact(const civ::Out& o, uint8_t wantCmd, int wantSub, int timeoutMs, civ::Frame* reply);
    bool identify(std::string& err);
    bool runPreflight(std::string& err);
    void readInitialState();
    void ioLoop();
    void onFrame(const civ::Frame& f, unsigned& changed);
    void notify(unsigned what) { if (what && onState_) onState_(what); }
    bool reconnect();
    static double now();

    // sweep rate
    double rateT0_ = 0; int rateN_ = 0;
    double lastByteAt_ = 0;
    uint8_t modeByte_ = 0xFF;   ///< last 04/01 mode byte — USB-D needs it with the 1A 06 flag
    bool dataOn_ = false;
};

}  // namespace icom
