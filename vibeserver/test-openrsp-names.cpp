// test-openrsp-names.cpp — OpenRSP's device table gives EVERY RSP a name of its own (VibeSDR change, 2026-11-18).
//
// ★★★ WHY. Almere's two RSP1 clones both report the placeholder serial "0000000001" (the owner: "the rsp1 clones do
//     not list a serial number at all"), and an original RSP1 has no serial string at all. OpenRSP keys its table,
//     SelectDevice and the unplug check on the serial, so the two clones MERGED into one slot — only one could ever be
//     listed or opened — and a device with no serial string failed the whole scan, hiding every RSP on the machine.
//     vendor/openrsp/src/service/Enumerator.cpp uniqueNames() now names them apart by USB port. This drives the REAL
//     Enumerator through a fake USB bus:
//       · two same-serial clones → two entries: the lower port keeps "0000000001", the other "0000000001_3-1.4";
//       · a device with no serial string → "RSP_<port>", and the scan no longer fails;
//       · a device the table already holds keeps its name when a clone on a LOWER port is plugged in beside it;
//       · a unique serial (a genuine RSP1A) is untouched; names stay the same across rescans and an unplug/re-plug.
//
// Built and run by scripts/test-openrsp-names.sh (the core sources with the portable stub platform — no libusb,
// no firmware, no sdcc). Silent.
#include "service/Enumerator.h"
#include "core/device/Models.h"
#include "core/log/Logger.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace sdrplay;

static int fails = 0, count = 0;
static void ok(bool c, const char* what) {
    ++count;
    std::printf("  %s %s\n", c ? "ok  " : "FAIL", what);
    if (!c) ++fails;
}

struct FakeRsp {
    std::string serial;     // "" = no serial string (iSerialNumber 0)
    std::string port;       // "3-1.2"
    std::uint8_t bus, address;
    bool held = false;      // claimInterface refuses: in use
};

static std::vector<FakeRsp> g_bus;

class FakeDevice final : public platform::UsbDevice {
public:
    explicit FakeDevice(const FakeRsp& d) : d_(d) {}
    int controlTransfer(std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t*, std::uint16_t,
                        std::uint32_t) override { return -12; }
    int claimInterface(int) override { return d_.held ? -6 /* BUSY */ : 0; }
    int releaseInterface(int) override { return 0; }
    int setAltSetting(int, int) override { return 0; }
    int getStringDescriptorAscii(std::uint8_t index, char* buf, int len) override {
        if (index == 0) return -2;   // libusb: INVALID_PARAM for descriptor 0
        const int n = (int)d_.serial.size() < len ? (int)d_.serial.size() : len;
        std::memcpy(buf, d_.serial.data(), (size_t)n);
        return n;
    }
private:
    FakeRsp d_;
};

class FakeBus final : public platform::UsbEnumerator {
public:
    std::vector<platform::UsbDeviceInfo> enumerate(std::uint16_t vid, bool) override {
        std::vector<platform::UsbDeviceInfo> out;
        for (const auto& d : g_bus) {
            platform::UsbDeviceInfo i;
            i.vid = vid; i.pid = 0x2500; i.bcdDevice = 0x0300;   // RSP1, running firmware
            i.busNumber = d.bus; i.deviceAddress = d.address; i.portNumber = (std::uint8_t)d.port.back();
            i.iSerialNumber = d.serial.empty() ? 0 : 3;
            i.portPath = d.port;
            i.opaqueId = ((std::uint64_t)d.bus << 8) | d.address;
            out.push_back(i);
        }
        return out;
    }
    std::unique_ptr<platform::UsbDevice> open(const platform::UsbDeviceInfo& info, int* err) override {
        for (const auto& d : g_bus)
            if ((((std::uint64_t)d.bus << 8) | d.address) == info.opaqueId) return std::make_unique<FakeDevice>(d);
        if (err) *err = -4;
        return nullptr;
    }
    std::unique_ptr<platform::UsbDevice> openWrapped(std::intptr_t, int* err) override { if (err) *err = -12; return nullptr; }
    bool setHotplugCallback(std::uint16_t, HotplugFn) override { return false; }
};

static std::map<std::string, int> listed(service::Enumerator& e) {
    e.scanUsbDevices();
    e.refreshDeviceList();
    sdrplay_api_DeviceT devs[16];
    std::uint32_t n = 0;
    e.getDevices(devs, &n);
    std::map<std::string, int> out;
    for (std::uint32_t i = 0; i < n; ++i) out[devs[i].SerNo]++;
    return out;
}

static bool has(const std::map<std::string, int>& m, const char* s) { return m.count(s) && m.at(s) == 1; }

int main() {
    FakeBus bus;
    service::Enumerator::Deps deps;
    deps.usb = &bus;
    deps.sleepMs = [](std::uint32_t) {};
    deps.firmwareDir = "/nonexistent";

    std::printf("Almere: two RSP1 clones, both \"0000000001\", and a genuine RSP1A\n");
    {
        service::Enumerator e(deps);
        g_bus = { {"0000000001", "3-1.4", 3, 12}, {"0000000001", "3-1.2", 3, 9}, {"240513CA60", "3-2", 3, 15} };
        auto m = listed(e);
        ok(m.size() == 3, "★★★ three devices listed, not two (the clones no longer merge)");
        ok(has(m, "0000000001"), "★★ the clone on the LOWER port keeps \"0000000001\" — an existing config keeps working");
        ok(has(m, "0000000001_3-1.4"), "★★ the other is \"0000000001_3-1.4\"");
        ok(has(m, "240513CA60"), "a unique serial is untouched");
        ok(e.findSerial("0000000001_3-1.4") >= 0 && e.findSerial("0000000001") >= 0
           && e.findSerial("0000000001_3-1.4") != e.findSerial("0000000001"),
           "★★★ SelectDevice can find each clone by its own name (two different slots)");
        auto m2 = listed(e);
        ok(m2 == m, "a rescan lists the same names");
    }

    std::printf("an original RSP1 with no serial string\n");
    {
        service::Enumerator e(deps);
        g_bus = { {"", "1-3", 1, 4}, {"0000000001", "1-4", 1, 5} };
        auto m = listed(e);
        ok(m.size() == 2, "★★★ the scan no longer fails — both devices are listed");
        ok(has(m, "RSP_1-3"), "★★ the serial-less one is \"RSP_1-3\"");
        ok(has(m, "0000000001"), "the other keeps its serial");
    }

    std::printf("a held radio is never renamed when a clone is plugged in on a lower port\n");
    {
        service::Enumerator e(deps);
        g_bus = { {"0000000001", "3-1.4", 3, 12} };
        auto m = listed(e);
        ok(has(m, "0000000001"), "alone, it is plain \"0000000001\"");
        g_bus[0].held = true;   // now on air
        g_bus.push_back({"0000000001", "3-1.2", 3, 20});
        listed(e);
        const int held = e.findSerial("0000000001");
        ok(held >= 0 && e.slot(held).rec.usb.portPath == "3-1.4",
           "★★★ the radio on air keeps \"0000000001\" (same device, port 3-1.4)");
        ok(e.findSerial("0000000001_3-1.2") >= 0, "★★ the newcomer is \"0000000001_3-1.2\"");
    }

    std::printf("unplug and re-plug into the same socket\n");
    {
        service::Enumerator e(deps);
        g_bus = { {"0000000001", "3-1.2", 3, 9}, {"0000000001", "3-1.4", 3, 12} };
        listed(e);
        g_bus.pop_back();
        auto m = listed(e);
        ok(m.size() == 1 && has(m, "0000000001"), "unplugged: one left, its name unchanged");
        g_bus.push_back({"0000000001", "3-1.4", 3, 31});   // new address, same socket
        m = listed(e);
        ok(has(m, "0000000001_3-1.4"), "★★ back in the same socket: the same name as before");
    }

    std::printf("%d checks, %d failures\n", count, fails);
    return fails ? 1 : 0;
}
