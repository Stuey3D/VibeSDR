// test-spot-report.cpp — V12 beta90 PSK REPORTER + WSPRnet upload (android/app/src/main/cpp/vibe_spot_report.h,
// docs/v12/PSK-REPORTER.md). ★ NOTHING HERE TOUCHES THE NETWORK: the datagrams go to a UDP socket on 127.0.0.1 and the
// WSPRnet upload to an HTTP sink on 127.0.0.1 — never to report.pskreporter.info or wsprnet.org.
//   · the IPFIX encoder, BYTE FOR BYTE against the complete example datagram on https://pskreporter.info/pskdev.html
//     (and its "later in the session" datagram without the descriptors), using the page's own descriptors
//   · our production descriptors, decoded back by an independent little IPFIX reader written here
//   · one record per callsign per batch, the same call + band + mode not again for an hour, a new band at once
//   · a batch every 5 minutes from the reporter's start (+ 0–60 s), never sooner; descriptors in the first three
//     datagrams, then once an hour; the sequence counts data records; a full batch splits under 1400 bytes
//   · off, or no callsign, or a mode with no ADIF name, or a non-callsign: nothing is sent
//   · WSPRnet: only WSPR spots, multipart call + grid + allmept, the ALL_MEPT line format; a failed upload is kept once
//   · the status line, the machine-wide aggregate, the ranking kept only for this callsign
#include "vibe_spot_report.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace vibe::spotreport;
static int g_pass = 0, g_fail = 0;
#define CHECK(c, what) do { if (c) { ++g_pass; std::printf("   ok   %s\n", what); } else { ++g_fail; std::printf("   FAIL %s\n", what); } } while (0)

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> o; int hi = -1;
    for (const char* p = s; *p; ++p) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0'; else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10; else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        if (v < 0) continue;
        if (hi < 0) hi = v; else { o.push_back(uint8_t(hi * 16 + v)); hi = -1; }
    }
    return o;
}
static std::string dump(const std::vector<uint8_t>& b) {
    std::string o; char t[4];
    for (size_t i = 0; i < b.size(); ++i) { std::snprintf(t, sizeof t, "%02X ", b[i]); o += t; if (i % 16 == 15) o += "\n"; }
    return o;
}

// ── An independent IPFIX reader (RFC 7011's layout, written for this test) ──────────────────────────────────────────
struct Decoded {
    bool ok = false; uint16_t version = 0, length = 0; uint32_t exportTime = 0, seq = 0, domain = 0;
    int templateSets = 0;
    std::vector<std::vector<std::string>> rx, tx;   // each record's fields as text ("N1DQ", "14070567", …)
};
static Decoded decode(const std::vector<uint8_t>& b, std::map<uint16_t, Template>& known) {
    Decoded d;
    auto u16 = [&](size_t i) { return uint16_t(b[i] << 8 | b[i + 1]); };
    auto u32 = [&](size_t i) { return uint32_t(u16(i)) << 16 | u16(i + 2); };
    if (b.size() < 16) return d;
    d.version = u16(0); d.length = u16(2); d.exportTime = u32(4); d.seq = u32(8); d.domain = u32(12);
    if (d.length != b.size()) return d;
    size_t i = 16;
    while (i + 4 <= b.size()) {
        const uint16_t sid = u16(i), slen = u16(i + 2);
        if (slen < 4 || i + slen > b.size()) return d;
        const size_t end = i + slen;
        if (sid == 2 || sid == 3) {
            ++d.templateSets;
            size_t j = i + 4;
            Template t; t.id = u16(j); const uint16_t n = u16(j + 2); j += 4;
            if (sid == 3) { t.options = true; j += 2; }
            for (uint16_t k = 0; k < n; ++k) {
                Field f; uint16_t id = u16(j); f.len = u16(j + 2); j += 4;
                f.ent = (id & 0x8000) != 0; f.id = id & 0x7FFF;
                if (f.ent) { if (u32(j) != kPen) return d; j += 4; }
                t.fields.push_back(f);
            }
            known[t.id] = t;
        } else {
            auto it = known.find(sid);
            if (it == known.end()) return d;
            size_t j = i + 4;
            while (j < end) {
                // the rest is padding when it is all zero and shorter than 4
                bool zeros = end - j < 4; for (size_t z = j; z < end && zeros; ++z) zeros = b[z] == 0;
                if (zeros) break;
                std::vector<std::string> rec;
                for (const auto& f : it->second.fields) {
                    if (f.len == kVar) { const uint8_t n = b[j]; rec.push_back(std::string((const char*)&b[j + 1], n)); j += 1 + n; }
                    else { uint64_t v = 0; for (int k = 0; k < f.len; ++k) v = v << 8 | b[j + k];
                           if (f.id == F_SNR) rec.push_back(std::to_string((int)(int8_t)v)); else rec.push_back(std::to_string(v)); j += f.len; }
                }
                (it->second.options ? d.rx : d.tx).push_back(rec);
            }
        }
        i = end;
    }
    d.ok = i == b.size();
    return d;
}

// ── A UDP sink on 127.0.0.1 ──
struct UdpSink {
    int fd = -1; int port = 0;
    UdpSink() {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        ::bind(fd, (sockaddr*)&a, sizeof a);
        socklen_t l = sizeof a; ::getsockname(fd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        timeval tv{0, 300000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~UdpSink() { if (fd >= 0) ::close(fd); }
    std::vector<std::vector<uint8_t>> drain(int* srcPort = nullptr) {
        std::vector<std::vector<uint8_t>> out;
        for (;;) {
            uint8_t buf[65536]; sockaddr_in from{}; socklen_t fl = sizeof from;
            const ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (n <= 0) break;
            if (srcPort) *srcPort = ntohs(from.sin_port);
            out.emplace_back(buf, buf + n);
        }
        return out;
    }
};
// ── An HTTP sink on 127.0.0.1: answers each request with `code` and keeps the request ──
struct HttpSink {
    int fd = -1, port = 0, code = 200; std::vector<std::string> got; std::thread th; std::atomic<bool> stop{false};
    HttpSink() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, (sockaddr*)&a, sizeof a); ::listen(fd, 4);
        socklen_t l = sizeof a; ::getsockname(fd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!stop) {
                pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 100) != 1) continue;
                const int c = ::accept(fd, nullptr, nullptr); if (c < 0) continue;
                timeval tv{2, 0}; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                std::string req; char buf[4096];
                for (;;) {
                    const size_t he = req.find("\r\n\r\n");
                    if (he != std::string::npos) {
                        const size_t cl = req.find("Content-Length: ");
                        const size_t want = cl == std::string::npos ? 0 : (size_t)std::atol(req.c_str() + cl + 16);
                        if (req.size() >= he + 4 + want) break;
                    }
                    const ssize_t n = ::recv(c, buf, sizeof buf, 0); if (n <= 0) break; req.append(buf, (size_t)n);
                }
                got.push_back(req);
                const std::string r = "HTTP/1.1 " + std::to_string(code) + " X\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                ::send(c, r.data(), r.size(), 0); ::close(c);
            }
        });
    }
    ~HttpSink() { stop = true; th.join(); ::close(fd); }
};

int main() {
    std::printf("── PSK Reporter IPFIX encoder vs pskdev.html ──\n");
    // The page's own descriptors: 99 92 (receiverCallsign, receiverLocator, decodingSoftware) and 99 93 (senderCallsign,
    // frequency, mode, informationSource (1 byte), flowStartSeconds).
    Template prx = receiverTemplate(0x9992, false);
    Template ptx; ptx.id = 0x9993;
    ptx.fields = { {F_SENDER_CALL, kVar, true}, {F_FREQ, 4, true}, {F_MODE, kVar, true}, {F_INFO_SOURCE, 1, true}, {F_FLOW_START, 4, false} };
    Receiver rx{"N1DQ", "FN42hn", "Homebrew v5.6", ""};
    std::vector<Report> two(2);
    two[0].call = "N1DQ"; two[0].hz = 14070567; two[0].mode = "PSK"; two[0].t = 1200960084;
    two[1].call = "KB1MBX"; two[1].hz = 14070987; two[1].mode = "PSK"; two[1].t = 1200960104;
    // "An example of a complete datagram (using the sample data above), sent at 1200960114, with sequence number 1."
    const auto golden = hex(
        "00 0A 00 AC 47 95 32 72 00 00 00 01 00 00 00 00"
        "00 03 00 24  99 92 00 03 00 01  80 02 FF FF 00 00 76 8F  80 04 FF FF 00 00 76 8F  80 08 FF FF 00 00 76 8F  00 00"
        "00 02 00 2C  99 93 00 05  80 01 FF FF 00 00 76 8F  80 05 00 04 00 00 76 8F  80 0A FF FF 00 00 76 8F  80 0B 00 01 00 00 76 8F  00 96 00 04"
        "99 92 00 20  04 4E 31 44 51  06 46 4E 34 32 68 6E  0D 48 6F 6D 65 62 72 65 77 20 76 35 2E 36  00 00"
        "99 93 00 2C  04 4E 31 44 51  00 D6 B3 27  03 50 53 4B  01  47 95 32 54"
        "             06 4B 42 31 4D 42 58  00 D6 B4 CB  03 50 53 4B  01  47 95 32 68"
        "             00 00");
    const auto mine = encodeDatagram(prx, rx, ptx, two, 1200960114, 1, 0, true);
    CHECK(golden.size() == 172, "the page's complete datagram is 172 bytes (00 AC)");
    CHECK(mine == golden, "our datagram == the page's complete example, byte for byte");
    if (mine != golden) std::printf("ours:\n%s\npage:\n%s\n", dump(mine).c_str(), dump(golden).c_str());
    // "…later in the session (say with sequence number 4) after the record format descriptors had already been transmitted"
    // ★ the page prints that one's mode as "PSL" (03 50 53 4C) — its own example data, reproduced as printed.
    std::vector<Report> twoL = two; twoL[0].mode = twoL[1].mode = "PSL";
    const auto golden2 = hex(
        "00 0A 00 5C 47 95 32 72 00 00 00 04 00 00 00 00"
        "99 92 00 20  04 4E 31 44 51  06 46 4E 34 32 68 6E  0D 48 6F 6D 65 62 72 65 77 20 76 35 2E 36  00 00"
        "99 93 00 2C  04 4E 31 44 51  00 D6 B3 27  03 50 53 4C  01  47 95 32 54"
        "             06 4B 42 31 4D 42 58  00 D6 B4 CB  03 50 53 4C  01  47 95 32 68  00 00");
    CHECK(encodeDatagram(prx, rx, ptx, twoL, 1200960114, 4, 0, false) == golden2, "…and the page's datagram without the descriptors (seq 4, 00 5C)");
    {   // the page's second receiver descriptor (with antennaInformation)
        std::vector<uint8_t> o; putTemplateSet(o, receiverTemplate(0x9992, true));
        CHECK(o == hex("00 03 00 2C 99 92 00 04 00 01 80 02 FF FF 00 00 76 8F 80 04 FF FF 00 00 76 8F 80 08 FF FF 00 00 76 8F 80 09 FF FF 00 00 76 8F 00 00"),
              "the page's receiver descriptor with antennaInformation, byte for byte");
        std::vector<uint8_t> s; Template t4; t4.id = 0x9993;
        t4.fields = { {F_SENDER_CALL, kVar, true}, {F_FREQ, 4, true}, {F_SNR, 1, true}, {F_IMD, 1, true}, {F_MODE, kVar, true}, {F_INFO_SOURCE, 1, true}, {F_SENDER_LOC, kVar, true}, {F_FLOW_START, 4, false} };
        putTemplateSet(s, t4);
        CHECK(s == hex("00 02 00 44 99 93 00 08 80 01 FF FF 00 00 76 8F 80 05 00 04 00 00 76 8F 80 06 00 01 00 00 76 8F 80 07 00 01 00 00 76 8F 80 0A FF FF 00 00 76 8F 80 0B 00 01 00 00 76 8F 80 03 FF FF 00 00 76 8F 00 96 00 04"),
              "the page's fourth sender descriptor (sNR, iMD, locator), byte for byte");
    }

    std::printf("── our descriptors, read back ──\n");
    {
        Receiver r2{"G4ABC", "IO91wm", "VibeServer 12.0.0-beta90", "Wideband loop"};
        std::vector<Report> rs(1);
        rs[0].call = "DL1XYZ"; rs[0].locator = "JO62"; rs[0].mode = "FT8"; rs[0].hz = 14075612; rs[0].snr = -17; rs[0].t = 1700000000;
        uint32_t seq = 0; int tpl = 1;
        const auto g = buildDatagrams(r2, rs, 1700000100, seq, 0x11223344, tpl);
        std::map<uint16_t, Template> known;
        const Decoded d = g.size() == 1 ? decode(g[0], known) : Decoded{};
        CHECK(d.ok && d.version == 10 && d.length == g[0].size() && d.domain == 0x11223344 && d.templateSets == 2, "a valid IPFIX message: version 10, its length, our domain id, two descriptor sets");
        CHECK(d.rx.size() == 1 && d.rx[0] == std::vector<std::string>({"G4ABC", "IO91wm", "VibeServer 12.0.0-beta90", "Wideband loop"}), "the receiver record: callsign, locator, software, antenna");
        CHECK(d.tx.size() == 1 && d.tx[0] == std::vector<std::string>({"DL1XYZ", "14075612", "-17", "FT8", "1", "JO62", "1700000000"}), "the sender record: call, Hz, SNR (signed byte), mode, informationSource 1, locator, flowStartSeconds");
        CHECK(seq == 2, "the sequence advanced by the data records (receiver + 1 sender)");
        CHECK(g[0].size() % 4 == 0, "every set padded to four bytes");
        // a later datagram without the descriptors decodes with the cached ones
        int tpl0 = 0; const auto g2 = buildDatagrams(r2, rs, 1700000400, seq, 0x11223344, tpl0);
        const Decoded d2 = decode(g2[0], known);
        CHECK(d2.ok && d2.templateSets == 0 && d2.tx.size() == 1 && d2.seq == 2, "without descriptors it decodes from the cached ones, seq carried on");
    }

    std::printf("── batching, dedupe, timing (a local UDP sink) ──\n");
    {
        UdpSink sink;
        long now = 1'800'000'000;
        Reporter rp;
        rp.setClock([&] { return now; });
        rp.setRandomSeed(7);
        Config c; c.on = true; c.call = "g4abc"; c.grid = "io91WM"; c.antenna = "Wideband loop"; c.software = "VibeServer 12.0.0-test";
        c.pskHost = "127.0.0.1"; c.pskPort = sink.port;
        rp.configure(c);
        CHECK(rp.config().call == "G4ABC" && rp.config().grid == "IO91wm", "the owner's callsign and grid cleaned (upper case call, AA00aa grid)");
        Spot s; s.mode = "FT8"; s.call = "DL1XYZ"; s.grid = ""; s.snr = -12; s.rfHz = 14075612; s.slotUtc = now;
        rp.note(s);
        s.snr = -9; s.grid = "JO62"; rp.note(s);              // the same station again in the batch
        s.call = "EA1AA"; s.grid = "IN53"; s.rfHz = 14074900; rp.note(s);
        Spot ft2 = s; ft2.mode = "FT2"; ft2.call = "F4XYZ"; rp.note(ft2);                  // no ADIF name
        Spot junk = s; junk.call = "CQ"; rp.note(junk); junk.call = "12345"; rp.note(junk); // not callsigns
        Spot hfar = s; hfar.call = "W1AW"; hfar.rfHz = 0; rp.note(hfar);
        CHECK(rp.pendingCount() == 2, "one record per callsign per batch; FT2, non-callsigns and no frequency dropped");
        const long first = rp.nextFlushAt();
        CHECK(first - now >= kBatchSec && first - now <= kBatchSec + kStartJitterSec, "the first batch 5 minutes from start, + 0–60 s");
        now += 299; rp.tick();
        CHECK(sink.drain().empty(), "nothing sent before five minutes");
        now = first; rp.tick();
        int src1 = 0; auto got = sink.drain(&src1);
        std::map<uint16_t, Template> known;
        Decoded d = got.size() == 1 ? decode(got[0], known) : Decoded{};
        CHECK(got.size() == 1 && d.ok && d.templateSets == 2, "one datagram, with the descriptors");
        CHECK(d.tx.size() == 2, "two stations in it");
        bool dlGrid = false; for (auto& r : d.tx) if (r[0] == "DL1XYZ" && r[5] == "JO62") dlGrid = true;
        CHECK(dlGrid, "the locator a later decode carried is kept");
        CHECK(d.rx.size() == 1 && d.rx[0][0] == "G4ABC" && d.rx[0][1] == "IO91wm" && d.rx[0][3] == "Wideband loop", "the receiver record names the owner's call, the grid and the antenna");
        // the same station on the same band in the next hour: not again; on another band: at once
        now += 400; s.call = "DL1XYZ"; s.rfHz = 14075700; rp.note(s);
        CHECK(rp.pendingCount() == 0, "the same call + band + mode within the hour is not reported again");
        s.rfHz = 7075600; rp.note(s);
        CHECK(rp.pendingCount() == 1, "the same call on a NEW band is reported (the page's 'change')");
        const long second = rp.nextFlushAt();
        CHECK(second - first >= kBatchSec, "the next batch at least five minutes after the last");
        now = second; rp.tick();
        int src2 = 0; got = sink.drain(&src2);
        d = got.size() == 1 ? decode(got[0], known) : Decoded{};
        CHECK(d.ok && d.templateSets == 2 && d.tx.size() == 1 && d.tx[0][1] == "7075600", "the second datagram (still within the first three: descriptors again)");
        CHECK(src1 == src2 && src1 != 0, "the same UDP source port for every datagram");
        CHECK(d.seq == 3, "the sequence counts data records (1 receiver + 2 senders before this one)");
        // the third and the fourth
        for (int k = 0; k < 2; ++k) {
            now = rp.nextFlushAt(); s.call = k ? "K1ABC" : "N1DQ"; rp.note(s); rp.tick();
            got = sink.drain(); d = got.size() == 1 ? decode(got[0], known) : Decoded{};
            if (k == 0) CHECK(d.ok && d.templateSets == 2, "the third datagram carries the descriptors");
            else CHECK(d.ok && d.templateSets == 0 && d.tx.size() == 1, "the fourth does not (cached by the collector)");
        }
        // an hour after the descriptors were last sent: again; and DL1XYZ on 20 m may be reported again
        now += 3600; rp.tick(); sink.drain();
        s.call = "DL1XYZ"; s.rfHz = 14075612; rp.note(s);
        CHECK(rp.pendingCount() == 1, "after an hour the same station on the same band is reported again");
        now = rp.nextFlushAt(); rp.tick();
        got = sink.drain(); d = got.size() == 1 ? decode(got[0], known) : Decoded{};
        CHECK(d.ok && d.templateSets == 2, "the descriptors again once an hour");
        // an empty batch sends nothing
        now = rp.nextFlushAt(); rp.tick();
        CHECK(sink.drain().empty(), "an empty batch sends no datagram");
        // a big batch splits
        for (int k = 0; k < 300; ++k) { Spot b = s; b.call = "M" + std::to_string(k % 10) + "A" + std::string(1, char('A' + k / 26 % 26)) + std::string(1, char('A' + k % 26)); b.rfHz = 21074000 + k; rp.note(b); }
        const size_t want = rp.pendingCount();
        now = rp.nextFlushAt(); rp.tick();
        got = sink.drain();
        size_t recs = 0; bool small = true, eachRx = true;
        for (auto& gm : got) { small = small && gm.size() <= kDatagramBytes; Decoded dd = decode(gm, known); recs += dd.tx.size(); eachRx = eachRx && dd.rx.size() == 1; }
        CHECK(got.size() >= 3 && small && recs == want && eachRx, "a full batch splits into datagrams ≤ 1400 bytes, every one with the receiver record");
        const std::string st = rp.statusJson();
        CHECK(st.find("\"on\":true") != std::string::npos && st.find("\"sentHour\":") != std::string::npos && st.find("\"call\":\"G4ABC\"") != std::string::npos, "the status line: on, call, spots sent in the hour");
        // switched off: nothing waits, nothing goes
        c.on = false; rp.configure(c); rp.note(s); now = rp.nextFlushAt() + 1000; rp.tick();
        CHECK(rp.pendingCount() == 0 && sink.drain().empty(), "off: nothing is kept or sent");
        c.on = true; c.call = ""; rp.configure(c); rp.note(s); rp.flushNow();
        CHECK(rp.pendingCount() == 0 && sink.drain().empty(), "no callsign: nothing is kept or sent");
        // ★ V12 beta90: an RTTY skimmer spot (the shape vibe_decoder_host.h noteReporter_ sends) — kept on, dropped off
        c.call = "G4ABC"; c.on = true; rp.configure(c);
        Spot rt; rt.mode = "RTTY"; rt.call = "DL1XYZ"; rt.snr = 14; rt.rfHz = 7043000; rt.slotUtc = now; rp.note(rt);
        CHECK(rp.pendingCount() == 1, "an RTTY skimmer spot reaches the reporter when it is on");
        c.on = false; rp.configure(c); Spot rt2 = rt; rt2.call = "G4XYZ"; rp.note(rt2);
        CHECK(rp.pendingCount() == 0, "…and not when it is off");
    }

    std::printf("── WSPRnet (a local HTTP sink) ──\n");
    {
        UdpSink sink; HttpSink http;
        long now = 1'800'000'000;
        Reporter rp; rp.setClock([&] { return now; });
        Config c; c.on = true; c.wsprnet = true; c.call = "G4ABC"; c.grid = "IO91wm"; c.software = "VibeServer 12.0.0-test";
        c.pskHost = "127.0.0.1"; c.pskPort = sink.port; c.wsprHost = "127.0.0.1"; c.wsprPort = http.port;
        rp.configure(c);
        Spot w; w.mode = "WSPR"; w.call = "K1JT"; w.grid = "FN20"; w.snr = -21; w.dt = 0.3f; w.drift = 0; w.dbm = 30;
        w.rfHz = 10140211; w.slotUtc = 1293843720;   // 2011-01-01 01:02 UTC
        rp.note(w); rp.note(w);                       // the same transmission twice
        Spot w2 = w; w2.slotUtc += 120; w2.snr = -24; rp.note(w2);   // the next slot: its own line
        Spot f8; f8.mode = "FT8"; f8.call = "DL1XYZ"; f8.rfHz = 14075612; f8.slotUtc = now; rp.note(f8);
        CHECK(rp.wsprPendingCount() == 2, "each WSPR transmission once (two slots); FT8 is not a WSPRnet spot");
        rp.flushNow();
        CHECK(http.got.size() == 1, "one upload");
        const std::string& rq = http.got.empty() ? std::string() : http.got[0];
        CHECK(rq.rfind("POST /meptspots.php HTTP/1.1\r\n", 0) == 0, "POST /meptspots.php");
        CHECK(rq.find("User-Agent: VibeServer 12.0.0-test\r\n") != std::string::npos, "a clear User-Agent naming the software");
        CHECK(rq.find("name=\"call\"\r\n\r\nG4ABC\r\n") != std::string::npos && rq.find("name=\"grid\"\r\n\r\nIO91wm\r\n") != std::string::npos, "call and grid as form fields");
        CHECK(rq.find("name=\"allmept\"; filename=\"ALL_MEPT.TXT\"") != std::string::npos, "the spots as the allmept file");
        CHECK(rq.find("110101 0102   5 -21  0.3   10.140211  K1JT FN20 30 0\r\n") != std::string::npos, "the ALL_MEPT line: date time sync snr dt MHz call grid dBm drift");
        CHECK(rq.find("110101 0104") != std::string::npos && rq.find("DL1XYZ") == std::string::npos, "the next slot's line; no FT8 in the file");
        CHECK(rp.statusJson().find("\"wsprHour\":2") != std::string::npos, "the status counts the WSPR spots sent");
        // a failed upload is kept once for the next batch
        http.code = 503; Spot w3 = w; w3.slotUtc += 600; rp.note(w3); rp.flushNow();
        CHECK(rp.wsprPendingCount() == 1 && rp.statusJson().find("WSPRnet answered 503") != std::string::npos, "a failed upload: said in words, its spots kept for the next batch");
        http.code = 200; rp.flushNow();
        CHECK(rp.wsprPendingCount() == 0 && http.got.size() == 3, "…and sent then");
        // the switch off: nothing goes to WSPRnet
        c.wsprnet = false; rp.configure(c); rp.note(w3); rp.flushNow();
        CHECK(http.got.size() == 3 && rp.wsprPendingCount() == 0, "WSPRnet switch off: no upload");
        sink.drain();
    }

    std::printf("── the machine-wide status, the ranking ──\n");
    {
        char tmpl[] = "/tmp/spotreport-test-XXXXXX";
        const std::string dir = mkdtemp(tmpl) ? tmpl : "";
        long now = 1'800'000'000;
        Reporter rp; rp.setClock([&] { return now; }); rp.setStatusDir(dir); rp.setRadioLabel("HF loop");
        Config c; c.on = true; c.call = "G4ABC"; c.grid = "IO91"; c.pskHost = "127.0.0.1"; c.pskPort = 9;
        int asked = 0; std::string url;
        rp.setUdp([](const std::string&, int, const std::vector<uint8_t>&, std::string&) { return true; });
        rp.setHttpGet([&](const std::string& u) { ++asked; url = u; return std::string("{\"call\":\"G4ABC\",\"psk\":{\"fetched\":1800000000,\"bands\":{\"40m\":{\"reports\":5}}}}"); });
        rp.configure(c);
        Spot s; s.mode = "FT8"; s.call = "DL1XYZ"; s.rfHz = 7074500; s.slotUtc = now; rp.note(s); rp.flushNow();
        const std::string agg = aggregateStatus(dir, (long)std::time(nullptr));
        CHECK(agg.find("\"on\":true") != std::string::npos && agg.find("\"sentHour\":1") != std::string::npos && agg.find("\"label\":\"HF loop\"") != std::string::npos,
              "the setup page's line from this process's status file (1 spot in the hour, the radio named)");
        CHECK(rp.ranksJson() == "{\"on\":true}", "no ranking until it has been fetched");
        rp.forceRanksDue(); rp.tick();
        CHECK(asked == 1 && url == "https://vibeserver.vibesdr.net/api/ranks?call=G4ABC", "the ranking asked of the DIRECTORY, for this callsign only");
        CHECK(rp.ranksJson().find("\"ranks\":{\"call\":\"G4ABC\"") != std::string::npos, "the gallery's ranking carries the directory's answer");
        rp.tick();
        CHECK(asked == 1, "not asked again within the hour");
        c.call = "M0XYZ"; rp.configure(c);
        CHECK(rp.ranksJson() == "{\"on\":true}", "another callsign: the old ranking is not shown");
        c.on = false; rp.configure(c);
        CHECK(rp.ranksJson() == "{\"on\":false}", "reporting off: no ranking shown");
        rp.setHttpGet([&](const std::string&) { return std::string("<html>error</html>"); });
        c.on = true; c.call = "G4ABC"; rp.configure(c); rp.forceRanksDue(); rp.tick();
        CHECK(rp.ranksJson().find("\"40m\":{\"reports\":5}") != std::string::npos && rp.ranksJson().find("<html>") == std::string::npos,
              "an answer that is not the directory's JSON never replaces the last good one");
        std::string rm = "rm -rf '" + dir + "'"; if (!dir.empty()) (void)!std::system(rm.c_str());
    }

    std::printf("── helpers ──\n");
    CHECK(adifMode("ft8") == "FT8" && adifMode("Q65-60A") == "Q65" && adifMode("FST4W-120") == "FST4W" && adifMode("FT2").empty() && adifMode("JTTY").empty(), "ADIF mode names; FT2 and JTTY have none");
    CHECK(adifMode("CW") == "CW" && adifMode("cw") == "CW", "CW is reported as ADIF CW (beta90: CW skimmer spots go to PSK Reporter)");
    CHECK(adifMode("RTTY") == "RTTY" && adifMode("rtty") == "RTTY", "RTTY is reported as ADIF RTTY (beta90: RTTY skimmer spots go to PSK Reporter)");
    CHECK(adifMode("BPSK31") == "PSK31" && adifMode("BPSK63") == "PSK63", "the PSK skimmer's BPSK31 / BPSK63 are PSK Reporter's PSK31 / PSK63");
    CHECK(cleanGrid("io91wm") == "IO91wm" && cleanGrid("RR73").empty() && cleanGrid("-12").empty() && cleanGrid("ZZ99").empty(), "grids: AA00aa only, never a report");
    CHECK(gridOf(51.5, -0.12) == "IO91wm", "grid from a position (London)");
    CHECK(plausibleCall("G4ABC") && plausibleCall("EA8/DF4UE") && !plausibleCall("CQ") && !plausibleCall("12345") && !plausibleCall("/G4ABC"), "callsign shape");
    CHECK(std::string(bandOf(7074000)) == "40m" && std::string(bandOf(14074000)) == "20m" && std::string(bandOf(9000000)).empty(), "band names");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
