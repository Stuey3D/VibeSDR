// VibeSDR v6.1 — RTL-TCP SERVER implementation. See rtl_tcp_server.h.
#include "rtl_tcp_server.h"
#include "net_shim.h"

#include <android/log.h>
#include <rtl-sdr.h>
#include <unistd.h>
#include <sys/prctl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define LOG_TAG "VibeRtlTcpSrv"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vibe {

// rtl_tcp command codes (client → server), from rtl_tcp.c.
enum {
    CMD_SET_FREQ            = 0x01,
    CMD_SET_SAMPLE_RATE     = 0x02,
    CMD_SET_GAIN_MODE       = 0x03,  // 1 = manual, 0 = auto
    CMD_SET_GAIN            = 0x04,  // tenths of dB
    CMD_SET_FREQ_CORRECTION = 0x05,  // ppm
    CMD_SET_AGC_MODE        = 0x08,  // RTL2832 digital AGC
    CMD_SET_DIRECT_SAMPLING = 0x09,
    CMD_SET_OFFSET_TUNING   = 0x0a,
    CMD_SET_GAIN_BY_INDEX   = 0x0d,
    CMD_SET_BIAS_TEE        = 0x0e,
};

// Max bytes we'll buffer for a slow client before dropping — keeps latency and
// memory bounded and, crucially, never lets a slow socket stall the USB reader.
static constexpr size_t kClientQueueCapBytes = 4 * 1024 * 1024;

// Kernel send buffer for the client socket. Absorbs the WiFi radio's brief
// power-save / retransmit stalls before the userspace queue above starts dropping.
static constexpr int kClientSendBufBytes = 1024 * 1024;

struct RtlTcpServer::Impl {
    rtlsdr_dev_t* dev   = nullptr;
    int           usbFd = -1;
    int           port  = 0;
    uint32_t      sampleRate  = 2400000;
    std::atomic<uint32_t> overrideRate{0};   // 0 = client-controlled
    uint32_t      tunerType   = 0;
    uint32_t      gainCount    = 0;

    std::shared_ptr<net::Listener> listener;
    std::thread   acceptThread;
    std::thread   rtlThread;
    std::atomic<bool> running{false};

    // The single connected client (or null). Guarded by clientMtx.
    struct Client {
        std::shared_ptr<net::Socket> sock;
        std::string  addr;
        std::thread  writer;
        std::thread  reader;
        std::mutex   qmtx;
        std::condition_variable qcv;
        std::deque<std::vector<uint8_t>> queue;
        size_t queuedBytes = 0;
        std::atomic<bool> alive{true};
    };
    std::shared_ptr<Client> client;
    mutable std::mutex clientMtx;

    // Serialises all rtlsdr_* control calls (reader thread + override changes).
    std::mutex devMtx;

    // IQ bytes dropped because the client's queue was full. Reset per client, so
    // the UI reports "this session" rather than a number the user can't clear.
    std::atomic<uint64_t> droppedBytes{0};

    // Fan the raw u8 IQ buffer to the connected client's send queue (drop-newest
    // if the client is too slow — never blocks the USB callback thread).
    void fanoutIq(const uint8_t* buf, size_t len) {
        std::shared_ptr<Client> c;
        { std::lock_guard<std::mutex> lk(clientMtx); c = client; }
        if (!c || !c->alive.load()) return;
        {
            std::lock_guard<std::mutex> lk(c->qmtx);
            if (c->queuedBytes + len > kClientQueueCapBytes) {
                droppedBytes.fetch_add(len, std::memory_order_relaxed);
                return;                                               // drop
            }
            c->queue.emplace_back(buf, buf + len);
            c->queuedBytes += len;
        }
        c->qcv.notify_one();
    }

    // Apply one decoded rtl_tcp command to the dongle (under devMtx).
    void applyCommand(uint8_t code, uint32_t param) {
        std::lock_guard<std::mutex> lk(devMtx);
        if (!dev) return;
        switch (code) {
            case CMD_SET_FREQ:
                rtlsdr_set_center_freq(dev, param);
                break;
            case CMD_SET_SAMPLE_RATE:
                if (overrideRate.load() == 0) {           // honour only if not capped
                    rtlsdr_set_sample_rate(dev, param);
                    sampleRate = rtlsdr_get_sample_rate(dev);
                }
                break;
            case CMD_SET_GAIN_MODE:
                rtlsdr_set_tuner_gain_mode(dev, (int)param);
                break;
            case CMD_SET_GAIN:
                rtlsdr_set_tuner_gain(dev, (int)param);
                break;
            case CMD_SET_FREQ_CORRECTION:
                rtlsdr_set_freq_correction(dev, (int)param);
                break;
            case CMD_SET_AGC_MODE:
                rtlsdr_set_agc_mode(dev, (int)param);
                break;
            case CMD_SET_DIRECT_SAMPLING:
                rtlsdr_set_direct_sampling(dev, (int)param);
                break;
            case CMD_SET_OFFSET_TUNING:
                rtlsdr_set_offset_tuning(dev, (int)param);
                break;
            case CMD_SET_GAIN_BY_INDEX:
                rtlsdr_set_tuner_gain_mode(dev, 1);
                // No direct "by index" in librtlsdr's public API; map via the gain
                // table so index-based clients still work.
                {
                    int n = rtlsdr_get_tuner_gains(dev, nullptr);
                    /* ★ UNSIGNED (audit 2026-10-03): param is a client's uint32; (int)0xFFFFFFFF
                     *  is -1, passed "< n", and indexed gains[] 4 GB before its start. */
                    if (n > 0 && param < (uint32_t)n) {
                        std::vector<int> gains(n);
                        rtlsdr_get_tuner_gains(dev, gains.data());
                        rtlsdr_set_tuner_gain(dev, gains[param]);
                    }
                }
                break;
            case CMD_SET_BIAS_TEE:
                rtlsdr_set_bias_tee(dev, (int)param);
                break;
            default:
                break;  // ignore unsupported commands
        }
    }

    void applyOverride(uint32_t rate) {
        overrideRate.store(rate);
        if (rate == 0) return;                 // back to client-controlled: leave as-is
        std::lock_guard<std::mutex> lk(devMtx);
        if (!dev) return;
        rtlsdr_set_sample_rate(dev, rate);
        sampleRate = rtlsdr_get_sample_rate(dev);
        LOGI("bandwidth override -> %u Hz (actual %u)", rate, sampleRate);
    }

    void startClient(std::shared_ptr<net::Socket> sock, const std::string& addr) {
        auto c = std::make_shared<Client>();
        c->sock = sock;
        c->addr = addr;
        droppedBytes.store(0, std::memory_order_relaxed);
        if (!sock->setSendBufferSize(kClientSendBufBytes))
            LOGI("SO_SNDBUF %d not honoured (kernel default retained)", kClientSendBufBytes);

        // 12-byte "RTL0" dongle header, exactly as rtl_tcp sends it.
        uint8_t hdr[12];
        std::memcpy(hdr, "RTL0", 4);
        hdr[4] = (tunerType >> 24) & 0xFF; hdr[5] = (tunerType >> 16) & 0xFF;
        hdr[6] = (tunerType >> 8)  & 0xFF; hdr[7] =  tunerType        & 0xFF;
        hdr[8] = (gainCount >> 24) & 0xFF; hdr[9] = (gainCount >> 16) & 0xFF;
        hdr[10]= (gainCount >> 8)  & 0xFF; hdr[11]=  gainCount        & 0xFF;
        if (sock->send(hdr, 12) != 12) { sock->close(); return; }

        Impl* self = this;
        // Writer: drain the IQ queue to the socket.
        c->writer = std::thread([self, c]() {
            while (c->alive.load() && self->running.load()) {
                std::vector<uint8_t> chunk;
                {
                    std::unique_lock<std::mutex> lk(c->qmtx);
                    c->qcv.wait_for(lk, std::chrono::milliseconds(200),
                                    [&]{ return !c->queue.empty() || !c->alive.load(); });
                    if (!c->alive.load()) break;
                    if (c->queue.empty()) continue;
                    chunk.swap(c->queue.front());
                    c->queue.pop_front();
                    c->queuedBytes -= chunk.size();
                }
                if (self->running.load() && c->sock->send(chunk.data(), chunk.size()) < 0) {
                    c->alive.store(false);
                    break;
                }
            }
            c->alive.store(false);
            c->qcv.notify_all();
        });

        // Reader: parse 5-byte [code][BE u32] commands.
        c->reader = std::thread([self, c]() {
            uint8_t cmd[5];
            while (c->alive.load() && self->running.load()) {
                int r = c->sock->recv(cmd, 5, /*forceLen=*/true, /*timeout=*/300);
                if (r == 5) {
                    uint32_t param = ((uint32_t)cmd[1] << 24) | ((uint32_t)cmd[2] << 16) |
                                     ((uint32_t)cmd[3] << 8)  |  (uint32_t)cmd[4];
                    self->applyCommand(cmd[0], param);
                } else if (r < 0 || !c->sock->isOpen()) {
                    break;                     // closed or error
                }
                // r == 0: timeout with nothing — keep waiting.
            }
            c->alive.store(false);
            c->qcv.notify_all();
        });

        { std::lock_guard<std::mutex> lk(clientMtx); client = c; }
        LOGI("client connected: %s", addr.c_str());
    }

    void reapClient() {
        std::shared_ptr<Client> c;
        { std::lock_guard<std::mutex> lk(clientMtx);
          if (client && !client->alive.load()) { c = client; client.reset(); } }
        if (!c) return;
        c->alive.store(false);
        c->qcv.notify_all();
        if (c->sock) c->sock->close();
        if (c->writer.joinable()) c->writer.join();
        if (c->reader.joinable()) c->reader.join();
        LOGI("client disconnected: %s", c->addr.c_str());
    }
};

// ── singleton ────────────────────────────────────────────────────────────────
RtlTcpServer& RtlTcpServer::instance() {
    static RtlTcpServer s;
    return s;
}

int RtlTcpServer::start(int fd, int /*vid*/, int /*pid*/,
                        uint32_t sampleRate, uint32_t centerFreq, int gainTenthDb,
                        int port, uint32_t overrideRate, std::string& err) {
    stop();  // ensure clean

    auto impl = new Impl();
    impl->usbFd = dup(fd);
    if (impl->usbFd < 0) { err = "dup(fd) failed"; delete impl; return -1; }

    int ret = rtlsdr_open_sys_dev(&impl->dev, (intptr_t)impl->usbFd);
    if (ret != 0 || !impl->dev) {
        err = "rtlsdr_open_sys_dev failed (" + std::to_string(ret) + ")";
        ::close(impl->usbFd); delete impl; return -1;
    }

    uint32_t initRate = overrideRate ? overrideRate : sampleRate;
    rtlsdr_set_sample_rate(impl->dev, initRate);
    impl->sampleRate = rtlsdr_get_sample_rate(impl->dev);
    rtlsdr_set_center_freq(impl->dev, centerFreq ? centerFreq : 100000000);
    if (gainTenthDb < 0) {
        rtlsdr_set_tuner_gain_mode(impl->dev, 0);      // auto
    } else {
        rtlsdr_set_tuner_gain_mode(impl->dev, 1);
        rtlsdr_set_tuner_gain(impl->dev, gainTenthDb);
    }
    rtlsdr_reset_buffer(impl->dev);
    impl->tunerType = (uint32_t)rtlsdr_get_tuner_type(impl->dev);
    int gc = rtlsdr_get_tuner_gains(impl->dev, nullptr);
    impl->gainCount = gc > 0 ? (uint32_t)gc : 0;
    impl->overrideRate.store(overrideRate);

    try {
        impl->listener = net::listen("0.0.0.0", port);
    } catch (const std::exception& e) {
        err = std::string("listen failed: ") + e.what();
        rtlsdr_close(impl->dev); ::close(impl->usbFd); delete impl; return -1;
    }
    impl->port = port;
    impl->running.store(true);

    Impl* self = impl;
    // USB reader: raw u8 IQ → fan out to the client.
    impl->rtlThread = std::thread([self]() {
        prctl(PR_SET_NAME, "vibe-srv-usb");
        // Size the USB transfer by TIME, not sample count. librtlsdr's default
        // (buf_len = 0 -> 131072 samples) covers ~55 ms at 2.4 MSPS but ~136 ms at
        // 0.96 — so at low rates the IQ arrives in big infrequent lumps and the
        // stream breaks up. Same bug the local shim had.
        double r = 0;
        { r = (double)rtlsdr_get_sample_rate(self->dev); }
        if (r <= 0) r = 2400000.0;
        uint32_t bufLen = (uint32_t)((r * 2.0 * 0.032) / 512.0 + 0.5) * 512;
        if (bufLen < 16384)  bufLen = 16384;
        if (bufLen > 262144) bufLen = 262144;
        rtlsdr_read_async(self->dev,
            [](unsigned char* buf, uint32_t len, void* ctx) {
                static_cast<Impl*>(ctx)->fanoutIq(buf, len);
            }, self, 0, bufLen);
    });

    // Accept loop: single client; reap dead client each iteration.
    impl->acceptThread = std::thread([self]() {
        prctl(PR_SET_NAME, "vibe-srv-acc");
        while (self->running.load()) {
            self->reapClient();
            auto sock = self->listener->accept(nullptr, 500);
            if (!sock) continue;
            bool haveClient;
            { std::lock_guard<std::mutex> lk(self->clientMtx);
              haveClient = (self->client != nullptr); }
            if (haveClient) { sock->close(); continue; }   // single-client: refuse extras
            std::string addr = sock->peerAddress();
            self->startClient(sock, addr.empty() ? std::string("client") : addr);
        }
    });

    p = impl;
    LOGI("RTL-TCP server listening on 0.0.0.0:%d (rate %u, override %u)",
         port, impl->sampleRate, overrideRate);
    return port;
}

void RtlTcpServer::stop() {
    Impl* impl = p;
    if (!impl) return;
    p = nullptr;

    impl->running.store(false);

    // Tear down the client first.
    {
        std::shared_ptr<Impl::Client> c;
        { std::lock_guard<std::mutex> lk(impl->clientMtx); c = impl->client; impl->client.reset(); }
        if (c) {
            c->alive.store(false);
            c->qcv.notify_all();
            if (c->sock) c->sock->close();
            if (c->writer.joinable()) c->writer.join();
            if (c->reader.joinable()) c->reader.join();
        }
    }

    // Stop the USB async reader and join.
    if (impl->dev) rtlsdr_cancel_async(impl->dev);
    if (impl->rtlThread.joinable()) impl->rtlThread.join();

    // The accept loop exits via the 500 ms poll + running flag; ~Listener closes fd.
    if (impl->acceptThread.joinable()) impl->acceptThread.join();
    impl->listener.reset();

    if (impl->dev) { rtlsdr_close(impl->dev); impl->dev = nullptr; }
    if (impl->usbFd >= 0) { ::close(impl->usbFd); impl->usbFd = -1; }

    delete impl;
    LOGI("RTL-TCP server stopped");
}

bool RtlTcpServer::isRunning() const {
    return p != nullptr && p->running.load();
}

void RtlTcpServer::setSampleRateOverride(uint32_t rate) {
    if (p) p->applyOverride(rate);
}

RtlTcpServer::Status RtlTcpServer::getStatus() const {
    Status s;
    Impl* impl = p;
    if (!impl) return s;
    s.running      = impl->running.load();
    s.sampleRate   = impl->sampleRate;
    s.overrideRate = impl->overrideRate.load();
    s.droppedBytes = impl->droppedBytes.load(std::memory_order_relaxed);
    s.port         = impl->port;
    std::lock_guard<std::mutex> lk(impl->clientMtx);
    if (impl->client && impl->client->alive.load()) {
        s.clientConnected = true;
        s.clientAddr      = impl->client->addr;
    }
    return s;
}

} // namespace vibe
