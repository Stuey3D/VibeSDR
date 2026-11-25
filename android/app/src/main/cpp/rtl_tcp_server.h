// VibeSDR v6.1 — RTL-TCP SERVER (Android only).
//
// Turns an Android phone with a USB RTL-SDR into a networked rtl_tcp server so
// any rtl_tcp client (including another VibeSDR / SDR#) can use the dongle over
// the LAN. This is the mirror image of the shim's rtl_tcp CLIENT path: here we
// OPEN the dongle, stream raw u8 IQ to a connected client, and apply the 5-byte
// rtl_tcp commands the client sends (freq / sample-rate / gain / ...).
//
// Deliberately self-contained (uses only net_shim + librtlsdr, none of the
// demod/FFT/audio pipeline) — a server has no reason to demodulate locally, and
// the user model is dedicated-server-only (no simultaneous on-device use), so
// there's no dongle-tuning conflict to arbitrate.
//
// SINGLE CLIENT, like real rtl_tcp: while one client is connected, further
// connection attempts are accepted-then-immediately-closed. When the client
// drops, the server keeps running and waits for the next.
#pragma once
#include <cstdint>
#include <string>

namespace vibe {

class RtlTcpServer {
public:
    static RtlTcpServer& instance();

    // Open the RTL-SDR on `fd` (owned by Kotlin — we dup it), bind 0.0.0.0:port
    // and start serving. `sampleRate`==the initial/default rate; `overrideRate`
    // != 0 forces that rate and ignores client SET_SAMPLE_RATE (bandwidth cap).
    // Returns `port` on success, or -1 with `err` set.
    int start(int fd, int vid, int pid,
              uint32_t sampleRate, uint32_t centerFreq, int gainTenthDb,
              int port, uint32_t overrideRate, std::string& err);

    void stop();
    bool isRunning() const;

    // Bandwidth override: 0 = client-controlled (honour SET_SAMPLE_RATE),
    // >0 = force this rate and ignore the client's requests. Applied live.
    void setSampleRateOverride(uint32_t rate);

    // Status snapshot for the notification / server UI.
    struct Status {
        bool     running       = false;
        bool     clientConnected = false;
        std::string clientAddr;         // "ip:port" of the connected client, or ""
        uint32_t sampleRate    = 0;     // rate currently applied to the dongle
        uint32_t overrideRate  = 0;     // 0 = client-controlled
        uint64_t droppedBytes  = 0;     // IQ dropped since this client connected
        int      port          = 0;     // bound listen port (for re-adopting a running server)
        uint64_t iqBytes       = 0;     // IQ bytes the radio has delivered since start
        bool     usbEnded      = false; // the radio's stream ended under a running server
    };
    Status getStatus() const;

private:
    RtlTcpServer() = default;
    struct Impl;
    Impl* p = nullptr;
};

} // namespace vibe
