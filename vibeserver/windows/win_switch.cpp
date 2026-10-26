// win_switch.cpp — see win_switch.h. Windows only; pure logic (no SetupAPI).
#ifdef _WIN32
#include "win_switch.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace vibewin {
namespace {

std::string upperS(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }

unsigned hex4After(const std::string& id, const char* tag, int digits) {
    const size_t p = id.find(tag);
    if (p == std::string::npos) return 0xFFFFFFFFu;
    const std::string h = id.substr(p + strlen(tag), (size_t)digits);
    if ((int)h.size() != digits) return 0xFFFFFFFFu;
    for (char c : h) if (!isxdigit((unsigned char)c)) return 0xFFFFFFFFu;
    return (unsigned)strtoul(h.c_str(), nullptr, 16);
}

bool endsWithI(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && !_stricmp(s.c_str() + s.size() - n, tail);
}

bool isLibusbDriver(Driver d) { return d == Driver::WinUsb || d == Driver::LibusbK || d == Driver::Libusb0; }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out; size_t a = 0;
    for (;;) { const size_t b = s.find(sep, a); out.push_back(s.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
    return out;
}

const UsbNode* byId(const std::vector<UsbNode>& nodes, const std::string& id) {
    for (const auto& n : nodes) if (!_stricmp(n.instanceId.c_str(), id.c_str())) return &n;
    return nullptr;
}

}  // namespace

const char* dongleStateName(DongleState s) {
    switch (s) {
        case DongleState::Usable: return "usable"; case DongleState::TvStick: return "tv";
        case DongleState::NoDriver: return "nodriver"; default: return "other";
    }
}

DongleState classify(const UsbNode& n, bool& switchedByUs, std::string& why) {
    switchedByUs = false; why.clear();
    if (isLibusbDriver(n.driver) || n.driver == Driver::UsbDk) {
        if (n.hasProblem) {
            // ★ On a libusb driver but not started (code 10, 43 …): switching again would not help; say what Windows says.
            why = std::string("on ") + driverName(n.driver) + ", but Windows reports a problem with it (code "
                + std::to_string(n.problem) + ") — unplug it and plug it back in";
            return DongleState::Other;
        }
        switchedByUs = n.driver == Driver::WinUsb && n.ourInterface && endsWithI(n.infPath, "winusb.inf");
        why = switchedByUs ? "switched to SDR by VibeServer" : std::string("ready (") + driverName(n.driver) + ")";
        return DongleState::Usable;
    }
    if (n.driver == Driver::TvTuner) {
        // ★ A TV driver that failed to start (code 10, or 52 for a driver Windows would not load) is still a TV stick,
        //   and switching it is exactly the cure — so it is offered, with the problem noted.
        why = n.hasProblem ? "Windows' TV driver for it reports code " + std::to_string(n.problem) : "Windows is using it as a TV tuner";
        return DongleState::TvStick;
    }
    if (n.driver == Driver::None) {
        // ★ No driver: code 28 (none installed), 1 (not configured) or no problem recorded yet. Any OTHER code with
        //   no driver is a device that is failing at the USB level (43: descriptor request failed) — a driver
        //   switch cannot fix that, so it is explained, not offered.
        if (!n.hasProblem || n.problem == 28 || n.problem == 1) { why = "Windows has no driver for it"; return DongleState::NoDriver; }
        why = "Windows reports a fault with this dongle (code " + std::to_string(n.problem)
            + ") — try another USB port or cable";
        return DongleState::Other;
    }
    why = "it is on another driver (" + (n.service.empty() ? std::string(driverName(n.driver)) : n.service)
        + "); VibeServer leaves it alone";
    return DongleState::Other;
}

std::vector<Dongle> rtlDongles(const std::vector<UsbNode>& nodes) {
    std::vector<Dongle> out;
    for (const auto& n : nodes) {
        if (n.kind != Kind::Rtl || !n.sdrNode || n.compositeParent) continue;
        Dongle d;
        d.instanceId = n.instanceId; d.serial = n.serial; d.name = n.model.empty() ? "RTL-SDR" : n.model;
        d.state = classify(n, d.switchedByUs, d.why);
        d.driverLabel = driverName(n.driver);
        out.push_back(d);
    }
    return out;
}

std::string describe(const Dongle& d) {
    return d.serial.empty() ? d.name : d.name + ", serial " + d.serial;
}

std::string offerText(const Dongle& d) {
    if (d.state == DongleState::TvStick)
        return "Windows is using this dongle as a TV tuner (" + describe(d) + "). Switch it to SDR so VibeServer can "
               "use it? You can switch it back any time.";
    if (d.state == DongleState::NoDriver)
        return "Windows has no driver for this dongle (" + describe(d) + "). Switch it to SDR so VibeServer can "
               "use it? You can switch it back any time.";
    return {};
}

std::string consoleOffer(const Dongle& d, int port) {
    const std::string t = offerText(d);
    if (t.empty()) return {};
    return "VibeServer: " + t + "\n            → open http://localhost:" + std::to_string(port)
         + "/setup and press SWITCH (Windows asks once for permission), or NOT NOW.";
}

std::string jsonEsc(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string donglesJson(const std::vector<Dongle>& ds) {
    std::string j = "[";
    for (size_t i = 0; i < ds.size(); ++i) {
        const auto& d = ds[i];
        if (i) j += ",";
        j += "{\"id\":\"" + jsonEsc(d.instanceId) + "\",\"name\":\"" + jsonEsc(d.name) + "\",\"serial\":\"" + jsonEsc(d.serial)
           + "\",\"state\":\"" + dongleStateName(d.state) + "\",\"offer\":" + (offerable(d.state) ? "true" : "false")
           + ",\"switchedByUs\":" + (d.switchedByUs ? "true" : "false")
           + ",\"driver\":\"" + jsonEsc(d.driverLabel) + "\",\"why\":\"" + jsonEsc(d.why)
           + "\",\"offerText\":\"" + jsonEsc(offerText(d)) + "\"}";
    }
    return j + "]";
}

void deriveNode(UsbNode& n) {
    const std::string uid = upperS(n.instanceId);
    n.vid = hex4After(uid, "VID_", 4); n.pid = hex4After(uid, "PID_", 4);
    const unsigned mi = hex4After(uid, "&MI_", 2);
    n.mi = mi <= 0xFF ? (int)mi : -1;
    if (n.vid > 0xFFFF || n.pid > 0xFFFF || !identifyRadio(n.vid, n.pid, n.kind, n.model)) { n.kind = Kind::Other; n.model.clear(); }
    n.driver = classifyDriver(n.service, n.className);
    n.compositeParent = n.mi < 0 && n.driver == Driver::Composite;
    n.sdrNode = (n.mi == 0) || (n.mi < 0 && !n.compositeParent);
    n.serial = serialFromInstanceId(n.mi >= 0 ? n.parentId : n.instanceId);
}

bool parseFakeDevices(const std::string& text, std::vector<UsbNode>& out, std::string& err) {
    out.clear();
    int lineNo = 0;
    for (std::string line : split(text, '\n')) {
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f = split(line, '|');
        if (f.size() < 6 || f.size() > 7) { err = "line " + std::to_string(lineNo) + ": expected 6 or 7 fields"; return false; }
        UsbNode n;
        n.instanceId = f[0]; n.service = f[1]; n.className = f[2];
        char* end = nullptr;
        const unsigned long prob = strtoul(f[3].c_str(), &end, 10);
        if (f[3].empty() || (end && *end)) { err = "line " + std::to_string(lineNo) + ": problem must be a number"; return false; }
        n.problem = (unsigned)prob; n.hasProblem = prob != 0;
        n.infPath = f[4]; n.ourInterface = f[5] == "1";
        if (f.size() == 7) n.parentId = f[6];
        if (!validInstanceId(n.instanceId)) { err = "line " + std::to_string(lineNo) + ": not a USB instance id"; return false; }
        deriveNode(n);
        out.push_back(n);
    }
    return true;
}

// ── the helper's command line ───────────────────────────────────────────────────────────────────────────────────
bool validInstanceId(const std::string& id) {
    if (id.size() < 22 || id.size() > 200) return false;
    const std::string u = upperS(id);
    if (u.rfind("USB\\VID_", 0) != 0) return false;
    if (hex4After(u, "VID_", 4) > 0xFFFF || u.compare(12, 5, "&PID_") != 0 || hex4After(u, "&PID_", 4) > 0xFFFF) return false;
    size_t p = 21;                       // after USB\VID_xxxx&PID_xxxx
    if (u.compare(p, 4, "&MI_") == 0) { if (hex4After(u, "&MI_", 2) > 0xFF) return false; p += 6; }
    if (p >= u.size() || u[p] != '\\' || p + 1 >= u.size()) return false;
    for (size_t i = p + 1; i < id.size(); ++i) {
        const char c = id[i];
        if (!(isalnum((unsigned char)c) || c == '&' || c == '_' || c == '.' || c == '-' || c == '#' || c == '{' || c == '}'))
            return false;               // ★ no quote, space, backslash, or anything a command line could split on
    }
    return true;
}

bool validPipeName(const std::string& p) {
    static const char* pre = "\\\\.\\pipe\\vibeserver-helper-";
    const size_t n = strlen(pre);
    if (p.size() <= n || p.size() > n + 64 || p.compare(0, n, pre) != 0) return false;
    for (size_t i = n; i < p.size(); ++i) if (!(isdigit((unsigned char)p[i]) || (p[i] >= 'a' && p[i] <= 'z') || p[i] == '-')) return false;
    return true;
}

bool validSidString(const std::string& s) {
    if (s.size() < 5 || s.size() > 184 || s.compare(0, 4, "S-1-") != 0) return false;
    int parts = 0; bool digit = false;
    for (size_t i = 4; i < s.size(); ++i) {
        if (isdigit((unsigned char)s[i])) digit = true;
        else if (s[i] == '-' && digit) { ++parts; digit = false; }
        else return false;
    }
    return digit && parts >= 1;
}

bool isHelperInvocation(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--winusb-bind" || a == "--winusb-giveback" || a == "--autostart-set") return true;
    }
    return false;
}

HelperArgs parseHelperArgs(int argc, char** argv) {
    HelperArgs h;
    auto refuse = [&](const std::string& why) { h.op = HelperOp::None; h.error = why; return h; };
    int ops = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasVal = i + 1 < argc;
        if (a == "--winusb-bind" || a == "--winusb-giveback") {
            if (!hasVal) return refuse(a + " needs a device instance id");
            h.op = a == "--winusb-bind" ? HelperOp::Bind : HelperOp::GiveBack; h.instanceId = argv[++i]; ++ops;
            if (!validInstanceId(h.instanceId)) return refuse("not a USB device instance id: " + h.instanceId);
        } else if (a == "--autostart-set") {
            if (!hasVal) return refuse("--autostart-set needs boot or off");
            const std::string v = argv[++i]; ++ops;
            if (v == "boot") h.op = HelperOp::AutostartBoot; else if (v == "off") h.op = HelperOp::AutostartOff;
            else return refuse("--autostart-set takes boot or off, not " + v);
        } else if (a == "--reply") {
            if (!hasVal) return refuse("--reply needs a pipe name");
            h.replyPipe = argv[++i];
            if (!validPipeName(h.replyPipe)) return refuse("not VibeServer's reply pipe: " + h.replyPipe);
        } else if (a == "--grant-sid") {
            if (!hasVal) return refuse("--grant-sid needs a SID");
            h.grantSid = argv[++i];
            if (!validSidString(h.grantSid)) return refuse("not a SID: " + h.grantSid);
        } else {
            return refuse("unexpected argument for the helper: " + a);   // ★ nothing else rides along elevated
        }
    }
    if (ops != 1) return refuse("exactly one helper operation is allowed");
    if (h.replyPipe.empty()) return refuse("--reply is required");
    if (!h.grantSid.empty() && h.op != HelperOp::AutostartBoot) return refuse("--grant-sid belongs to --autostart-set boot");
    return h;
}

std::string helperCommandLine(const HelperArgs& a) {
    if (!validPipeName(a.replyPipe)) return {};
    std::string s;
    switch (a.op) {
        case HelperOp::Bind: case HelperOp::GiveBack:
            if (!validInstanceId(a.instanceId)) return {};
            s = std::string(a.op == HelperOp::Bind ? "--winusb-bind" : "--winusb-giveback") + " \"" + a.instanceId + "\"";
            break;
        case HelperOp::AutostartBoot:
            s = "--autostart-set boot";
            if (!a.grantSid.empty()) { if (!validSidString(a.grantSid)) return {}; s += " --grant-sid " + a.grantSid; }
            break;
        case HelperOp::AutostartOff: s = "--autostart-set off"; break;
        default: return {};
    }
    return s + " --reply \"" + a.replyPipe + "\"";
}

bool helperMayBind(const std::vector<UsbNode>& real, const std::string& id, std::string& why) {
    const UsbNode* n = byId(real, id);
    if (!n) { why = "that device is not plugged in"; return false; }
    if (n->kind != Kind::Rtl) { why = "that device is not an RTL-SDR — VibeServer switches nothing else"; return false; }
    if (!n->sdrNode || n->compositeParent) { why = "that is not the dongle's radio interface"; return false; }
    bool ours = false; std::string w;
    const DongleState s = classify(*n, ours, w);
    if (s == DongleState::Usable) { why = "that dongle is already usable — nothing to switch"; return false; }
    if (!offerable(s)) { why = "that dongle is not one VibeServer offers to switch (" + w + ")"; return false; }
    return true;
}

bool helperMayGiveBack(const std::vector<UsbNode>& real, const std::string& id, std::string& why) {
    const UsbNode* n = byId(real, id);
    if (!n) { why = "that device is not plugged in"; return false; }
    if (n->kind != Kind::Rtl || !n->sdrNode || n->compositeParent) { why = "that is not an RTL-SDR's radio interface"; return false; }
    bool ours = false; std::string w;
    classify(*n, ours, w);
    // ★★ Only what WE switched: a Zadig or SDR# setup belongs to its owner, and uninstalling it is not ours to do.
    if (!ours) { why = "VibeServer did not switch that dongle, so it will not undo it"; return false; }
    return true;
}

std::string jsonUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char c = s[++i];
        switch (c) {
            case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break;
            case 'u':
                if (i + 4 < s.size()) { const unsigned v = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16); i += 4;
                    if (v < 0x80) o += (char)v; else o += '?'; }
                break;
            default: o += c;   // \" \\ \/
        }
    }
    return o;
}

std::string jsonField(const std::string& body, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t a = body.find(k);
    if (a == std::string::npos) return {};
    a = body.find(':', a + k.size());
    if (a == std::string::npos) return {};
    a = body.find('"', a);
    if (a == std::string::npos) return {};
    size_t b = a + 1;
    while (b < body.size() && body[b] != '"') b += body[b] == '\\' ? 2 : 1;
    if (b >= body.size()) return {};
    return jsonUnescape(body.substr(a + 1, b - a - 1));
}

}  // namespace vibewin
#endif
