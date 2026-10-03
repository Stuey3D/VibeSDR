// VibeSDR — SpyServer message (de)serialisation. See spyserver_messages.h.
#include "spyserver_messages.h"

#include <cstring>

namespace vibe::spyserver {

// ── client → server ─────────────────────────────────────────────────────────
std::vector<uint8_t> encodeHello(uint32_t protocolVersion, const std::string& clientName) {
    const uint32_t bodySize = 4 + (uint32_t)clientName.size();
    std::vector<uint8_t> out(8 + bodySize);
    putU32(out.data() + 0, CMD_HELLO);
    putU32(out.data() + 4, bodySize);
    putU32(out.data() + 8, protocolVersion);
    if (!clientName.empty())
        std::memcpy(out.data() + 12, clientName.data(), clientName.size());
    return out;
}

std::vector<uint8_t> encodeSetSetting(uint32_t settingId, uint32_t value) {
    std::vector<uint8_t> out(16);
    putU32(out.data() + 0, CMD_SET_SETTING);
    putU32(out.data() + 4, 8);
    putU32(out.data() + 8, settingId);
    putU32(out.data() + 12, value);
    return out;
}

std::vector<uint8_t> encodePing(uint64_t token) {
    std::vector<uint8_t> out(16);
    putU32(out.data() + 0, CMD_PING);
    putU32(out.data() + 4, 8);
    putU32(out.data() + 8,  (uint32_t)(token & 0xFFFFFFFFu));
    putU32(out.data() + 12, (uint32_t)(token >> 32));
    return out;
}

bool parseCommand(const uint8_t* buf, size_t len, Command_t* out, size_t* consumed) {
    if (len < 8) return false;
    const uint32_t bodySize = getU32(buf + 4);
    /* ★★★ SUBTRACT, NEVER ADD (audit 2026-10-03). On a 32-bit build size_t is 32 bits, so
     *  8 + 0xFFFFFFFC wrapped to 4, passed this test, and handed back a body pointer and size
     *  running 4 GB past the buffer. len >= 8 is established above, so len - 8 cannot wrap. */
    if (bodySize > kMaxBodySize || bodySize > len - 8) return false;
    out->type = getU32(buf + 0);
    out->bodySize = bodySize;
    out->body = bodySize ? buf + 8 : nullptr;
    *consumed = (size_t)8 + bodySize;
    return true;
}

bool parseHelloBody(const uint8_t* body, uint32_t bodySize,
                    uint32_t* version, std::string* clientName) {
    if (bodySize < 4) return false;
    *version = getU32(body);
    // No NUL terminator on the wire; the name runs to the end of the body.
    clientName->assign((const char*)body + 4, bodySize - 4);
    return true;
}

bool parseSetSettingBody(const uint8_t* body, uint32_t bodySize,
                         uint32_t* settingId, uint32_t* value) {
    if (bodySize < 8) return false;
    *settingId = getU32(body);
    *value     = getU32(body + 4);
    return true;
}

// ── server → client ─────────────────────────────────────────────────────────
std::vector<uint8_t> encodeMessageHeader(uint32_t messageType, uint32_t streamType,
                                         uint32_t sequenceNumber, uint32_t bodySize,
                                         uint32_t aux) {
    std::vector<uint8_t> out(20);
    putU32(out.data() + 0,  kProtocolVersion);
    putU32(out.data() + 4,  (messageType & 0xFFFF) | (aux << 16));
    putU32(out.data() + 8,  streamType);
    putU32(out.data() + 12, sequenceNumber);
    putU32(out.data() + 16, bodySize);
    return out;
}

std::vector<uint8_t> encodeDeviceInfo(const DeviceInfo& i) {
    std::vector<uint8_t> b(48);
    const uint32_t f[12] = {
        i.deviceType, i.deviceSerial, i.maximumSampleRate, i.maximumBandwidth,
        i.decimationStageCount, i.gainStageCount, i.maximumGainIndex,
        i.minimumFrequency, i.maximumFrequency, i.resolution,
        i.minimumIQDecimation, i.forcedIQFormat,
    };
    for (int k = 0; k < 12; ++k) putU32(b.data() + k * 4, f[k]);
    return b;
}

std::vector<uint8_t> encodeClientSync(const ClientSync& s) {
    std::vector<uint8_t> b(40);
    const uint32_t f[10] = {
        s.canControl, s.gain, s.deviceCenterFrequency, s.iqCenterFrequency,
        s.fftCenterFrequency, s.minimumIQCenterFrequency, s.maximumIQCenterFrequency,
        s.minimumFFTCenterFrequency, s.maximumFFTCenterFrequency, s.reserved,
    };
    for (int k = 0; k < 10; ++k) putU32(b.data() + k * 4, f[k]);
    return b;
}

std::vector<uint8_t> encodeMessage(uint32_t messageType, uint32_t streamType,
                                   uint32_t sequenceNumber,
                                   const uint8_t* body, uint32_t bodySize,
                                   uint32_t aux) {
    auto out = encodeMessageHeader(messageType, streamType, sequenceNumber, bodySize, aux);
    if (bodySize) {
        out.resize(20 + bodySize);
        std::memcpy(out.data() + 20, body, bodySize);
    }
    return out;
}

bool parseMessage(const uint8_t* buf, size_t len, Message_t* out, size_t* consumed) {
    if (len < 20) return false;
    const uint32_t bodySize = getU32(buf + 16);
    // ★★★ SUBTRACT, NEVER ADD — the same 32-bit wrap as parseCommand (audit 2026-10-03)
    if (bodySize > kMaxBodySize || bodySize > len - 20) return false;
    const uint32_t rawType = getU32(buf + 4);
    out->protocolId     = getU32(buf + 0);
    out->type           = messageTypeOf(rawType);
    out->aux            = messageAuxOf(rawType);
    out->streamType     = getU32(buf + 8);
    out->sequenceNumber = getU32(buf + 12);
    out->bodySize       = bodySize;
    out->body           = bodySize ? buf + 20 : nullptr;   // points INTO buf
    *consumed = (size_t)20 + bodySize;
    return true;
}

bool parseDeviceInfo(const uint8_t* body, uint32_t bodySize, DeviceInfo* o) {
    if (bodySize < 48) return false;
    uint32_t f[12];
    for (int k = 0; k < 12; ++k) f[k] = getU32(body + k * 4);
    *o = DeviceInfo{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]};
    return true;
}

bool parseClientSync(const uint8_t* body, uint32_t bodySize, ClientSync* o) {
    if (bodySize < 40) return false;
    uint32_t f[10];
    for (int k = 0; k < 10; ++k) f[k] = getU32(body + k * 4);
    *o = ClientSync{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]};
    return true;
}

}  // namespace vibe::spyserver
