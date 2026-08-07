// rtl_eeprom.h — read, understand and safely rewrite an RTL-SDR dongle's EEPROM.
//
// ★★★ WHY THIS EXISTS. Two RTL dongles of the same model are indistinguishable: they ship with the
//     same factory serial (stock Realtek ones are all "00000001"). Any server that binds settings
//     to a radio therefore binds them to the WRONG radio when a second identical dongle appears —
//     and those settings include a locked frequency range, so the failure puts a receiver
//     somewhere its owner never agreed to.
//
//     The fix is to give each dongle a unique serial, which is a normal step for anyone running
//     several — OpenWebRX requires it too. But there was nowhere to DO it: Stuart had to boot a
//     Windows PC and use SDR Console to rename one (2026-08-07). A receiver that asks you to find
//     another operating system to finish setting it up is not finished.
//
// ★★ THIS IS THE ONE DESTRUCTIVE OPERATION IN THE WHOLE PRODUCT. A bad EEPROM write bricks the
//    dongle — not "resets it", bricks it. So the parsing and rebuilding live here, separately from
//    any USB code, and are covered by tests against a REAL dumped image. The bytes are proven
//    before hardware is ever touched; the write itself is then backup → write → read back → verify.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vibe {

/** The layout, as librtlsdr's own rtl_eeprom defines it. */
enum : size_t {
    RTL_EEPROM_SIZE   = 256,
    RTL_EEPROM_STR_AT = 0x09,   // three USB string descriptors, back to back
};

struct RtlEeprom {
    std::vector<uint8_t> raw;          // exactly as read
    uint16_t vendorId = 0, productId = 0;
    bool     hasSerial = false;
    std::string manufacturer, product, serial;
};

/** Parse a 256-byte image. Refuses anything it does not fully understand — a half-understood
 *  EEPROM must never be written back, because the parts we did not understand are the parts we
 *  would destroy. */
bool rtlEepromParse(const uint8_t* data, size_t len, RtlEeprom& out, std::string& err);

/** Rebuild the image with a new serial and NOTHING else changed. Fails rather than truncating if
 *  the serial does not fit. */
bool rtlEepromWithSerial(const RtlEeprom& in, const std::string& newSerial,
                         std::vector<uint8_t>& out, std::string& err);

/** Is this a serial we are willing to write? ASCII, printable, no spaces (it travels in USB
 *  descriptors and shows up in file names and config keys), and short enough to fit. */
bool rtlSerialAcceptable(const std::string& s, std::string& err);

}  // namespace vibe
