// icom_source.cpp — see icom_source.h. ★ The safety properties live in three places, and this file
// must keep all three true: sendFrame() is the ONLY write; open() refuses on a bad preflight; and
// nothing here builds a frame except through civ::build.
#include "icom_source.h"
#include "vibe_thread.h"
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#  include <android/log.h>
#  define ILOG(...) __android_log_print(ANDROID_LOG_INFO, "VibeIcom", __VA_ARGS__)
#else
#  define ILOG(...) do { std::fprintf(stderr, "[VibeIcom] "); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif

namespace icom {

double IcomSource::now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

IcomSource::~IcomSource() { close(); }

civ::Policy IcomSource::policyFor(const Model& m, uint8_t addr) {
    civ::Policy p;
    p.radio = addr;
    p.nRanges = 0;
    for (int i = 0; i < m.nRanges && i < 8; i++) p.ranges[p.nRanges++] = {m.ranges[i].lo, m.ranges[i].hi};
    p.nModes = 0;
    for (int i = 0; i < m.nModes; i++)
        if (!civ::inList(p.modes, p.nModes, m.modeBytes[i]) && p.nModes < 16) p.modes[p.nModes++] = m.modeBytes[i];
    p.nAtt = 0;
    for (int i = 0; i < m.nAtt && i < 4; i++) p.att[p.nAtt++] = m.att[i];
    p.preampMax = m.preampMax;
    p.dataModeMax = m.dataModeMax;
    p.nSpans = 0;
    for (int i = 0; i < m.nSpans && i < 12; i++) p.spansHalfHz[p.nSpans++] = m.spansHalfHz[i];
    p.nMenuItems = 0;
    for (int i = 0; i < m.nMenu && i < 12; i++) {
        p.menuItems[p.nMenuItems][0] = m.menu[i].item[0];
        p.menuItems[p.nMenuItems][1] = m.menu[i].item[1];
        p.nMenuItems++;
    }
    p.hasScopeSelector = true;
    return p;
}

double IcomSource::sRawToDbOverS9(int raw) {
    // ★ Hamlib's IC7300_STR_CAL points (a FACT, cited — plan §4): raw → dB relative to S9.
    static const int kx[] = {0, 10, 30, 60, 90, 120, 241};
    static const int ky[] = {-54, -48, -36, -24, -12, 0, 64};
    if (raw <= 0) return ky[0];
    for (int i = 1; i < 7; i++)
        if (raw <= kx[i]) return ky[i - 1] + (double)(raw - kx[i - 1]) * (ky[i] - ky[i - 1]) / (double)(kx[i] - kx[i - 1]);
    return ky[6];
}

// ── The ONE write path ─────────────────────────────────────────────────────────────────────

bool IcomSource::sendFrame(const civ::Out& o) {
    // ★★★ THE LAST FUNCTION BEFORE THE WIRE. Whatever built `o`, this is where it is judged — the
    //     whole frame, against the table, every time. A refusal is a FAULT in our code (the builders
    //     should never produce one for a request the controls accepted), so it is counted and logged
    //     loudly, and the frame is gone.
    const civ::Verdict v = civ::mayTransmit(o, policy_);
    if (v != civ::Verdict::Ok) {
        refused_.fetch_add(1);
        char hex[80] = {0}; int at = 0;
        for (int i = 0; i < o.n && at < 74; i++) at += std::snprintf(hex + at, sizeof hex - at, "%02X ", o.b[i]);
        ILOG("REFUSED a CI-V frame (%s): %s — not sent", civ::verdictName(v), hex);
        return false;
    }
    return transport_ && transport_->write(o.b, o.n);
}

bool IcomSource::transact(const civ::Out& o, uint8_t wantCmd, int wantSub, int timeoutMs, civ::Frame* reply) {
    if (!sendFrame(o)) return false;
    const double until = now() + timeoutMs / 1000.0;
    uint8_t buf[256];
    bool got = false, ok = false;
    while (!got && now() < until) {
        const int n = transport_->read(buf, sizeof buf, 20);
        if (n < 0) return false;
        if (n > 0) lastByteAt_ = now();
        parser_.feed(buf, (size_t)n, [&](const civ::Frame& f) {
            if (got || f.from != policy_.radio) return;          // our echo, or another device on the bus
            if (f.to != civ::kController && f.to != civ::kBroadcast) return;
            if (f.cmd == civ::kNg) { got = true; ok = false; return; }
            if (f.cmd == civ::kOk && wantCmd == civ::kOk) { got = true; ok = true; return; }
            if (f.cmd != wantCmd) return;                        // scope data, transceive: not our reply
            if (wantSub >= 0 && (f.len < 1 || f.data[0] != wantSub)) return;
            if (reply) *reply = f;
            got = true; ok = true;
        });
    }
    return ok;
}

// ── Open ───────────────────────────────────────────────────────────────────────────────────

bool IcomSource::identify(std::string& err) {
    // ★ Before we know the model, the frames we may send are 19 00 and 03 — which every Icom answers,
    //   and which are reads. The policy here is the hinted (or 7300) model's: the shapes are identical.
    civ::Frame r;
    if (!transact(civ::build::readId(policy_.radio), civ::cmd::Id, 0x00, 600, &r) || r.len < 2) {
        if (!transact(civ::build::readFreq(policy_.radio), civ::cmd::ReadFreq, -1, 600, &r)) {
            char a[8]; std::snprintf(a, sizeof a, "%02X", policy_.radio);
            err = "The radio did not answer on CI-V (address " + std::string(a) + ", " + std::to_string(cfg_.baud)
                + " baud). On the radio check MENU > SET > Connectors > CI-V: CI-V USB Baud Rate must be "
                + std::to_string(cfg_.baud) + " (or Auto), CI-V USB Port must be \"Unlink from [REMOTE]\", and the "
                  "CI-V Address must match the one set in VibeServer.";
            return false;
        }
        // Answered 03 but not 19 00: trust the address's model (older firmware) [unverified].
        r.len = 2; r.data[0] = 0x00; r.data[1] = policy_.radio;
    }
    const Model* m = modelByAddr(r.data[1]);
    char id[8]; std::snprintf(id, sizeof id, "%02X", r.data[1]);
    if (!m) { err = "This radio identifies as CI-V model " + std::string(id) + ", which VibeServer does not know."; return false; }
    if (!m->known) {
        err = std::string("This is an ") + m->name + ", which VibeServer does not support yet (only the IC-7300 has been "
              "checked). Nothing was sent to it except an identity read.";
        return false;
    }
    model_ = m;
    policy_ = policyFor(*m, policy_.radio);
    return true;
}

bool IcomSource::runPreflight(std::string& err) {
    // ★★★ PLAN §3.3 — READ ONLY, EVERY CONNECT. If the radio's own menu can key it from a modem line
    //     or from audio, we do not start. We never change the setting ourselves: the owner does, and
    //     this names exactly which item.
    pre_ = Preflight{};
    pre_.ran = true;
    pre_.verified = model_->menuVerified;
    bool pass = true;
    std::string bad;
    for (int i = 0; i < model_->nMenu; i++) {
        const MenuItem& mi = model_->menu[i];
        PreflightItem it; it.name = mi.name; it.where = mi.path; it.gate = mi.gate;
        civ::Frame r;
        if (transact(civ::build::readMenu(policy_.radio, mi.item), civ::cmd::Misc, 0x05, 500, &r)
                && r.len >= 4 && r.data[1] == mi.item[0] && r.data[2] == mi.item[1]) {
            const uint8_t v = r.data[3];
            if (mi.gate) it.value = v == 0 ? "OFF" : v == 1 ? "DTR" : v == 2 ? "RTS" : "set (" + std::to_string(v) + ")";
            else { char b[8]; std::snprintf(b, sizeof b, "%u", v); it.value = b; }
            it.ok = !mi.gate || v == 0;
        } else {
            it.value = "not readable";
            it.ok = !mi.gate;      // ★ fail CLOSED: a keying item we cannot read is a keying item we cannot trust
        }
        if (!it.ok) { pass = false; bad += (bad.empty() ? "" : "; ") + it.name + " is " + it.value + " (" + it.where + ")"; }
        pre_.items.push_back(it);
    }
    {   // ★ VOX by its own command, not a menu number — the audio path to the transmitter.
        PreflightItem it; it.name = "VOX"; it.where = "the VOX key on the radio (or MENU > … > VOX)"; it.gate = true;
        civ::Frame r;
        if (transact(civ::build::readFunc(policy_.radio, 0x46), civ::cmd::Func, 0x46, 500, &r) && r.len >= 2) {
            it.value = r.data[1] ? "ON" : "OFF"; it.ok = r.data[1] == 0;
        } else { it.value = "not readable"; it.ok = false; }
        if (!it.ok) { pass = false; bad += (bad.empty() ? "" : "; ") + std::string("VOX is ") + it.value; }
        pre_.items.push_back(it);
    }
    pre_.passed = pass;
    if (pass) {
        pre_.summary = "Safe to receive: nothing on this radio can key the transmitter from VibeServer's connection "
                       "(USB SEND, USB Keying and VOX are all OFF).";
        if (!pre_.verified) pre_.summary += " (These menu items have not yet been confirmed on a real " + std::string(model_->name) + ".)";
        return true;
    }
    pre_.summary = "VibeServer will not start this radio, because it could transmit: " + bad
                 + ". Set each to OFF on the radio, then restart the radio from setup. VibeServer never changes these itself.";
    err = pre_.summary;
    return false;
}

void IcomSource::readInitialState() {
    civ::Frame r; unsigned ch = 0;
    auto rd = [&](const civ::Out& o, uint8_t c, int s) { if (transact(o, c, s, 300, &r)) onFrame(r, ch); };
    rd(civ::build::readFreq(policy_.radio), civ::cmd::ReadFreq, -1);
    rd(civ::build::readDataMode(policy_.radio), civ::cmd::Misc, 0x06);
    rd(civ::build::readMode(policy_.radio), civ::cmd::ReadMode, -1);
    rd(civ::build::readAtt(policy_.radio), civ::cmd::Att, -1);
    rd(civ::build::readFunc(policy_.radio, 0x02), civ::cmd::Func, 0x02);
    rd(civ::build::readFunc(policy_.radio, 0x12), civ::cmd::Func, 0x12);
    rd(civ::build::readFunc(policy_.radio, 0x22), civ::cmd::Func, 0x22);
    rd(civ::build::readFunc(policy_.radio, 0x40), civ::cmd::Func, 0x40);
    rd(civ::build::readFunc(policy_.radio, 0x41), civ::cmd::Func, 0x41);
    rd(civ::build::readLevel(policy_.radio, 0x02), civ::cmd::Level, 0x02);
    rd(civ::build::readTx(policy_.radio), civ::cmd::Tx, 0x00);
    rd(civ::build::readScope(policy_.radio, 0x14), civ::cmd::Scope, 0x14);
    rd(civ::build::readScope(policy_.radio, 0x15), civ::cmd::Scope, 0x15);
    // ★ Scope ON and its data out ON — without 27 11 nothing is streamed (plan §1.2).
    transact(civ::build::scopeSpeed(policy_.radio, (uint8_t)(cfg_.scopeSpeed < 0 ? 0 : cfg_.scopeSpeed > 2 ? 2 : cfg_.scopeSpeed)), civ::kOk, -1, 300, nullptr);
    transact(civ::build::scopeOn(policy_.radio, true), civ::kOk, -1, 300, nullptr);
    transact(civ::build::scopeData(policy_.radio, true), civ::kOk, -1, 300, nullptr);
}

bool IcomSource::open(const IcomConfig& cfg, std::string& err, std::unique_ptr<CivTransport> injected) {
    close();
    cfg_ = cfg;
    const Model* hint = modelBySerial(cfg.modelHint.c_str());
    int n = 0; const Model* all = models(n);
    const Model& base = hint ? *hint : all[0];
    policy_ = policyFor(base, (uint8_t)(cfg.civAddr > 0 ? cfg.civAddr : base.addr));
    model_ = &base;
    transport_ = injected ? std::move(injected) : makeTransport(cfg.port, cfg.baud, cfg.ttyOk, err);
    if (!transport_) return false;
    if (!transport_->open(err)) { transport_.reset(); return false; }
    parser_.reset(); scope_.reset();
    lastByteAt_ = now();
    if (!identify(err) || !runPreflight(err)) { transport_->close(); transport_.reset(); return false; }
    readInitialState();
    { std::lock_guard<std::mutex> lk(stMtx_); st_.linkUp = true; }
    // ★ Audio is a separate device: missing audio leaves a working dial and waterfall, and says so.
    audioErr_.clear();
    audio_ = makeAudio(cfg.audio, transportUsbPath(transport_.get()), audioErr_);
    if (audio_) {
        std::string ae;
        if (!audio_->start([this](const int16_t* p, int frames, int ch, int rate) {
                if (!onPcm_) return;
                // ★★ MUTED WHILE THE OWNER TRANSMITS, and for 300 ms after — the codec carries the
                //    radio's TX monitor, and the tail of the unkey with it (plan §3.4).
                if (tx_.load(std::memory_order_relaxed) || now() < txHoldUntil_.load(std::memory_order_relaxed)) {
                    static thread_local int16_t z[8192 * 2];
                    const int k = frames * ch > 8192 * 2 ? 8192 * 2 : frames * ch;
                    std::memset(z, 0, sizeof(int16_t) * (size_t)k);
                    onPcm_(z, k / ch, ch, rate);
                    return;
                }
                onPcm_(p, frames, ch, rate);
            }, ae)) { audioErr_ = ae; audio_.reset(); }
    }
    if (!audioErr_.empty()) ILOG("audio: %s", audioErr_.c_str());
    run_.store(true);
    io_ = std::thread([this] { ioLoop(); });
    ILOG("%s open: CI-V %02X at %d baud over %s%s, scope %d points, preflight %s",
         model_->name, policy_.radio, cfg_.baud, transport_->kind(),
         transport_->linesHeldLow() ? " (DTR/RTS held low)" : " (tty)", model_->points,
         pre_.passed ? "passed" : "FAILED");
    return true;
}

void IcomSource::close() {
    const bool wasRunning = run_.exchange(false);
    if (io_.joinable()) io_.join();
    if (audio_) { audio_->stop(); audio_.reset(); }
    if (transport_) {
        // ★ Leave the radio as we found it as far as we can: stop the scope streaming at it. The
        //   scope's own on/off is the owner's; we only switch off the data OUTPUT we asked for.
        if (wasRunning) transact(civ::build::scopeData(policy_.radio, false), civ::kOk, -1, 200, nullptr);
        transport_->close();
        transport_.reset();
    }
    std::lock_guard<std::mutex> lk(stMtx_);
    st_.linkUp = false;
}

bool IcomSource::reconnect() {
    if (!transport_) return false;
    transport_->close();
    std::string e;
    if (!transport_->open(e)) return false;
    parser_.reset(); scope_.reset();
    lastByteAt_ = now();
    // ★★ THE PREFLIGHT AGAIN — a radio that went away may come back with its menu changed.
    if (!identify(e) || !runPreflight(e)) {
        ILOG("reconnect refused: %s", e.c_str());
        transport_->close();
        if (onLost_) onLost_(e);
        return false;
    }
    readInitialState();
    { std::lock_guard<std::mutex> lk(stMtx_); st_.linkUp = true; }
    ILOG("%s back on CI-V", model_->name);
    notify(kLink | kFreq | kMode | kRf | kScope);
    return true;
}

// ── The queue ──────────────────────────────────────────────────────────────────────────────

bool IcomSource::enqueue(const civ::Out& o, bool high, std::string& why) {
    // ★ Judged NOW, so a refusal reaches the person who asked, in words — and judged again at the
    //   wire (sendFrame), which is the check that matters.
    const civ::Verdict v = civ::mayTransmit(o, policy_);
    if (v != civ::Verdict::Ok) {
        why = v == civ::Verdict::OutOfRange ? "that frequency is outside this radio's receive range"
                                            : std::string("the radio does not take that setting (") + civ::verdictName(v) + ")";
        return false;
    }
    const uint8_t c = o.b[4];
    const int sub = (c == civ::cmd::SetFreq || c == civ::cmd::SetMode || c == civ::cmd::Att
                     || c == civ::cmd::ReadFreq || c == civ::cmd::ReadMode) ? -1 : (o.n > 6 ? o.b[5] : -1);
    std::lock_guard<std::mutex> lk(qMtx_);
    Req* lane = high ? hi_ : lo_;
    // ★ COALESCED: a newer request for the same thing REPLACES the queued one. A held tune key or a
    //   dragged slider becomes "the latest value", never a backlog the radio works through for seconds.
    for (int i = 0; i < kLane; i++)
        if (lane[i].used && lane[i].cmd == c && lane[i].sub == sub) { lane[i].out = o; return true; }
    for (int i = 0; i < kLane; i++)
        if (!lane[i].used) { lane[i].out = o; lane[i].cmd = c; lane[i].sub = sub; lane[i].used = true; return true; }
    why = "the radio is busy — try again";
    return false;
}

bool IcomSource::takeNext(Req& r) {
    std::lock_guard<std::mutex> lk(qMtx_);
    for (Req* lane : {hi_, lo_})
        for (int i = 0; i < kLane; i++)
            if (lane[i].used) {
                r = lane[i];
                // keep FIFO order within the lane
                for (int k = i; k < kLane - 1; k++) lane[k] = lane[k + 1];
                lane[kLane - 1].used = false;
                return true;
            }
    return false;
}

// ── The CI-V thread ────────────────────────────────────────────────────────────────────────

void IcomSource::ioLoop() {
    // ★ SPECTRUM priority: this thread is the waterfall. Audio has its own (higher) thread, and the
    //   network sends are higher still — a slow CI-V link must never hold up either.
    vibeSpectrumThread("vibe-civ");
    uint8_t buf[512];
    double tMeter = 0, tTx = 0, tFreq = 0, tRf = 0, tRetry = 0;
    int rfIdx = 0;
    std::string ignore;
    while (run_.load()) {
        const double t = now();
        bool up;
        { std::lock_guard<std::mutex> lk(stMtx_); up = st_.linkUp; }
        if (!up) {
            if (t >= tRetry) { tRetry = t + 3.0; if (reconnect()) continue; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // Polls — on the LOW lane, coalesced, so at 19200 baud they slow down rather than pile up.
        if (t >= tMeter) { enqueue(civ::build::readSmeter(policy_.radio), false, ignore); tMeter = t + 0.25; }
        if (t >= tTx)    { enqueue(civ::build::readTx(policy_.radio), false, ignore); tTx = t + 0.5; }
        if (t >= tFreq)  {   // ★ catches the knob even with CI-V Transceive OFF
            enqueue(civ::build::readFreq(policy_.radio), false, ignore);
            enqueue(civ::build::readDataMode(policy_.radio), false, ignore);
            enqueue(civ::build::readMode(policy_.radio), false, ignore);
            tFreq = t + 1.0;
        }
        if (t >= tRf) {
            static const uint8_t f[] = {0x02, 0x12, 0x22, 0x40, 0x41};
            const int k = rfIdx++ % 7;
            if (k < 5) enqueue(civ::build::readFunc(policy_.radio, f[k]), false, ignore);
            else if (k == 5) enqueue(civ::build::readAtt(policy_.radio), false, ignore);
            else enqueue(civ::build::readLevel(policy_.radio, 0x02), false, ignore);
            tRf = t + 0.7;
        }
        // One request in flight: the radio answers in order, and a reply we cannot attribute is useless.
        if (!inflight_.used || t - inflightAt_ > 0.25) {
            inflight_.used = false;
            Req r;
            if (takeNext(r)) { if (sendFrame(r.out)) { inflight_ = r; inflightAt_ = t; } }
        }
        const int n = transport_->read(buf, sizeof buf, 10);
        if (n < 0 || t - lastByteAt_ > 3.0) {
            const std::string why = n < 0 ? "the radio's USB connection was lost" : "the radio stopped answering CI-V";
            ILOG("%s — will keep trying", why.c_str());
            { std::lock_guard<std::mutex> lk(stMtx_); st_.linkUp = false; }
            inflight_.used = false;
            tRetry = t + 3.0;
            if (onLost_) onLost_(why);
            notify(kLink);
            continue;
        }
        unsigned changed = 0;
        if (n > 0) {
            lastByteAt_ = t;
            parser_.feed(buf, (size_t)n, [&](const civ::Frame& f) { onFrame(f, changed); });
        }
        if (audio_ && audio_->lost() && audioErr_.empty()) { audioErr_ = "the radio's audio device went away"; changed |= kLink; }
        notify(changed);
    }
}

void IcomSource::onFrame(const civ::Frame& f, unsigned& changed) {
    if (f.from != policy_.radio) return;                                   // ★ our own echo (from E0), or another device
    if (f.to != civ::kController && f.to != civ::kBroadcast) return;
    const uint8_t* d = f.data;
    const int len = f.len;
    // ★ Is this the answer to the request in flight? Then the next one may go.
    auto answers = [&]() {
        if (!inflight_.used) return false;
        if (f.cmd == civ::kOk || f.cmd == civ::kNg) return true;
        if (f.cmd != inflight_.cmd) return false;
        return inflight_.sub < 0 || (len >= 1 && d[0] == inflight_.sub);
    };
    const bool done = answers();
    std::unique_lock<std::mutex> lk(stMtx_);
    auto setI = [&](int& field, int v, unsigned bit) { if (field != v) { field = v; changed |= bit; } };
    auto setB = [&](bool& field, bool v, unsigned bit) { if (field != v) { field = v; changed |= bit; } };
    switch (f.cmd) {
    case civ::kOk: {
        // ★ The radio accepted a write: adopt what we asked for at once (the poll confirms it later) —
    //   so the dial moves the moment the radio says yes. Read from the request itself, never guessed.
        if (done) {
            const civ::Out& o = inflight_.out;
            const uint8_t c = o.b[4];
            const uint8_t* v = o.b + 5;
            if (c == civ::cmd::SetFreq) { const int64_t hz = civ::bcdToFreq(v, 5); if (hz > 0 && (uint64_t)hz != st_.freq) { st_.freq = (uint64_t)hz; changed |= kFreq; } }
            else if (c == civ::cmd::Att && o.n == 7) setI(st_.att, civ::bcdByte(v[0]), kRf);
            else if (c == civ::cmd::Func && o.n == 8) {
                switch (v[0]) { case 0x02: setI(st_.preamp, v[1], kRf); break; case 0x12: setI(st_.agc, v[1], kRf); break;
                                case 0x22: setB(st_.nb, v[1] != 0, kRf); break; case 0x40: setB(st_.nr, v[1] != 0, kRf); break;
                                case 0x41: setB(st_.anf, v[1] != 0, kRf); break; }
            } else if (c == civ::cmd::Level && o.n == 9 && v[0] == 0x02) setI(st_.rfGain, civ::bcdBigToInt(v + 1, 2), kRf);
            else if (c == civ::cmd::Scope && v[0] == 0x15 && o.n == 13) { const int64_t h = civ::bcdToFreq(v + 2, 5); if (h > 0 && (uint32_t)h != st_.spanHalfHz) { st_.spanHalfHz = (uint32_t)h; changed |= kScope; } }
            else if (c == civ::cmd::Scope && v[0] == 0x14 && o.n == 9) setI(st_.scopeMode, v[2], kScope);
        }
        break;
    }
    case civ::kNg:
        if (done) ILOG("the radio said NG to %02X", inflight_.cmd);
        break;
    case civ::cmd::TransceiveFreq: case civ::cmd::ReadFreq:
        if (len >= 5) { const int64_t hz = civ::bcdToFreq(d, 5); if (hz > 0 && (uint64_t)hz != st_.freq) { st_.freq = (uint64_t)hz; changed |= kFreq; } }
        break;
    case civ::cmd::TransceiveMode: case civ::cmd::ReadMode:
        if (len >= 1) {
            modeByte_ = d[0];
            const std::string m = modeName(*model_, d[0], dataOn_);
            if (m != st_.mode) { st_.mode = m; changed |= kMode; }
            if (len >= 2) setI(st_.fil, d[1], kMode);
        }
        break;
    case civ::cmd::Misc:
        if (len >= 2 && d[0] == 0x06) {
            dataOn_ = d[1] != 0;
            const std::string m = modeName(*model_, modeByte_, dataOn_);
            if (!m.empty() && m != st_.mode) { st_.mode = m; changed |= kMode; }
        }
        break;
    case civ::cmd::Meter:
        if (len >= 3 && d[0] == 0x02) setI(st_.sRaw, civ::bcdBigToInt(d + 1, 2), kMeter);
        break;
    case civ::cmd::Tx:
        if (len >= 2 && d[0] == 0x00) {
            const bool tx = d[1] != 0;
            if (tx != st_.tx) {
                st_.tx = tx; changed |= kTx;
                tx_.store(tx);
                if (!tx) txHoldUntil_.store(now() + 0.3);
                ILOG("the owner is %s", tx ? "TRANSMITTING — listeners muted" : "receiving again");
            }
        }
        break;
    case civ::cmd::Att:
        if (len >= 1) setI(st_.att, civ::bcdByte(d[0]), kRf);
        break;
    case civ::cmd::Func:
        if (len >= 2) switch (d[0]) {
            case 0x02: setI(st_.preamp, d[1], kRf); break;
            case 0x12: setI(st_.agc, d[1], kRf); break;
            case 0x22: setB(st_.nb, d[1] != 0, kRf); break;
            case 0x40: setB(st_.nr, d[1] != 0, kRf); break;
            case 0x41: setB(st_.anf, d[1] != 0, kRf); break;
        }
        break;
    case civ::cmd::Level:
        if (len >= 3 && d[0] == 0x02) setI(st_.rfGain, civ::bcdBigToInt(d + 1, 2), kRf);
        if (len >= 3 && d[0] == 0x03) setI(st_.squelch, civ::bcdBigToInt(d + 1, 2), kRf);
        break;
    case civ::cmd::Scope:
        if (len >= 1 && d[0] == 0x00) {
            if (scope_.feed(d, len)) {
                const Sweep& s = scope_.out();
                const int sm = (s.mode == 1 || s.mode == 3) ? 1 : 0;
                setI(st_.scopeMode, sm, kScope);
                if (sm == 0) {
                    const uint32_t half = (uint32_t)((s.hiHz - s.loHz) / 2);
                    if (half != st_.spanHalfHz) { st_.spanHalfHz = half; changed |= kScope; }
                }
                const double t = now();
                if (rateT0_ <= 0) rateT0_ = t;
                if (++rateN_ >= 20 || t - rateT0_ > 5.0) {
                    st_.sweepsPerSec = rateN_ / std::max(0.001, t - rateT0_);
                    rateN_ = 0; rateT0_ = t;
                }
                // ★★ FROZEN WHILE THE OWNER TRANSMITS: a TX sweep is his own signal, not the band.
                if (!st_.tx && onSweep_) {
                    lk.unlock();              // ★ never call out holding the state lock
                    onSweep_(s);
                    lk.lock();
                }
            }
        } else if (len >= 3 && d[0] == 0x14) setI(st_.scopeMode, d[2] & 1, kScope);
        else if (len >= 7 && d[0] == 0x15) {
            const int64_t h = civ::bcdToFreq(d + 2, 5);
            if (h > 0 && (uint32_t)h != st_.spanHalfHz) { st_.spanHalfHz = (uint32_t)h; changed |= kScope; }
        }
        break;
    }
    if (done) inflight_.used = false;
}

// ── Controls ───────────────────────────────────────────────────────────────────────────────

bool IcomSource::tune(uint64_t hz, std::string& why) {
    return enqueue(civ::build::setFreq(policy_.radio, hz), true, why);
}
bool IcomSource::setMode(const std::string& m, int fil, std::string& why) {
    const int i = modeIndex(*model_, m.c_str());
    if (i < 0) { why = "the " + std::string(model_->name) + " has no " + m + " mode"; return false; }
    const bool data = m.size() == 4 && m[3] == 'd';
    int f = fil;
    if (f <= 0) { std::lock_guard<std::mutex> lk(stMtx_); f = st_.fil > 0 ? st_.fil : 1; }
    if (!enqueue(civ::build::setMode(policy_.radio, model_->modeBytes[i], (uint8_t)f), true, why)) return false;
    // ★ DATA is a separate switch on an Icom (USB-D is USB + 1A 06 01). Off means 00 00.
    return enqueue(civ::build::setDataMode(policy_.radio, data ? 1 : 0, data ? (uint8_t)f : 0), true, why);
}
bool IcomSource::setFilter(int fil, std::string& why) {
    std::string m; { std::lock_guard<std::mutex> lk(stMtx_); m = st_.mode; }
    if (m.empty()) { why = "the radio's mode is not known yet"; return false; }
    if (fil < 1 || fil > 3) { why = "the filter is FIL1, FIL2 or FIL3"; return false; }
    return setMode(m, fil, why);
}
bool IcomSource::setAtt(int db, std::string& why) {
    if (db < 0 || db > 99) { why = "no such attenuator setting"; return false; }
    return enqueue(civ::build::setAtt(policy_.radio, civ::toBcdByte(db)), true, why);
}
bool IcomSource::setPreamp(int v, std::string& why) { return enqueue(civ::build::setFunc(policy_.radio, 0x02, (uint8_t)(v < 0 ? 0xFF : v)), true, why); }
bool IcomSource::setAgc(int v, std::string& why)    { return enqueue(civ::build::setFunc(policy_.radio, 0x12, (uint8_t)(v < 0 ? 0xFF : v)), true, why); }
bool IcomSource::setRfGain(int v, std::string& why) {
    if (v < 0 || v > 255) { why = "RF gain runs 0-255"; return false; }
    return enqueue(civ::build::setLevel(policy_.radio, 0x02, v), true, why);
}
bool IcomSource::setNb(bool on, std::string& why)  { return enqueue(civ::build::setFunc(policy_.radio, 0x22, on ? 1 : 0), true, why); }
bool IcomSource::setNr(bool on, std::string& why)  { return enqueue(civ::build::setFunc(policy_.radio, 0x40, on ? 1 : 0), true, why); }
bool IcomSource::setAnf(bool on, std::string& why) { return enqueue(civ::build::setFunc(policy_.radio, 0x41, on ? 1 : 0), true, why); }
bool IcomSource::setSpanTotal(uint32_t total, std::string& why) {
    return enqueue(civ::build::scopeSpan(policy_.radio, total / 2), true, why);
}
bool IcomSource::setScopeMode(int m, std::string& why) {
    if (m != 0 && m != 1) { why = "the scope is centre (0) or fixed (1)"; return false; }
    return enqueue(civ::build::scopeMode(policy_.radio, (uint8_t)m), true, why);
}
bool IcomSource::setScopeSpeed(int s, std::string& why) {
    if (s < 0 || s > 2) { why = "scope speed is fast, mid or slow"; return false; }
    return enqueue(civ::build::scopeSpeed(policy_.radio, (uint8_t)s), true, why);
}

RadioState IcomSource::state() const { std::lock_guard<std::mutex> lk(stMtx_); return st_; }
std::string IcomSource::audioDescription() const { return audio_ ? audio_->describe() : std::string(); }

}  // namespace icom
