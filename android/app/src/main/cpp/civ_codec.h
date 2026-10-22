// civ_codec.h — Icom CI-V, as pure data: framing, BCD, typed builders, the parser, and the
// OUTGOING ALLOW-LIST that is the whole of VibeServer's receive-only promise for an Icom.
//
// ★★★ CLEAN ROOM. Written from Icom's published CI-V Reference Guides and the facts table in
//     docs/ICOM-SUPPORT-PLAN.md (§1, §3.2) — never from wfview/wfweb (GPLv3, and their code in an
//     App Store binary would void APPSTORE-EXCEPTION.md, plan §4) and never from Hamlib's code
//     (used only as a reference for FACTS: addresses, scope caps, calibration points).
//
// ★★★ THE RULE THIS FILE EXISTS FOR (plan §3.2): THE READ AND WRITE FORMS OF A CI-V COMMAND DIFFER
//     ONLY BY DATA LENGTH. `1C 00` reads the TX state; `1C 00 01` TRANSMITS. `1A 05 00 89` reads a
//     menu item; `1A 05 00 89 01` WRITES it. A filter on command + subcommand alone would pass the
//     transmit form of every read we use. So mayTransmit() checks the WHOLE FRAME — address, cmd,
//     sub, EXACT data length and every value — against one static table, and anything else is
//     refused and counted as a fault. Nothing reaches the wire without passing it: IcomSource's only
//     write path calls it as the very last step, and test-icom-civ.cpp enumerates every cmd/sub pair
//     at 0-6 data bytes to prove that only the table passes.
//   ★★ There is NO builder for any transmit command (1C 00 01, 1C 01, 17, 28, 18, …) — not even a
//      private one. A frame that cannot be built cannot be sent by mistake; the allow-list is the
//      second wall, for a bug that assembles bytes by hand.
//   ★ Clients never send raw CI-V. They send the same semantic JSON as for any radio; the server
//     alone builds frames, from the typed builders here (plan §3.1).
//
// ★ Header-only, no allocation anywhere: a Frame is a fixed array, the parser is a byte state
//   machine with a fixed buffer. It runs on the CI-V thread of a Pi 2 at 115200 baud and never
//   touches the heap (thread priority + potato-phone rules).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace civ {

constexpr uint8_t kPreamble   = 0xFE;
constexpr uint8_t kEnd        = 0xFD;
constexpr uint8_t kOk         = 0xFB;   ///< reply: accepted
constexpr uint8_t kNg         = 0xFA;   ///< reply: refused ("NG")
constexpr uint8_t kJam        = 0xFC;   ///< collision jammer (multi-drop CI-V bus)
constexpr uint8_t kController = 0xE0;   ///< our address, the conventional controller one
constexpr uint8_t kBroadcast  = 0x00;   ///< transceive announcements are addressed to 00

/** ★ Big enough for a LAN sweep in ONE division (689 points on a 7610 + header). USB divisions are
 *  ≤ 50 amplitude bytes; anything longer than this is not a frame we understand and is dropped. */
constexpr size_t kMaxData  = 720;
constexpr size_t kMaxFrame = kMaxData + 8;

// ── BCD ───────────────────────────────────────────────────────────────────────────────────────

/** One byte of packed BCD (two digits) → 0-99, or -1 if either nibble is not a digit. */
inline int bcdByte(uint8_t b) {
    const int hi = b >> 4, lo = b & 0x0F;
    if (hi > 9 || lo > 9) return -1;
    return hi * 10 + lo;
}
inline uint8_t toBcdByte(int v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }

/** ★ Frequency: 5 bytes, 10 digits, LEAST significant byte FIRST (1 Hz digits in byte 0).
 *  `hz` above 9 999 999 999 cannot be expressed and returns false. */
inline bool freqToBcd(uint64_t hz, uint8_t out[5]) {
    if (hz > 9999999999ULL) return false;
    for (int i = 0; i < 5; i++) { out[i] = toBcdByte((int)(hz % 100)); hz /= 100; }
    return true;
}
/** Reverse of freqToBcd over `n` bytes (5 normally; the 7300 also sends 4-byte forms in some
 *  replies). Returns -1 for a non-digit nibble — a corrupt frame, never a frequency. */
inline int64_t bcdToFreq(const uint8_t* b, int n) {
    int64_t v = 0, mul = 1;
    for (int i = 0; i < n; i++) {
        const int d = bcdByte(b[i]);
        if (d < 0) return -1;
        v += (int64_t)d * mul; mul *= 100;
    }
    return v;
}
/** ★ Levels (RF gain, squelch, menu values): BIG-endian BCD, "0255" = 02 55. */
inline int bcdBigToInt(const uint8_t* b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) { const int d = bcdByte(b[i]); if (d < 0) return -1; v = v * 100 + d; }
    return v;
}
inline void intToBcdBig(int v, uint8_t* out, int n) {
    for (int i = n - 1; i >= 0; i--) { out[i] = toBcdByte(v % 100); v /= 100; }
}

// ── Commands we know (Icom CI-V Reference, IC-7300 and family) ────────────────────────────────
namespace cmd {
constexpr uint8_t TransceiveFreq = 0x00;   ///< unsolicited: the radio's frequency changed
constexpr uint8_t TransceiveMode = 0x01;   ///< unsolicited: mode/filter changed
constexpr uint8_t ReadFreq  = 0x03;
constexpr uint8_t ReadMode  = 0x04;
constexpr uint8_t SetFreq   = 0x05;
constexpr uint8_t SetMode   = 0x06;
constexpr uint8_t Att       = 0x11;
constexpr uint8_t Level     = 0x14;        ///< 14 02 RF gain, 14 03 squelch
constexpr uint8_t Meter     = 0x15;        ///< 15 02 S-meter
constexpr uint8_t Func      = 0x16;        ///< 16 02 preamp, 16 12 AGC, 16 22 NB, 16 40 NR, 16 41 ANF, 16 46 VOX
constexpr uint8_t Id        = 0x19;        ///< 19 00 transceiver ID
constexpr uint8_t Misc      = 0x1A;        ///< 1A 03 filter width, 1A 05 menu, 1A 06 data mode
constexpr uint8_t Tx        = 0x1C;        ///< 1C 00 TX state — READ ONLY, here and forever
constexpr uint8_t Scope     = 0x27;
}  // namespace cmd

/** The modes the 06 command takes (Icom's numbering). */
namespace mode {
constexpr uint8_t LSB = 0x00, USB = 0x01, AM = 0x02, CW = 0x03, RTTY = 0x04, FM = 0x05,
                  WFM = 0x06, CWR = 0x07, RTTYR = 0x08, DV = 0x17;
}

// ── A frame ───────────────────────────────────────────────────────────────────────────────────

/** A parsed frame. `data` is everything after `cmd` (the sub-command, when there is one, is
 *  data[0] — CI-V does not mark which commands have one, so the reader decides). */
struct Frame {
    uint8_t to = 0, from = 0, cmd = 0;
    uint16_t len = 0;                 ///< bytes in data
    uint8_t data[kMaxData];
};

/** An outgoing frame, built by the typed builders below. ★ The only kind of thing the write path
 *  accepts — there is no "send these bytes" entry point. */
struct Out {
    uint8_t b[24];
    uint8_t n = 0;
};

/** Begin a frame to `radio`: FE FE <radio> E0 <cmd> */
inline void begin(Out& o, uint8_t radio, uint8_t c) {
    o.n = 0; o.b[o.n++] = kPreamble; o.b[o.n++] = kPreamble;
    o.b[o.n++] = radio; o.b[o.n++] = kController; o.b[o.n++] = c;
}
inline void put(Out& o, uint8_t v) { if (o.n < sizeof o.b - 1) o.b[o.n++] = v; }
inline void end(Out& o) { o.b[o.n++] = kEnd; }

// ── The policy the allow-list is checked against ──────────────────────────────────────────────

/** What THIS radio may be sent — filled from the model table (IcomSource) once, at open.
 *  ★ Everything is a fixed array: mayTransmit runs before every write and must not allocate. */
struct Policy {
    uint8_t radio = 0x94;                 ///< the radio's CI-V address — frames to anything else are refused
    struct Range { uint64_t lo, hi; };
    Range ranges[8] = {};                 ///< RECEIVE ranges; 05 outside all of them is refused
    int nRanges = 0;
    uint8_t modes[16] = {};               ///< 06 mode bytes this radio takes
    int nModes = 0;
    uint8_t att[4] = {0x00, 0x20};        ///< 11 values (00 = off, 20 = 20 dB on the 7300)
    int nAtt = 2;
    int preampMax = 2;                    ///< 16 02 00..preampMax
    int dataModeMax = 1;                  ///< 1A 06 00..dataModeMax (705/9700 have D1-D3)
    uint32_t spansHalfHz[12] = {};        ///< 27 15: the HALF-spans the scope takes, exactly
    int nSpans = 0;
    /** ★★ 1A 05 READS ARE ALLOWED FOR THESE ITEMS ONLY — the preflight's. Each is the item's two
     *  BCD bytes (e.g. 00 89). A 1A 05 frame with a value after the item is a menu WRITE and is
     *  refused whatever the item: TX power, USB SEND, VOX gain all live in that menu. */
    uint8_t menuItems[12][2] = {};
    int nMenuItems = 0;
    bool hasScopeSelector = true;         ///< 27 1x commands carry a main/sub byte (00) first
};

inline bool inList(const uint8_t* l, int n, uint8_t v) { for (int i = 0; i < n; i++) if (l[i] == v) return true; return false; }

/** Why a frame was refused — counted and logged by the caller, never sent. */
enum class Verdict : uint8_t {
    Ok = 0, BadShape, WrongAddress, StrayByte, UnknownCommand, WrongLength, BadValue, OutOfRange,
};
inline const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "ok";
        case Verdict::BadShape: return "not a CI-V frame";
        case Verdict::WrongAddress: return "wrong address";
        case Verdict::StrayByte: return "FE/FD inside the frame";
        case Verdict::UnknownCommand: return "command not on the allow-list";
        case Verdict::WrongLength: return "wrong data length for this command";
        case Verdict::BadValue: return "value not allowed";
        case Verdict::OutOfRange: return "frequency outside the receive ranges";
    }
    return "?";
}

namespace detail {
inline bool bcdRange(const uint8_t* d, int n, int lo, int hi) {
    const int v = bcdBigToInt(d, n);
    return v >= lo && v <= hi;
}
inline bool onOff(uint8_t v) { return v == 0x00 || v == 0x01; }
}  // namespace detail

/**
 * ★★★ THE ALLOW-LIST — may this complete frame be written to the radio?
 *
 * The frame must be EXACTLY: FE FE <policy.radio> E0 <cmd> [sub] [data] FD, with no FE or FD
 * inside, and (cmd, sub, data length, values) must be one row of the table below. Every row is
 * written out by hand; there is no default that passes.
 *
 *   reads, NO data:  03 · 04 · 11 · 15 02 · 19 00 · 1C 00 · 14 02 · 14 03 · 16 02/12/22/40/41/46
 *                    · 1A 03 · 1A 06 · 27 10 · 27 11 · 27 14 00 · 27 15 00 · 27 19 00 · 27 1A 00
 *                    · 1A 05 <item>  (preflight items only, item bytes and nothing after)
 *   writes:          05 <5 BCD in an RX range> · 06 <mode in list> <fil 1-3>
 *                    · 1A 06 <0..max> <fil 0-3> · 1A 03 <00-49 BCD> · 11 <att in list>
 *                    · 16 02 <0..preampMax> · 16 12 <1-3> · 16 22/40/41 <0/1>
 *                    · 14 02 / 14 03 <0000-0255 BCD> · 27 10 <0/1> · 27 11 <0/1>
 *                    · 27 14 00 <0-3> · 27 15 00 <5 BCD = a published half-span>
 *                    · 27 17 00 <0/1> · 27 19 00 <0000-2000 BCD> <sign 0/1> · 27 1A 00 <0-2>
 *
 * ★ 16 46 is VOX: its READ is a preflight item, its write (turning VOX on) is refused. ★ 27 10/11
 *   on the 7300 take no main/sub selector (they are whole-scope switches); 27 14 onwards do.
 */
inline Verdict mayTransmit(const uint8_t* f, size_t n, const Policy& p) {
    using namespace detail;
    if (!f || n < 6 || n > 24) return Verdict::BadShape;
    if (f[0] != kPreamble || f[1] != kPreamble || f[n - 1] != kEnd) return Verdict::BadShape;
    // ★ A third FE would be a power-on WAKE preamble (a run of FE) — never ours to send.
    for (size_t i = 2; i < n - 1; i++) if (f[i] == kPreamble || f[i] == kEnd) return Verdict::StrayByte;
    if (f[2] != p.radio || f[3] != kController) return Verdict::WrongAddress;
    const uint8_t c = f[4];
    const uint8_t* d = f + 5;              // data, sub-command first when there is one
    const int len = (int)n - 6;            // bytes between cmd and FD
    auto sub = [&]() -> int { return len >= 1 ? d[0] : -1; };

    switch (c) {
    case cmd::ReadFreq: case cmd::ReadMode:
        return len == 0 ? Verdict::Ok : Verdict::WrongLength;
    case cmd::SetFreq: {
        if (len != 5) return Verdict::WrongLength;
        const int64_t hz = bcdToFreq(d, 5);
        if (hz < 0) return Verdict::BadValue;
        for (int i = 0; i < p.nRanges; i++)
            if ((uint64_t)hz >= p.ranges[i].lo && (uint64_t)hz <= p.ranges[i].hi) return Verdict::Ok;
        return Verdict::OutOfRange;
    }
    case cmd::SetMode:
        if (len != 2) return Verdict::WrongLength;
        if (!inList(p.modes, p.nModes, d[0])) return Verdict::BadValue;
        return (d[1] >= 1 && d[1] <= 3) ? Verdict::Ok : Verdict::BadValue;
    case cmd::Att:
        if (len == 0) return Verdict::Ok;                      // read
        if (len != 1) return Verdict::WrongLength;
        return inList(p.att, p.nAtt, d[0]) ? Verdict::Ok : Verdict::BadValue;
    case cmd::Meter:
        return (len == 1 && d[0] == 0x02) ? Verdict::Ok
             : (sub() == 0x02 ? Verdict::WrongLength : Verdict::UnknownCommand);
    case cmd::Id:
        return (len == 1 && d[0] == 0x00) ? Verdict::Ok
             : (sub() == 0x00 ? Verdict::WrongLength : Verdict::UnknownCommand);
    case cmd::Tx:
        // ★★★ 1C 00 WITH NO DATA IS THE TX-STATE READ. 1C 00 01 KEYS THE TRANSMITTER; 1C 01 xx is
        //     the antenna TUNER (1C 01 02 starts a tune — that transmits too). Only the bare read.
        return (len == 1 && d[0] == 0x00) ? Verdict::Ok
             : (sub() == 0x00 ? Verdict::WrongLength : Verdict::UnknownCommand);
    case cmd::Level: {
        const int s = sub();
        if (s != 0x02 && s != 0x03) return Verdict::UnknownCommand;   // 14 0A RF POWER, 14 0B MIC: never
        if (len == 1) return Verdict::Ok;                              // read
        if (len != 3) return Verdict::WrongLength;
        return bcdRange(d + 1, 2, 0, 255) ? Verdict::Ok : Verdict::BadValue;
    }
    case cmd::Func: {
        const int s = sub();
        switch (s) {
        case 0x02:   // preamp
            if (len == 1) return Verdict::Ok;
            if (len != 2) return Verdict::WrongLength;
            return d[1] <= p.preampMax ? Verdict::Ok : Verdict::BadValue;
        case 0x12:   // AGC
            if (len == 1) return Verdict::Ok;
            if (len != 2) return Verdict::WrongLength;
            return (d[1] >= 1 && d[1] <= 3) ? Verdict::Ok : Verdict::BadValue;
        case 0x22: case 0x40: case 0x41:   // NB, NR, ANF
            if (len == 1) return Verdict::Ok;
            if (len != 2) return Verdict::WrongLength;
            return onOff(d[1]) ? Verdict::Ok : Verdict::BadValue;
        case 0x46:   // ★ VOX — READ ONLY (a preflight item). Writing it on is how audio keys the radio.
            return len == 1 ? Verdict::Ok : Verdict::WrongLength;
        default:
            return Verdict::UnknownCommand;   // 16 47 break-in and everything else
        }
    }
    case cmd::Misc: {
        const int s = sub();
        switch (s) {
        case 0x03:   // filter width index
            if (len == 1) return Verdict::Ok;
            if (len != 2) return Verdict::WrongLength;
            return bcdRange(d + 1, 1, 0, 49) ? Verdict::Ok : Verdict::BadValue;
        case 0x05: { // ★★ MENU: READS OF THE PREFLIGHT ITEMS, AND NOTHING ELSE. Never a write.
            if (len != 3) return Verdict::WrongLength;                 // 05 + exactly two item bytes
            for (int i = 0; i < p.nMenuItems; i++)
                if (p.menuItems[i][0] == d[1] && p.menuItems[i][1] == d[2]) return Verdict::Ok;
            return Verdict::BadValue;
        }
        case 0x06:   // data mode
            if (len == 1) return Verdict::Ok;
            if (len != 3) return Verdict::WrongLength;
            if (d[1] > p.dataModeMax) return Verdict::BadValue;
            return d[2] <= 3 ? Verdict::Ok : Verdict::BadValue;
        default:
            return Verdict::UnknownCommand;   // 1A 00 memory, 1A 01 band stack, 1A 02 keyer memory, …
        }
    }
    case cmd::Scope: {
        const int s = sub();
        switch (s) {
        case 0x10: case 0x11:   // scope on/off, data output on/off — no selector byte
            if (len == 1) return Verdict::Ok;
            if (len != 2) return Verdict::WrongLength;
            return onOff(d[1]) ? Verdict::Ok : Verdict::BadValue;
        case 0x14: case 0x15: case 0x17: case 0x19: case 0x1A: {
            const int sel = p.hasScopeSelector ? 1 : 0;
            if (p.hasScopeSelector && (len < 2 || d[1] != 0x00)) return len < 2 ? Verdict::WrongLength : Verdict::BadValue;
            const int vlen = len - 1 - sel;          // bytes after sub + selector
            const uint8_t* v = d + 1 + sel;
            if (vlen == 0) return s == 0x17 ? Verdict::WrongLength : Verdict::Ok;   // read (hold has none we use)
            switch (s) {
            case 0x14: return vlen == 1 ? (v[0] <= 3 ? Verdict::Ok : Verdict::BadValue) : Verdict::WrongLength;
            case 0x17: return vlen == 1 ? (onOff(v[0]) ? Verdict::Ok : Verdict::BadValue) : Verdict::WrongLength;
            case 0x1A: return vlen == 1 ? (v[0] <= 2 ? Verdict::Ok : Verdict::BadValue) : Verdict::WrongLength;
            case 0x19:
                if (vlen != 3) return Verdict::WrongLength;
                return (bcdRange(v, 2, 0, 2000) && onOff(v[2])) ? Verdict::Ok : Verdict::BadValue;
            case 0x15: {
                if (vlen != 5) return Verdict::WrongLength;
                const int64_t hz = bcdToFreq(v, 5);
                if (hz < 0) return Verdict::BadValue;
                for (int i = 0; i < p.nSpans; i++) if ((int64_t)p.spansHalfHz[i] == hz) return Verdict::Ok;
                return Verdict::BadValue;
            }
            }
            return Verdict::UnknownCommand;
        }
        default:
            return Verdict::UnknownCommand;   // 27 00 is the radio's to SEND, never ours
        }
    }
    default:
        // ★ 17 CW send, 28 TX voice memory, 18 power, 0F split, 0D offset, 09/0B memory write/clear,
        //   0E scan, 1E band edges, 07/08 VFO/memory select … and every byte we have never heard of.
        return Verdict::UnknownCommand;
    }
}
inline Verdict mayTransmit(const Out& o, const Policy& p) { return mayTransmit(o.b, o.n, p); }

// ── Typed builders — the ONLY way a frame comes into existence ─────────────────────────────────
// ★ Each returns an Out; whether it may be sent is still mayTransmit's decision (a frequency
//   outside the ranges builds fine and is refused there — one rule, one reader).
namespace build {
inline Out simple(uint8_t radio, uint8_t c) { Out o; begin(o, radio, c); end(o); return o; }
inline Out sub(uint8_t radio, uint8_t c, uint8_t s) { Out o; begin(o, radio, c); put(o, s); end(o); return o; }
inline Out sub1(uint8_t radio, uint8_t c, uint8_t s, uint8_t v) { Out o; begin(o, radio, c); put(o, s); put(o, v); end(o); return o; }

inline Out readFreq(uint8_t r)  { return simple(r, cmd::ReadFreq); }
inline Out readMode(uint8_t r)  { return simple(r, cmd::ReadMode); }
inline Out readId(uint8_t r)    { return sub(r, cmd::Id, 0x00); }
inline Out readSmeter(uint8_t r){ return sub(r, cmd::Meter, 0x02); }
inline Out readTx(uint8_t r)    { return sub(r, cmd::Tx, 0x00); }      // ★ the READ. There is no write builder.
inline Out readAtt(uint8_t r)   { return simple(r, cmd::Att); }
inline Out readFunc(uint8_t r, uint8_t s)  { return sub(r, cmd::Func, s); }
inline Out readLevel(uint8_t r, uint8_t s) { return sub(r, cmd::Level, s); }
inline Out readDataMode(uint8_t r) { return sub(r, cmd::Misc, 0x06); }
inline Out readMenu(uint8_t r, const uint8_t item[2]) {
    Out o; begin(o, r, cmd::Misc); put(o, 0x05); put(o, item[0]); put(o, item[1]); end(o); return o;
}
inline Out setFreq(uint8_t r, uint64_t hz) {
    Out o; begin(o, r, cmd::SetFreq);
    uint8_t b[5] = {0}; freqToBcd(hz, b);            // an impossible hz encodes as 0 — refused by range
    for (uint8_t x : b) put(o, x);
    end(o); return o;
}
inline Out setMode(uint8_t r, uint8_t m, uint8_t fil) { Out o; begin(o, r, cmd::SetMode); put(o, m); put(o, fil); end(o); return o; }
inline Out setDataMode(uint8_t r, uint8_t on, uint8_t fil) {
    Out o; begin(o, r, cmd::Misc); put(o, 0x06); put(o, on); put(o, fil); end(o); return o;
}
inline Out setAtt(uint8_t r, uint8_t v)          { Out o; begin(o, r, cmd::Att); put(o, v); end(o); return o; }
inline Out setFunc(uint8_t r, uint8_t s, uint8_t v) { return sub1(r, cmd::Func, s, v); }
inline Out setLevel(uint8_t r, uint8_t s, int v) {
    Out o; begin(o, r, cmd::Level); put(o, s);
    uint8_t b[2]; intToBcdBig(v < 0 ? 0 : (v > 255 ? 255 : v), b, 2); put(o, b[0]); put(o, b[1]);
    end(o); return o;
}
inline Out scopeOn(uint8_t r, bool on)     { return sub1(r, cmd::Scope, 0x10, on ? 1 : 0); }
inline Out scopeData(uint8_t r, bool on)   { return sub1(r, cmd::Scope, 0x11, on ? 1 : 0); }
inline Out scopeSel(uint8_t r, uint8_t s, int v, bool selector = true) {
    Out o; begin(o, r, cmd::Scope); put(o, s); if (selector) put(o, 0x00); if (v >= 0) put(o, (uint8_t)v); end(o); return o;
}
inline Out scopeMode(uint8_t r, uint8_t m)  { return scopeSel(r, 0x14, m); }
inline Out scopeSpeed(uint8_t r, uint8_t s) { return scopeSel(r, 0x1A, s); }
inline Out scopeSpan(uint8_t r, uint32_t halfHz) {
    Out o; begin(o, r, cmd::Scope); put(o, 0x15); put(o, 0x00);
    uint8_t b[5]; freqToBcd(halfHz, b); for (uint8_t x : b) put(o, x);
    end(o); return o;
}
inline Out readScope(uint8_t r, uint8_t s)  { return scopeSel(r, s, -1, s != 0x10 && s != 0x11); }
}  // namespace build

// ── The parser — bytes in, frames out ─────────────────────────────────────────────────────────

/** ★ An incremental byte parser. Feed it whatever the transport read; it calls `onFrame` for each
 *  complete frame. Handles: any number of FE before a frame (sync), a jammer (FC) abandoning the
 *  frame in progress, a frame too long for kMaxData (dropped whole, counted), and an FE in the
 *  middle of a frame (a new frame starting — the half one is dropped).
 *  ★ It does NOT decide what is an echo of our own frame: that is `from == kController`, which the
 *    caller checks (the 7300's *CI-V USB Echo Back* may be on or off — plan §1.1). */
class Parser {
public:
    uint64_t framesOk = 0, framesDropped = 0;

    template <class F> void feed(const uint8_t* b, size_t n, F&& onFrame) {
        for (size_t i = 0; i < n; i++) step(b[i], onFrame);
    }
    void reset() { st_ = Idle; pos_ = 0; }

private:
    enum State : uint8_t { Idle, Pre1, Body } st_ = Idle;
    uint8_t buf_[kMaxFrame];
    size_t pos_ = 0;
    Frame fr_;

    template <class F> void step(uint8_t c, F& onFrame) {
        switch (st_) {
        case Idle: if (c == kPreamble) st_ = Pre1; return;
        case Pre1:
            if (c == kPreamble) { st_ = Body; pos_ = 0; } else st_ = Idle;
            return;
        case Body:
            if (c == kPreamble) {             // FE FE <FE…> sync run, or a new frame cut the old one
                if (pos_ > 0) framesDropped++;
                pos_ = 0; return;
            }
            if (c == kJam) { framesDropped++; st_ = Idle; pos_ = 0; return; }
            if (c == kEnd) {
                if (pos_ >= 3) {
                    fr_.to = buf_[0]; fr_.from = buf_[1]; fr_.cmd = buf_[2];
                    fr_.len = (uint16_t)(pos_ - 3);
                    if (fr_.len) std::memcpy(fr_.data, buf_ + 3, fr_.len);
                    framesOk++;
                    onFrame((const Frame&)fr_);
                } else if (pos_ > 0) framesDropped++;
                st_ = Idle; pos_ = 0; return;
            }
            if (pos_ >= sizeof buf_) { framesDropped++; st_ = Idle; pos_ = 0; return; }
            buf_[pos_++] = c;
            return;
        }
    }
};

}  // namespace civ
