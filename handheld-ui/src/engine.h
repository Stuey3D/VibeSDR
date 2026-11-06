// engine.h — vibe-hhui's link to the handheld engine: the supervisor's /handheld/* endpoints and the radio's two
// WebSockets (spectrum + audio), speaking the same protocol as every VibeSDR client (docs/PROTOCOL.md, proto 1).
//
// ★★★ THE SHARED-DIAL CONTRACT (memory: shared_dial_contract): the server sends the FULL state, the client has no vote,
//     and the client sends only what the person did. So the UI's frequency, mode and view are whatever the last
//     `config` said; a press sends a message and the screen follows the answer (with a local prediction only where
//     the app predicts too — the readout moves at once on a tune).
// ★ Audio: the /ws/audio socket carries `speaker=1` — the ENGINE plays the sound on the R36S speaker
//   (vibe_local_speaker.h) and sends this socket nothing. It must stay open for the session, uncompressed.
// ★ Threads: one worker owns connecting / reconnecting and the 5 s ping; the sockets' readers queue messages; the UI
//   thread calls pump() once a frame and gets everything on its own thread. Nothing here touches GL.
#pragma once
#include "http.h"
#include "json.h"
#include "ws.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace hh {

/** The per-mode passband the server applies on a mode change (web/client/src/spectrum.ts MODE_BANDWIDTHS). */
inline bool modePassband(const std::string& m, int& lo, int& hi) {
    struct E { const char* m; int lo, hi; };
    static const E t[] = {
        {"usb", 50, 2700}, {"lsb", -2700, -50}, {"am", -5000, 5000}, {"sam", -5000, 5000},
        {"cwu", -200, 200}, {"cwl", -200, 200}, {"fm", -6000, 6000}, {"nfm", -5000, 5000},
        {"wfm", -100000, 100000}, {"drm", -5000, 5000},
    };
    for (auto& e : t) if (m == e.m) { lo = e.lo; hi = e.hi; return true; }
    return false;
}

struct RadioState {
    // the supervisor
    std::string session = "unknown";     // none | starting | running | unknown (supervisor not answering)
    std::string radioName, radioDriver, radioSerial;
    // config (this listener's view)
    double viewCentreHz = 0, spanHz = 0, maxSpanHz = 0, binBwHz = 0, vfoHz = 0;
    int binCount = 0;
    std::string mode;
    bool locked = false, shared = false;
    int bwLow = 0, bwHigh = 0;            // ★ the client's mirror (the server never reports it back)
    // hwinfo
    std::vector<int> gains;              // tenth dB
    int gainNow = 0;
    bool agc = false;
    std::vector<double> rates;
    double rateNow = 0;
    std::vector<std::string> blockedModes;
    // live
    double chanDb = -200, floorDb = -200;
    std::string rdsPs, rdsRt;
    bool rdsStereo = false;
    bool connected = false;              // the spectrum socket is up
    uint64_t configSeq = 0;              // bumps on every config (the UI re-reads)
};

/** Hardware-service events relayed by the supervisor (/handheld/events). */
struct DeviceEvent { std::string kind; Json body; };

class Engine {
public:
    explicit Engine(int port = 48000) : port_(port) { sid_ = uuid(); bid_ = loadBid(); }
    ~Engine() { stop(); }

    void start() {
        run_ = true;
        worker_ = std::thread([this] { linkLoop(); });
        events_ = std::thread([this] { eventLoop(); });
    }
    void stop() {
        if (!run_.exchange(false)) return;
        cv_.notify_all();
        spec_.close(); audio_.close();
        if (worker_.joinable()) worker_.join();
        if (events_.joinable()) events_.detach();     // ★ a long poll in flight ends by itself (≤ 35 s); never block quit
    }

    /** UI thread, once a frame. `onSpec(binsLowToHigh, n, viewCentreHz, tSec)` for every SPEC frame, oldest first. */
    void pump(const std::function<void(const uint8_t*, int, double, double)>& onSpec,
              std::vector<DeviceEvent>& deviceEvents) {
        std::vector<WsMessage> msgs;
        spec_.drain(msgs);
        st_.connected = spec_.alive();
        for (auto& m : msgs) {
            if (!m.text) { specFrame(m.data, onSpec); continue; }
            onText(m.data);
        }
        std::vector<WsMessage> amsgs;
        audio_.drain(amsgs);                          // ★ speaker mode: only text, if anything
        { std::lock_guard<std::mutex> lk(sm_); if (sessionDirty_) { st_.session = sessState_; st_.radioName = sessName_;
              st_.radioDriver = sessDriver_; st_.radioSerial = sessSerial_; sessionDirty_ = false; } }
        { std::lock_guard<std::mutex> lk(em_); for (auto& e : evq_) deviceEvents.push_back(std::move(e)); evq_.clear(); }
    }

    const RadioState& state() const { return st_; }

    // ── what the person did ──
    void tune(double hz) {
        st_.vfoHz = hz;                                // ★ the readout moves at once (the app predicts the same)
        char b[256];
        std::snprintf(b, sizeof b, "{\"type\":\"tune\",\"frequency\":%.0f,\"mode\":%s,\"bandwidthLow\":%d,\"bandwidthHigh\":%d}",
                      hz, jsonQuote(st_.mode).c_str(), st_.bwLow, st_.bwHigh);
        spec_.sendText(b);
    }
    void setMode(const std::string& m) {
        int lo, hi;
        if (!modePassband(m, lo, hi)) return;
        st_.mode = m; st_.bwLow = lo; st_.bwHigh = hi;
        char b[256];
        // ★ two messages, as the web client: the server ignores bandwidth on a tune that changes mode
        std::snprintf(b, sizeof b, "{\"type\":\"tune\",\"frequency\":%.0f,\"mode\":%s,\"modeSet\":1}", st_.vfoHz, jsonQuote(m).c_str());
        spec_.sendText(b);
        setBandwidth(lo, hi);
    }
    void setBandwidth(int lo, int hi) {
        st_.bwLow = lo; st_.bwHigh = hi;
        char b[160];
        std::snprintf(b, sizeof b, "{\"type\":\"bandwidth\",\"bandwidthLow\":%d,\"bandwidthHigh\":%d}", lo, hi);
        spec_.sendText(b);
    }
    /** View of `spanHz` centred on `centreHz` (the server clamps it and answers with config). */
    void zoom(double centreHz, double spanHz, bool free = false) {
        if (st_.binCount <= 0) return;
        char b[200];
        std::snprintf(b, sizeof b, "{\"type\":\"zoom\",\"frequency\":%.0f,\"binBandwidth\":%.6f%s}", centreHz,
                      spanHz / st_.binCount, free ? ",\"free\":1" : "");
        spec_.sendText(b);
    }
    void zoomReset() { spec_.sendText("{\"type\":\"reset\"}"); }
    void fftRate(int fps) { spec_.sendText("{\"type\":\"fftRate\",\"value\":" + std::to_string(fps) + "}"); }
    void speakerLevel(double v) { char b[80]; std::snprintf(b, sizeof b, "{\"type\":\"speaker\",\"level\":%.3f}", v); spec_.sendText(b); }
    void gain(int tenthDb) { spec_.sendText("{\"type\":\"gain\",\"value\":" + std::to_string(tenthDb) + "}"); }
    void agc(bool on) { spec_.sendText(std::string("{\"type\":\"agc\",\"on\":") + (on ? "true" : "false") + "}"); }

    /** The hardware service, through the supervisor (POST /handheld/device). Off the UI thread. */
    void device(const std::string& bodyJson) {
        std::thread([p = port_, bodyJson] { httpRequest(p, "POST", "/handheld/device", bodyJson, 5); }).detach();
    }

    int port() const { return port_; }

private:
    int port_;
    std::string sid_, bid_;
    std::atomic<bool> run_{false};
    std::thread worker_, events_;
    std::mutex cvm_;
    std::condition_variable cv_;
    WsClient spec_, audio_;
    RadioState st_;
    // session (written by the link worker, read by pump)
    std::mutex sm_;
    bool sessionDirty_ = false;
    std::string sessState_ = "unknown", sessName_, sessDriver_, sessSerial_;
    // device events
    std::mutex em_;
    std::vector<DeviceEvent> evq_;

    static std::string uuid() {
        std::random_device rd;
        std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd());
        uint8_t b[16];
        for (auto& x : b) x = (uint8_t)g();
        b[6] = (b[6] & 0x0F) | 0x40; b[8] = (b[8] & 0x3F) | 0x80;
        char s[40];
        std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return s;
    }
    /** ★ One per DEVICE (the web client's per-browser id): kept in $HOME, made once. */
    static std::string loadBid() {
        const char* home = std::getenv("HOME");
        const std::string path = std::string(home && *home ? home : "/tmp") + "/.vibe-hhui-bid";
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            char b[64] = {0};
            const bool ok = std::fgets(b, sizeof b, f) != nullptr;
            std::fclose(f);
            std::string s(b);
            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (ok && s.size() == 36) return s;
        }
        const std::string id = uuid();
        if (FILE* f = std::fopen(path.c_str(), "w")) { std::fputs(id.c_str(), f); std::fclose(f); }
        return id;
    }

    void sleepFor(int ms) { std::unique_lock<std::mutex> lk(cvm_); cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !run_.load(); }); }

    void pollSession() {
        const HttpResult r = httpRequest(port_, "GET", "/handheld/session", "", 4);
        std::string st = "unknown", name, drv, ser;
        if (r.status == 200) {
            const Json j = Json::parse(r.body);
            if (j["handheld"].boolean()) {
                st = j["state"].str();
                name = j["radio"]["name"].str(); drv = j["radio"]["driver"].str(); ser = j["radio"]["serial"].str();
            }
        }
        std::lock_guard<std::mutex> lk(sm_);
        if (st != sessState_ || name != sessName_ || ser != sessSerial_) {
            sessState_ = st; sessName_ = name; sessDriver_ = drv; sessSerial_ = ser; sessionDirty_ = true;
        }
    }

    void linkLoop() {
        auto lastPing = std::chrono::steady_clock::now();
        while (run_) {
            pollSession();
            std::string state;
            { std::lock_guard<std::mutex> lk(sm_); state = sessState_; }
            if (state != "running") {
                if (spec_.alive()) spec_.close();
                if (audio_.alive()) audio_.close();
                sleepFor(1000);
                continue;
            }
            if (!spec_.alive()) {
                const std::string q = "?user_session_id=" + sid_ + "&bid=" + bid_ + "&proto=1&client=vibe-hhui";
                if (spec_.open(port_, "/ws/user-spectrum" + q + "&mode=binary8&bins=1024")) {
                    // ★ the audio socket of the same session: the engine plays it on the speaker (speaker=1)
                    audio_.close();
                    audio_.open(port_, "/ws/audio" + q + "&speaker=1");
                } else { sleepFor(1000); continue; }
            } else if (!audio_.alive()) {
                const std::string q = "?user_session_id=" + sid_ + "&bid=" + bid_ + "&proto=1&client=vibe-hhui";
                audio_.open(port_, "/ws/audio" + q + "&speaker=1");
            }
            // ★ proto ≥ 1 liveness: an application message on each socket well inside 30 s
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPing > std::chrono::seconds(5)) {
                spec_.sendText("{\"type\":\"ping\"}");
                audio_.sendText("{\"type\":\"ping\"}");
                lastPing = now;
            }
            sleepFor(1000);
        }
    }

    void eventLoop() {
        long since = 0;
        while (run_) {
            const HttpResult r = httpRequest(port_, "GET", "/handheld/events?since=" + std::to_string(since), "", 35);
            if (r.status != 200) { sleepFor(1500); continue; }
            const Json j = Json::parse(r.body);
            const long seq = (long)j["seq"].num(0);
            if (seq < since) since = 0;                // ★ the engine restarted: its numbering began again
            std::vector<DeviceEvent> got;
            for (auto& e : j["events"].a) got.push_back({e["event"].str(), e});
            { std::lock_guard<std::mutex> lk(em_); for (auto& e : got) evq_.push_back(std::move(e)); }
            since = std::max(since, seq);
        }
    }

    void specFrame(const std::string& d, const std::function<void(const uint8_t*, int, double, double)>& onSpec) {
        if (d.size() < 22) return;
        const uint8_t* p = (const uint8_t*)d.data();
        if (!(p[0] == 'S' && p[1] == 'P' && p[2] == 'E' && p[3] == 'C')) return;   // SPWB etc.: not asked for (no wide=1)
        if (p[4] != 1 || p[5] != 0x03) return;
        uint64_t ts = 0, c = 0;
        for (int i = 7; i >= 0; i--) { ts = ts << 8 | p[6 + i]; c = c << 8 | p[14 + i]; }
        const int n = (int)(d.size() - 22);
        if (n <= 0) return;
        // ★ FFT order (DC first) → low to high: rotate by N/2
        thread_local std::vector<uint8_t> bins;
        bins.resize((size_t)n);
        for (int i = 0; i < n; i++) bins[(size_t)i] = p[22 + (i + n / 2) % n];
        onSpec(bins.data(), n, (double)c, (double)ts * 1e-9);
    }

    void onText(const std::string& s) {
        const Json j = Json::parse(s);
        const std::string& t = j["type"].str();
        if (t == "config") {
            st_.viewCentreHz = j["centerFreq"].num(st_.viewCentreHz);
            st_.binCount = (int)j["binCount"].num(st_.binCount);
            st_.binBwHz = j["binBandwidth"].num(st_.binBwHz);
            st_.spanHz = j["totalBandwidth"].num(st_.spanHz);
            st_.maxSpanHz = j["maxBandwidth"].num(st_.maxSpanHz);
            st_.vfoHz = j["vfo"].num(st_.vfoHz);
            st_.locked = j["locked"].boolean(); st_.shared = j["shared"].boolean();
            const std::string m = j["mode"].str();
            if (!m.empty() && m != st_.mode) { st_.mode = m; modePassband(m, st_.bwLow, st_.bwHigh); }
            if (j.has("gainNow")) st_.gainNow = (int)j["gainNow"].num();
            if (j.has("agc")) st_.agc = j["agc"].boolean();
            st_.configSeq++;
        } else if (t == "hwinfo") {
            if (j.has("gains")) { st_.gains.clear(); for (auto& g : j["gains"].a) st_.gains.push_back((int)g.num()); }
            if (j.has("gainNow")) st_.gainNow = (int)j["gainNow"].num();
            if (j.has("agc")) st_.agc = j["agc"].boolean();
            if (j.has("rates")) { st_.rates.clear(); for (auto& r : j["rates"].a) st_.rates.push_back(r.num()); }
            if (j.has("rateNow")) st_.rateNow = j["rateNow"].num();
            if (j.has("blocked")) { st_.blockedModes.clear(); for (auto& b : j["blocked"].a) st_.blockedModes.push_back(b.str()); }
            if (j["radio"].has("model") && st_.radioName.empty()) st_.radioName = j["radio"]["model"].str();
        } else if (t == "sig") {
            st_.chanDb = j["chan"].num(st_.chanDb); st_.floorDb = j["floor"].num(st_.floorDb);
        } else if (t == "rds") {
            st_.rdsPs = j["ps"].str(); st_.rdsRt = j["radiotext"].str(); st_.rdsStereo = j["stereo"].boolean();
        }
    }
};

}  // namespace hh
