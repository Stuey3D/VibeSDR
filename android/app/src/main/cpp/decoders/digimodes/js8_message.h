// VibeSDR V12 — JS8 frames: what the 75 bits of a decoded JS8 transmission say (2026-10-26).
//
// ★★ THE FRAME. 72 message bits (FT8's first 75-bit message, less its 3-bit type field) + 3 flag bits carried
//  beside them ("i3": 1 = first frame of a message, 2 = last, 4 = DATA — a frame with no header of its own).
//  When the DATA flag is clear the first three message bits say what the frame is:
//    000 heartbeat / CQ   [3][50 call][16 extra][3 kind]   — extra bit 15 set = CQ, the low 15 bits a grid
//    001 compound         [3][50 call][16 extra][3]        — a call too long for 28 bits ("VE3/LB9YHX"), + grid
//    010 compound directed — the same, the extra a command (and an SNR)
//    011 directed         [3][28 from][28 to][5 command][1 from /P][1 to /P][6 number]
//    1xx data             [1][1 compressed][text bits][0][1 … 1]  — text, a frame of a longer message
//  ★ Text: an UNCOMPRESSED data frame is a fixed Huffman code (44 characters, below); a COMPRESSED one — and every
//    frame with the DATA flag — uses JS8's word dictionary (JSC, 262,144 entries). That dictionary is a big table
//    of JS8Call's (GPL-3) that cannot be derived, so it is not here: such a frame decodes (its CRC checks, it is
//    counted) but its text is reported as unknown. Every callsign, grid, CQ, heartbeat and command is decoded —
//    they are what a spot is made of; the dictionary only carries free text after the first frame.
//  ★ Everything here is written from the frame's meaning (bit positions, radices, the 6-bit call alphabet); the
//    group names, command words and the Huffman code are protocol facts, typed in and tested (test-js8).
#pragma once
#include <cstdint>
#include <string>

namespace vibe {
namespace js8 {

enum class FrameKind { Heartbeat, Cq, Compound, CompoundDirected, Directed, Data, DataCompressed };
const char* frameKindName(FrameKind k);

/** The flags beside the 72 bits. */
enum : int { kFlagFirst = 1, kFlagLast = 2, kFlagData = 4 };

struct Frame {
    FrameKind kind = FrameKind::Data;
    int  flags = 0;              ///< i3
    std::string from;            ///< the sender ("" for a data frame)
    std::string to;              ///< directed: the addressee, a call or a group ("@ALLCALL")
    std::string cmd;             ///< directed/compound directed: " SNR", " HEARING?", " " (free text follows)
    std::string num;             ///< a number with the command: "-12" ("+05" for an SNR)
    std::string grid;            ///< a 4-character locator, when one was sent
    std::string text;            ///< the line as a JS8 operator reads it ("G4ABC: @HB HEARTBEAT IO91")
    bool textKnown = true;       ///< false: a dictionary-compressed text frame (see above)
};

/** `bits72`: the 72 message bits, one per byte. False when the frame cannot be a JS8 frame (a heartbeat with no
 *  callsign, a data frame with no text) — counted as no decode. */
bool unpack(const uint8_t* bits72, int flags, Frame& out);

// ── Packing (for the tests and the synthetic bench: what a JS8 station would transmit) ──
/** A heartbeat (cqIndex < 0) or a CQ (cqIndex 0-7: "CQ CQ CQ", "CQ DX" …) from `call` with a 4-character grid. */
bool packHeartbeat(const std::string& call, const std::string& grid, int cqIndex, uint8_t* bits72);
/** A directed frame: command index 0-31 (31 = free text follows), number -30..31 or NaN-like 99 = none. */
bool packDirected(const std::string& from, const std::string& to, int cmdIndex, int num, uint8_t* bits72);
/** An uncompressed (Huffman) text frame; returns how many characters of `text` fitted. */
int  packHuffText(const std::string& text, uint8_t* bits72);

/** The pieces, exposed for the tests. */
uint32_t packCall28(const std::string& call, bool* portable);    ///< 0 = not packable
std::string unpackCall28(uint32_t v, bool portable);
uint64_t packCall50(const std::string& call);                     ///< 0 = not packable
std::string unpackCall50(uint64_t v);
uint16_t packGrid15(const std::string& grid);                     ///< 32767 = none
std::string unpackGrid15(uint16_t v);
const char* commandWord(int index);                               ///< 0-31
/** A callsign a spot can be made of: a real amateur call (letters and a digit), not a group, not "<....>". */
bool isSpotCall(const std::string& call);

} // namespace js8
} // namespace vibe
