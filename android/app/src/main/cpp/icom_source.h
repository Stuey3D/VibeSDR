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
//       5. ★ The ONE frame outside the table is POWER ON (18 01 behind its FE wake run, civ::power):
//          sent by sendWake() alone, only when the owner ticked `powerOn`, only while the radio is not
//          answering, at most once per kWakeEverySec — or on the owner's RADIO ON press. POWER OFF
//          (18 00) is sent only by requestPowerOff(): the owner's RADIO OFF press, admin-gated in the
//          shim, NEVER automatic (not on stop, restart or idle). Both have their own exact checks.
//     ★★ A SWITCHED-OFF RADIO IS A NORMAL STATE (2026-11-04, Saber's Pi 4: other software switches
//        his 7300 on, so VibeServer often starts first). USB present + CI-V silent = open() SUCCEEDS
//        in the waiting state: link down, linkProblem() in plain words, identify retried every few
//        seconds, and the full path (identify, PREFLIGHT, state, scope, audio) runs when it answers.
//        An ABSENT USB device still fails open() — that is "not detected", not "switched off".
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
    /** ★ Owner option "Power on the radio over CI-V" (config icomPowerOn, default OFF): when the radio is
     *  not answering, send it 18 01 behind the manual's FE run (civ::power). Without it VibeServer NEVER sends
     *  a power command by itself (the owner's RADIO ON / RADIO OFF buttons still work). With it AND `autoOff`,
     *  the radio is also switched OFF once nobody has used it for `offDelayMin`: the shim calls idlePowerOff()
     *  at that park, and the next listener's arrival powers it back on (setInUse). */
    bool powerOn      = false;
    /** ★ "Automatically power off the radio when not in use" (config icomAutoOff, default ON; Stuart 2026-11-05). Only
     *  with `powerOn` — otherwise nothing could switch it back on for the next listener. It, not the generic idle
     *  power-down ("Power down the radio"), decides whether an Icom is switched off when nobody is using it. */
    bool autoOff      = true;
    /** ★ "How long after the last user leaves" (config icomOffDelayMin): 0 (Instant), 1, 5, 10, 15, 30, 60 minutes —
     *  so a radio is not switched on and off by listeners coming and going. */
    int  offDelayMin  = 5;
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
    /** ★ USB present, CI-V silent: switched off (or not answering). The source is up and waiting for it. */
    bool waitingForRadio() const { return waiting_.load(); }
    /** Power-on commands sent (owner option) since open. */
    uint64_t wakesSent() const { return wakes_.load(); }
    /** ★ Why the link is down, in plain words for every listener's banner and the setup page; "" when up. */
    std::string linkProblem() const;
    /** ★ The owner's next step for that, for the setup page (the CI-V menu hint, or the preflight's words). */
    std::string linkHint() const;
    /** ★★ THE OWNER'S BUTTONS (admin-gated by the caller). RADIO OFF sends 18 00 once, then the source waits
     *  quietly ("Switched off by the owner") and the auto power-on option is held off until RADIO ON or the
     *  radio is switched on by hand. RADIO ON sends the wake once, whether or not `powerOn` is ticked.
     *  Both only queue: the CI-V thread sends. False + `why` when it makes no sense now. */
    bool requestPowerOff(std::string& why);
    bool requestPowerOn(std::string& why);
    bool offByOwner() const { return offByOwner_.load(); }
    bool usbMissing() const { return usbMissing_.load(); }
    bool autoPowerOn() const { return powerOn_.load(); }
    /** ★ Automatic power-off in effect: "Power on the radio over CI-V" AND "Automatically power off … when not in use". */
    bool autoPowerOff() const { return powerOn_.load() && autoOff_.load(); }
    int  offDelayMin() const { return offDelay_.load(); }
    /** ★ The Setup page's three power settings, applied LIVE (the radio process re-reads config.json on a change). */
    void setPowerOptions(bool powerOn, bool autoOff, int offDelayMin) { powerOn_.store(powerOn); autoOff_.store(autoOff); offDelay_.store(offDelayMin); }
    /** ★ Somebody (a listener, a held session, a decoder) is using the radio — the shim's housekeeping says so
     *  every tick. A radio the idle park switched off is woken only while this is true. Any thread. */
    void setInUse(bool inUse);
    /** ★★ THE IDLE PARK, ON AN ICOM — called by the shim exactly where any other radio parks (nobody watching, no
     *  background decoder, the owner's idle grace passed). Only with `powerOn` (otherwise nothing could switch it
     *  back on for the next listener) and only while it answers. Returns true when a power-off was queued. */
    bool idlePowerOff();
    /** ★ A power-on was sent and the radio has not answered yet — listeners see "Switching the radio on". */
    bool switchingOn() const;
    uint64_t powerOffsSent() const { return offs_.load(); }
    static constexpr double kWakeEverySec = 30.0;   ///< at most one power-on per this, while waiting
    static constexpr double kBootSec = 4.0;         ///< after a power-on, let the radio boot before identify
    static constexpr int kDrainMs = 600;            ///< a power frame's wait to leave the USB chip (157 B at 4800 = 330 ms)
    static constexpr double kOffSettleSec = 15.0;   ///< after OUR power-off: an IC-7300 answers CI-V for a few s while it shuts down
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
    std::atomic<bool> waiting_{false};      ///< USB there, CI-V silent — switched off (or not answering)
    std::atomic<bool> preRefused_{false};   ///< the radio answered, and the preflight refused it
    std::atomic<bool> usbMissing_{false};   ///< the transport would not open on a retry
    std::atomic<uint64_t> wakes_{0}, offs_{0};
    std::atomic<bool> wantOff_{false}, wantOn_{false};   ///< the owner's presses, for the CI-V thread
    std::atomic<bool> offByOwner_{false};                ///< RADIO OFF was pressed and the radio has not answered since
    std::atomic<bool> offByIdle_{false};                 ///< the idle park switched it off (powerOn): nobody was using it
    std::atomic<bool> wantIdleOff_{false};
    std::atomic<bool> inUse_{false}, kick_{false}, arrivalWake_{false};
    std::atomic<bool> powerOn_{false}, autoOff_{true};   ///< cfg_.powerOn / cfg_.autoOff, live (setPowerOptions)
    std::atomic<int>  offDelay_{5};                       ///< cfg_.offDelayMin, live
    std::atomic<double> wakeAt_{-1e9};                   ///< lastWakeAt_, readable from other threads
    double lastWakeAt_ = -1e9;              ///< CI-V thread (and open) only
    double bootUntil_ = 0;
    /** ★★ After OUR 18 00, an answer is NOT "switched on" until the radio has been silent once (offSeenDown_) or
     *  kOffSettleSec has passed. An IC-7300 answers CI-V for a few seconds while it shuts down, and a probe 3 s after
     *  the power-off took that for "switched on" and started everything again (Saber, 2026-11-05: an off / on / off
     *  cycle every five minutes). Once it has gone quiet it is down, and a wake may go at once. */
    double offSettleUntil_ = 0;
    bool   offSeenDown_ = true;   ///< since our last 18 00, has the radio been SILENT once (= it really went down)?
    bool audioStarted_ = false;
    std::string lastRetryErr_;              ///< so a radio that stays off logs once, not every 3 s
    mutable std::mutex audioMtx_;           ///< audio_ is created on the CI-V thread when a waiting radio answers

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
    bool identify(std::string& err, bool* silent = nullptr);
    bool sendWake(bool byOwner);
    void doPowerOff(bool byOwner);
    bool autoWakeWanted() const;
    void enterWaiting();
    void startAudio();
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
