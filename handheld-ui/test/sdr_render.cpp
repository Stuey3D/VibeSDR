// sdr_render.cpp — MENU → SDR HARDWARE drawn for every radio driver from a CANNED hwinfo (+ rspstat), offscreen, so the
// drivers the e2e rig cannot run (it has a fake RTL only) can still be looked at. The canned messages follow the shim's
// writers (android/app/src/main/cpp/local_sdr_shim.cpp: the hwinfo builder, radioCapsJsonCore, the rspstat printf;
// vibe_usrp_caps.h capsJson). No engine: App::eng_ is null, so a press sends nothing.
// Usage (in the arm64 test image, SDL offscreen): sdr_render <outDir>   → <outDir>/sdr-<scenario>-<n>.ppm
#include "../src/app.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hh;

struct Scenario { const char* name; const char* hwinfo; const char* rspstat; };

static const Scenario kScen[] = {
    {"rtl-v4",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"ifGrFloor":-1,"gainNow":297,"dsActive":false,"autoDs":true,"dsBelowHz":24000000,"ds":0,"agc":0,"digitalAgc":0,
        "gains":[0,9,14,27,37,77,87,125,144,157,166,197,207,229,254,280,297,328,338,364,372,386,402,421,434,439,445,480,496],
        "rates":[1024000,1400000,1800000,2048000,2400000,2560000],"rateNow":2400000,"tunerBwAuto":false,"tunerBw":1000000,
        "tunerBws":[2800000,2000000,1400000,1000000,700000,500000,350000],"ppm":0,"ifCap":1400000,"ifLocked":false,"biasT":true,
        "radio":{"driver":"rtl","model":"RTL-SDR Blog V4"}})", nullptr},
    {"rsp1a",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"ifGrFloor":-1,"gainNow":0,"agc":0,"gains":[],
        "rates":[2000000,6000000,8000000,10000000],"rateNow":6000000,"ppm":0,"biasT":false,"rspBiasT":false,"rfNotch":false,"dabNotch":true,
        "radio":{"driver":"sdrplay","model":"RSP1A","lnaStates":10,"ifGrMin":20,"ifGrMax":59,"agcSetPoint":true,"rfNotch":true,"dabNotch":true,
                 "biasT":true,"biasTOn":false,"antennas":[],"antennaLocked":false,"hdr":false,"amNotch":false,"extRef":false,"vagcMode":0,"vagcAuto":true}})",
     R"({"type":"rspstat","sysGain":38.2,"lna":3,"ifgr":42,"overload":1,"settling":0,"rfNotch":0,"dabNotch":1,"autoNotch":0,"userNotch":1,"rfAgc":1,
        "agcSet":-30,"adcPeak":-24.5,"adcClip":0,"lnaN":10,"agcReinit":0,"agcInit":0,"gainStuck":0,"vagc":1,"vst":2,"vrs":0,"vtgt":-20,"vwpm":3,
        "vmiss":0,"vblind":0,"vrail":0})"},
    {"rspdx",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"ifGrFloor":30,"gainNow":0,"agc":0,"gains":[],
        "rates":[2000000,6000000,8000000],"rateNow":2000000,"ppm":3,"biasT":false,"rspBiasT":true,
        "radio":{"driver":"sdrplay","model":"RSPdx-R2","lnaStates":28,"ifGrMin":20,"ifGrMax":59,"agcSetPoint":true,"rfNotch":true,"dabNotch":true,
                 "biasT":true,"biasTOn":true,"antennas":["Antenna A","Antenna B","Antenna C"],"antenna":"Antenna B","antennaLocked":true,
                 "antennaNames":{"Antenna A":"Long wire","Antenna C":"Loop"},
                 "hdr":true,"hdrOn":true,"amNotch":true,"amNotchOn":false,"extRef":true,"extRefOn":false,"vagcMode":2,"vagcAuto":false}})",
     R"({"type":"rspstat","sysGain":22.0,"lna":12,"ifgr":51,"overload":0,"settling":0,"rfNotch":1,"dabNotch":0,"autoNotch":1,"userNotch":1,"rfAgc":0,
        "agcSet":-45,"adcPeak":-31,"lnaN":19,"gainStuck":1,"vagc":0,"vst":0,"vrail":0})"},
    {"airspyhf",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"gainNow":0,"agc":0,"gains":[],"rates":[768000],"rateNow":768000,
        "radio":{"driver":"airspyhf","model":"Airspy HF+ Discovery","attSteps":9,"attStepDb":6,"hfLna":true,"hfAgc":true,"agcThreshold":true,"calPpb":true,
                 "ranges":[[500,31000000],[60000000,260000000]],"rates":[768000]}})", nullptr},
    {"airspy",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"gainNow":120,"agc":0,
        "gains":[0,10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160,170,180,190,200,210],"rates":[2500000,10000000],"rateNow":10000000,
        "radio":{"driver":"airspy","model":"Airspy R2","serial":"A1","gainPresets":22,"stageMax":15,"curve":"linearity","gainMode":1,"lnaAgc":false,
                 "mixerAgc":false,"lna":8,"mixer":6,"vga":10,"biasT":false,"packing":true,"rates":[2500000,10000000],"aspLimit":null,
                 "hasBiasT":true,"hasPacking":true,"noDirectSampling":true}})", nullptr},
    {"airspy-free",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"gainNow":120,"agc":0,
        "gains":[0,10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160,170,180,190,200,210],"rates":[2500000,10000000],"rateNow":10000000,
        "radio":{"driver":"airspy","model":"Airspy Mini","serial":"A2","gainPresets":22,"stageMax":15,"curve":"linearity","gainMode":2,"lnaAgc":true,
                 "mixerAgc":false,"lna":8,"mixer":6,"vga":10,"biasT":true,"packing":false,"rates":[3000000,6000000],"aspLimit":null,
                 "hasBiasT":true,"hasPacking":false,"noDirectSampling":true}})", nullptr},
    {"hackrf",
     R"({"type":"hwinfo","gainCap":600,"agcLocked":false,"gainLocked":false,"gainNow":0,"agc":0,"gains":[],"rates":[2000000,8000000,10000000,20000000],"rateNow":8000000,
        "radio":{"driver":"hackrf","model":"HackRF One","hrfAmp":true,"hrfLna":true,"hrfVga":true,"hrfBiasT":true,"hwAgc":false,"lnaMax":40,"lnaStep":8,
                 "vgaMax":62,"vgaStep":2,"amp":0,"lna":16,"vga":20,"biast":0,"ranges":[[1000000,6000000000]],"rates":[2000000,8000000,10000000,20000000]}})", nullptr},
    {"usrp",
     R"({"type":"hwinfo","gainCap":-1,"agcLocked":false,"gainLocked":false,"gainNow":400,"agc":0,
        "gains":[0,50,100,150,200,250,300,350,400,450,500,550,600,650,700,760],"rates":[1000000,2000000,4000000,8000000,16000000],"rateNow":8000000,
        "radio":{"driver":"usrp","model":"USRP B210","serial":"U1","hwAgc":true,"agcOn":false,"usb":3,"rates":[1000000,2000000,4000000,8000000,16000000],
                 "antennas":["RX2","TX/RX"],"antenna":"RX2","antennaLocked":false,"antennaNames":{"RX2":"Discone"},
                 "clockSources":["internal","external","gpsdo"],"clock":"gpsdo","refLocked":true,"gpsdo":true,"gpsLocked":false,
                 "hasBiasT":false,"noDirectSampling":true,"noTunerBw":true,"experimental":true}})", nullptr},
    {"rtl-locked",
     R"({"type":"hwinfo","gainCap":250,"agcLocked":true,"gainLocked":true,"gainNow":229,"autoDs":false,"ds":0,"agc":1,"digitalAgc":1,
        "gains":[0,9,14,27,37,77,87,125,144,157,166,197,207,229,254,280,297],"rates":[2400000],"rateNow":2400000,"lockedCentre":0,
        "tunerBwAuto":true,"tunerBw":0,"tunerBws":[2000000,1000000],"ppm":-4,"biasT":false,
        "radio":{"driver":"rtl","model":"Nooelec NESDR SMArt v5","antennas":["HF loop","VHF vertical"],"antenna":"HF loop","antennaLocked":false,
                 "antennaSwitch":{"connected":true,"state":"unknown"}}})", nullptr},
};

static void shot(App& a, const std::string& path) {
    std::vector<uint8_t> px((size_t)a.W * a.H * 4);
    glReadPixels(0, 0, a.W, a.H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", a.W, a.H);
        for (int y = a.H - 1; y >= 0; y--) for (int x = 0; x < a.W; x++) std::fwrite(&px[((size_t)y * a.W + x) * 4], 1, 3, f);
        std::fclose(f);
    }
}

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    App a;
    a.win_ = SDL_CreateWindow("sdr", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(R, 16); a.f_.body.load(R, 19); a.f_.bodyB.load(B, 19); a.f_.btn.load(B, 21); a.f_.title.load(B, 22);
    a.f_.big.load(B, 34); a.f_.freq.load(B, 38); a.f_.freqFine.load(B, 22); a.f_.unit.load(R, 18);
    uint64_t frame = 0;
    for (const Scenario& sc : kScen) {
        RadioState s;
        s.session = "running"; s.connected = true; s.mode = "am";
        const Json h = Json::parse(sc.hwinfo);
        if (h.isNull()) { std::fprintf(stderr, "%s: hwinfo does not parse\n", sc.name); return 1; }
        s.last["hwinfo"] = h;
        if (sc.rspstat) s.last["rspstat"] = Json::parse(sc.rspstat);
        s.hwDriver = h["radio"]["driver"].str();
        for (auto& g : h["gains"].a) s.gains.push_back((int)g.num());
        for (auto& r : h["rates"].a) s.rates.push_back(r.num());
        s.gainNow = (int)h["gainNow"].num(); s.agc = h["agc"].boolean(); s.rateNow = h["rateNow"].num();
        s.maxSpanHz = s.rateNow;
        a.num_.clear(); a.str_.clear();
        a.sheets_ = {App::S_MENU}; a.menuOpen_ = "sdr"; a.menuScroll_ = {};
        a.nav_.forget("menu"); a.nav_.setFocus("menu", "sec.sdr");
        int n = 0;
        std::string lastFocus;
        for (int step = 0; step < 40; step++) {
            // one DOWN, then frames for the scroll to settle; a shot whenever the view has moved on a screenful
            for (int f = 0; f < 14; f++) {
                Input in;
                if (f == 0 && step > 0) in.nav = D_DOWN;
                a.tc_.tick(++frame);
                glViewport(0, 0, a.W, a.H);
                glClearColor(th::bg.r, th::bg.g, th::bg.b, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_BLEND);
                a.nav_.begin("menu", in);
                Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
                a.nav_.layer("menu");
                a.drawMenu(c, s);
                a.nav_.end();
                a.g_.flush();
            }
            const std::string fnow = a.nav_.focus("menu");
            if (step > 0 && fnow == lastFocus) break;          // the bottom
            if (step == 0 || step % 4 == 0) shot(a, out + "/sdr-" + sc.name + "-" + std::to_string(n++) + ".ppm");
            lastFocus = fnow;
            std::fprintf(stderr, "%s step %d focus %s\n", sc.name, step, fnow.c_str());
        }
        shot(a, out + "/sdr-" + sc.name + "-" + std::to_string(n++) + ".ppm");
    }
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(a.win_);
    return 0;
}
