// paging.h — V12 PAGING (POCSAG / FLEX, AUTO) on the R36S: the rules of src/services/paging.ts (the app reads that file)
// plus what the handheld draws — a list of pages, not a text log. docs/v12/DECODERS-WAVE2.md "PAGING", 2026-11-16.
//
// ★★★ LOCAL HARDWARE ONLY. The receiver runs in the handheld's own radio process and is offered there only
//     (vibeserver.json `"paging":true` + `pagingModes` — android/app/src/main/cpp/vibe_paging_gate.h: never a server,
//     never while serving, only a radio plugged into the device, only a loopback decoder socket). No "paging" = no key.
// ★★★ NOTHING IS KEPT. The pages live in a bounded list in memory (kPagingMax) and nowhere else: no file, no card, no
//     gallery, no log line (vibe-hhui's stderr says the decoder started, never what it decoded). STOP / CLR / a restart
//     and they are gone.
// ★★ SILENT. While PAGE runs the ENGINE plays silence on the speaker (local_sdr_shim.cpp, pagingSpeakerLevel) and the
//    person's own mute and volume are never touched — so it comes back as it was however PAGE ends, a crash included
//    (the decoder socket closes, the receiver stops). Here only the glyph and the words say so (pagingSilentNow).
// ★ The wire (vibe_paging_run.h): 0x01 one line per page "[POCSAG 1200 · 1234567 · F3] text" / "[FLEX 3200/4 · 0123456 · A]
//   text" ("(tone only)", "(binary data)", "(secure message)", " (cut short)"); 0x03 locked 0/1;
//   {"type":"paging_status","data":{mode,detected,pages,rate,short}}.
// Golden-tested against the TS: test/gen_vts_golden.ts (op PS) → test/vts_golden_test.cpp; the line parser and the bar's
// line by test/paging_test.cpp.
#pragma once
#include "json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace hh {

/** paging.ts PagingStatus. */
struct PagingStatus { std::string mode = "auto", detected; double pages = 0, rate = 0; bool shortRate = false; };

namespace pagd {
/** JavaScript's String.slice(0, n) on a UTF-8 string: n UTF-16 units (a character above U+FFFF counts two). */
inline std::string slice16(const std::string& s, size_t n) {
    size_t units = 0, i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        const size_t w = len == 4 ? 2 : 1;
        if (units + w > n || i + len > s.size()) break;
        units += w; i += len;
    }
    return s.substr(0, i);
}
/** String(number) for the counts (integers in practice; anything else as JS would print it). */
inline std::string num(double v) {
    char b[40];
    if (std::floor(v) == v && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}
}  // namespace pagd

/** paging.ts parsePagingStatus: false (null) when `d` is not an object. */
inline bool parsePagingStatus(const Json& d, PagingStatus& s) {
    if (d.kind != Json::Obj && d.kind != Json::Arr) return false;   // ★ the TS's `typeof d === 'object'` lets an array through
    s = PagingStatus{};
    s.mode = d["mode"].kind == Json::Str ? d["mode"].str() : "auto";
    s.detected = d["detected"].kind == Json::Str ? pagd::slice16(d["detected"].str(), 32) : "";
    s.pages = d["pages"].kind == Json::Num && std::isfinite(d["pages"].n) ? d["pages"].n : 0;
    s.rate = d["rate"].kind == Json::Num && std::isfinite(d["rate"].n) ? d["rate"].n : 0;
    s.shortRate = d["short"].kind == Json::Bool && d["short"].b;
    return true;
}

/** paging.ts pagingStatusText: what is locked (or that it is listening), the page count, the narrow-filter warning. */
inline std::string pagingStatusText(const PagingStatus* s) {
    if (!s) return "listening\xe2\x80\xa6";
    const std::string head = !s->detected.empty() ? s->detected : s->mode == "auto" ? "listening \xe2\x80\x94 AUTO (rate from the signal)" : "listening";
    const std::string n = s->pages != 0 ? " \xc2\xb7 " + pagd::num(s->pages) + " page" + (s->pages == 1 ? "" : "s") : "";
    const std::string w = s->shortRate ? " \xc2\xb7 filter narrow: widen NFM to 12.5 kHz or more" : "";
    return head + n + w;
}

/** paging.ts PAGING_LAW_NOTICE — shown first in the hint (the empty box, the picker) and once a session as PAGE opens. */
inline const char* const kPagingLawNotice =
    "Decoding pagers is illegal in certain countries \xe2\x80\x94 please check the laws in your country before using it.";
/** paging.ts pagingHintFor('PROTOCOL', 'RATE') — the handheld's names for the rows. */
inline const char* const kPagingHint =
    "Decoding pagers is illegal in certain countries \xe2\x80\x94 please check the laws in your country before using it. "
    "Tune a paging channel in NFM with the filter at 12.5 kHz or wider. AUTO finds the protocol and the rate \xe2\x80\x94 POCSAG "
    "512, 1200 or 2400, or FLEX 1600, 3200 or 6400 \xe2\x80\x94 and names it; if AUTO cannot lock, pick the PROTOCOL (and for POCSAG "
    "the RATE) below. FLEX names its own speed in every frame. The speaker is muted while PAGE runs and comes back as it was "
    "when you close it. Pages are shown here only \xe2\x80\x94 never saved or sent anywhere.";

// ── one page ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/** One page as the list draws it. `at` = when it arrived here (seconds since the epoch). */
struct PageRow {
    uint64_t id = 0;     // this run's own number (the list's key; ENTER reads that page whole)
    double at = 0;
    std::string proto;   // "POCSAG 1200", "FLEX 3200/4" — "" when the line could not be read (then `text` is the line)
    std::string addr;    // the 21-bit address / the capcode, as sent ("1234567")
    std::string fn;      // POCSAG's function bits "F0".."F3"; FLEX's phase "A".."D"
    std::string text;    // the message, "(tone only)", "(binary data)", "(secure message)"
    bool cut = false;    // " (cut short)": the lock went mid-message
};
constexpr size_t kPagingMax = 200;       // ★ bounded, always (no OOM, ever) — the oldest goes
constexpr size_t kPagingTextMax = 600;   // one message, in bytes

/** Control characters to spaces, at most `n` bytes (never cutting a UTF-8 character). */
inline std::string pagingClean(const std::string& s, size_t n) {
    std::string o;
    for (size_t i = 0; i < s.size() && o.size() < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        o += (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    while (!o.empty() && ((unsigned char)o.back() & 0xC0) == 0x80) {   // a cut character's tail: drop it whole
        size_t k = o.size();
        while (k > 0 && ((unsigned char)o[k - 1] & 0xC0) == 0x80) k--;
        if (k == 0) { o.clear(); break; }
        const unsigned char lead = (unsigned char)o[k - 1];
        const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (o.size() - (k - 1) >= want) break;
        o.resize(k - 1);
    }
    return o;
}

/** vibe_paging_run.h PagingRun::line, read back: "[PROTO RATE · ADDRESS · FN] text\n". A line that is not in that shape
 *  still becomes a row (its text = the line), so nothing the receiver says is lost. */
inline PageRow pagingParseLine(const std::string& raw, double at) {
    PageRow r; r.at = at;
    std::string line = raw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    static const std::string dot = " \xc2\xb7 ", cutTag = " (cut short)";
    if (line.size() >= cutTag.size() && line.compare(line.size() - cutTag.size(), cutTag.size(), cutTag) == 0) {
        r.cut = true; line.resize(line.size() - cutTag.size());
    }
    const size_t close = line.find("] ");
    if (!line.empty() && line[0] == '[' && close != std::string::npos) {
        const std::string head = line.substr(1, close - 1);
        const size_t a = head.find(dot), b = a == std::string::npos ? a : head.find(dot, a + dot.size());
        if (a != std::string::npos && b != std::string::npos) {
            r.proto = pagingClean(head.substr(0, a), 24);
            r.addr = pagingClean(head.substr(a + dot.size(), b - a - dot.size()), 12);
            r.fn = pagingClean(head.substr(b + dot.size()), 4);
            r.text = pagingClean(line.substr(close + 2), kPagingTextMax);
            return r;
        }
    }
    r.text = pagingClean(line, kPagingTextMax);
    return r;
}

/** The list: newest first, at most kPagingMax. */
inline void pagingPush(std::deque<PageRow>& pages, PageRow r) {
    pages.push_front(std::move(r));
    while (pages.size() > kPagingMax) pages.pop_back();
}

/** The VTS bar's line while PAGE runs: "PAGE · FLEX 3200/4 · 3 pages · speaker muted". `st` null = no report yet. */
inline std::string pagingVts(const PagingStatus* st) {
    return "PAGE \xc2\xb7 " + pagingStatusText(st) + " \xc2\xb7 speaker muted";
}

/** ★ What the VTS bar shows for PAGE (vts.h reads it; dec_state.h decTick writes it): "" while PAGE is not running. */
inline std::string& pagingVtsLine() { static std::string s; return s; }

}  // namespace hh
