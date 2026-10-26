// win_switch.h — which RTL-SDR sticks VibeServer may OFFER to switch to SDR, the words it uses, and the rules the
// elevated helper enforces (Windows only). Pure logic over UsbNode records: no SetupAPI calls here, so every rule is
// unit-tested with made-up devices (windows/test/test-win-switch.cpp).
//
// ★★★ STUART (2026-10-26): "ONLY show a stick that is showing as a TV stick." So, per RTL radio node:
//       USABLE    — WinUSB / libusbK / libusb0 (Zadig, SDR#, SDR++, or our own switch) or UsbDk: used silently,
//                   never offered anything. One WE switched (WinUSB + our interface GUID) gets a Give back.
//       TV STICK  — bound to the BDA / DVB-T TV driver: the ONLY kind offered "Switch it to SDR?".
//       NO DRIVER — Windows found none (state a1, usually problem code 28): offered exactly like a TV stick.
//       OTHER     — anything else (a failed driver, another program's driver): explained, never offered.
// ★★ The helper (vibeserver.exe --winusb-bind <instance id>, elevated) trusts NOTHING from the un-elevated server:
//    it re-scans with SetupAPI itself and refuses any instance id that is not, right now, an RTL-SDR's radio node in
//    an offerable state. The fake-device seam below is for the un-elevated server's tests and is never consulted by
//    the helper.
#pragma once
#ifdef _WIN32
#include <string>
#include <vector>
#include "win_usb.h"

namespace vibewin {

enum class DongleState { Usable, TvStick, NoDriver, Other };
const char* dongleStateName(DongleState s);   // "usable" | "tv" | "nodriver" | "other"

struct Dongle {
    std::string instanceId, serial, name;     // name: librtlsdr's model name, e.g. "Generic RTL2832U OEM"
    DongleState state = DongleState::Other;
    bool        switchedByUs = false;         // WinUSB from winusb.inf with VibeServer's interface: Give back offered
    std::string driverLabel;                  // "TV tuner (DVB-T/BDA)", "WinUSB", …
    std::string why;                          // plain words for OTHER (and a note for the rest)
};

/** Classify one node. Only meaningful for an RTL-SDR's radio node (kind Rtl, sdrNode). */
DongleState classify(const UsbNode& n, bool& switchedByUs, std::string& why);
inline bool offerable(DongleState s) { return s == DongleState::TvStick || s == DongleState::NoDriver; }

/** Every RTL-SDR radio node in `nodes`, classified, in scan order. Composite parents and other interfaces are left
 *  out (the IR receiver on interface 1 is not a radio). */
std::vector<Dongle> rtlDongles(const std::vector<UsbNode>& nodes);

/** "Generic RTL2832U OEM, serial 00000001" — or without the serial when Windows made the id up. */
std::string describe(const Dongle& d);
/** The offer, word for word (Stuart's brief). Empty for anything not offerable — the guard is here, not in callers. */
std::string offerText(const Dongle& d);
/** The console line for an offerable dongle; `port` is the setup page's. Empty for anything not offerable. */
std::string consoleOffer(const Dongle& d, int port);

/** JSON array of the dongles for the setup page (no secrets: names, serials, states). */
std::string donglesJson(const std::vector<Dongle>& ds);

// ── the test seam ────────────────────────────────────────────────────────────────────────────────────────────────
/** VIBE_WIN_FAKE_USB=<file>: the un-elevated server reads its device list from this file instead of SetupAPI (Wine
 *  has no USB). One device per line, '|'-separated:
 *      instanceId|service|class|problem|infPath|ours
 *  e.g. USB\VID_0BDA&PID_2838&MI_00\7&1&0&0000|RTL2832UBDA|MEDIA|0|oem12.inf|0
 *  The serial comes from a parent id "USB\VID_0BDA&PID_2838\<serial>" given as an optional 7th field.
 *  '#' starts a comment. Returns false (with `err`) on a malformed line. */
bool parseFakeDevices(const std::string& text, std::vector<UsbNode>& out, std::string& err);
/** Fill the derived fields (vid/pid/mi/kind/model/driver/sdrNode/serial) of a node read from text. */
void deriveNode(UsbNode& n);

// ── the elevated helper's command line ───────────────────────────────────────────────────────────────────────────
enum class HelperOp { None, Bind, GiveBack, AutostartBoot, AutostartOff };
struct HelperArgs {
    HelperOp    op = HelperOp::None;
    std::string instanceId;   // Bind / GiveBack
    std::string replyPipe;    // \\.\pipe\vibeserver-helper-…  (required)
    std::string grantSid;     // AutostartBoot: the account that runs VibeServer by hand keeps write access
    std::string error;        // set when the line is refused
};
/** True when argv asks for one of the helper operations (checked before anything else in main). */
bool isHelperInvocation(int argc, char** argv);
/** Parse and VALIDATE: exactly one operation, each value well-formed, nothing else on the line. */
HelperArgs parseHelperArgs(int argc, char** argv);
/** The reverse, for the server: the parameter string handed to ShellExecuteEx (each value quoted; values are
 *  validated first, so none contains a quote). Empty if a value is not acceptable. */
std::string helperCommandLine(const HelperArgs& a);

bool validInstanceId(const std::string& id);   // USB\VID_xxxx&PID_xxxx[&MI_xx]\<id part>, no quotes, ≤ 200 chars
bool validPipeName(const std::string& p);      // \\.\pipe\vibeserver-helper-[0-9a-z-]{1,64}
bool validSidString(const std::string& s);     // S-1-<digits>(-<digits>)+, ≤ 184 chars

/** The helper's checks against the REAL device list (it scanned itself). `why` says what was refused. */
bool helperMayBind(const std::vector<UsbNode>& realNodes, const std::string& instanceId, std::string& why);
bool helperMayGiveBack(const std::vector<UsbNode>& realNodes, const std::string& instanceId, std::string& why);

/** A JSON string literal's contents → text (\" \\ \/ \n \t \uXXXX for ASCII); tiny, for our own POST bodies. */
std::string jsonUnescape(const std::string& s);
/** The value of "key":"…" in a small flat JSON object, unescaped; "" when absent. */
std::string jsonField(const std::string& body, const char* key);
std::string jsonEsc(const std::string& s);

}  // namespace vibewin
#endif
