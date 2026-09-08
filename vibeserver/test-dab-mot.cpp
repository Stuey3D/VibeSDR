// test-dab-mot.cpp — MOT header parameters, and the categorised slideshow that rides in them.
//
// ★★★ WHY THIS TEST EXISTS. MOT had no test at all: the slideshow was verified on air (7D, the
//     Horizon Radio logo) and that was taken as enough. It was not. Only ContentName was ever
//     parsed out of the header, and adding the CatSLS parameters of ETSI TS 101 499 immediately
//     produced the one mistake a spec-reader makes here — assuming every string parameter is
//     framed like ContentName. It is not, and the two specs are why:
//         ContentName (0x0C) is ETSI EN 301 234's, defined as charset(4) + rfa(4) then the name.
//         0x25-0x29 are TS 101 499's own, each "a string using UTF-8 encoding" with NO prefix.
//     Strip a byte from those and the first character of every category title disappears — which
//     reads as a font or encoding fault, not a parser one, and would have been hunted somewhere
//     else entirely.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "vibe_dab_mot.h"

using namespace vibedab;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); ++fails; } } while (0)

/** One MOT header parameter: PLI(2) | id(6), then the length in the form the PLI selects. */
static std::vector<uint8_t> param(int id, const std::vector<uint8_t>& v) {
    std::vector<uint8_t> o;
    if (v.empty())        { o.push_back(uint8_t((0 << 6) | id)); return o; }
    if (v.size() == 1)    { o.push_back(uint8_t((1 << 6) | id)); }
    else if (v.size() == 4) { o.push_back(uint8_t((2 << 6) | id)); }
    else {                                     // PLI 3: an explicit length, short or long form
        o.push_back(uint8_t((3 << 6) | id));
        if (v.size() < 0x80) o.push_back(uint8_t(v.size()));
        else { o.push_back(uint8_t(0x80 | (v.size() >> 8))); o.push_back(uint8_t(v.size())); }
    }
    o.insert(o.end(), v.begin(), v.end());
    return o;
}
static std::vector<uint8_t> bytesOf(const std::string& s) { return { s.begin(), s.end() }; }
/** ContentName: EN 301 234 puts a charset/rfa byte in front of the name. */
static std::vector<uint8_t> contentName(const std::string& s) {
    std::vector<uint8_t> v{ 0x00 };                       // charset 0, rfa 0
    v.insert(v.end(), s.begin(), s.end());
    return param(0x0C, v);
}

/** A MOT header: bodySize(28) headerSize(13) contentType(6) contentSubType(9), then parameters. */
static std::vector<uint8_t> header(uint32_t bodySize, int ct, int st, const std::vector<uint8_t>& params) {
    const size_t headerSize = 7 + params.size();
    std::vector<uint8_t> h(7, 0);
    h[0] = uint8_t(bodySize >> 20); h[1] = uint8_t(bodySize >> 12); h[2] = uint8_t(bodySize >> 4);
    h[3] = uint8_t(((bodySize & 0x0F) << 4) | ((headerSize >> 9) & 0x0F));
    h[4] = uint8_t((headerSize >> 1) & 0xFF);
    const uint32_t tail = (uint32_t(headerSize & 1) << 15) | (uint32_t(ct & 0x3F) << 9) | uint32_t(st & 0x1FF);
    h[5] = uint8_t(tail >> 8); h[6] = uint8_t(tail);
    h.insert(h.end(), params.begin(), params.end());
    return h;
}

/** One MSC data group carrying a whole segment: type 3 = header, 4 = body. */
static std::vector<uint8_t> dataGroup(int type, uint16_t tid, const std::vector<uint8_t>& seg) {
    std::vector<uint8_t> g;
    g.push_back(uint8_t(0x20 | 0x10 | type));      // segmentation + user access, no ext, no CRC
    g.push_back(0x00);                             // continuity / repetition
    g.push_back(0x80); g.push_back(0x00);           // last = 1, segment number 0
    g.push_back(uint8_t(0x10 | 2));                 // transport id present, length 2
    g.push_back(uint8_t(tid >> 8)); g.push_back(uint8_t(tid));
    g.push_back(uint8_t((seg.size() >> 8) & 0x1F)); g.push_back(uint8_t(seg.size()));
    g.insert(g.end(), seg.begin(), seg.end());
    return g;
}

/** Feed one complete object and return it. The body is a stub PNG — only its size is checked. */
static bool sendObject(MotAssembler& a, uint16_t tid, const std::vector<uint8_t>& params, MotObject& out) {
    const std::vector<uint8_t> body{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const auto h = header(uint32_t(body.size()), 2, 3, params);      // contentType 2/3 = PNG
    const auto gh = dataGroup(3, tid, h);
    const auto gb = dataGroup(4, tid, body);
    a.feedDataGroup(gh.data(), gh.size());
    a.feedDataGroup(gb.data(), gb.size());
    return a.take(out);
}

int main() {
    printf("test-dab-mot\n");

    // ── the plain slideshow object that already worked ─────────────────────────────────
    {
        MotAssembler a; MotObject o;
        CHECK(sendObject(a, 1, contentName("logo.png"), o), "a header and a body make an object");
        CHECK(o.name == "logo.png", "★ ContentName drops its charset byte (EN 301 234)");
        CHECK(o.mime() == "image/png" && o.isImage(), "content type 2/3 is a PNG");
        CHECK(o.categoryId == -1 && o.slideId == -1, "no category signalled means no category");
    }

    // ── ★★★ the categorised slideshow (TS 101 499 clause 6.2, table 3) ─────────────────
    {
        MotAssembler a; MotObject o;
        std::vector<uint8_t> p = contentName("slide1.png");
        { auto q = param(0x25, { 0x07, 0x03 }); p.insert(p.end(), q.begin(), q.end()); }   // cat 7, slide 3
        { auto q = param(0x26, bytesOf("Now Playing"));  p.insert(p.end(), q.begin(), q.end()); }
        { auto q = param(0x27, bytesOf("https://x.example/1")); p.insert(p.end(), q.begin(), q.end()); }
        { auto q = param(0x28, bytesOf("https://x.example/i.png")); p.insert(p.end(), q.begin(), q.end()); }
        { auto q = param(0x29, { 0x01 }); p.insert(p.end(), q.begin(), q.end()); }
        CHECK(sendObject(a, 2, p, o), "the categorised object assembles");
        CHECK(o.categoryId == 7 && o.slideId == 3,
              "★ CategoryID is the upper byte of 0x25 and SlideID the lower (clause 6.2.6)");
        /* ★★★ THE MISTAKE THIS TEST WAS WRITTEN FOR. These are TS 101 499's own parameters and it
         *  defines each as a plain UTF-8 string. Framing them like ContentName eats the "N". */
        CHECK(o.categoryTitle == "Now Playing",
              "★★★ CategoryTitle is plain UTF-8 — NO charset byte (clause 5.3.5.3)");
        CHECK(o.clickUrl == "https://x.example/1", "★ ClickThroughURL keeps its first character too");
        CHECK(o.altUrl == "https://x.example/i.png", "★ AlternativeLocationURL likewise");
        CHECK(o.alert == 1, "Alert 1 is the emergency warning (table 4)");
    }

    // ── a category title long enough to need PLI 3's long form ─────────────────────────
    {
        MotAssembler a; MotObject o;
        const std::string big(200, 'A');       // > 0x7F, so the length takes two bytes
        std::vector<uint8_t> p = contentName("s.png");
        { auto q = param(0x26, bytesOf(big)); p.insert(p.end(), q.begin(), q.end()); }
        CHECK(sendObject(a, 3, p, o), "the object assembles with a long parameter");
        CHECK(o.categoryTitle == big, "★ a parameter longer than 127 bytes uses the two-byte length");
    }

    // ── what must not happen to a corrupt header ───────────────────────────────────────
    {
        /* A parameter whose declared length runs past the header must stop the walk, not read
         * whatever follows it in memory. Run under ASan, this is the check that matters. */
        MotAssembler a; MotObject o;
        std::vector<uint8_t> p = contentName("s.png");
        p.push_back(uint8_t((3 << 6) | 0x26));      // PLI 3, CategoryTitle
        p.push_back(0x7F);                          // says 127 bytes; none follow
        CHECK(!sendObject(a, 4, p, o) || o.categoryTitle.empty(),
              "★ a parameter length past the end of the header reads nothing");

        uint32_t rng = 0x5EEDu;
        auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int i = 0; i < 60000; ++i) {
            MotAssembler f; MotObject fo;
            std::vector<uint8_t> junk(2 + next() % 40);
            for (auto& b : junk) b = uint8_t(next());
            f.feedDataGroup(junk.data(), junk.size());
            (void)f.take(fo);
        }
        CHECK(true, "60000 corrupt data groups did not crash");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
