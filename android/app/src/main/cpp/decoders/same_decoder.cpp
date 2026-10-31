// decoders/same_decoder.cpp — see same_decoder.h. EAS / SAME from 47 CFR § 11.31.
#include "decoders/same_decoder.h"
#include "decoders/digimodes/q65_simd.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace vibe {

namespace {
constexpr double kBaud = 520.0 + 5.0 / 6.0;    // 520.83 bit/s: 1.92 ms a bit
constexpr double kMark = 2083.0 + 1.0 / 3.0;   // 4 cycles a bit
constexpr double kSpace = 1562.5;              // 3 cycles a bit
constexpr int kTab = 576;                      // 25 bits at 12 kHz: 100 mark cycles, 75 space cycles — exact

struct Ev { const char* c; const char* n; };
const Ev kEvents[] = {
    // national (§ 11.31(e), required)
    {"EAN", "Emergency Action Notification"}, {"NIC", "National Information Center"}, {"NPT", "National Periodic Test"},
    {"RMT", "Required Monthly Test"}, {"RWT", "Required Weekly Test"},
    // state and local (§ 11.31(e))
    {"ADR", "Administrative Message"}, {"AVW", "Avalanche Warning"}, {"AVA", "Avalanche Watch"}, {"BZW", "Blizzard Warning"},
    {"BLU", "Blue Alert"}, {"CAE", "Child Abduction Emergency"}, {"CDW", "Civil Danger Warning"}, {"CEM", "Civil Emergency Message"},
    {"CFW", "Coastal Flood Warning"}, {"CFA", "Coastal Flood Watch"}, {"DSW", "Dust Storm Warning"}, {"EQW", "Earthquake Warning"},
    {"EVI", "Evacuation Immediate"}, {"EWW", "Extreme Wind Warning"}, {"FRW", "Fire Warning"}, {"FFW", "Flash Flood Warning"},
    {"FFA", "Flash Flood Watch"}, {"FFS", "Flash Flood Statement"}, {"FLW", "Flood Warning"}, {"FLA", "Flood Watch"},
    {"FLS", "Flood Statement"}, {"HMW", "Hazardous Materials Warning"}, {"HWW", "High Wind Warning"}, {"HWA", "High Wind Watch"},
    {"HUW", "Hurricane Warning"}, {"HUA", "Hurricane Watch"}, {"HLS", "Hurricane Statement"}, {"LEW", "Law Enforcement Warning"},
    {"LAE", "Local Area Emergency"}, {"MEP", "Missing and Endangered Persons"}, {"NMN", "Network Message Notification"},
    {"TOE", "911 Telephone Outage Emergency"}, {"NUW", "Nuclear Power Plant Warning"}, {"DMO", "Practice/Demo Warning"},
    {"RHW", "Radiological Hazard Warning"}, {"SVR", "Severe Thunderstorm Warning"}, {"SVA", "Severe Thunderstorm Watch"},
    {"SVS", "Severe Weather Statement"}, {"SPW", "Shelter in Place Warning"}, {"SMW", "Special Marine Warning"},
    {"SPS", "Special Weather Statement"}, {"SSA", "Storm Surge Watch"}, {"SSW", "Storm Surge Warning"}, {"TOR", "Tornado Warning"},
    {"TOA", "Tornado Watch"}, {"TRW", "Tropical Storm Warning"}, {"TRA", "Tropical Storm Watch"}, {"TSW", "Tsunami Warning"},
    {"TSA", "Tsunami Watch"}, {"VOW", "Volcano Warning"}, {"WSW", "Winter Storm Warning"}, {"WSA", "Winter Storm Watch"},
};
// FIPS state and territory numbers (ANSI INCITS 38, as § 11.31(f) tabulates them)
const char* kStates[100] = {};
void initStates() {
    static bool done = false; if (done) return; done = true;
    const struct { int n; const char* a; } t[] = {
        {1,"AL"},{2,"AK"},{4,"AZ"},{5,"AR"},{6,"CA"},{8,"CO"},{9,"CT"},{10,"DE"},{11,"DC"},{12,"FL"},{13,"GA"},{15,"HI"},
        {16,"ID"},{17,"IL"},{18,"IN"},{19,"IA"},{20,"KS"},{21,"KY"},{22,"LA"},{23,"ME"},{24,"MD"},{25,"MA"},{26,"MI"},
        {27,"MN"},{28,"MS"},{29,"MO"},{30,"MT"},{31,"NE"},{32,"NV"},{33,"NH"},{34,"NJ"},{35,"NM"},{36,"NY"},{37,"NC"},
        {38,"ND"},{39,"OH"},{40,"OK"},{41,"OR"},{42,"PA"},{44,"RI"},{45,"SC"},{46,"SD"},{47,"TN"},{48,"TX"},{49,"UT"},
        {50,"VT"},{51,"VA"},{53,"WA"},{54,"WV"},{55,"WI"},{56,"WY"},{60,"AS"},{66,"GU"},{69,"MP"},{72,"PR"},{78,"VI"}};
    for (auto& x : t) kStates[x.n] = x.a;
}
bool isDig(char c) { return c >= '0' && c <= '9'; }
int num(const std::string& s, size_t p, int n) { int v = 0; for (int i = 0; i < n; ++i) v = v * 10 + (s[p + (size_t)i] - '0'); return v; }
} // namespace

SameDecoder::SameDecoder(double rateHz) {
    dec_ = std::max(1, (int)std::floor(rateHz / 12000.0 + 0.5));
    fs_ = rateHz / dec_;
    L_ = (int)std::floor(fs_ / kBaud + 0.5);
    inc_ = (float)(kBaud / fs_);
    cm_.resize(kTab + 3); sm_.resize(kTab + 3); cs_.resize(kTab + 3); ss_.resize(kTab + 3);
    for (int i = 0; i < kTab + 3; ++i) {
        const double a = 2 * M_PI * kMark * i / fs_, b = 2 * M_PI * kSpace * i / fs_;
        cm_[(size_t)i] = (float)std::cos(a); sm_[(size_t)i] = (float)std::sin(a);
        cs_[(size_t)i] = (float)std::cos(b); ss_[(size_t)i] = (float)std::sin(b);
    }
    initStates();
    reset();
}

void SameDecoder::reset() {
    ring_.assign((size_t)L_ * 4, 0.f); rPos_ = 0; tPos_ = 0;
    sMr_ = sMi_ = sSr_ = sSi_ = 0; dPrev_ = 0; ph_ = 0; dInc_ = 0; warm_ = 0;
    sr_ = 0; st_ = 0; nBit_ = 0; cur_ = 0; buf_.clear(); dashes_ = -1; badRun_ = 0;
    bursts_.clear(); soft_.clear(); last8_.assign(8, 0.f); invalidRun_ = 0; decN_ = 0; decAcc_ = 0;
}

void SameDecoder::feed(const float* x, int n) {
    using namespace q65v;
    // ★ box-average to ≈ 12 kHz (the host's 48 kHz → 4); the NFM / broadcast audio above 6 kHz carries nothing of SAME's
    std::vector<float> y; y.reserve((size_t)n / (size_t)dec_ + 2);
    const float inv = 1.0f / (float)dec_;
    for (int i = 0; i < n; ++i) { decAcc_ += x[i]; if (++decN_ == dec_) { y.push_back(decAcc_ * inv); decAcc_ = 0; decN_ = 0; } }
    const int m = (int)y.size();
    if (!m) return;
    // ★ the four mixer products, four samples at a time: planar outputs, the tables read straight (padded by 3 at the end
    //   with their first entries, so a load across the wrap is still one contiguous load) — q65v lanes
    y.resize(((size_t)m + 3) & ~(size_t)3, 0.f);
    const size_t m4 = y.size();
    pMr_.resize(m4); pMi_.resize(m4); pSr_.resize(m4); pSi_.resize(m4);
    {
        int t = tPos_;
        for (size_t i = 0; i < m4; i += 4) {
            const V xv = ld(&y[i]);
            st(&pMr_[i], mul(xv, ld(&cm_[(size_t)t]))); st(&pMi_[i], mul(xv, ld(&sm_[(size_t)t])));
            st(&pSr_[i], mul(xv, ld(&cs_[(size_t)t]))); st(&pSi_[i], mul(xv, ld(&ss_[(size_t)t])));
            t += 4; if (t >= kTab) t -= kTab;
        }
        tPos_ += m; while (tPos_ >= kTab) tPos_ -= kTab;   // ★ the real count (the padding is not signal)
    }
    for (int i = 0; i < m; ++i) {
        // ★ one-bit boxcar correlators — running sums, the oldest product out (per-sample feedback: scalar)
        float* r = &ring_[(size_t)rPos_ * 4];
        const float p0 = pMr_[(size_t)i], p1 = pMi_[(size_t)i], p2 = pSr_[(size_t)i], p3 = pSi_[(size_t)i];
        sMr_ += p0 - r[0]; sMi_ += p1 - r[1]; sSr_ += p2 - r[2]; sSi_ += p3 - r[3];
        r[0] = p0; r[1] = p1; r[2] = p2; r[3] = p3;
        if (++rPos_ == L_) {
            rPos_ = 0;
            // ★ the running sums re-made from the ring once a bit, so float rounding never accumulates
            float a = 0, b = 0, c = 0, d = 0;
            for (int k = 0; k < L_; ++k) { a += ring_[(size_t)k * 4]; b += ring_[(size_t)k * 4 + 1]; c += ring_[(size_t)k * 4 + 2]; d += ring_[(size_t)k * 4 + 3]; }
            sMr_ = a; sMi_ = b; sSr_ = c; sSi_ = d;
        }
        const float em = sMr_ * sMr_ + sMi_ * sMi_, es = sSr_ * sSr_ + sSi_ * sSi_;
        const float dv = em - es;
        t_ += 1.0 / fs_;
        if (warm_ < L_) { ++warm_; dPrev_ = dv; continue; }
        if ((dv >= 0) != (dPrev_ >= 0)) {
            // ★ a crossing is half a bit before the decision: pull the phase toward 0.5
            float e = ph_ - 0.5f;
            if (e > 0.5f) e -= 1.0f; else if (e < -0.5f) e += 1.0f;
            const bool hunt = st_ == 0;
            ph_ -= (hunt ? 0.12f : 0.03f) * e;
            // ★ second order: the sender's clock error learnt (a first-order loop leaves drift ÷ gain as a standing error)
            dInc_ -= (hunt ? 0.004f : 0.0008f) * e * inc_;
            if (hunt) dInc_ *= 0.97f;   // ★ leaks while hunting: on noise the crossings are random and it must not wander to the clamp
            if (dInc_ > 0.02f * inc_) dInc_ = 0.02f * inc_; else if (dInc_ < -0.02f * inc_) dInc_ = -0.02f * inc_;
            if (ph_ < 0) ph_ += 1.0f;
        }
        dPrev_ = dv;
        ph_ += inc_ + dInc_;
        if (ph_ >= 1.0f) { ph_ -= 1.0f; bit(dv > 0 ? 1 : 0, dv / (em + es + 1e-20f)); }   // ★ one divide a BIT, not a sample
    }
    if (!bursts_.empty() && (bursts_.size() >= 3 || t_ - lastBurstT_ > 3.0)) resolve();
}

void SameDecoder::bit(int b, float soft) {
    sr_ = (sr_ >> 1) | ((uint32_t)b << 31);
    for (int k = 0; k < 7; ++k) last8_[(size_t)k] = last8_[(size_t)k + 1];
    last8_[7] = soft;
    if (st_ == 2) soft_.push_back(soft);
    if (st_ == 0) {
        if (sr_ == 0xABABABABu) { st_ = 1; nBit_ = 0; cur_ = 0; if (onState && state_ != 1) { state_ = 1; onState(1); } }
        return;
    }
    cur_ |= (uint8_t)(b << nBit_);
    if (++nBit_ == 8) { const uint8_t c = cur_; nBit_ = 0; cur_ = 0; byteIn(c); }
}

void SameDecoder::byteIn(uint8_t c) {
    if (st_ == 1) {
        if (c == 0xAB) return;                         // the rest of the preamble
        if (c == 'Z' || c == 'N') { st_ = 2; buf_.assign(1, (char)c); dashes_ = -1; eom_ = c == 'N'; soft_ = last8_; invalidRun_ = 0; return; }
        st_ = 0; if (onState && state_ != 0) { state_ = 0; onState(0); }
        return;
    }
    // st_ == 2: reading. ★ An invalid byte does not end a header (the soft sum may repair it); three in a row do.
    const bool bad = (c & 0x80) || c < 0x20 || c > 0x7E;
    if (bad) { if (eom_ || ++invalidRun_ >= 3) { endBurst(); return; } buf_ += '?'; return; }
    invalidRun_ = 0;
    buf_ += (char)c;
    if (eom_) {
        if (buf_.size() == 4) {
            if (buf_ == "NNNN" && t_ - lastEomT_ > 10.0) { lastEomT_ = t_; if (onEom) onEom(); }
            st_ = 0; sr_ = 0; if (onState && state_ != 0) { state_ = 0; onState(0); }
        }
        return;
    }
    if (buf_.size() == 4 && buf_ != "ZCZC") { endBurst(); return; }
    if (c == '+') dashes_ = 0;
    else if (c == '-' && dashes_ >= 0 && ++dashes_ == 3) { endBurst(); return; }
    if (buf_.size() > 268) endBurst();
}

void SameDecoder::endBurst() {
    if (!eom_ && buf_.size() > 8 && buf_.compare(0, 4, "ZCZC") == 0) {
        // ★ the soft bits up to the last good character (a burst ended by three bad bytes drops them)
        size_t keep = buf_.size(); while (keep > 0 && buf_[keep - 1] == '?') --keep;
        soft_.resize(std::min(soft_.size(), keep * 8));
        bursts_.push_back(soft_); lastBurstT_ = t_;
        if (onBurst) onBurst(buf_);
    }
    buf_.clear(); soft_.clear(); st_ = 0; sr_ = 0;
    if (onState && state_ != 0) { state_ = 0; onState(0); }
}

void SameDecoder::resolve() {
    std::vector<std::vector<float>> b0; b0.swap(bursts_);
    // ★ a burst that slipped a bit (a drop-out, a click) is misaligned with the others from there on: each burst's hard
    //   characters are compared, and one that agrees with no other burst (≥ 80 % of the shared characters) is left out
    auto hard = [](const std::vector<float>& v) { std::string h; for (size_t by = 0; (by + 1) * 8 <= v.size(); ++by) { uint8_t c = 0; for (int k = 0; k < 8; ++k) if (v[by * 8 + (size_t)k] > 0) c |= (uint8_t)(1u << k); h += (char)c; } return h; };
    std::vector<std::string> hs; for (auto& v : b0) hs.push_back(hard(v));
    std::vector<std::vector<float>> b;
    for (size_t i = 0; i < b0.size(); ++i) {
        bool agrees = b0.size() == 1;
        for (size_t j = 0; j < b0.size() && !agrees; ++j) {
            if (i == j) continue;
            const size_t m = std::min(hs[i].size(), hs[j].size()); size_t same = 0;
            for (size_t k = 0; k < m; ++k) same += hs[i][k] == hs[j][k];
            agrees = m >= 8 && same * 5 >= m * 4;
        }
        if (agrees) b.push_back(b0[i]);
    }
    if (b.empty()) return;
    // ★ sum the bursts bit by bit (aligned at the 'Z'), then decide the bytes; the weakest bit's mean margin is kept
    size_t n = 0; for (auto& v : b) n = std::max(n, v.size());
    std::string hdr; float margin = 1.0f; double mSum = 0; int mN = 0;
    for (size_t by = 0; (by + 1) * 8 <= n; ++by) {
        uint8_t c = 0;
        float mByte = 1.0f;
        for (int k = 0; k < 8; ++k) {
            float sum = 0; int cnt = 0;
            for (auto& v : b) if (by * 8 + (size_t)k < v.size()) { sum += v[by * 8 + (size_t)k]; ++cnt; }
            if (!cnt) break;
            if (sum > 0) c |= (uint8_t)(1u << k);
            mByte = std::min(mByte, std::fabs(sum) / (float)cnt);
            mSum += std::fabs(sum) / (float)cnt; ++mN;
        }
        if ((c & 0x80) || c < 0x20 || c > 0x7E) break;
        hdr += (char)c;
        margin = std::min(margin, mByte);
        const size_t plus = hdr.find('+');
        if (plus != std::string::npos) { int d = 0; for (size_t q = plus; q < hdr.size(); ++q) d += hdr[q] == '-'; if (d == 3) break; }
    }
    Alert a;
    if (!parse(hdr, a)) return;
    (void)margin;
    a.heard = (int)b.size(); a.margin = mN ? (float)(mSum / mN) : 0.f;
    /* ★★★ NO CHECKSUM IN SAME — so two independent readings must agree before anything is shown: (1) two bursts whose
     *  hard characters are identical, or (2) the soft sum of three equals their character-by-character majority. A lone
     *  burst (tuned in mid-alert) is NOT shown: in the noise sweep its "clean" bits (mean margin 0.8) still misread digits. */
    {
        std::vector<std::string> hh; for (auto& v : b) { std::string h = hard(v); const size_t e = h.size() >= hdr.size() ? hdr.size() : h.size(); hh.push_back(h.substr(0, e)); }
        bool two = false;
        for (size_t i = 0; i < hh.size() && !two; ++i) for (size_t j = i + 1; j < hh.size(); ++j) if (hh[i] == hdr && hh[j] == hdr) { two = true; break; }
        bool maj = false;
        if (!two && hh.size() == 3 && hh[0].size() == hdr.size() && hh[1].size() == hdr.size() && hh[2].size() == hdr.size()) {
            std::string v = hh[0];
            for (size_t k = 0; k < v.size(); ++k) if (hh[1][k] == hh[2][k]) v[k] = hh[1][k];
            maj = v == hdr;
        }
        if (a.heard >= 2 && !two && !maj) return;
        if (a.heard < 2) return;   // ★ measured: a lone burst's clean-looking bits still misread digits — never shown
    }
    if (hdr == lastAlert_ && t_ - lastAlertT_ < 600.0) return;   // the same alert again within 10 minutes: once
    lastAlert_ = hdr; lastAlertT_ = t_;
    if (onAlert) onAlert(a);
}

bool SameDecoder::parse(const std::string& s, Alert& a) {
    a = Alert{}; a.raw = s;
    // ZCZC-ORG-EEE-PSSCCC(-PSSCCC){0,30}+TTTT-JJJHHMM-LLLLLLLL-
    if (s.size() < 40 || s.compare(0, 5, "ZCZC-") != 0) return false;
    size_t p = 5;
    a.org = s.substr(p, 3); p += 3;
    if (!orgName(a.org)[0] || s[p] != '-') return false; ++p;
    a.event = s.substr(p, 3); p += 3;
    for (char c : a.event) if (c < 'A' || c > 'Z') return false;
    if (s[p] != '-') return false; ++p;
    for (;;) {
        if (p + 7 > s.size()) return false;
        for (int k = 0; k < 6; ++k) if (!isDig(s[p + (size_t)k])) return false;
        a.areas.push_back(s.substr(p, 6)); p += 6;
        if (a.areas.size() > 31) return false;
        if (s[p] == '+') { ++p; break; }
        if (s[p] != '-') return false; ++p;
    }
    if (p + 4 + 1 + 7 + 1 + 1 > s.size()) return false;
    for (int k = 0; k < 4; ++k) if (!isDig(s[p + (size_t)k])) return false;
    const int hh = num(s, p, 2), mm = num(s, p + 2, 2);
    if (mm > 59) return false;
    a.validMin = hh * 60 + mm; p += 4;
    if (s[p] != '-') return false; ++p;
    for (int k = 0; k < 7; ++k) if (!isDig(s[p + (size_t)k])) return false;
    a.day = num(s, p, 3); a.hour = num(s, p + 3, 2); a.minute = num(s, p + 5, 2); p += 7;
    if (a.day < 1 || a.day > 366 || a.hour > 23 || a.minute > 59) return false;
    if (s[p] != '-') return false; ++p;
    const size_t e = s.find('-', p);
    if (e == std::string::npos || e == p || e - p > 8 || e + 1 != s.size()) return false;
    a.sender = s.substr(p, e - p);
    return true;
}

const char* SameDecoder::eventName(const std::string& e) {
    for (const auto& x : kEvents) if (e == x.c) return x.n;
    return "";
}
const char* SameDecoder::orgName(const std::string& o) {
    if (o == "EAS") return "EAS Participant";
    if (o == "CIV") return "Civil authorities";
    if (o == "WXR") return "National Weather Service";
    if (o == "PEP") return "Primary Entry Point (US Government)";
    return "";
}
const char* SameDecoder::stateAbbr(int ss) { initStates(); return (ss >= 0 && ss < 100 && kStates[ss]) ? kStates[ss] : ""; }

std::string SameDecoder::line(const Alert& a) {
    std::string s = "[SAME] ";
    const char* en = eventName(a.event);
    s += en[0] ? std::string(en) + " (" + a.event + ")" : a.event;
    s += std::string(" \xc2\xb7 ") + orgName(a.org) + " \xc2\xb7 areas";
    for (size_t i = 0; i < a.areas.size(); ++i) {
        const std::string& z = a.areas[i];
        const int ss = (z[1] - '0') * 10 + (z[2] - '0');
        const char* st = stateAbbr(ss);
        s += i ? ", " : " ";
        s += z;
        if (st[0]) s += std::string(" (") + st + (z.compare(3, 3, "000") == 0 ? ", whole state)" : ")");
        else if (ss >= 57) s += " (marine)";
    }
    char t[160];
    std::snprintf(t, sizeof t, " \xc2\xb7 issued day %03d %02d:%02d UTC \xc2\xb7 valid %d h %02d min \xc2\xb7 from %s",
                  a.day, a.hour, a.minute, a.validMin / 60, a.validMin % 60, a.sender.c_str());
    s += t;
    return s + "\n";
}

} // namespace vibe
