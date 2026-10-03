// VibeSDR — SpyServer message (de)serialisation. Clean-room; see
// spyserver_protocol.h for provenance and licensing.
//
// Pure functions over byte buffers: no sockets, no allocation policy, nothing
// platform-specific — so this is unit-testable on the host and compiles into both
// the Android JNI target and the iOS static lib.
//
// The wire is little-endian. We do NOT memcpy the packed structs directly, even
// though every ARM/x86 target we ship is little-endian: the explicit codecs cost
// nothing measurable next to the DSP and they make the layout auditable against
// PROTOCOL_NOTES.md. Reading IQ payload is the hot path, and that stays a plain
// pointer into the buffer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "spyserver_protocol.h"

namespace vibe::spyserver {

// ── little-endian primitives ────────────────────────────────────────────────
inline void putU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);  p[3] = (uint8_t)(v >> 24);
}
inline uint32_t getU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ── client → server ─────────────────────────────────────────────────────────

// HELLO: u32 version + client name as raw chars, NO NUL terminator (the name's
// length is bodySize - 4). Confirmed: SDR++ sent exactly 9 body bytes for "SDR++".
std::vector<uint8_t> encodeHello(uint32_t protocolVersion, const std::string& clientName);

// SET_SETTING: u32 id + u32 value.
std::vector<uint8_t> encodeSetSetting(uint32_t settingId, uint32_t value);

// PING: 8 opaque bytes the server echoes back in a PONG.
std::vector<uint8_t> encodePing(uint64_t token);

// Parse a client command. Returns false if `len` doesn't yet hold a whole
// message; on success sets *consumed to the total bytes used.
struct Command_t {
    uint32_t type = 0;
    const uint8_t* body = nullptr;
    uint32_t bodySize = 0;
};
/** ★ The largest body either parser will accept (audit 2026-10-03). The biggest real message is
 *  an int16 IQ block at low decimation — well under 1 MB; anything past 4 MB is a desync or an
 *  attack, and the parsers report it as incomplete so the caller's overflow guard drops the link. */
constexpr uint32_t kMaxBodySize = 4u * 1024 * 1024;
bool parseCommand(const uint8_t* buf, size_t len, Command_t* out, size_t* consumed);

// Body helpers for the commands a SERVER must understand.
bool parseHelloBody(const uint8_t* body, uint32_t bodySize,
                    uint32_t* version, std::string* clientName);
bool parseSetSettingBody(const uint8_t* body, uint32_t bodySize,
                         uint32_t* settingId, uint32_t* value);

// ── server → client ─────────────────────────────────────────────────────────

// 20-byte header. `aux` occupies the high 16 bits of the messageType field; send
// 0 unless emulating the auto-digital-gain reporting we observed but did not
// confirm (see PROTOCOL_NOTES.md).
std::vector<uint8_t> encodeMessageHeader(uint32_t messageType, uint32_t streamType,
                                         uint32_t sequenceNumber, uint32_t bodySize,
                                         uint32_t aux = 0);

std::vector<uint8_t> encodeDeviceInfo(const DeviceInfo& info);
std::vector<uint8_t> encodeClientSync(const ClientSync& sync);

// A whole server message: header + body, ready to write to the socket.
std::vector<uint8_t> encodeMessage(uint32_t messageType, uint32_t streamType,
                                   uint32_t sequenceNumber,
                                   const uint8_t* body, uint32_t bodySize,
                                   uint32_t aux = 0);

// Parse a server message. `body` points INTO `buf` (no copy) — the IQ path must
// not allocate. Returns false if the buffer doesn't hold a whole message.
struct Message_t {
    uint32_t protocolId = 0;
    uint32_t type = 0;         // low 16 bits, already masked
    uint32_t aux = 0;          // high 16 bits
    uint32_t streamType = 0;
    uint32_t sequenceNumber = 0;
    const uint8_t* body = nullptr;
    uint32_t bodySize = 0;
};
bool parseMessage(const uint8_t* buf, size_t len, Message_t* out, size_t* consumed);

bool parseDeviceInfo(const uint8_t* body, uint32_t bodySize, DeviceInfo* out);
bool parseClientSync(const uint8_t* body, uint32_t bodySize, ClientSync* out);

}  // namespace vibe::spyserver
