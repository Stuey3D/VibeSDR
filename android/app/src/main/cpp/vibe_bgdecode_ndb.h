// vibe_bgdecode_ndb.h — BACKGROUND DECODING's NDB SKIMMER VFO (V12, 2026-10-27): the decoder behind an "ndb-skim" VFO.
//
// ★★ Stuart (2026-10-27): the NDB skimmer is a VFO decoder like the CW/PSK skimmers and Band digital spots — it runs BESIDE
//    NAVTEX and FT8 on the same capture (his Lenovo RSP at 1200 kHz, 3 MS/s, covers 190–535 kHz whole). So the runner
//    (vibe_bgdecode.h) cuts ONE IQ slice over the band off its own channeliser, and this hands it to the listener's own NDB
//    service in its fed-slice mode (vibendb::Service::startFed — the same skimmer, beacon list, harmonic flags) with the
//    same gallery sink a listener's NDB mode has (vibe_ndb_gallery.h DayLog — the day's heard log and map, the last 10
//    minutes and one card per UTC hour), labelled "background" and the profile.
// ★ Split from vibe_bgdecode.h so the runner and its tests do not carry the beacon list (383 KB of navaids): the host that
//   links the NDB decoder installs makeNdbFeedFactory() as BgRunner::Params::ndbMake.
#pragma once
#include "vibe_bgdecode.h"
#include "vibe_ndb_gallery.h"   // ★ DayLog — and through it vibe_ndb_service.h

#include <cmath>
#include <functional>
#include <memory>

namespace vibe {
namespace bg {

// ★ One band, one rule: the save path's copy of the band (vibe_bgdecode_config.h) must be the decoder's.
static_assert(kNdbBandLoHz == vibendb::kBandLoHz && kNdbBandHiHz == vibendb::kBandHiHz && kNdbMinSpanHz == vibendb::kMinSpanHz,
              "the NDB band in vibe_bgdecode_config.h must match vibe_ndb_service.h");

class ServiceNdbFeed : public NdbFeed {
public:
    ServiceNdbFeed(const NdbStart& s, std::shared_ptr<vibendb::Sink> sink, std::function<int64_t()> clock = nullptr) {
        if (clock) svc_.setClockForTest(std::move(clock));
        const bool havePos = std::isfinite(s.lat) && std::isfinite(s.lon);
        svc_.startFed(s.rate, s.hwCentreHz, s.loHz, s.hiHz, havePos, havePos ? s.lat : 0, havePos ? s.lon : 0, std::move(sink));
    }
    ~ServiceNdbFeed() override { svc_.stop(); }
    void feedSlice(const vibedsp::cf32* x, int n, double rate, double centreRfHz) override {
        svc_.feedSlice(reinterpret_cast<const vibendb::Service::cf32*>(x), n, rate, centreRfHz);
    }
    void aim(double hwCentreHz, double loHz, double hiHz) override { svc_.aimFed(hwCentreHz, loHz, hiHz); }
    void stop() override { svc_.stop(); }
    uint64_t cpuNs() const override { return svc_.fedCpuNs(); }
    uint64_t droppedBlocks() const override { return svc_.droppedBlocks(); }
    std::string brief() const override { return svc_.brief(); }
    vibendb::Service& service() { return svc_; }
private:
    vibendb::Service svc_;
};

/** ★★ THE FACTORY a host installs as Params::ndbMake. `store` null = no gallery (the skimmer still runs — the admin page's
 *  line — but keeps nothing). `clock` is the tests' (null = the wall clock). */
inline std::function<std::shared_ptr<NdbFeed>(const NdbStart&)> makeNdbFeedFactory(gallery::Store* store, std::function<int64_t()> clock = nullptr) {
    return [store, clock](const NdbStart& s) -> std::shared_ptr<NdbFeed> {
        std::shared_ptr<vibendb::Sink> sink;
        if (store) {
            ndbgal::DayLog::Options o;
            o.origin = "background"; o.profile = "Profile " + std::to_string(s.profileIdx + 1);
            o.havePos = std::isfinite(s.lat) && std::isfinite(s.lon);
            o.lat = o.havePos ? s.lat : 0; o.lon = o.havePos ? s.lon : 0;
            o.clock = clock;
            sink = std::make_shared<ndbgal::DayLog>(*store, o);
        }
        return std::make_shared<ServiceNdbFeed>(s, sink, clock);
    };
}

} // namespace bg
} // namespace vibe
