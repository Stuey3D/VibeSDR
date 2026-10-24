// vibe_rate_guard.h — when a whole-radio mode BOOSTS the sample rate (ADS-B to 2.4/3 MS/s, DAB to 2.048, under the
// owner's "change the sample rate when a mode needs it" switch) and the radio falls over there, notice, recover at
// the radio's own rate, and say so in red beside that switch (Stuart, 2026-10-24: "if the SDR crashes when being
// run in 3.2 MS/s mode can we detect that and restart the SDR at a safe rate", generalised the same day to any rate a
// mode boosted to once the RTL 3.2 MS/s option was withdrawn).
//
// ★★★ TWO WAYS TO FALL OVER, TWO KINDS OF EVIDENCE.
//     STALLED — the process lives, the samples stop (no IQ for 3 s, the stream ends). The watchdog sees it: the mode is
//               left (the radio back at its base rate) and the fault recorded. LocalSdrShim, rateBoostStalled.
//     CRASHED — the process dies (the libusb ABORT of a re-rate teardown on 32-bit, memory
//               dab_crash_was_the_dongle_not_the_code). Nothing runs after an abort — no destructor, no handler — so the
//               only evidence that can survive is a file that was ALREADY there: <base>.marker, written immediately
//               BEFORE the radio is asked for the boosted rate and removed once it is back at its base rate or the
//               process stops cleanly. Found at the next start: the last run died at that rate.
// ★★ Either way the rate switch stays OFF until the owner clears it — a radio that died at a rate is not sent back
//    there by the next listener who picks the mode; and with the switch off a remembered DAB multiplex cannot resume
//    at 2.048 on start, which would otherwise be a crash loop.
// ★ Pure, no radio — vibeserver/test-adsb-capture.cpp holds it to known answers. Empty base = memory only.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace vibe {

struct RateFault {
    std::string kind;          ///< "" | "stalled" | "crashed"
    long long   at = 0;        ///< epoch seconds
    double      rate = 0;      ///< Hz the radio was at (or going to)
    std::string mode;          ///< "ADS-B" | "DAB" | "" (unknown)

    bool any() const { return !kind.empty(); }
    static std::string rateText(double hz) {
        if (hz <= 0) return "a boosted rate";
        char b[32]; std::snprintf(b, sizeof b, "%.3f", hz / 1e6);
        std::string s = b;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s + " MS/s";
    }
    /** The red line beside the rate switch. The clients add the local time from `at`. */
    std::string say() const {
        const std::string where = rateText(rate) + (mode.empty() ? "" : " during " + mode);
        if (kind == "crashed")
            return "SDR crashed at " + where + " \xe2\x80\x94 the server came back at its own rate, and the rate switch is "
                   "off until you clear this.";
        if (kind == "stalled")
            return "SDR stopped responding at " + where + " \xe2\x80\x94 it is back at its own rate, and the rate switch is "
                   "off until you clear this.";
        return "";
    }
    std::string json() const {
        char b[96];
        std::snprintf(b, sizeof b, "\",\"at\":%lld,\"rate\":%.0f,\"mode\":\"", at, rate);
        return "{\"fault\":\"" + esc(kind) + b + esc(mode) + "\",\"blocked\":" + (any() ? "true" : "false")
             + ",\"say\":\"" + esc(say()) + "\"}";
    }
    bool parse(const std::string& s) {
        *this = RateFault{};
        if (s.find("\"fault\":") == std::string::npos) return false;
        kind = str(s, "fault"); mode = str(s, "mode");
        at = (long long)num(s, "at"); rate = num(s, "rate");
        if (!kind.empty() && kind != "stalled" && kind != "crashed") kind = "crashed";
        if (mode.size() > 16) mode.resize(16);
        return true;
    }
    static std::string esc(const std::string& in) {
        std::string o;
        for (char c : in) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n' || c == '\r') continue; o += c; }
        return o;
    }
private:
    static double num(const std::string& s, const char* k) {
        const std::string key = std::string("\"") + k + "\":";
        const size_t p = s.find(key);
        return p == std::string::npos ? 0 : std::strtod(s.c_str() + p + key.size(), nullptr);
    }
    static std::string str(const std::string& s, const char* k) {
        const std::string key = std::string("\"") + k + "\":\"";
        const size_t p = s.find(key);
        if (p == std::string::npos) return "";
        std::string o;
        for (size_t i = p + key.size(); i < s.size() && s[i] != '"'; ++i) { if (s[i] == '\\' && i + 1 < s.size()) ++i; o += s[i]; }
        return o;
    }
};

class RateGuard {
public:
    void setBase(const std::string& base) { std::lock_guard<std::mutex> lk(m_); base_ = base; loadLocked(); }

    /** At start, once per process: a marker still present means the last run died at the rate it names. Records the
     *  crash (at the marker's time, or `nowEpoch`), removes the marker, returns true. */
    bool boot(long long nowEpoch) {
        std::lock_guard<std::mutex> lk(m_);
        if (booted_ || base_.empty()) return false;
        booted_ = true;
        FILE* f = std::fopen((base_ + ".marker").c_str(), "rb");
        if (!f) return false;
        char b[160] = {0};
        const size_t n = std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        b[n] = 0;
        std::remove((base_ + ".marker").c_str());
        // "entering <hz> Hz for <mode> at <epoch>"
        RateFault x; x.kind = "crashed";
        if (const char* p = std::strstr(b, "entering ")) x.rate = std::strtod(p + 9, nullptr);
        if (const char* p = std::strstr(b, " for ")) {
            const char* e = std::strstr(p + 5, " at ");
            if (!e) e = p + 5 + std::strcspn(p + 5, "\r\n");
            if (e - (p + 5) <= 16) x.mode.assign(p + 5, size_t(e - (p + 5)));
        }
        if (const char* p = std::strstr(b, " at ")) x.at = std::atoll(p + 4);
        if (x.at <= 0) x.at = nowEpoch;
        f_ = x;
        saveLocked();
        return true;
    }
    /** Immediately BEFORE asking the radio for `rateHz` on behalf of `mode`. */
    void mark(double rateHz, const std::string& mode, long long nowEpoch) {
        std::lock_guard<std::mutex> lk(m_);
        markRate_ = rateHz; markMode_ = mode; marked_ = true;
        if (base_.empty()) return;
        if (FILE* f = std::fopen((base_ + ".marker").c_str(), "wb")) {
            std::fprintf(f, "entering %.0f Hz for %s at %lld\n", rateHz, mode.c_str(), nowEpoch);
            std::fflush(f);
            std::fclose(f);
        }
    }
    /** The radio has settled at `nowRateHz`: a rate other than the one marked means the boost is over. */
    void settled(double nowRateHz) {
        std::lock_guard<std::mutex> lk(m_);
        if (marked_ && std::fabs(nowRateHz - markRate_) > 1000) unmarkLocked();
    }
    /** A clean stop. */
    void unmark() { std::lock_guard<std::mutex> lk(m_); unmarkLocked(); }
    /** Is the radio at a rate a mode boosted it to — and which mode? */
    bool boosted(double* rate = nullptr, std::string* mode = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        if (rate) *rate = markRate_;
        if (mode) *mode = markMode_;
        return marked_;
    }
    bool markerPresent() {
        std::lock_guard<std::mutex> lk(m_);
        if (base_.empty()) return false;
        FILE* f = std::fopen((base_ + ".marker").c_str(), "rb");
        if (f) std::fclose(f);
        return f != nullptr;
    }
    void recordStall(double rateHz, const std::string& mode, long long at) {
        std::lock_guard<std::mutex> lk(m_);
        f_.kind = "stalled"; f_.rate = rateHz; f_.mode = mode; f_.at = at;
        saveLocked();
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); f_ = RateFault{}; saveLocked(); }
    bool blocked() { std::lock_guard<std::mutex> lk(m_); return f_.any(); }
    RateFault fault() { std::lock_guard<std::mutex> lk(m_); return f_; }
    std::string json() { std::lock_guard<std::mutex> lk(m_); return f_.json(); }

private:
    void unmarkLocked() {
        if (!marked_) return;            // ★ cheap on every rate change that was never a boost
        marked_ = false; markRate_ = 0; markMode_.clear();
        if (!base_.empty()) std::remove((base_ + ".marker").c_str());
    }
    void loadLocked() {
        f_ = RateFault{};
        if (base_.empty()) return;
        FILE* f = std::fopen((base_ + ".json").c_str(), "rb");
        if (!f) return;
        char b[1024]; const size_t n = std::fread(b, 1, sizeof b - 1, f); b[n] = 0;
        std::fclose(f);
        f_.parse(b);
    }
    void saveLocked() {
        if (base_.empty()) return;
        const std::string path = base_ + ".json", tmp = path + ".tmp", j = f_.json();
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            const bool ok = std::fwrite(j.data(), 1, j.size(), f) == j.size();
            std::fclose(f);
            if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
        }
    }
    std::mutex m_;
    std::string base_;
    RateFault f_;
    bool booted_ = false, marked_ = false;
    double markRate_ = 0;
    std::string markMode_;
};

} // namespace vibe
