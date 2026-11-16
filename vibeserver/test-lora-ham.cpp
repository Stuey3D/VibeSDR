// test-lora-ham.cpp — V12 LoRa-APRS, FANET and MeshCom on the LoRa engine (vibe_lora_ham.h, vibe_lora_service.h,
// vibe_lora_track.h): packets built from each published description, parsed (payload level), then modulated as LoRa
// (vibe_lora.h encode/modulate) into noisy IQ at 2.4 MS/s on the 433 and 868 bands and decoded by the real engine
// through its channel plan; the node table's rows, the privacy rule (amateur exact; a FANET pilot exact only with the
// online-tracking bit), the public messages, the owner's per-network switches, and junk that must not parse. Silent.
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-lora-ham vibeserver/test-lora-ham.cpp -lpthread
#include "vibe_ism_service.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibelora;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); (c ? passes : fails)++; }
static bool has(const std::string& s, const std::string& k) { return s.find(k) != std::string::npos; }

// ── packet builders ───────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> laPacket(const std::string& tnc2) {
    std::vector<uint8_t> b = {'<', 0xFF, 0x01};
    b.insert(b.end(), tnc2.begin(), tnc2.end());
    return b;
}
static void put24(std::vector<uint8_t>& b, double v) {
    int32_t i = int32_t(std::lround(v));
    const uint32_t u = uint32_t(i) & 0xFFFFFF;
    b.push_back(uint8_t(u)); b.push_back(uint8_t(u >> 8)); b.push_back(uint8_t(u >> 16));
}
/** FANET type 1: manufacturer 0x11, id `id`, at lat/lon, `altM`, `kmh`, climb m/s, heading; `online` the tracking bit. */
static std::vector<uint8_t> fnTrack(uint16_t id, double lat, double lon, int altM, double kmh, double climb, double hdg, int type, bool online) {
    std::vector<uint8_t> b = {0x01, 0x11, uint8_t(id), uint8_t(id >> 8)};
    put24(b, lat * 93206.0); put24(b, lon * 46603.0);
    int alt = altM, sc = 0; if (alt > 2047) { alt = (alt + 2) / 4; sc = 1; }
    const int st = (online ? 0x8000 : 0) | (type & 7) << 12 | sc << 11 | (alt & 0x7FF);
    b.push_back(uint8_t(st)); b.push_back(uint8_t(st >> 8));
    int sp = int(std::lround(kmh / 0.5)); uint8_t spb = sp > 127 ? uint8_t(0x80 | (int(std::lround(kmh / 2.5)) & 0x7F)) : uint8_t(sp);
    b.push_back(spb);
    int cl = int(std::lround(climb / 0.1)); b.push_back(uint8_t(cl & 0x7F));
    b.push_back(uint8_t(std::lround(hdg * 256.0 / 360.0) & 0xFF));
    return b;
}
static std::vector<uint8_t> fnName(uint16_t id, const std::string& n) {
    std::vector<uint8_t> b = {0x02, 0x11, uint8_t(id), uint8_t(id >> 8)};
    b.insert(b.end(), n.begin(), n.end());
    return b;
}
static std::vector<uint8_t> fnGround(uint16_t id, double lat, double lon, int state, bool online) {
    std::vector<uint8_t> b = {0x07, 0x11, uint8_t(id), uint8_t(id >> 8)};
    put24(b, lat * 93206.0); put24(b, lon * 46603.0);
    b.push_back(uint8_t(state << 4 | (online ? 1 : 0)));
    return b;
}
/** FANET type 4: a weather station with temperature, wind and pressure. */
static std::vector<uint8_t> fnWeather(uint16_t id, double lat, double lon, double tC, double dir, double kmh, double gust, double hpa) {
    std::vector<uint8_t> b = {0x04, 0x11, uint8_t(id), uint8_t(id >> 8), uint8_t(0x40 | 0x20 | 0x08)};
    put24(b, lat * 93206.0); put24(b, lon * 46603.0);
    b.push_back(uint8_t(int8_t(std::lround(tC * 2))));
    b.push_back(uint8_t(std::lround(dir * 256.0 / 360.0)));
    b.push_back(uint8_t(std::lround(kmh / 0.2)) & 0x7F); b.push_back(uint8_t(std::lround(gust / 0.2)) & 0x7F);
    const int p = int(std::lround((hpa - 430.0) * 10)); b.push_back(uint8_t(p)); b.push_back(uint8_t(p >> 8));
    return b;
}
/** MeshCom: type, id, hop byte, "SRC>DEST" + payload, 0x00, hardware, modulation, the 16-bit sum FCS. */
static std::vector<uint8_t> mxPacket(uint8_t type, uint32_t id, uint8_t hop, const std::string& text) {
    std::vector<uint8_t> b = {type, uint8_t(id), uint8_t(id >> 8), uint8_t(id >> 16), uint8_t(id >> 24), hop};
    b.insert(b.end(), text.begin(), text.end());
    b.push_back(0); b.push_back(9); b.push_back(3);
    unsigned s = 0; for (uint8_t v : b) s += v;
    b.push_back(uint8_t(s)); b.push_back(uint8_t(s >> 8));
    return b;
}
static Frame frameOf(const std::vector<uint8_t>& b, uint8_t sync, double hz, int sf, double bw) {
    Frame f; f.bytes = b; f.sync = sync; f.freqHz = hz; f.sf = sf; f.bw = bw; f.cr = 1; f.crcOn = true; return f;
}

// ── IQ ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Tx { double hz, bw; int sf, cr; uint8_t sync; std::vector<uint8_t> bytes; double at; double cfo; };
static std::vector<cf> render(const std::vector<Tx>& txs, double fs, double centre, double secs, double snrDb, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<cf> x(size_t(secs * fs), cf(0, 0));
    for (const auto& t : txs) {
        Phy p; p.sf = t.sf; p.bw = t.bw; p.cr = t.cr; p.sync = t.sync; p.preamble = 10; p.ldro = ldroFor(t.sf, t.bw);
        const auto w = modulate(p, encode(p, t.bytes), fs, t.hz - centre + t.cfo, -t.cfo / t.hz * 1e6, 0.4, 0.3);
        const double amp = std::sqrt(std::pow(10.0, snrDb / 10) * 1e-4 * t.bw / fs);
        const size_t s0 = size_t(t.at * fs);
        for (size_t i = 0; i < w.size() && s0 + i < x.size(); ++i) x[s0 + i] += w[i] * float(amp);
    }
    std::normal_distribution<float> g(0, float(std::sqrt(0.5e-4)));
    for (auto& v : x) v += cf(g(rng), g(rng));
    return x;
}
static std::vector<Heard> run(const std::string& band, double hc, const std::vector<cf>& x, std::vector<std::string> off = {}, size_t* chans = nullptr,
                              double fs = 2.4e6, std::vector<std::string>* names = nullptr) {
    Engine e;
    e.setNetsOff(std::move(off));
    e.startInline(band, hc, fs, INFINITY);
    if (chans) *chans = e.channelsRun();
    if (names) *names = e.channelNames();
    for (size_t i = 0; i < x.size(); i += 65536) e.process(x.data() + i, std::min<size_t>(65536, x.size() - i));
    return e.take();
}

int main() {
    std::printf("── LoRa-APRS: '<' 0xFF 0x01 + TNC2 through our APRS parser ──\n");
    {
        Msg m;
        ok(loraaprs::parse(laPacket("M0ABC-7>APLRT1,WIDE1-1:!5214.40N/00052.20W[Walking the dog"), m) && m.proto == "loraaprs",
           "a tracker's position packet parses");
        ok(m.node == "M0ABC-7" && std::fabs(m.lat - 52.24) < 1e-4 && std::fabs(m.lon + 0.87) < 1e-4, "callsign and position (52.2400, -0.8700)");
        ok(m.sym == "/[" && m.path == "WIDE1-1" && has(m.aprsLine, "Walking the dog"), "symbol, path and comment (" + m.aprsLine + ")");
        Msg i;
        ok(loraaprs::parse(laPacket("MB7UAP-10>APLG01:!5212.00NL00050.00W&LoRa iGate 433.775"), i) && nodeType(i.proto, i.role, i.sym) == std::string("repeater"),
           "an iGate (symbol &) draws as infrastructure");
        Msg msg;
        ok(loraaprs::parse(laPacket("G4XYZ-5>APLRT1::M0ABC-7  :see you at the club{12"), msg) && msg.text == "see you at the club" && msg.channel == "M0ABC-7",
           "an APRS message: the text, to whom, without its {id");
        Msg wx;
        ok(loraaprs::parse(laPacket("M0WX-13>APLRW1:!5213.00N/00051.00W_090/005g010t050h80b10132"), wx) && std::fabs(wx.tempC - 10.0) < 0.1 && std::fabs(wx.hpa - 1013.2) < 0.05,
           "a LoRa weather station: 10 °C, 1013.2 hPa");
        Msg bad;
        ok(!loraaprs::parse(laPacket("not a packet"), bad) && !loraaprs::parse({'<', 0xFF, 0x02, 'A', '>', 'B', ':', '!'}, bad)
           && !loraaprs::parse(laPacket("M0 ABC>APRS:!x"), bad), "junk, a wrong prefix and a bad callsign do not parse");
        // through interpret() as the engine calls it
        const Msg im = interpret(frameOf(laPacket("M0ABC-7>APLRT1:!5214.40N/00052.20W["), 0x12, 433.775e6, 12, 125e3), "loraaprs");
        ok(im.proto == "loraaprs" && im.sf == 12, "interpret(): a 0x12 frame with the prefix is LoRa-APRS");
    }

    std::printf("── FANET: tracking, name, ground tracking, a weather station, a message ──\n");
    {
        Msg t;
        ok(fanet::parse(fnTrack(0x1234, 46.6800, 7.8600, 2450, 36, 1.8, 225, 1, true), t) && t.proto == "fanet" && t.what == "tracking", "type 1 tracking parses");
        ok(t.node == "11:1234" && std::fabs(t.lat - 46.68) < 2e-5 && std::fabs(t.lon - 7.86) < 3e-5, "address 11:1234, position 46.6800 / 7.8600");
        ok(std::fabs(t.altM - 2448) <= 4 && std::fabs(t.speedKmh - 36) < 0.3 && std::fabs(t.climb - 1.8) < 0.06 && std::fabs(t.course - 225) < 1.5 && t.online
           && t.role == "paraglider", "2450 m (4 m steps), 36 km/h, +1.8 m/s, 225°, paraglider, online tracking");
        Msg n;
        ok(fanet::parse(fnName(0x1234, "Anna B"), n) && n.longName == "Anna B", "type 2 name");
        Msg g;
        ok(fanet::parse(fnGround(0x7777, 46.70, 7.90, 13, false), g) && g.what == "ground" && g.role == "needs medical help" && !g.online, "type 7 ground tracking: needs medical help");
        Msg w;
        ok(fanet::parse(fnWeather(0x0042, 46.75, 7.95, 12.5, 270, 18, 30, 1015.3), w) && w.role == "weather station" && std::fabs(w.tempC - 12.5) < 0.01
           && std::fabs(w.windKmh - 18) < 0.11 && std::fabs(w.windDir - 270) < 1.5 && std::fabs(w.hpa - 1015.3) < 0.05, "type 4 service: weather 12.5 °C, 18 km/h from 270°, 1015.3 hPa");
        Msg bad;
        ok(!fanet::parse({0x01, 0x11, 0x00}, bad) && !fanet::parse({0x01, 0x11, 0x34, 0x12, 1, 2}, bad), "short frames do not parse");
        // the node table: name + track merge, and the privacy rule
        MeshTable mt;
        mt.accept(n, 1); mt.accept(t, 2);
        Msg t2; fanet::parse(fnTrack(0x5555, 46.6912, 7.8734, 1800, 30, -0.5, 90, 4, false), t2); mt.accept(t2, 3);
        std::string all; for (auto& r : mt.rowsJson(3)) all += r.second + "\n";
        ok(has(all, "\"name\":\"Anna B\"") && has(all, "\"lat\":46.6") && !has(all, "\"lat\":46.68,\"lon\":7.86,\"approx") && has(all, "\"ntype\":\"aircraft\"") && has(all, "\"online\":1"),
           "Anna B (online tracking on): one row, exact, an aircraft");
        if (fails) std::printf("%s", all.c_str());
        ok(has(all, "\"lat\":46.69,\"lon\":7.87,\"approx\":1") && !has(all, "46.6912") && !has(all, "7.8734"),
           "a glider without online tracking: ~1 km and approximate, the exact figure nowhere");
    }

    std::printf("── MeshCom: text and position ──\n");
    {
        Msg x;
        ok(meshcom::parse(mxPacket(':', 0x01020304, 0x05, "OE1KBC-12>*:Servus aus Wien"), x) && x.proto == "meshcom" && x.what == "text", "a text to all parses");
        ok(x.from == "OE1KBC-12" && x.text == "Servus aus Wien" && x.broadcast && x.channel == "all" && x.hopLimit == 5 && x.fcsOk == 1 && x.hwId == 9,
           "from, text, to all, MAX-HOP 5, the FCS sum agrees, hardware 9 (RAK4631)");
        Msg g;
        ok(meshcom::parse(mxPacket(':', 7, 0x84, "DL2ABC-1>232:Gruppe 232 hallo"), g) && g.channel == "232" && g.broadcast && g.route == "via MQTT",
           "a group message (232), arrived via MQTT");
        Msg p;
        ok(meshcom::parse(mxPacket('!', 9, 0x03, "OE3XYZ-1>*!4812.34N/01622.15E#/B=085/A=000656"), p) && std::fabs(p.lat - 48.2057) < 1e-3 && std::fabs(p.lon - 16.3692) < 1e-3,
           "a position: 48.2057 / 16.3692 (" + std::to_string(p.lat) + ", " + std::to_string(p.lon) + ")");
        ok(nodeType(p.proto, p.role, p.sym) == std::string("repeater") && std::fabs(p.altM - 200) < 1, "symbol # = a digipeater; altitude 656 ft = 200 m");
        Msg bad;
        ok(!meshcom::parse(mxPacket(':', 1, 5, "no arrow here"), bad) && !meshcom::parse(mxPacket('X', 1, 5, "OE1KBC>*:hi"), bad)
           && !meshcom::parse({':', 1, 2, 3, 4, 5, 'O', 'E', '>', '*', ':', 'x'}, bad), "junk, an unknown type and no terminator do not parse");
        const Msg im = interpret(frameOf(mxPacket(':', 3, 5, "OE1KBC-12>*:hello"), 0x2B, 433.175e6, 11, 250e3), "meshcom");
        ok(im.proto == "meshcom", "interpret(): 0x2B on the MeshCom channel is MeshCom");
        // the table: amateur = exact; the text in the public messages
        MeshTable mt;
        std::vector<Event> ev;
        mt.accept(p, 1, &ev); mt.accept(x, 2, &ev);
        std::string all; for (auto& r : mt.rowsJson(2)) all += r.second;
        ok(has(all, "\"lat\":48.205") && !has(all, "approx"), "an amateur station's position exact (public by design)");
        ok(has(mt.msgsJson(2), "\"from\":\"OE1KBC-12\"") && has(mt.msgsJson(2), "Servus aus Wien"), "the text in the public messages");
        bool evOk = false; for (auto& e : ev) evOk |= e.kind == "meshmsg" && has(e.line, "MeshCom");
        ok(evOk, "the gallery's message line says MeshCom");
    }

    std::printf("── the plans: where the channels are, what they cost ──\n");
    {
        bool la = false, mx = false, fn = false, fz = false;
        for (const auto& c : planFor("433")) { la |= std::string(c.net) == "loraaprs" && c.hz == 433.775e6 && c.tier == 0; mx |= std::string(c.net) == "meshcom" && c.hz == 433.175e6; }
        for (const auto& c : planFor("868")) fn |= std::string(c.net) == "fanet" && c.hz == 868.2e6 && c.tier == 1;
        for (const auto& c : planFor("920")) fz |= std::string(c.net) == "fanet" && c.hz == 920.8e6 && c.tier == 2;
        ok(la && mx && fn && fz, "LoRa-APRS 433.775 (default set), MeshCom 433.175, FANET 868.2 (tier 1) and 920.8 (tier 2)");
        double u = 0; for (const auto& c : planFor("868")) if (c.tier == 0) u += chanCost(c);
        ok(std::fabs(u - 25.85) < 0.01, "the 868 default set (the Pi 2's budget yardstick) is unchanged: " + std::to_string(u) + " units");
    }

    std::printf("── on the air (synthetic): 433 band at 3.2 MS/s, radio at 434.17 MHz ──\n");
    {
        // ★ 3.2 MS/s: at 2.4 MS/s the capture's edge (±1.08 MHz) stops short of 433.175 ± 125 kHz — checked below
        const double hc = 434.17e6, fs = 3.2e6;
        std::vector<Tx> txs = {
            {433.775e6, 125e3, 12, 1, 0x12, laPacket("M0ABC-7>APLRT1,WIDE1-1:!5214.40N/00052.20W[Walking the dog"), 0.3, 1500},
            {433.775e6, 125e3, 12, 1, 0x12, laPacket("MB7UAP-10>APLG01:!5212.00NL00050.00W&LoRa iGate"), 3.2, -900},
            {433.175e6, 250e3, 11, 2, 0x2B, mxPacket(':', 0x0A0B0C0D, 5, "OE1KBC-12>*:Servus aus Wien"), 1.1, 600},
        };
        const auto x = render(txs, fs, hc, 6.0, 0.0, 3);
        size_t ch = 0;
        const auto got = run("433", hc, x, {}, &ch, fs);
        int la = 0, mx = 0;
        for (const auto& h : got) { la += h.m.proto == "loraaprs"; mx += h.m.proto == "meshcom";
            std::printf("    %-28s SF%-2d %6.1f kHz SNR %5.1f  %s\n", h.f.chan.c_str(), h.f.sf, h.f.bw / 1e3, h.f.snrDb, hamLine(h.m).c_str()); }
        std::printf("    %zu channels run\n", ch);
        ok(la == 2, "both LoRa-APRS packets decoded at 0 dB SNR (" + std::to_string(la) + ")");
        ok(mx == 1, "the MeshCom text decoded at 0 dB SNR, CR 4/6 (" + std::to_string(mx) + ")");
        // the owner's switches
        const auto offd = run("433", hc, x, {"loraaprs", "meshcom"}, nullptr, fs);
        std::vector<std::string> nm; const std::vector<cf> none(1024);
        run("433", hc, none, {}, nullptr, 2.4e6, &nm);
        bool la24 = false, mx24 = false; for (auto& n : nm) { la24 |= has(n, "LoRa APRS"); mx24 |= has(n, "MeshCom"); }
        ok(la24 && !mx24, "at 2.4 MS/s (radio at 434.17) LoRa-APRS runs; MeshCom 433.175 is past the capture's edge and is not run");
        int any = 0; for (const auto& h : offd) any += h.m.proto == "loraaprs" || h.m.proto == "meshcom";
        ok(any == 0, "LoRa-APRS and MeshCom switched off: their channels are not run, nothing decoded");
    }
    std::printf("── on the air (synthetic): 868 band at 2.4 MS/s, radio at 868.8 MHz ──\n");
    {
        const double hc = 868.8e6;
        std::vector<Tx> txs = {
            {868.2e6, 250e3, 7, 1, 0xF1, fnTrack(0x1234, 46.68, 7.86, 2450, 36, 1.8, 225, 1, true), 0.2, 2000},
            {868.2e6, 250e3, 7, 1, 0xF1, fnName(0x1234, "Anna B"), 0.9, 2000},
            {868.2e6, 250e3, 7, 1, 0xF1, fnWeather(0x0042, 46.75, 7.95, 12.5, 270, 18, 30, 1015.3), 1.5, -1300},
        };
        const auto x = render(txs, 2.4e6, hc, 2.5, 3.0, 5);
        const auto got = run("868", hc, x);
        int fn = 0; bool named = false;
        MeshTable mt;
        for (const auto& h : got) if (h.m.proto == "fanet") { ++fn; named |= h.m.longName == "Anna B"; mt.accept(h.m, 1); }
        ok(fn == 3 && named, "three FANET frames decoded at +3 dB, the name among them (" + std::to_string(fn) + ")");
        std::string all; for (auto& r : mt.rowsJson(1)) all += r.second;
        ok(has(all, "\"kind\":\"fanet\"") && has(all, "\"ntype\":\"sensor\"") && has(all, "Anna B"), "the rows: Anna B and the weather station");
        const auto offd = run("868", hc, x, {"fanet"});
        int any = 0; for (const auto& h : offd) any += h.m.proto == "fanet";
        ok(any == 0, "FANET switched off: nothing decoded");
    }
    std::printf("── noise: 10 s at 2.4 MS/s on each band, every channel ──\n");
    {
        const auto x = render({}, 2.4e6, 434.17e6, 10.0, 0, 7);
        int bad = 0; for (const auto& h : run("433", 434.17e6, x)) bad += h.m.proto == "loraaprs" || h.m.proto == "meshcom";
        const auto y = render({}, 2.4e6, 868.8e6, 10.0, 0, 8);
        for (const auto& h : run("868", 868.8e6, y)) bad += h.m.proto == "fanet";
        ok(bad == 0, "no LoRa-APRS, MeshCom or FANET out of noise");
    }

    std::printf("test-lora-ham: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
