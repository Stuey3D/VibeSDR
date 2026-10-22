// icom_models.h — what each Icom IS, as data: address, scope caps, receive ranges, modes, the
// span ladder and the preflight menu items. One table, read by IcomSource (to build the CI-V
// allow-list Policy and hwinfo) and by the setup page (through hwinfo). Nothing else hard-codes a
// property of a radio (feedback_app_no_hardcoded_settings: the client draws only what this says).
//
// ★★ HOW SURE EACH ROW IS — the same labels as docs/ICOM-SUPPORT-PLAN.md:
//    IC-7300 is the row Phase 1 is built for. Its address, scope caps, spans, ranges and command
//    set are [verified] (Hamlib ic7300.c as a facts reference, Icom's CI-V guide). Its PREFLIGHT
//    MENU ITEM NUMBERS are [unverified]: `menuVerified = false` until Saber's capture
//    (tools/icom/civ-capture.py --menu-scan) pins them, and the setup page says so in plain words.
//    The other rows are [unverified] beyond their CI-V address and scope point count, and exist so
//    that a radio we have never seen is NAMED correctly and refused safely rather than mistaken for
//    a 7300. ★ A radio that is not in this table is refused at open (we would not know its ranges,
//    so we could not check a single 05).
//
// ★★★ WHY A WRONG MENU NUMBER CANNOT KEY THE RADIO. The preflight READS items; it never writes one
//     (civ_codec.h refuses every 1A 05 with a value). A wrong number reads the wrong item, which is
//     either NG (FA) — preflight fails CLOSED — or some other item's value, which can falsely pass.
//     The false pass is why the preflight is the SECOND wall, not the first: on the libusb path DTR
//     and RTS are never asserted at all (icom_transport.cpp), so USB SEND = RTS has nothing to key
//     from. And VOX, the audio path, is read by its own command (16 46), not by menu number.
#pragma once
#include <cstdint>

namespace icom {

/** A preflight menu item: read by 1A 05 <hi> <lo>; "safe" means the value read is 00 (OFF). */
struct MenuItem {
    uint8_t item[2];
    const char* name;       ///< as the radio's menu shows it
    const char* path;       ///< where the owner finds it, in plain words
    bool gate;              ///< true = a non-OFF value REFUSES the start (keying from a line)
};

struct Range { uint64_t lo, hi; };

/** The factory filter widths per mode family, Hz — FIL1/FIL2/FIL3. ★ Defaults: an owner can change
 *  them in the radio and CI-V does not report them, so a client draws them as approximate. */
struct Widths { const char* mode; int fil[3]; };

struct Model {
    uint8_t     addr;               ///< the default CI-V address — also the 19 00 ID reply
    const char* name;
    bool        known;              ///< false = a placeholder row: refused at open
    bool        canTransmit;        ///< the IC-R8600 is a receiver; every other row is a transceiver
    int         points;             ///< scope points per sweep
    int         ampMax;             ///< amplitude full scale (0..ampMax)
    double      dbRange;            ///< what 0..ampMax spans, dB (Hamlib maps the 7300's 0-160 to 80 dB)
    Range       ranges[4]; int nRanges;
    const char* modes[12]; uint8_t modeBytes[12]; int nModes;   ///< our name ↔ the 06 byte
    uint8_t     att[3]; int nAtt;
    int         preampMax;
    int         dataModeMax;
    uint32_t    spansHalfHz[8]; int nSpans;   ///< the 27 15 ladder (HALF-spans: ±)
    MenuItem    menu[5]; int nMenu;
    bool        menuVerified;
    Widths      widths[5]; int nWidths;
};

/** Our mode names. ★ "usbd"/"lsbd" are USB/LSB with DATA on (1A 06 01) — the FT8 mode. */
inline const Model* models(int& n) {
    static const Model k[] = {
        {0x94, "IC-7300", true, true, 475, 160, 80.0,
         {{30000, 74800000}}, 1,
         {"lsb", "usb", "am", "cw", "rtty", "fm", "cwr", "rttyr", "usbd", "lsbd"},
         {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x08, 0x01, 0x00}, 10,
         {0x00, 0x20}, 2, 2, 1,
         {2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000}, 8,
         {{{0x00, 0x89}, "USB SEND", "MENU > SET > Connectors > USB SEND/Keying > USB SEND", true},
          {{0x00, 0x90}, "USB Keying (CW)", "MENU > SET > Connectors > USB SEND/Keying > USB Keying (CW)", true},
          {{0x00, 0x91}, "USB Keying (RTTY)", "MENU > SET > Connectors > USB SEND/Keying > USB Keying (RTTY)", true},
          {{0x00, 0x67}, "DATA MOD", "MENU > SET > Connectors > DATA MOD", false}}, 4,
         /*menuVerified=*/false,
         {{"ssb", {3000, 2400, 1800}}, {"cw", {1200, 500, 250}}, {"rtty", {2400, 500, 250}},
          {"am", {9000, 6000, 3000}}, {"fm", {15000, 10000, 7000}}}, 5},
        // ★ [unverified] beyond address + points. Refused at open until someone with one tests it.
        {0xB6, "IC-7300MK2", false, true, 475, 160, 80.0, {{30000, 74800000}}, 1,
         {}, {}, 0, {0x00, 0x20}, 2, 2, 1, {}, 0, {}, 0, false, {}, 0},
        {0xA4, "IC-705",  false, true, 475, 160, 80.0, {{30000, 199999999}, {400000000, 470000000}}, 2,
         {}, {}, 0, {0x00, 0x20}, 2, 2, 3, {}, 0, {}, 0, false, {}, 0},
        {0xA2, "IC-9700", false, true, 475, 160, 80.0, {{144000000, 148000000}, {430000000, 450000000}, {1240000000, 1300000000}}, 3,
         {}, {}, 0, {0x00, 0x10}, 2, 1, 3, {}, 0, {}, 0, false, {}, 0},
        {0x98, "IC-7610", false, true, 689, 200, 80.0, {{30000, 60000000}}, 1,
         {}, {}, 0, {0x00, 0x06, 0x12}, 3, 2, 3, {}, 0, {}, 0, false, {}, 0},
        {0x96, "IC-R8600", false, false, 475, 160, 80.0, {{10000, 3000000000ULL}}, 1,
         {}, {}, 0, {0x00, 0x10}, 2, 2, 0, {}, 0, {}, 0, false, {}, 0},
    };
    n = (int)(sizeof k / sizeof k[0]);
    return k;
}

inline const Model* modelByAddr(uint8_t addr) {
    int n = 0; const Model* m = models(n);
    for (int i = 0; i < n; i++) if (m[i].addr == addr) return &m[i];
    return nullptr;
}
/** ★ By the USB serial string the radio's own CP2102 carries ("IC-7300 03001234") — the model is
 *  the text before the space. See radios.cpp: this is how an Icom is told from any other CP2102
 *  WITHOUT opening it or sending it a byte. */
inline const Model* modelBySerial(const char* serial) {
    if (!serial) return nullptr;
    int n = 0; const Model* m = models(n);
    const Model* best = nullptr; size_t bestLen = 0;
    for (int i = 0; i < n; i++) {
        size_t l = 0; while (m[i].name[l]) l++;
        bool match = true;
        for (size_t k = 0; k < l; k++) if (serial[k] != m[i].name[k]) { match = false; break; }
        // ★ The LONGEST match, so "IC-7300MK2 …" is not read as an IC-7300; and the name must end
        //   at a word boundary ("IC-7300 0300…"), not run on into another model's digits.
        if (match && (serial[l] == ' ' || serial[l] == 0 || serial[l] == '_') && l > bestLen) { best = &m[i]; bestLen = l; }
    }
    return best;
}
inline int modeIndex(const Model& m, const char* name) {
    for (int i = 0; i < m.nModes; i++) {
        const char* a = m.modes[i]; const char* b = name; bool eq = true;
        while (*a || *b) { if (*a++ != *b++) { eq = false; break; } }
        if (eq) return i;
    }
    return -1;
}
/** Our name for an 06 mode byte (+ data flag), or "" if the model does not list it. */
inline const char* modeName(const Model& m, uint8_t b, bool data) {
    if (data) { if (b == 0x01) return "usbd"; if (b == 0x00) return "lsbd"; }
    for (int i = 0; i < m.nModes; i++) {
        const char* n = m.modes[i];
        const bool isData = n[0] && n[1] && n[2] && n[3] == 'd';
        if (!isData && m.modeBytes[i] == b) return n;
    }
    return "";
}
/** The filter-width family a mode belongs to (for Widths lookup). */
inline const char* widthFamily(const char* mode) {
    switch (mode[0]) {
        case 'l': case 'u': return "ssb";
        case 'c': return "cw";
        case 'r': return "rtty";
        case 'a': return "am";
        case 'f': return "fm";
    }
    return "ssb";
}

}  // namespace icom
