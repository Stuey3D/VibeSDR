// vibe_nav_run.h — V12 the VOR / ILS decoder inside a decoder host (vibe_decoder_host.h, ext "vor"): the receiver
// (decoders/nav_rx.h) on the listener's CHANNEL IQ, the ident read by consensus (the NDB skimmer's IdentVote) and matched to the
// published stations on the channel (vibe_vor_db.h). docs/v12/VOR-ILS.md.
//
// ★★ WHAT IS SENT, AND TO WHOM
//   A VOR / ILS is a navigation broadcast meant for anyone to receive: the radial, the deviation, the ident, the station are
//   shown to every listener on this decoder. ★ What reveals WHERE THIS SERVER IS more precisely than a radial already does —
//   the distance from the station to within a kilometre and how far the radial passes from the server's own position (the
//   decoder's own check) — goes to the OWNER's signed-in sockets only (DecoderPeer::privileged); everyone else gets the
//   distance to the nearest 10 km, as the NDB list's precision.
//
// Wire (on the decoder socket):
//   0x01 | u64 BE ts | u32 BE len | UTF-8      a line when a bearing / deviation is first held, the ident is confirmed, the
//                                              station changes, and once a minute while it holds
//   0x03 | u8 state                            0 searching · 1 VOR bearing held · 2 ILS deviation held
//   {"type":"vor","data":{…}}                  once a second (see sendJson)
#pragma once
#include "decoders/nav_rx.h"
#include "decoders/ndb_skimmer.h"
#include "vibe_vor_db.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace vibe {

class NavRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;
        std::function<void(const std::string&, const std::string&)> textPriv;   ///< (everyone's line, the owner's line)
        std::function<void(uint8_t)> state;
        std::function<void(const std::string&)> json;
    };
    /** `position`: the server's place (false = not given). */
    NavRun(std::function<double()> dialHz, std::function<bool(double&, double&)> position, Out out)
        : dial_(std::move(dialHz)), pos_(std::move(position)), out_(std::move(out)) {
        where_();
        sendJson_();
    }
    void feedIq(const std::complex<float>* x, int n, double rate) {
        if (n <= 0 || !(rate > 0)) return;
        if (!rx_ || std::fabs(rate - rate_) > 0.5) start_(rate);
        rx_->feed(x, n);
    }
    // for the tests
    const nav::Report& last() const { return last_; }
    std::string ident() const { return ident_(); }

private:
    static std::string clock_() {
        const time_t t = std::time(nullptr); struct tm u{}; gmtime_r(&t, &u);
        char b[16]; std::snprintf(b, sizeof b, "%02d:%02d:%02dz", u.tm_hour, u.tm_min, u.tm_sec);
        return b;
    }
    static int64_t nowMs_() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void start_(double rate) {
        rate_ = rate;
        rx_.reset(new nav::NavRx(rate));
        rx_->onSecond = [this](const nav::Report& r) { onSecond_(r); };
        rx_->keyer.onWord = [this](const std::string& w, double wpm) { onWord_(w, wpm); };
    }
    /** Re-reads the dial; true when the channel changed (a new station: the vote and the run start again). */
    bool where_() {
        const double hz = dial_ ? dial_() : 0.0;
        const int64_t ch = nav::channelHz(hz);
        if (ch == chan_ && known_) return false;
        known_ = true; chan_ = ch; expect_ = nav::channelKind(hz);
        havePos_ = pos_ && pos_(lat_, lon_) && std::isfinite(lat_) && std::isfinite(lon_) && !(lat_ == 0 && lon_ == 0);
        const int kind = expect_ == 2 ? 1 : 0;
        cands_ = expect_ == 3 ? std::vector<vibevor::Station>() : vibevor::on((int)(ch / 1000), kind, havePos_, lat_, lon_);
        vote_ = ndb::IdentVote();
        seen_.clear(); held_ = 0; lastLineMs_ = 0; lastIdent_.clear(); reading_.clear();
        return true;
    }
    const vibevor::Station* station_() const {
        const std::string id = ident_();
        if (!id.empty()) {
            for (const auto& s : cands_) if (id == s.r->ident) return &s;
            return nullptr;   // an ident that is not listed on this channel
        }
        return nullptr;
    }
    /** ★ The ident: the NDB skimmer's consensus (three clean identical readings …), OR a reading that IS a station
     *  published on this channel, read twice (the list is the second witness). */
    std::string ident_() const {
        if (!vote_.ident().empty()) return vote_.ident();
        for (const auto& kv : seen_) if (kv.second >= 2) return kv.first;
        return std::string();
    }
    void onWord_(const std::string& w, double wpm) {
        wpm_ = wpm;
        reading_ = w;
        bool listed = false;
        for (const auto& s : cands_) if (w == s.r->ident) listed = true;
        vote_.add(w, nowMs_());
        if (listed) ++seen_[w];
        const std::string id = ident_();
        if (!id.empty() && id != lastIdent_) { lastIdent_ = id; line_(true); }
    }
    void onSecond_(const nav::Report& r) {
        if (where_()) { /* channel moved: the run restarts */ }
        last_ = r;
        const int held = r.kind == nav::Report::Vor ? 1 : r.kind == nav::Report::Ils ? 2 : 0;
        if (held != held_) { held_ = held; if (out_.state) out_.state((uint8_t)held); if (held) line_(false); }
        else if (held && nowMs_() - lastLineMs_ >= 60000) line_(false);
        sendJson_();
    }
    std::string chanStr_() const {
        char b[24]; std::snprintf(b, sizeof b, "%.2f", chan_ / 1e6); return b;
    }
    double trueBearing_(double radial, const vibevor::Station* s) const { return std::fmod(radial + (s ? s->r->var : 0.0) + 720.0, 360.0); }
    void line_(bool identEvent) {
        if (!held_ && !identEvent) return;
        lastLineMs_ = nowMs_();
        const vibevor::Station* s = station_();
        std::string who = chanStr_() + " MHz";
        if (s) who = std::string(s->r->ident) + " " + s->name() + " (" + who + ")";
        else if (!ident_().empty()) who = ident_() + " (" + who + ", not in the published list)";
        char b[400], p[520];
        std::string pub, priv;
        if (held_ == 1) {
            std::snprintf(b, sizeof b, "%s  VOR %s  radial %05.1f\xc2\xb0 FROM (magnetic)%s  \xc2\xb7 \xc2\xb1%.1f\xc2\xb0", clock_().c_str(), who.c_str(),
                          last_.bearing, "", last_.sigma);
            pub = b;
            if (s) {
                const double tb = trueBearing_(last_.bearing, s);
                std::snprintf(b, sizeof b, " \xc2\xb7 true %05.1f\xc2\xb0", tb); pub += b;
                if (havePos_) {
                    bool ahead = true;
                    const double xk = vibevor::crossTrackKm(s->r->lat, s->r->lon, tb, lat_, lon_, ahead);
                    std::snprintf(p, sizeof p, "%s \xc2\xb7 %.1f km from the station \xc2\xb7 the radial passes %.1f km from this server%s (shown to you only)\n",
                                  pub.c_str(), s->km, xk, ahead ? "" : " \xe2\x80\x94 BEHIND the station: check the reading");
                    priv = p;
                    std::snprintf(b, sizeof b, " \xc2\xb7 ~%.0f km from the station", std::round(s->km / 10) * 10); pub += b;
                }
            }
        } else if (held_ == 2) {
            const bool gs = expect_ == 3 || last_.sdm > 0.6;
            const double full = gs ? 0.175 : 0.155;
            const char* side = gs ? (last_.ddm > 0 ? "above the glide path (fly down)" : "below the glide path (fly up)")
                                  : (last_.ddm > 0 ? "left of the course (fly right)" : "right of the course (fly left)");
            if (std::fabs(last_.ddm) < 0.004) side = gs ? "on the glide path" : "on the course line";
            std::snprintf(b, sizeof b, "%s  %s %s  DDM %+.3f \xc2\xb7 %s \xc2\xb7 %.0f %% of full scale", clock_().c_str(), gs ? "ILS glideslope" : "ILS localiser",
                          who.c_str(), last_.ddm, side, std::min(999.0, std::fabs(last_.ddm) / full * 100));
            pub = b;
            const int pair = nav::pairedKhz((double)chan_);
            if (pair) { std::snprintf(b, sizeof b, " \xc2\xb7 paired %s %.2f MHz", gs ? "localiser" : "glideslope", pair / 1e3); pub += b; }
        } else {
            std::snprintf(b, sizeof b, "%s  ident %s heard on %s MHz", clock_().c_str(), ident_().c_str(), chanStr_().c_str());
            pub = b;
        }
        pub += "\n";
        if (!priv.empty() && out_.textPriv) out_.textPriv(pub, priv);
        else if (out_.text) out_.text(pub);
    }
    static std::string esc_(const std::string& s) {
        std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
    }
    void sendJson_() {
        if (!out_.json) return;
        const char* exp = expect_ == 1 ? "vor" : expect_ == 2 ? "loc" : expect_ == 3 ? "gs" : "none";
        const char* kind = held_ == 1 ? "vor" : held_ == 2 ? ((expect_ == 3 || last_.sdm > 0.6) ? "gs" : "loc") : "none";
        std::string j = "{\"type\":\"vor\",\"data\":{\"chan\":\"" + chanStr_() + "\",\"expect\":\"" + exp + "\",\"kind\":\"" + kind + "\"";
        char b[200];
        std::snprintf(b, sizeof b, ",\"rate\":%.0f,\"secs\":%d", rate_, last_.seconds); j += b;
        const nav::Second& s = last_.last;
        std::snprintf(b, sizeof b, ",\"q\":{\"carrier\":%s,\"var30\":%.3f,\"sub\":%.3f,\"dev\":%.0f,\"coh\":%.2f,\"m90\":%.3f,\"m150\":%.3f,\"ilsCoh\":%.2f}",
                      s.carrier > 0 ? "true" : "false", s.var30, s.sub, s.dev, s.refCoh, s.m90, s.m150, s.ilsCoh); j += b;
        const vibevor::Station* st = station_();
        if (held_ == 1) {
            std::snprintf(b, sizeof b, ",\"radial\":%.1f,\"sigma\":%.2f", last_.bearing, last_.sigma); j += b;
            if (st) { std::snprintf(b, sizeof b, ",\"trueBrg\":%.1f", trueBearing_(last_.bearing, st)); j += b; }
        } else if (held_ == 2) {
            std::snprintf(b, sizeof b, ",\"ddm\":%.4f,\"sdm\":%.3f", last_.ddm, last_.sdm); j += b;
        }
        const int pair = expect_ >= 2 ? nav::pairedKhz((double)chan_) : 0;
        if (pair) { std::snprintf(b, sizeof b, ",\"pair\":\"%.2f\"", pair / 1e3); j += b; }
        j += ",\"ident\":\"" + esc_(ident_()) + "\",\"reading\":\"" + esc_(reading_) + "\"";
        if (wpm_ > 0) { std::snprintf(b, sizeof b, ",\"wpm\":%.1f", wpm_); j += b; }
        auto stJson = [&](const vibevor::Station& x) {
            char c[300];
            std::snprintf(c, sizeof c, "{\"ident\":\"%s\",\"name\":\"%s\",\"cc\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"var\":%.1f,\"dme\":\"%s\"",
                          esc_(x.r->ident).c_str(), esc_(x.name()).c_str(), esc_(x.r->cc).c_str(), x.r->lat, x.r->lon, x.r->var, esc_(x.r->dme).c_str());
            std::string o = c;
            if (std::isfinite(x.km)) { std::snprintf(c, sizeof c, ",\"km10\":%.0f", std::round(x.km / 10) * 10); o += c; }
            return o + "}";
        };
        if (st) j += ",\"station\":" + stJson(*st);
        j += ",\"listed\":[";
        for (size_t i = 0; i < cands_.size() && i < 4; ++i) j += (i ? "," : "") + stJson(cands_[i]);
        j += "],\"credit\":\"" + std::string(vibevor::kStationCredit) + "\"}}";
        out_.json(j);
    }

    std::function<double()> dial_;
    std::function<bool(double&, double&)> pos_;
    Out out_;
    std::unique_ptr<nav::NavRx> rx_;
    double rate_ = 0;
    nav::Report last_;
    int64_t chan_ = 0;
    int expect_ = 0, held_ = 0;
    bool known_ = false, havePos_ = false;
    double lat_ = 0, lon_ = 0, wpm_ = 0;
    std::vector<vibevor::Station> cands_;
    ndb::IdentVote vote_;
    std::map<std::string, int> seen_;
    std::string reading_, lastIdent_;
    int64_t lastLineMs_ = 0;
};

} // namespace vibe
