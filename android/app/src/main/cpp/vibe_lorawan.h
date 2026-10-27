// vibe_lorawan.h — a LoRaWAN frame's HEADER, from the LoRaWAN 1.0.x / 1.1 Link Layer Specification (LoRa Alliance,
// public): MHDR (MType, Major), FHDR (DevAddr, FCtrl, FCnt, FOpts), FPort, and the MIC; the join request's JoinEUI,
// DevEUI and DevNonce. docs/v12/LORA-MESH.md §LoRaWAN.
//
// ★★★ PRIVACY (Stuart, 2026-10-27): the application payload is encrypted with keys only the device and its network hold,
//     and it stays exactly that — its LENGTH is shown and the word "encrypted", never a byte (the wM-Bus meters' rule,
//     WMBUS-DECODER.md). FOpts (MAC commands, plain in 1.0) are counted, not shown. The MIC is not checked (it needs
//     the network's key) — which is why a frame is only called LoRaWAN when its header is self-consistent.
#pragma once
#include "vibe_lora_msg.h"
#include "vibe_lora_crypto.h"
#include <cstdio>

namespace vibelora {
namespace lorawan {

inline const char* mtypeName(int m) {
    static const char* n[8] = {"Join request", "Join accept", "Unconfirmed up", "Unconfirmed down", "Confirmed up",
                               "Confirmed down", "Rejoin request", "Proprietary"};
    return n[m & 7];
}
/** ★ The network, where its DevAddr block is published: The Things Network holds NetID 0x000013, DevAddr 26000000/7
 *  (TTN's addressing documentation). Others are named by NetID type only — guessing an operator would be a claim. */
inline std::string netOf(uint32_t devAddr) {
    if ((devAddr >> 25) == (0x26u >> 1)) return "The Things Network";
    return "";
}

/** Parse `b` as LoRaWAN. False when it cannot be one (then the frame is listed as plain LoRa). */
inline bool parse(const std::vector<uint8_t>& b, bool crcPresent, Msg& m) {
    if (b.size() < 5) return false;
    const int mtype = b[0] >> 5, major = b[0] & 3, rfu = b[0] >> 2 & 7;
    if (major != 0 || rfu != 0) return false;
    m.proto = "lorawan";
    m.mtype = mtypeName(mtype);
    if (mtype == 0) {                                     // join request: MHDR + JoinEUI(8) + DevEUI(8) + DevNonce(2) + MIC(4)
        if (b.size() != 23 || !crcPresent) return false;
        uint8_t je[8], de[8];
        for (int i = 0; i < 8; ++i) { je[i] = b[size_t(8 - i)]; de[i] = b[size_t(16 - i)]; }   // sent little-endian
        m.joinEui = crypto::toHex(je, 8, true); m.devEui = crypto::toHex(de, 8, true);
        m.node = m.devEui; m.what = "join"; m.uplink = true;
        return true;
    }
    if (mtype == 1) {                                     // join accept: wholly encrypted — 17 or 33 bytes
        if (b.size() != 17 && b.size() != 33) return false;
        m.what = "joinaccept"; m.uplink = false; m.encLen = int(b.size()) - 1;
        return true;
    }
    if (mtype == 7 || mtype == 6) return false;          // proprietary / rejoin: nothing standard to read — plain LoRa
    // data frames: MHDR(1) DevAddr(4) FCtrl(1) FCnt(2) FOpts(0-15) [FPort(1) FRMPayload] MIC(4)
    if (b.size() < 12) return false;
    const uint32_t da = uint32_t(b[1]) | uint32_t(b[2]) << 8 | uint32_t(b[3]) << 16 | uint32_t(b[4]) << 24;
    const uint8_t fc = b[5];
    m.uplink = mtype == 2 || mtype == 4;
    // ★ uplinks carry the PHY CRC; downlinks do not (LoRaWAN Regional Parameters) — a mismatch is not LoRaWAN
    if (m.uplink != crcPresent) return false;
    m.adr = fc & 0x80; m.ack = fc & 0x20;
    if (m.uplink) m.adrAckReq = fc & 0x40; else m.fpending = fc & 0x10;
    m.foptsLen = fc & 15;
    m.fcnt = int(b[6] | b[7] << 8);
    const size_t hdr = 8 + size_t(m.foptsLen);
    if (hdr + 4 > b.size()) return false;
    const size_t rest = b.size() - hdr - 4;
    if (rest > 0) {
        m.fport = b[hdr];
        if (m.fport == 0 && m.foptsLen) return false;    // ★ the spec forbids MAC commands in both places
        m.encLen = int(rest - 1);
    }
    char h[12]; std::snprintf(h, sizeof h, "%08X", da);
    m.node = h; m.net = netOf(da);
    m.what = "data";
    return true;
}

/** "LoRaWAN · TTN 260B1234 · Unconfirmed up · FCnt 4211 · port 2 · 12 bytes encrypted · ADR" */
inline std::string line(const Msg& m) {
    std::string s = "LoRaWAN";
    const std::string d = mkDot();
    if (m.what == "join") return s + d + "Join request" + d + "DevEUI " + m.devEui + d + "JoinEUI " + m.joinEui;
    if (m.what == "joinaccept") return s + d + "Join accept" + d + std::to_string(m.encLen) + " bytes encrypted";
    s += d + (m.net == "The Things Network" ? std::string("TTN ") : std::string("device ")) + m.node;
    s += d + m.mtype;
    if (m.fcnt >= 0) s += d + "FCnt " + std::to_string(m.fcnt);
    if (m.fport > 0) s += d + "port " + std::to_string(m.fport);
    else if (m.fport == 0) s += d + "MAC commands";
    if (m.encLen > 0) s += d + std::to_string(m.encLen) + " bytes encrypted";
    if (m.foptsLen) s += d + std::to_string(m.foptsLen) + " bytes of options";
    if (m.adr) s += d + "ADR";
    if (m.ack) s += d + "ACK";
    return s;
}

} // namespace lorawan
} // namespace vibelora
