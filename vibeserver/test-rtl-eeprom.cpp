// ★★★ THE BYTES ARE PROVEN HERE, BEFORE ANY DONGLE IS WRITTEN.
//
// A bad EEPROM write bricks the device, so the image manipulation is tested against a REAL dump
// — the RTL-SDR Blog V4 on the Pi, read 2026-08-07 — rather than against something hand-written to
// match the parser's assumptions. Half of these cases are deliberately malformed images: a parser
// that accepts a broken EEPROM is worse than no parser, because it will happily write back its
// own misunderstanding.
#include "rtl_eeprom.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

/** The genuine article: RTLSDRBlog / "Blog V4" / "00000003". */
static std::vector<uint8_t> realImage() {
    static const char* hex =
        "2832da0b3828a516021603520054004c0053004400520042006c006f006700100342006c006f00"
        "6700200056003400120330003000300030003000300030003300";
    std::vector<uint8_t> v;
    for (const char* p = hex; *p && p[1]; p += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        v.push_back((uint8_t)((nib(p[0]) << 4) | nib(p[1])));
    }
    v.resize(vibe::RTL_EEPROM_SIZE, 0x00);
    return v;
}

int main() {
    std::string err;

    std::printf("\nReading a real RTL-SDR Blog V4 image\n");
    vibe::RtlEeprom e;
    const auto img = realImage();
    ok(vibe::rtlEepromParse(img.data(), img.size(), e, err), "it parses", err);
    ok(e.vendorId == 0x0bda, "vendor is Realtek (0x0bda)");
    ok(e.productId == 0x2838, "product is 0x2838");
    ok(e.manufacturer == "RTLSDRBlog", "manufacturer is RTLSDRBlog", e.manufacturer);
    ok(e.product == "Blog V4", "product is 'Blog V4'", e.product);
    ok(e.serial == "00000003", "serial is 00000003", e.serial);
    ok(e.hasSerial, "the have-serial flag is set");

    std::printf("\nRewriting the serial and NOTHING else\n");
    std::vector<uint8_t> out;
    ok(vibe::rtlEepromWithSerial(e, "vibe-rsp-01", out, err), "it rebuilds", err);
    ok(out.size() == vibe::RTL_EEPROM_SIZE, "still exactly 256 bytes");
    ok(std::memcmp(out.data(), img.data(), 6) == 0, "★ the header, VID and PID are untouched");
    vibe::RtlEeprom back;
    ok(vibe::rtlEepromParse(out.data(), out.size(), back, err), "the result parses again", err);
    ok(back.serial == "vibe-rsp-01", "★ the new serial is there", back.serial);
    ok(back.manufacturer == e.manufacturer, "★ the manufacturer survived", back.manufacturer);
    ok(back.product == e.product, "★ the product survived", back.product);
    ok(back.hasSerial, "the have-serial flag is set");

    std::printf("\nA SHORTER serial must not leave the old one trailing behind it\n");
    {
        std::vector<uint8_t> shortOut;
        vibe::RtlEeprom longSerial = e;
        ok(vibe::rtlEepromWithSerial(e, "a", shortOut, err), "rebuilds with a 1-character serial", err);
        vibe::RtlEeprom r;
        ok(vibe::rtlEepromParse(shortOut.data(), shortOut.size(), r, err), "and parses", err);
        ok(r.serial == "a", "the serial is exactly 'a'", r.serial);
        // ★ THE ASSERTION THAT MATTERS: everything past the three descriptors must be clean, or a
        //   fragment of the old serial sits in the chip and confuses the next reader.
        size_t used = vibe::RTL_EEPROM_STR_AT + (2 + r.manufacturer.size() * 2)
                                              + (2 + r.product.size() * 2) + (2 + r.serial.size() * 2);
        bool clean = true;
        for (size_t i = used; i < shortOut.size(); i++) if (shortOut[i] != 0) clean = false;
        ok(clean, "★ no remnant of the previous serial is left in the chip");
    }

    std::printf("\nRefusing what must be refused\n");
    {
        auto bad = img; bad[0] = 0x00;
        vibe::RtlEeprom junk;
        ok(!vibe::rtlEepromParse(bad.data(), bad.size(), junk, err), "★ a wrong signature is refused");
    }
    {
        auto bad = img; bad[9] = 0xfe;      // manufacturer length runs past the end
        vibe::RtlEeprom junk;
        ok(!vibe::rtlEepromParse(bad.data(), bad.size(), junk, err), "★ a descriptor running off the end is refused");
    }
    {
        auto bad = img; bad[10] = 0x01;     // not a string descriptor
        vibe::RtlEeprom junk;
        ok(!vibe::rtlEepromParse(bad.data(), bad.size(), junk, err), "★ a non-string descriptor is refused");
    }
    {
        auto bad = img; bad[9] = 21;        // odd length
        vibe::RtlEeprom junk;
        ok(!vibe::rtlEepromParse(bad.data(), bad.size(), junk, err), "★ an odd descriptor length is refused");
    }
    {
        vibe::RtlEeprom junk;
        ok(!vibe::rtlEepromParse(img.data(), 4, junk, err), "★ a short read is refused");
    }

    std::printf("\nRefusing serials we should not write\n");
    ok(!vibe::rtlSerialAcceptable("", err), "★ empty is refused");
    ok(!vibe::rtlSerialAcceptable("has space", err), "★ a space is refused");
    ok(!vibe::rtlSerialAcceptable(std::string(200, 'x'), err), "★ too long is refused");
    ok(vibe::rtlSerialAcceptable("vibe-01", err), "a sensible one is accepted", err);
    {
        std::vector<uint8_t> o;
        ok(!vibe::rtlEepromWithSerial(e, std::string(200, 'x'), o, err),
           "★ an over-long serial fails the rebuild rather than truncating");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
