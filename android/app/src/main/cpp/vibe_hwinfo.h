// VibeSDR — what this server runs on, for the directory listing (Stuart, 2026-09-19: "show the CPU and RAM specs
// of a machine on the Directory … people seeing the low end hardware that these are running on may prompt some
// installs", and "also show if running the 64bit 32bit NEON or 32Bit scalar").
//
// ★ MEASURED OR COMPILED IN, NEVER GUESSED. The instruction set is what THIS binary was built for (the compiler's
//   own macros), so "32-bit NEON" means the NEON paths are really the ones running. CPU, cores, clock and RAM are
//   read from the OS; anything it will not tell us is left out rather than invented.
// ★ Read once — none of it changes while the process runs.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace vibe {

/** "64-bit", "32-bit NEON" or "32-bit" — the code path this build actually takes. */
inline const char* isaClass() {
#if defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_ARM64)
    return "64-bit";
#elif defined(__arm__) && defined(__ARM_NEON)
    return "32-bit NEON";
#else
    return "32-bit";
#endif
}

namespace hwdetail {
inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
inline std::string jsonEsc(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
}
/** ARM's own name for a core from its CPU-part number — the Pi 2 and a TV SoC print no "model name". */
inline const char* armCore(unsigned part) {
    switch (part) {
        case 0xc07: return "Cortex-A7";   case 0xc08: return "Cortex-A8";  case 0xc09: return "Cortex-A9";
        case 0xc0f: return "Cortex-A15";  case 0xd03: return "Cortex-A53"; case 0xd04: return "Cortex-A35";
        case 0xd05: return "Cortex-A55";  case 0xd07: return "Cortex-A57"; case 0xd08: return "Cortex-A72";
        case 0xd09: return "Cortex-A73";  case 0xd0a: return "Cortex-A75"; case 0xd0b: return "Cortex-A76";
        case 0xd0d: return "Cortex-A77";  case 0xd41: return "Cortex-A78"; case 0xd44: return "Cortex-X1";
        case 0xd46: return "Cortex-A510"; case 0xd47: return "Cortex-A710"; case 0xd4d: return "Cortex-A715";
        default: return nullptr;
    }
}
} // namespace hwdetail

/** ★★★ HOW MANY CPUs PHYSICALLY EXIST — /sys/devices/system/cpu/present, a range list ("0-1", "0-3,6").
 *  This used _SC_NPROCESSORS_CONF, which glibc counts from the CPU slots the FIRMWARE DECLARES. Kiko's
 *  Celeron N4000 — 2 cores, 2 threads, no Hyper-Threading — reported "cores":4 (Stuart, 2026-09-27: "for
 *  some reason Kiko's server is reporting 4 cores on a dual core chip"). Budget Gemini Lake laptops share
 *  firmware with the quad N4100, declare four slots, and Linux boots saying "Allowing 4 CPUs, 2 hotplug
 *  CPUs". Stuart's own servers declare exactly what they have, which is why they always looked right.
 *  ★★ NOT the ONLINE count either: a phone switches idle cores OFF, so "online" would under-report an
 *     8-core XCover as 4 at idle. "present" is the hardware, whatever its power state.
 *  ★ The benchmark and the health pill already use the online count (they measure what can RUN now),
 *    so they were never wrong — only this published card was. Falls back to online where "present"
 *    cannot be read. */
/** Count a kernel CPU list — "0-1", "0-3,6", "0". 0 when it cannot be read. */
inline long countCpuList(const char* p) {
    long n = 0;
    while (*p && *p != '\n') {
        char* e; const long a = strtol(p, &e, 10); if (e == p) return 0; p = e;
        long b = a;
        if (*p == '-') { b = strtol(p + 1, &e, 10); if (e == p + 1) return 0; p = e; }
        if (b >= a) n += b - a + 1;
        if (*p == ',') p++;
        else if (*p && *p != '\n') return 0;   // ★ anything unexpected: do not guess, fall back
    }
    return n;
}
inline long presentCpus() {
    if (FILE* f = fopen("/sys/devices/system/cpu/present", "r")) {
        char buf[256] = {0};
        const bool got = fgets(buf, sizeof buf, f) != nullptr;
        fclose(f);
        if (got) { const long n = countCpuList(buf); if (n > 0) return n; }
    }
    return sysconf(_SC_NPROCESSORS_ONLN);
}

/** `{"cpu":"…","cores":4,"mhz":1000,"ramMb":1946,"isa":"32-bit NEON"}` — fields the OS would not give are omitted. */
inline std::string hardwareJson() {
    static const std::string cached = [] {
        using namespace hwdetail;
        std::string cpu; long mhz = 0, ramMb = 0;
#if defined(__APPLE__)
        const long cores = sysconf(_SC_NPROCESSORS_CONF);   // ★ macOS has no /sys; its count is the real one
#else
        const long cores = presentCpus();
#endif
#if defined(__APPLE__)
        char buf[256]; size_t n = sizeof buf;
        if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0) cpu = trim(std::string(buf, strnlen(buf, n)));
        uint64_t mem = 0; n = sizeof mem;
        if (sysctlbyname("hw.memsize", &mem, &n, nullptr, 0) == 0) ramMb = (long)(mem / (1024 * 1024));
#else
        if (FILE* f = fopen("/proc/cpuinfo", "r")) {
            char line[512]; std::string model, hardware; unsigned part = 0;
            while (fgets(line, sizeof line, f)) {
                const char* c = strchr(line, ':'); if (!c) continue;
                std::string key = trim(std::string(line, c - line)), val = trim(std::string(c + 1));
                if (model.empty() && key == "model name") model = val;
                else if (hardware.empty() && key == "Hardware") hardware = val;
                else if (!part && key == "CPU part") part = (unsigned)strtoul(val.c_str(), nullptr, 0);
            }
            fclose(f);
            // ★ x86 names itself ("Intel(R) Core(TM) i5 …"); ARM says "ARMv7 Processor rev 5" there, which tells a
            //   reader nothing — the core from its part number (and the SoC, where the kernel names one) does.
            if (!model.empty() && model.rfind("ARMv", 0) != 0) cpu = model;
            else if (const char* core = armCore(part)) cpu = hardware.empty() ? core : std::string(core) + " · " + hardware;
            else cpu = !hardware.empty() ? hardware : model;
        }
        if (FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r")) {
            long khz = 0; if (fscanf(f, "%ld", &khz) == 1 && khz > 0) mhz = khz / 1000; fclose(f);
        }
        if (FILE* f = fopen("/proc/meminfo", "r")) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                long kb = 0; if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) { ramMb = kb / 1024; break; }
            }
            fclose(f);
        }
#endif
        std::string j = "{";
        auto add = [&](const std::string& kv) { if (j.size() > 1) j += ','; j += kv; };
        if (!cpu.empty()) add("\"cpu\":\"" + jsonEsc(cpu.substr(0, 60)) + "\"");
        if (cores > 0)    add("\"cores\":" + std::to_string(cores));
        if (mhz > 0)      add("\"mhz\":" + std::to_string(mhz));
        if (ramMb > 0)    add("\"ramMb\":" + std::to_string(ramMb));
        add(std::string("\"isa\":\"") + isaClass() + "\"");
        return j + "}";
    }();
    return cached;
}

} // namespace vibe
