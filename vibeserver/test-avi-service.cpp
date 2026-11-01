// test-avi-service.cpp — V12 aviation service (vibe_avi_service.h): the aircraft table merges VDL2 (ICAO address),
// ACARS (registration) and HFDL (ICAO / flight) into one row per aircraft; message bodies only with the owner's opt-in;
// the thread drops the OLDEST block when it falls behind and counts it. docs/v12/AVIATION.md §Service.
//  `test-avi-service --cu8 <vdl2.u8> <rate>` runs a real VDL2 capture through the service and prints the table.
#include "vibe_avi_service.h"

#include <cstdio>
#include <cstring>
#include <thread>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc > 3 && !std::strcmp(argv[1], "--cu8")) {
        vibeavi::Tracker tr(false);
        vibeavi::Service svc(tr);
        const double rate = std::atof(argv[3]);
        svc.start("vdl2", 136.850e6, rate, vibevdl2::defaultChannels());
        FILE* f = std::fopen(argv[2], "rb");
        std::vector<uint8_t> buf(1 << 17); std::vector<vibeavi::cf32> z(1 << 16);
        size_t r;
        while (f && (r = std::fread(buf.data(), 2, 1 << 16, f)) > 0) {
            vibeavi::u8ToCf(buf.data(), r, z.data());
            svc.feed(z.data(), r);
            std::this_thread::sleep_for(std::chrono::microseconds(2000));   // faster than real time, not unbounded
        }
        if (f) std::fclose(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        svc.stop();
        std::printf("messages %llu, dropped blocks %llu\n%s\n", (unsigned long long)svc.messages(), (unsigned long long)svc.dropped(), tr.json(1e18, 1e30).c_str());
        return 0;
    }
    std::printf("1. one row per aircraft, from three decoders\n");
    {
        vibeavi::Tracker tr(false);
        vibevdl2::Frame v; v.src.addr = 0x4070E8; v.src.type = 1; v.dst.addr = 0x109E6A; v.dst.type = 4; v.hz = 136.975e6; v.levelDb = -20;
        v.hasAcars = true; v.acarsCrcOk = true; v.acars.reg = "G-JZHU"; v.acars.flight = "LS08DE"; v.acars.label = "H1"; v.acars.text = "#DFB SECRET";
        tr.addVdl2(v, 100);
        vibeacars::Block b; b.reg = "G-JZHU"; b.flight = "LS08DE"; b.label = "16"; b.text = "POS N51"; b.hz = 131.725e6; b.levelDb = -30;
        tr.addAcars(b, 110);
        vibehfdl::Message h; h.downlink = true; h.icao = 0x4070E8; h.hz = 8977e3; h.dstId = 3; h.flight = "EXS8DE"; h.lat = 51.0; h.lon = -1.0;
        tr.addHfdl(h, 120);
        vibevdl2::Frame g; g.src.addr = 0x10949A; g.src.type = 4; g.dst.addr = 0; g.dst.type = 7;   // ground to all: no aircraft
        tr.addVdl2(g, 121);
        CHECK(tr.size() == 1, "rows %zu (VDL2 ICAO + ACARS registration + HFDL ICAO = one aircraft)", tr.size());
        const std::string j = tr.json(130);
        CHECK(j.find("\"icao\":\"4070E8\"") != std::string::npos && j.find("\"reg\":\"G-JZHU\"") != std::string::npos && j.find("\"lat\":51.0000") != std::string::npos,
              "merged row: %s", j.c_str());
        CHECK(j.find("SECRET") == std::string::npos && j.find("POS N51") == std::string::npos, "no message body without the opt-in: %s", j.c_str());
        CHECK(j.find("\"via\":\"HFDL 8977 Reykjavik\"") != std::string::npos, "via the last decoder: %s", j.c_str());
        tr.setBodies(true);
        tr.addAcars(b, 140);
        CHECK(tr.json(141).find("POS N51") != std::string::npos, "the body with the owner's opt-in");
        tr.setBodies(false);
        CHECK(tr.json(141).find("POS N51") == std::string::npos, "and gone again when it is switched off");
    }
    std::printf("2. the thread drops the oldest and counts it (a decoder that cannot keep up never stalls the radio)\n");
    {
        vibeavi::Tracker tr;
        vibeavi::Service svc(tr);
        svc.start("vdl2", 136.850e6, 2.4e6, vibevdl2::defaultChannels());
        std::vector<vibeavi::cf32> blk(1 << 16, vibeavi::cf32(0.01f, 0));
        for (int i = 0; i < 400; ++i) svc.feed(blk.data(), blk.size());   // 11 s of capture at once
        svc.stop();
        std::printf("   dropped %llu blocks of 400\n", (unsigned long long)svc.dropped());
        CHECK(svc.dropped() > 0, "drops counted");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
