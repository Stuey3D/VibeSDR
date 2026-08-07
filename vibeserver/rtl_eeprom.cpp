#include "rtl_eeprom.h"
#include <cstring>

namespace vibe {
namespace {

/** A USB string descriptor: [length][0x03][UTF-16LE chars…]. `length` counts the two header
 *  bytes, so an 8-character serial is 2 + 16 = 18. */
bool readDescriptor(const uint8_t* d, size_t len, size_t at, std::string& out, size_t& next,
                    const char* what, std::string& err) {
    if (at + 2 > len) { err = std::string("the ") + what + " descriptor runs off the end"; return false; }
    const size_t n = d[at];
    const uint8_t type = d[at + 1];
    // ★ EVERY ONE OF THESE IS A REASON TO REFUSE, NOT TO GUESS. We are about to write this chip.
    if (type != 0x03) { err = std::string("the ") + what + " descriptor is not a string"; return false; }
    if (n < 2 || (n & 1)) { err = std::string("the ") + what + " descriptor has an odd length"; return false; }
    if (at + n > len) { err = std::string("the ") + what + " descriptor runs off the end"; return false; }
    out.clear();
    for (size_t i = at + 2; i + 1 < at + n; i += 2) {
        const uint16_t ch = (uint16_t)(d[i] | (d[i + 1] << 8));
        // Anything outside plain ASCII means this is not an image we understand well enough to
        // rewrite. Refusing costs the user a rename; guessing costs them the dongle.
        if (ch == 0 || ch > 0x7e) { err = std::string("the ") + what + " is not plain text"; return false; }
        out.push_back((char)ch);
    }
    next = at + n;
    return true;
}

void appendDescriptor(std::vector<uint8_t>& v, const std::string& s) {
    v.push_back((uint8_t)(2 + s.size() * 2));
    v.push_back(0x03);
    for (char c : s) { v.push_back((uint8_t)c); v.push_back(0); }
}

}  // namespace

bool rtlEepromParse(const uint8_t* data, size_t len, RtlEeprom& out, std::string& err) {
    if (!data || len < RTL_EEPROM_STR_AT + 6) { err = "the EEPROM is too short to be real"; return false; }
    // ★★ THE MAGIC IS THE GATE. Without it we could be looking at an unprogrammed chip, a
    //    different device, or a failed read — and writing a "fixed up" version of any of those is
    //    how a working dongle becomes a paperweight.
    if (data[0] != 0x28 || data[1] != 0x32) {
        err = "this does not look like an RTL-SDR EEPROM (wrong signature)";
        return false;
    }
    out.raw.assign(data, data + len);
    out.vendorId  = (uint16_t)(data[2] | (data[3] << 8));
    out.productId = (uint16_t)(data[4] | (data[5] << 8));
    out.hasSerial = (data[6] == 0xa5);

    size_t at = RTL_EEPROM_STR_AT, next = 0;
    if (!readDescriptor(data, len, at, out.manufacturer, next, "manufacturer", err)) return false;
    at = next;
    if (!readDescriptor(data, len, at, out.product, next, "product", err)) return false;
    at = next;
    if (!readDescriptor(data, len, at, out.serial, next, "serial", err)) return false;
    return true;
}

bool rtlSerialAcceptable(const std::string& s, std::string& err) {
    if (s.empty()) { err = "the serial cannot be empty"; return false; }
    for (unsigned char c : s) {
        // ★ No spaces: this ends up in USB descriptors, in file names and as a config key. A
        //   serial with a space in it works right up until one of those, and then it does not.
        if (c < 0x21 || c > 0x7e) { err = "use letters, digits and punctuation only — no spaces"; return false; }
    }
    // The three descriptors share the space from 0x09 to the end of the chip.
    if (RTL_EEPROM_STR_AT + 6 + s.size() * 2 > RTL_EEPROM_SIZE) { err = "that serial is too long"; return false; }
    return true;
}

bool rtlEepromWithSerial(const RtlEeprom& in, const std::string& newSerial,
                         std::vector<uint8_t>& out, std::string& err) {
    if (!rtlSerialAcceptable(newSerial, err)) return false;
    if (in.raw.size() != RTL_EEPROM_SIZE) { err = "the EEPROM image is the wrong size"; return false; }

    out.assign(in.raw.begin(), in.raw.begin() + RTL_EEPROM_STR_AT);
    // ★★ SAY THERE IS A SERIAL. A dongle whose serial we just set but whose have-serial flag is
    //    still clear reports no serial at all — which is precisely the ambiguity being fixed here.
    out[6] = 0xa5;
    appendDescriptor(out, in.manufacturer);
    appendDescriptor(out, in.product);
    appendDescriptor(out, newSerial);

    if (out.size() > RTL_EEPROM_SIZE) {
        err = "the manufacturer, product and serial do not fit together";
        return false;
    }
    // ★ Zero-fill the tail, which is what an unused region of these chips holds anyway. Carrying
    //   the old bytes forward would leave a fragment of the previous, longer serial behind them.
    out.resize(RTL_EEPROM_SIZE, 0x00);
    return true;
}

}  // namespace vibe
