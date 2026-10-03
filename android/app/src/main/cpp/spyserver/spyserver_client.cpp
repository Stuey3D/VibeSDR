// VibeSDR — SpyServer-compatible CLIENT transport. See spyserver_client.h.
#include "spyserver_client.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "spyserver_messages.h"

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "VibeSpyClient", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VibeSpyClient", __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
#define LOGE(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
#endif

namespace vibe::spyserver {

// A stream message can be large (int16 IQ at low decimation), so give the
// accumulator room to hold one whole message plus a partial next one.
static constexpr size_t kMaxMessageBytes = 4 * 1024 * 1024;
static constexpr int    kRecvTimeoutMs   = 300;

SpyServerClient::~SpyServerClient() { close(); }

void SpyServerClient::close() {
    if (sock_) { sock_->close(); sock_ = nullptr; }
    rx_.clear();
}

bool SpyServerClient::sendCommand(const std::vector<uint8_t>& bytes) {
    if (!sock_ || !sock_->isOpen()) return false;
    return sock_->send(bytes.data(), bytes.size()) == (int)bytes.size();
}

// Pull bytes until rx_ holds one complete message. Returns false on link death or
// when `running` is dropped by the caller (timeout returns true with msgLen 0).
bool SpyServerClient::readMessage(std::vector<uint8_t>& buf, size_t& msgLen, int timeoutMs) {
    msgLen = 0;
    for (;;) {
        Message_t m;
        size_t consumed = 0;
        if (parseMessage(rx_.data(), rx_.size(), &m, &consumed)) {
            buf.assign(rx_.begin(), rx_.begin() + consumed);
            rx_.erase(rx_.begin(), rx_.begin() + consumed);
            msgLen = consumed;
            return true;
        }
        /* ★ A header that already declares an impossible body is a desync now, not after 4 MB
         *  more has been read to prove it (audit 2026-10-03 — see kMaxBodySize). */
        if (rx_.size() >= 20 && getU32(rx_.data() + 16) > kMaxBodySize) {
            LOGE("message declares a %u-byte body — dropping link", (unsigned)getU32(rx_.data() + 16));
            return false;
        }
        if (rx_.size() > kMaxMessageBytes) {   // desync guard: never grow unbounded
            LOGE("rx buffer overflow (%zu bytes) — dropping link", rx_.size());
            return false;
        }
        uint8_t chunk[65536];
        int got = sock_->recv(chunk, sizeof(chunk), /*forceLen=*/false, timeoutMs);
        if (got < 0) return false;
        if (got == 0) {
            if (!sock_->isOpen()) return false;
            return true;                        // timeout, nothing yet
        }
        rx_.insert(rx_.end(), chunk, chunk + got);
    }
}

bool SpyServerClient::connect(const std::string& host, int port,
                              const std::string& clientName, std::string& err) {
    // 6 s: a public server across the internet needs more than a LAN hop, but the
    // caller holds a lifecycle lock, so an unreachable host must not wedge the app.
    try { sock_ = net::connect(host, port, /*timeoutMs=*/6000); }
    catch (...) { sock_ = nullptr; }
    if (!sock_) {
        err = "could not reach " + host + ":" + std::to_string(port) +
              " — the server may be offline or unreachable from this network";
        return false;
    }
    sock_->setRecvBufferSize(1024 * 1024);   // ride out WiFi stalls, as rtl_tcp does

    if (!sendCommand(encodeHello(kProtocolVersion, clientName))) {
        err = "SpyServer hello failed"; close(); return false;
    }

    // Wait for DEVICE_INFO + CLIENT_SYNC. Both arrive immediately on a healthy
    // server. A closed socket here means "no device" OR "another client holds the
    // only slot" — the protocol gives us no way to tell, so don't pretend.
    bool haveInfo = false, haveSync = false;
    for (int tries = 0; tries < 20 && !(haveInfo && haveSync); ++tries) {
        std::vector<uint8_t> buf;
        size_t len = 0;
        if (!readMessage(buf, len, 500)) {
            err = "SpyServer closed the connection during handshake — the server "
                  "has no radio attached, or it already has its one permitted client";
            close(); return false;
        }
        if (len == 0) continue;               // timeout slice
        Message_t m; size_t used = 0;
        if (!parseMessage(buf.data(), buf.size(), &m, &used)) continue;
        if (m.type == MSG_DEVICE_INFO) haveInfo = parseDeviceInfo(m.body, m.bodySize, &info_);
        else if (m.type == MSG_CLIENT_SYNC) haveSync = parseClientSync(m.body, m.bodySize, &sync_);
    }
    if (!haveInfo || !haveSync) {
        err = "SpyServer handshake timed out (no device info)";
        close(); return false;
    }
    LOGI("SpyServer %s:%d — device=%u rate=%u res=%u gainMax=%u range=%u..%u control=%u",
         host.c_str(), port, info_.deviceType, info_.maximumSampleRate, info_.resolution,
         info_.maximumGainIndex, info_.minimumFrequency, info_.maximumFrequency,
         sync_.canControl);
    return true;
}

bool SpyServerClient::setSetting(uint32_t settingId, uint32_t value) {
    return sendCommand(encodeSetSetting(settingId, value));
}

bool SpyServerClient::startStream(uint32_t streamMode, uint32_t iqFormat, uint32_t decimation,
                                  uint32_t iqFrequencyHz, uint32_t gainIndex,
                                  uint32_t fftDisplayPixels, uint32_t fftFrequencyHz) {
    // Exactly the order stock clients use. Decimation cannot be changed in place,
    // so a change means disable → resend everything → enable.
    bool ok = setSetting(SETTING_STREAMING_ENABLED, 0);
    ok = ok && setSetting(SETTING_IQ_FORMAT, iqFormat);
    ok = ok && setSetting(SETTING_IQ_DECIMATION, decimation);
    ok = ok && setSetting(SETTING_IQ_FREQUENCY, iqFrequencyHz);
    ok = ok && setSetting(SETTING_STREAMING_MODE, streamMode);
    ok = ok && setSetting(SETTING_GAIN, gainIndex);
    ok = ok && setSetting(SETTING_IQ_DIGITAL_GAIN, 0);   // explicit: keep header aux at 0
    if (streamMode & STREAM_MODE_FFT) {
        ok = ok && setSetting(SETTING_FFT_FORMAT, FORMAT_UINT8);
        ok = ok && setSetting(SETTING_FFT_DISPLAY_PIXELS, fftDisplayPixels);
        ok = ok && setSetting(SETTING_FFT_DB_OFFSET, 0);
        ok = ok && setSetting(SETTING_FFT_DB_RANGE, 140);
        ok = ok && setSetting(SETTING_FFT_FREQUENCY, fftFrequencyHz);
    }
    ok = ok && setSetting(SETTING_STREAMING_ENABLED, 1);
    return ok;
}

bool SpyServerClient::setIqFrequency(uint32_t hz)  { return setSetting(SETTING_IQ_FREQUENCY, hz); }
bool SpyServerClient::setFftFrequency(uint32_t hz) { return setSetting(SETTING_FFT_FREQUENCY, hz); }
bool SpyServerClient::setGainIndex(uint32_t index) { return setSetting(SETTING_GAIN, index); }

void SpyServerClient::run(std::atomic<bool>& running,
                          const IqCallback& onIq, const FftCallback& onFft) {
    std::vector<uint8_t> buf;
    while (running.load() && sock_ && sock_->isOpen()) {
        size_t len = 0;
        if (!readMessage(buf, len, kRecvTimeoutMs)) break;
        if (len == 0) continue;
        Message_t m; size_t used = 0;
        if (!parseMessage(buf.data(), buf.size(), &m, &used)) continue;

        if (m.streamType == STREAM_TYPE_IQ && m.bodySize) {
            const uint32_t fmt = m.type - MSG_IQ_BASE;   // 100→1 uint8, 101→2 int16…
            if (onIq) onIq(m.body, m.bodySize, fmt);
        } else if (m.streamType == STREAM_TYPE_FFT && m.bodySize) {
            if (onFft) onFft(m.body, m.bodySize);
        } else if (m.type == MSG_CLIENT_SYNC) {
            // The server re-syncs whenever control changes hands or it clamps a
            // setting we asked for. Keep our view current, especially canControl.
            parseClientSync(m.body, m.bodySize, &sync_);
        }
        // MSG_PONG and anything unknown: ignore. An unrecognised message must
        // never drop the link — the protocol grows by adding message types.
    }
}

uint32_t SpyServerClient::gainIndexForTenthDb(const std::vector<int>& gains, int tenthDb) {
    if (gains.empty()) return 0;
    size_t best = 0;
    int bestErr = std::abs(gains[0] - tenthDb);
    for (size_t i = 1; i < gains.size(); ++i) {
        const int e = std::abs(gains[i] - tenthDb);
        if (e < bestErr) { bestErr = e; best = i; }
    }
    return (uint32_t)best;
}

}  // namespace vibe::spyserver
