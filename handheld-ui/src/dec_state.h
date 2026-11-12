// dec_state.h — THE DECODERS on the R36S: what the web client's decoder panel KNOWS and DOES (web/client/src/decoders.ts
// DecoderClient + main.ts initDecoders / decParams / syncDecButtons / anyDecoderLeft / soleDecoder), with no drawing —
// that is dec_box.h. Include AFTER the App class and after vts.h (dec_box.h asks the VTS bar whether it is showing).
//
// ★★★ THE DECODERS RUN IN THE ENGINE. This file opens ONE extra WebSocket — /ws/dxcluster, the web's decoder socket —
//     sends the web's messages and reads the web's frames. Nothing here decodes; the UI thread only drains a queue once
//     a frame (Network > Audio > Spectrum > Decoders: the engine's decoder threads are the lowest priority there, and
//     the UI never blocks on them — the socket is opened and closed on worker threads).
// ★★ OPENED ONLY WHILE SOMETHING RUNS (decoders.ts _sync): the socket opens when a decoder or the spots start and closes
//    8 s after the last one stops (so switching RTTY → NAVTEX never reconnects). A dead socket is retried after 3 s, and
//    on (re)open the decoder is re-asserted WITH its parameters (the web's reconnect lesson: never the server defaults).
// ★★ A REFUSAL IS SAID IN THE SERVER'S OWN WORDS (decoder_refused), where the decoder's output would have been, and the
//    refused decoder is forgotten so a reconnect does not ask again.
// ★★ BOUNDED, ALWAYS (no OOM, ever): every log is a ring of display rows (600), every list is capped (signals 80, spots
//    300, calls 300, packets 60), the pending text is capped (64 KB), one line is cut at 3000 bytes.
// ★★ THE CLOCK FEEDS (R36S HARDWARE's "Set clock from <station>" / "Align to FT8"): every corroborated time-station
//    minute (`time_sync`) goes to hwNoteTimeSync, every FT8 spot's DT to hwNoteFt8Dt — whichever decoder is open.
// ★ AUTO decoders open in AUTO unless a manual choice was made THIS SESSION (src/services/decoderSession.ts): the
//   choices live in memory only — a restart is AUTO again.
// ★ PICTURE decoders (WEFAX, SSTV, HELL) are dec_pic.h's: their frames go there (decPicBinary), their pictures are GL
//   textures drawn by dec_pic_box.h, the gallery is dec_pic_gallery.h. MAP decoders (ADS-B, AIS, NDB, ISM) are in the
//   table (DK_MAP) so their pass plugs in here, but are not offered yet: a key whose every use is a no-op is not drawn.
//
// DEBUG (tests only): VIBE_HHUI_DEC_START=rtty,spots starts decoders once the session runs; VIBE_HHUI_DEC_CANNED=1 — a started decoder is fed CANNED frames through the very parse path the
//   socket uses (decCannedTick), instead of opening the socket. For the screenshots of decoders the bench cannot make
//   signals for.
#pragma once
#include "hw_state.h"
#include "ws.h"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hh {

// ── the decoder table ─────────────────────────────────────────────────────────────────────────────────────────────────

/** What a decoder's box draws. TEXT = a scrolling log; KB = CW/PSK (the one signal's text, the signals, the calls
 *  heard); JTTY / DSC / APRS / SPOTS = their own rows. PICTURE and MAP are the later passes' (not offered yet). */
enum DecKind { DK_TEXT, DK_KB, DK_JTTY, DK_DSC, DK_APRS, DK_SPOTS, DK_PICTURE, DK_MAP };
struct DecDef {
    const char* id;       // the web's data-dec ("spots" = DIGITAL SPOTS: SINGLE MODE)
    const char* label;    // the key's legend
    DecKind kind;
    const char* cap;      // the vibeserver.json flag that offers it ("" = every server; a capped one needs `<cap>: true`)
    bool reach;           // vibe_decoder_reach.h: may come back false WITH `<id>Why` — drawn greyed, a press says why
    bool built;           // ★ offered by THIS UI (false = a later pass: picture / map)
    const char* hint;     // index.html's hint, for under the options
};
inline const DecDef kDecs[] = {
    {"rtty",   "RTTY",    DK_TEXT, "",       false, true,  "AUTO finds the shift, centre, speed and polarity itself. If it cannot lock, pick a preset or set the signal by hand below."},
    {"cw",     "CW",      DK_KB,   "cw",     true,  true,  "Tune the signal in CW — the decoder finds the speed itself (5–60 WPM). A wider USB filter lists every CW signal inside it. Press a row to read its text back as far as it has been heard; TUNE there tunes to it."},
    {"psk",    "PSK",     DK_KB,   "psk",    true,  true,  "Tune USB with the PSK segment in your passband (e.g. 14.070). The signal in the middle of the passband is decoded; press a row to tune to it."},
    {"aprs",   "APRS",    DK_APRS, "aprs",   true,  true,  "Every station heard is listed, with its position, weather, messages and the digipeater that carried it."},
    {"navtex", "NAVTEX",  DK_TEXT, "",       true,  true,  "Tune 518 kHz (international) or 490 kHz (national) in USB, or 4209.5 kHz. Each message starts ZCZC and ends NNNN; a retune starts the next one."},
    {"jtty",   "JTTY",    DK_JTTY, "jtty",   true,  true,  "Tune USB — 14.090 MHz is the main meeting point — or press a band. Every JTTY signal in your passband is decoded at once, one row per station. Press a row to tune to it."},
    {"dsc",    "DSC",     DK_DSC,  "dsc",    true,  true,  "A listed DSC frequency is the ASSIGNED frequency, not the dial: tune USB with the dial 1.7 kHz below it — or press a channel. Channel 70 (156.525 MHz) is NFM. 8414.5 kHz is the busiest by day, 2187.5 kHz at night."},
    {"sitora", "SITOR-A", DK_TEXT, "sitora", false, true,  "SITOR-A (ARQ) — maritime teleprinter, 100 Bd, 170 Hz shift. Tune USB so the two tones sit around 1000 Hz; both stations of a link are printed, each under its own label."},
    {"selcal", "SELCAL",  DK_TEXT, "selcal", true,  true,  "The two-tone selective calls a ground station sends to one aircraft, shown as its code (AB-CD): HF aeronautical channels in USB, VHF airband in AM."},
    {"same",   "SAME",    DK_TEXT, "same",   true,  true,  "US emergency alert headers from NOAA Weather Radio (162.400–162.550 MHz NFM) and EAS broadcasts: event, areas and times."},
    {"mfsk",   "MFSK",    DK_TEXT, "mfsk",   false, true,  "MFSK16/32, OLIVIA, CONTESTIA and THOR: tune USB so the whole signal sits in your passband — for OLIVIA, 1.5 kHz below the published centre. AUTO tries the common sub-modes of all four side by side and names the one it finds; if it cannot lock, pick the family and then the sub-mode below."},
    {"time",   "TIME",    DK_TEXT, "",       false, true,  ""},
    {"spots",  "DIGITAL SPOTS", DK_SPOTS, "", false, true, "The digital modes at your dial (FT8 and FT4 on your passband), decoded on the server. Press a spot to tune to it."},
    // ★ the later passes (in the table so they plug in; never offered until they are built)
    {"wefax", "WEFAX", DK_PICTURE, "", false, true, "Weather charts by radio fax, drawn line by line. Tune USB 1.9 kHz below the station's published frequency — Northwood 4608.1, DWD 7878.1 kHz; 120 LPM for nearly every station. The margin and slant are found on each chart by itself; ADJ in the box sets them by hand. Each finished chart is kept in the GALLERY."},
    {"sstv", "SSTV", DK_PICTURE, "", false, true, "Slow-scan TV pictures — 14.230 MHz USB on 20 m (7.171 MHz LSB on 40 m; 145.800 MHz FM from the ISS). The mode is found by itself; each finished picture is kept in the GALLERY."},
    {"hell", "HELL", DK_PICTURE, "hell", false, true, "FELD HELL (Hellschreiber): drawn, not read — the text is painted as it is heard, a strip at a time, and you read it. Every column is printed twice, one copy above the other, so a slant or a fade that cuts one copy leaves the other whole. Tune USB so the signal is in your passband (the tone is found by itself); each new page puts the last one under PREV."},
    {"adsb", "ADS-B", DK_MAP, "adsb", false, false, ""}, {"ais", "AIS", DK_MAP, "ais", false, false, ""},
    {"ndb", "NDB", DK_MAP, "ndb", false, false, ""}, {"ism", "ISM", DK_MAP, "ism", false, false, ""},
};
inline const DecDef* decDef(const std::string& id) { for (auto& d : kDecs) if (id == d.id) return &d; return nullptr; }

/** src/utils/decoderBands.ts DECODER_PLACES — band → group (by use) → keys, in display order; only what this UI offers
 *  is drawn (dec_box.h filters). */
struct DecPlace { const char* group; std::vector<const char*> ids; };
inline const std::vector<DecPlace>& decPlaces(int band) {
    static const std::vector<DecPlace> hf = {
        {"AVIATION", {"selcal"}}, {"MARITIME", {"navtex", "dsc", "sitora", "wefax"}},
        {"AMATEUR", {"spots", "cw", "psk", "rtty", "jtty", "mfsk", "hell", "sstv", "aprs"}}, {"UTILITY", {"time"}}};
    static const std::vector<DecPlace> vhf = {
        {"AVIATION", {"selcal"}}, {"MARITIME", {"dsc"}}, {"AMATEUR", {"aprs", "spots", "cw", "sstv"}}, {"BROADCAST", {"same"}}};
    return band == 0 ? hf : vhf;
}

/** main.ts TIME_STATIONS: each station implies its own frequency AND mode (getting the mode wrong is the difference
 *  between decoding and a flat line — WWV is the odd one, AM not CW). */
struct DecTimeStation { const char* id; const char* key; const char* hint; double hz; const char* mode; };
inline const DecTimeStation kTimeStations[] = {
    {"msf", "MSF 60k", "MSF, Anthorn — tune 60.000 kHz in CW.", 60000, "cwu"},
    {"dcf77", "DCF77 77.5k", "DCF77, Mainflingen — tune 77.500 kHz in CW.", 77500, "cwu"},
    {"wwvb", "WWVB 60k", "WWVB, Fort Collins — tune 60.000 kHz in CW. MSF shares this frequency: which one you hear is geography.", 60000, "cwu"},
    {"wwv", "WWV (AM)", "WWV, Fort Collins — 2.5 / 5 / 10 / 15 / 20 MHz in AM (not CW). From Europe try 15 MHz in the afternoon.", 15000000, "am"},
    {"jjy", "JJY 40/60k", "JJY, Japan — 40.000 kHz or 60.000 kHz in CW. The time shown is JST (UTC+9).", 40000, "cwu"},
    {"rwm", "RWM", "RWM, Moscow — 4.996 / 9.996 / 14.996 MHz in CW. Markers only: RWM transmits no date or time code.", 4996000, "cwu"},
};

/** src/utils/dscChannels.ts DSC_DISTRESS (+ the tone rule): HF in USB 1.7 kHz below the assigned frequency, ch 70 NFM. */
struct DecDscCh { double hz; const char* label; bool vhf; };
inline const DecDscCh kDscDistress[] = {
    {2187.5e3, "2187.5", false}, {4207.5e3, "4207.5", false}, {6312e3, "6312", false}, {8414.5e3, "8414.5", false},
    {12577e3, "12577", false}, {16804.5e3, "16804.5", false}, {156525000, "Ch 70", true}};
inline const double kDscAll[] = {   // every HF DSC channel, distress + routine (ITU RR App. 15, ITU-R M.541) — dscCentreFor
    2177e3, 2187.5e3, 2189.5e3, 4207.5e3, 4208e3, 4208.5e3, 4209e3, 6312e3, 6312.5e3, 6313e3, 6313.5e3, 8414.5e3, 8415e3,
    8415.5e3, 8416e3, 12577e3, 12577.5e3, 12578e3, 12578.5e3, 16804.5e3, 16805e3, 16805.5e3, 16806e3, 18898.5e3, 18899e3,
    18899.5e3};
inline void decDscTuneFor(const DecDscCh& c, double& hz, std::string& mode) { if (c.vhf) { hz = c.hz; mode = "nfm"; } else { hz = c.hz - 1700; mode = "usb"; } }
inline bool decDscOn(const DecDscCh& c, double dial, const std::string& md) {
    double hz; std::string m; decDscTuneFor(c, hz, m);
    const bool modeOk = c.vhf ? (md == "nfm" || md == "fm") : md == "usb";
    return modeOk && std::fabs(dial - hz) <= 150;
}
/** dscCentreFor: the tones' audio centre from the NEAREST HF DSC channel (so a dial a little off still decodes). */
inline int decDscCentre(double dial, const std::string& md) {
    double centre = 1700, best = 1e30;
    for (double c : kDscAll) {
        const double a = md == "lsb" ? dial - c : c - dial;
        if (a >= 300 && a <= 3000 && std::fabs(a - 1700) < best) { best = std::fabs(a - 1700); centre = a; }
    }
    return (int)std::lround(centre);
}

/** web/client/src/jtty.ts JTTY_DIALS — announced in the WSJT-X 3.2 release candidates (PRELIMINARY; USB dial). */
struct DecDial { double hz; const char* label; };
inline const DecDial kJttyDials[] = {
    {1838000, "160m"}, {3575000, "80m"}, {7090000, "40m"}, {10140000, "30m"}, {14090000, "20m"}, {18100000, "17m"},
    {21090000, "15m"}, {24920000, "12m"}, {28090000, "10m"}, {50160000, "6m"}, {144160000, "2m"}};

/** src/services/autoModes.ts GroupedMode — the server's own list (vibeserver.json mfskModes), AUTO first. */
struct DecGMode { std::string id, label, group; };

// ── what arrives ─────────────────────────────────────────────────────────────────────────────────────────────────────

inline std::string decAscii(std::string s);
inline std::string decAsciiLog(const std::string& s) { return s.find('\xe2') == std::string::npos ? s : decAscii(s); }
/** A scrolling text log, bounded: finished DISPLAY rows (wrapped once, when their line ends) + the line still arriving.
 *  The wrapping is done on the UI thread when new text arrives — never per frame (dec_box.h draws only visible rows).
 *  ★★ THE READER'S PLACE (`back`, rows up from the newest) is kept by the log itself (2026-11-12, Stuart: CW "should
 *     auto scroll like RTTY does, and then selected the user should be able to scroll back"): every row that appears
 *     BELOW the reader — a finished line's, or the line still arriving growing a row — moves `back` up by one, so what
 *     is being read stays still; a log that was only counting FINISHED lines let CW (which never ends a line) drag the
 *     reader forward a row at a time, then jump 75 rows when its 3000-character line was cut. */
class DecLog {
public:
    static constexpr size_t kMaxRows = 600, kMaxPending = 64 * 1024, kMaxLine = 3000;
    /** New decoded text. `terminalCR` (the time decoders only): a leading \r REPLACES the line, as a terminal; for every
     *  other decoder a run of CRs (with or without its LF) is ONE end of line — RTTY's CR CR LF is data, not a rewrite. */
    void push(const std::string& t, bool terminalCR) {
        std::string s;
        s.reserve(t.size());
        for (size_t i = 0; i < t.size(); i++) {
            const unsigned char ch = (unsigned char)t[i];
            if (ch == '\r' && !terminalCR) {
                while (i + 1 < t.size() && t[i + 1] == '\r') i++;
                if (i + 1 < t.size() && t[i + 1] == '\n') i++;
                s += '\n';
            } else if (ch == '\t') s += ' ';
            else if (ch < 0x20 && ch != '\n' && ch != '\r') continue;
            else if (ch == 0x7f) continue;
            else s += (char)ch;
        }
        pending_ += decAsciiLog(s);
        if (pending_.size() > kMaxPending) pending_.erase(0, pending_.size() - kMaxPending);
    }
    /** A line of its own (a separator, a refusal): finishes the line arriving first. */
    void line(const std::string& t) { push("\n" + t + "\n", false); }
    void clear() { rows_.clear(); rowSer_.clear(); raw_.clear(); rawSer_.clear(); open_.clear(); openRows_.clear(); pending_.clear();
                   progress_ = false; back = 0; lastBlank_ = true; added_ = 0; }
    /** ★ How each LINE is shown (CW's EXPLAIN: cwPlainEnglish; nullptr = as decoded). A change re-reads the whole log
     *  from the lines as decoded (kept, up to kMaxRaw) at the next layout — a reader scrolled back stays on the same
     *  decoded line (its rows re-read), one following the newest still follows. */
    using Xform = std::string (*)(const std::string&);
    void setXform(Xform x) { if (x != xform_) { xform_ = x; reread_ = true; } }
    /** ★ CW's text never ends a line (the server sends a stream of characters, no line ends — vibe_cw_run.h clean()):
     *  `on`, a line ends at the first space after a CW break (= + <AR> <KN> <BK> <SK> <BT>) once it is 40 characters
     *  long, else at the first space after 240 — so lines finish as RTTY's do, EXPLAIN reads a paragraph at a time and
     *  only the short line still arriving is re-wrapped as it grows. */
    void setCwBreaks(bool on) { cwBreaks_ = on; }
    /** How many rows the box shows (for keeping the reader's place across a re-read). */
    void setView(int n) { view_ = std::max(1, n); }
    /** Lay out what arrived (UI thread; cheap — only the new text). */
    void layout(TextCache& tc, const Font& f, int w) {
        const long before = (long)rows();
        popped_ = 0;
        if (w != w_) { w_ = w; rewrap(tc, f); }
        if (reread_) { reread(tc, f); return layout(tc, f, w); }
        if (pending_.empty()) { added_ += (long)rows() - before + popped_; return; }
        std::string p; p.swap(pending_);
        bool openDirty = false;
        size_t i = 0;
        while (i < p.size()) {
            if (p[i] == '\r') {   // (terminal convention: the line arriving is replaced)
                open_.clear(); openDirty = true; progress_ = true; i++;
                continue;
            }
            if (progress_) { finish(tc, f); progress_ = false; openDirty = true; }
            const size_t j = p.find_first_of("\r\n", i);
            const size_t e = j == std::string::npos ? p.size() : j;
            open_.append(p, i, e - i);
            openDirty = true;
            if (cwBreaks_) cwBreak(tc, f);
            if (open_.size() > kMaxLine) finish(tc, f);
            i = e;
            if (i < p.size() && p[i] == '\n') { finish(tc, f); openDirty = true; i++; }
        }
        if (openDirty) { if (open_.empty()) openRows_.clear(); else wrap(tc, f, shown(open_), openRows_); }   // ★ no empty row at the foot
        added_ += (long)rows() - before + popped_;
    }
    size_t rows() const { return rows_.size() + openRows_.size(); }
    const std::string& row(size_t k) const { return k < rows_.size() ? rows_[k] : openRows_[k - rows_.size()]; }
    bool empty() const { return rows_.empty() && open_.empty() && openRows_.empty(); }
    /** Rows that appeared at the foot since the last call (negative: the line arriving re-read shorter). The box adds
     *  them to `back` while the reader is scrolled up, so the place holds still. */
    long takeAdded() { const long n = added_; added_ = 0; return n; }
    int back = 0;   // ★ rows scrolled up from the newest (0 = following the newest text)

private:
    static constexpr size_t kMaxRaw = 300;   // finished lines kept as decoded (a CW line is long: ~kMaxRows rows' worth)
    std::deque<std::string> rows_, raw_;
    std::deque<uint64_t> rowSer_, rawSer_;   // ★ the decoded line each row / kept line belongs to (serial numbers)
    uint64_t ser_ = 1;                       // the serial of the line arriving
    std::string open_, pending_;
    Xform xform_ = nullptr;
    bool reread_ = false, cwBreaks_ = false;
    int view_ = 0;
    std::string shown(const std::string& s) const { return xform_ ? xform_(s) : s; }
    std::vector<std::string> openRows_;
    bool progress_ = false, lastBlank_ = true;
    int w_ = 0;
    long added_ = 0, popped_ = 0;
    std::unordered_map<uint64_t, int> adv_;

    uint64_t serAt(size_t k) const { return k < rowSer_.size() ? rowSer_[k] : ser_; }
    void capRows() { while (rows_.size() > kMaxRows) { rows_.pop_front(); rowSer_.pop_front(); popped_++; } }
    /** The view re-read through a new Xform: the reader stays on the decoded line at the top of the view. */
    void reread(TextCache& tc, const Font& f) {
        reread_ = false;
        uint64_t keepSer = 0; double frac = 0;
        if (back > 0 && rows() > 0) {
            const long n = (long)rows();
            const long top = std::max(0L, n - view_ - back);
            keepSer = serAt((size_t)top);
            long first = top, cnt = 0;
            while (first > 0 && serAt((size_t)first - 1) == keepSer) first--;
            for (long k = first; k < n && serAt((size_t)k) == keepSer; k++) cnt++;
            frac = cnt > 0 ? (double)(top - first) / cnt : 0;
        }
        rows_.clear(); rowSer_.clear();
        std::vector<std::string> out;
        for (size_t k = 0; k < raw_.size(); k++) {
            wrap(tc, f, shown(raw_[k]), out);
            for (auto& x : out) { rows_.push_back(std::move(x)); rowSer_.push_back(rawSer_[k]); }
        }
        while (rows_.size() > kMaxRows) { rows_.pop_front(); rowSer_.pop_front(); }
        if (!open_.empty()) wrap(tc, f, shown(open_), openRows_); else openRows_.clear();
        added_ = 0;
        if (!keepSer) { back = 0; return; }
        const long n = (long)rows();
        long first = -1, cnt = 0;
        for (long k = 0; k < n; k++) if (serAt((size_t)k) == keepSer) { if (first < 0) first = k; cnt++; }
        if (first < 0) { back = 0; return; }
        const long top = first + std::min(cnt - 1, (long)std::lround(frac * cnt));
        back = (int)std::max(0L, n - view_ - top);
    }
    /** ★ CW's line breaks (setCwBreaks). */
    void cwBreak(TextCache& tc, const Font& f) {
        for (;;) {
            size_t cut = std::string::npos;
            if (open_.size() >= 40) {
                size_t s = 0;
                while (s < open_.size()) {
                    const size_t e = open_.find(' ', s);
                    if (e == std::string::npos) break;   // the last word may still be arriving
                    if (e > s && e >= 40) {
                        const std::string w = open_.substr(s, e - s);
                        if (w.back() == '=' || w.back() == '+' || w == "<AR>" || w == "<KN>" || w == "<BK>" || w == "<SK>" || w == "<BT>") { cut = e; break; }
                    }
                    s = e + 1;
                }
            }
            if (cut == std::string::npos && open_.size() > 240) cut = open_.find(' ', 240);
            if (cut == std::string::npos) return;
            std::string rest = open_.substr(cut + 1);
            open_.resize(cut);
            finish(tc, f);
            open_ = std::move(rest);
        }
    }
    void finish(TextCache& tc, const Font& f) {
        const bool blank = open_.find_first_not_of(' ') == std::string::npos;
        if (blank && lastBlank_) { open_.clear(); openRows_.clear(); return; }   // ★ one blank row, never a column of them
        lastBlank_ = blank;
        raw_.push_back(open_); rawSer_.push_back(ser_);
        while (raw_.size() > kMaxRaw) { raw_.pop_front(); rawSer_.pop_front(); }
        std::vector<std::string> out;
        wrap(tc, f, shown(open_), out);
        for (auto& r : out) { rows_.push_back(std::move(r)); rowSer_.push_back(ser_); }
        capRows();
        ser_++;
        open_.clear(); openRows_.clear();
    }
    void rewrap(TextCache& tc, const Font& f) { if (!open_.empty()) wrap(tc, f, shown(open_), openRows_); }
    int advance(TextCache& tc, const Font& f, uint32_t cp, const std::string& one) {
        const uint64_t k = ((uint64_t)(uintptr_t)f.get() << 21) ^ cp;
        auto it = adv_.find(k);
        if (it != adv_.end()) return it->second;
        if (adv_.size() > 4096) adv_.clear();
        return adv_[k] = tc.width(f, one);
    }
    /** Word-wrap to w_ pixels; a word wider than the line (RTTY noise has no spaces) is broken by characters. */
    void wrap(TextCache& tc, const Font& f, const std::string& s, std::vector<std::string>& out) {
        out.clear();
        if (s.empty()) { out.push_back(""); return; }
        const int maxW = std::max(40, w_);
        std::string line;
        int lineW = 0;
        size_t lastSpace = std::string::npos;   // in `line`
        int wAtSpace = 0;
        size_t i = 0;
        while (i < s.size()) {
            size_t n = 1;
            const unsigned char c0 = (unsigned char)s[i];
            if (c0 >= 0xF0) n = 4; else if (c0 >= 0xE0) n = 3; else if (c0 >= 0xC0) n = 2;
            if (i + n > s.size()) n = 1;
            const std::string one = s.substr(i, n);
            uint32_t cp = c0;
            if (n > 1) { cp = c0 & (0xFF >> (n + 1)); for (size_t k = 1; k < n; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F); }
            const int a = advance(tc, f, cp, one);
            if (lineW + a > maxW && !line.empty()) {
                if (cp == ' ') { out.push_back(line); line.clear(); lineW = 0; lastSpace = std::string::npos; i += n; continue; }
                if (lastSpace != std::string::npos && lastSpace > 0) {
                    out.push_back(line.substr(0, lastSpace));
                    line = line.substr(lastSpace + 1);
                    lineW -= wAtSpace;
                } else { out.push_back(line); line.clear(); lineW = 0; }
                lastSpace = std::string::npos;
            }
            if (cp == ' ') { lastSpace = line.size(); wAtSpace = lineW + a; }
            line += one; lineW += a;
            i += n;
        }
        out.push_back(line);
    }
};

/** One CW / PSK signal (cw_signals / psk_signals — decoders.ts), and one JTTY signal. */
struct DecSig { double id = 0, hz = 0, audio = 0, wpm = 0, wpmLo = 0, wpmHi = 0, snr = 0, baud = 0, imd = 0, last = 0, frames = 0;
                std::string q, call, country, text, raw; bool active = false; };   // raw = text as the server sent it
/** A call heard identifying itself (cw_spot / psk_spot / jtty_spot). */
struct DecCall { std::string call, country, how, mode; double hz = 0, snr = 0, wpm = 0, at = 0; };
/** A digital spot (digital_spot). */
struct DecSpot { std::string mode, call, band, grid, country, how; double snr = 0, hz = 0, at = 0, dt = 0; bool hasDt = false; };
/** A DSC call (dsc_call — dsc.ts): the server's own readable line, coloured by its level; a repeat updates its row. */
struct DecDsc { double id = 0, rf = 0, utc = 0; int repeat = 1, level = 0; std::string line; };
/** An APRS station (aprs_list) and a packet line (aprs_packet). */
struct DecAprsSt { std::string key, name, line, kind; double seen = 0; bool pos = false; };
struct DecAprsPkt { double at = 0; std::string line; };

// ── everything the decoders keep between frames (UI thread only) ─────────────────────────────────────────────────────

struct DecUi {
    // what runs
    std::string active;              // the text decoder attached ("" none) — the web's activeDec
    // ★★ CLR on a LIST the server keeps re-sending (Stuart, 2026-11-11: "the CLR button doesnt work"): the CW / PSK /
    //    JTTY signals and the APRS stations come back whole every second, so emptying them here did nothing. CLR now
    //    remembers each signal's text as it was (clrText, by id) and the moment (aprsClrAt); afterwards a signal shows
    //    only what it decodes AFTER the press, and is hidden until it does; a station shows once heard again.
    std::string clrFor;                          // the decoder these belong to ("" = no CLR in force)
    std::unordered_map<long long, std::string> clrText;
    double aprsClrAt = 0;
    std::string ext;                 // …and the extension it went out as (CW / PSK: by view)
    bool spotsOn = false;
    int spotsMin = 0; double spotsMinAt = 0;   // ★ the log's once-a-minute spot count (decTick)
    std::string view;                // what the box shows: `active` or "spots"
    bool boxOpen = false;            // the box is drawn (a decoder runs, or the person is choosing one)
    // the box
    enum Pane { P_OUT, P_PICK } pane = P_OUT;
    int sub = 0;                     // a list decoder's sub-pane (0 the first: TEXT / SIGNALS / STATIONS …)
    bool grabNext = false, wantPick = false;
    int pickBand = -1;               // the picker's band tab (0 HF, 1 VHF; -1 = the dial's)
    std::string pickFocus;           // a row to land the highlight on in the picker
    // status (the box's header)
    std::string status = "listening…", tuneHint, refusal;
    bool live = false;
    double liveAt = 0;
    // output
    DecLog log;
    std::vector<DecSig> sigs;        // CW / PSK / JTTY
    std::deque<DecCall> calls;       // cw / psk / jtty spots, newest first, one per call
    std::deque<DecSpot> spots;       // newest first, one per call|band|mode
    uint64_t spotsSeq = 0;           // ★ bumped on every change to `spots` (the spots map rebuilds its list only then)
    std::deque<DecDsc> dsc;
    std::vector<DecAprsSt> aprs;
    std::deque<DecAprsPkt> aprsLog;
    double navtexHz = 0;
    Json sitor;                      // the last sitora_status (a state frame redraws its line with the new link)
    std::string timeSynced;          // the last station whose minute was corroborated (the clock key's evidence)
    double timeSyncedAt = 0;
    // settings — THIS SESSION ONLY (AUTO again after a restart)
    struct Rtty { double shift = 170, baud = 45.45; std::string enc = "ITA2"; bool inv = false, autoOn = true, usos = false;
                  double stop = 1.5; int bits = 7; char parity = 'N'; } rtty;
    std::string timeStation = "msf";
    bool cwSkim = false; int cwWidth = 48000;
    // ★★ A CW SIGNAL'S OWN HISTORY (Stuart, 2026-11-12: "selected the user should be able to scroll back to see the
    //    decoded history"): the server sends each signal's LAST 120 characters (vibe_cw_run.h sendList), so the history
    //    is joined here from those windows (decHistMerge), by id, while the signal is listed. A row pressed opens it
    //    (readSig) in a log of its own (sigLog) — RTTY's reading: following the newest, UP / DOWN scroll back.
    std::unordered_map<long long, std::string> sigHist;
    long long readSig = 0;           // the CW signal being read (0 = the list)
    DecLog sigLog;
    bool pskSkim = false; int pskWidth = 12000;
    int aprsBaud = 1200;
    std::string mfskMode = "auto";
    double reattachAt = 0;           // a burst of option presses settles into one re-attach
    // the server's offer (GET /vibeserver.json, off the UI thread)
    std::mutex capMtx;
    bool capFetched = false, capFetching = false; double capAt = -1e9;
    std::map<std::string, bool> caps;
    std::map<std::string, std::string> why;
    bool skimOk = true;
    double aprsHz = 144.8e6;
    std::vector<DecGMode> mfskModes;
    std::vector<std::string> spotModes;
    // the socket
    std::shared_ptr<WsClient> ws;
    std::mutex netMtx;
    std::shared_ptr<WsClient> fresh;
    bool opening = false;
    double retryAt = 0, idleSince = 0;
    bool canned = false; double cannedAt = 0; int cannedN = 0;
    // cut-to-width cache (dec_box.h)
    std::map<std::string, std::string> fit;
};
/** ★★ LOWEST FREQUENCY FIRST, ALWAYS, AND A TIE THE SAME WAY EVERY TIME — the app's cwList sortCwSignals exactly
 *  (hz, the audio offset when either has no RF, then the id). Stuart, 2026-11-11: the R36S's skimmer "seems to flap about
 *  moving signals up and down into different orders" — CW / PSK rows were drawn in the SERVER's order, which changes
 *  from one update to the next; JTTY's sort had no tie-break. */
inline void decSortSigs(std::vector<DecSig>& v) {
    std::stable_sort(v.begin(), v.end(), [](const DecSig& a, const DecSig& b) {
        const double d = a.hz > 0 && b.hz > 0 ? a.hz - b.hz : a.audio - b.audio;
        if (d != 0) return d < 0;
        return a.id < b.id;
    });
}
inline DecUi& decUi() {
    static DecUi u;
    static const bool init = [] { const char* c = std::getenv("VIBE_HHUI_DEC_CANNED"); u.canned = c && *c && *c != '0'; return true; }();
    (void)init;
    return u;
}
/** The part of `now` that came after `then` (a signal's text when CLR was pressed). The server's text is a rolling
 *  window, so `then` is found by its last 16 characters, not its length; gone from the window = all of it is new. */
inline std::string decTextSince(const std::string& then, const std::string& now) {
    if (then.empty()) return now;
    if (now.compare(0, then.size(), then) == 0) return now.substr(then.size());
    const std::string tail = then.size() > 16 ? then.substr(then.size() - 16) : then;
    const size_t at = now.find(tail);
    return at == std::string::npos ? now : now.substr(at + tail.size());
}
/** CLR in force on this decoder's list: each signal shows only its text since the press; one with none is left out. */
inline void decApplyClr(std::vector<DecSig>& v) {
    DecUi& u = decUi();
    for (auto& g : v) g.raw = g.text;
    if (u.clrFor.empty() || u.clrFor != u.active) return;
    std::vector<DecSig> keep;
    for (auto& g : v) {
        auto it = u.clrText.find((long long)g.id);
        if (it != u.clrText.end()) {
            std::string t = decTextSince(it->second, g.raw);
            while (!t.empty() && t[0] == ' ') t.erase(0, 1);
            if (t.empty()) continue;
            g.text = t;
        }
        keep.push_back(std::move(g));
    }
    v.swap(keep);
}
/** ★ A signal's history (`hist`) joined with the server's latest window of its text (`win`, its last 120 characters):
 *  the longest end of `hist` that `win` starts with is the overlap, the rest of `win` is new and is appended (and
 *  returned). No overlap of at least 12 characters (or all of a shorter history / window) = the window moved on further
 *  than it holds, or the text was replaced: the history goes on after a gap (" … "). Bounded (kMax, the oldest go). */
inline std::string decHistMerge(std::string& hist, const std::string& win) {
    static constexpr size_t kMax = 6000;
    if (win.empty()) return "";
    std::string add;
    if (hist.empty()) add = win;
    else {
        const size_t need = std::min<size_t>({12, win.size(), hist.size()});   // (a young signal: all of it)
        size_t k = std::min(hist.size(), win.size());
        for (; k >= need && k > 0; k--) if (hist.compare(hist.size() - k, k, win, 0, k) == 0) break;
        if (k >= need && k > 0) add = win.substr(k);
        else if (hist.size() >= win.size() && hist.compare(hist.size() - win.size(), win.size(), win) == 0) add.clear();
        else add = " … " + win;
    }
    hist += add;
    if (hist.size() > kMax) { size_t cut = hist.find(' ', hist.size() - kMax); hist.erase(0, cut == std::string::npos ? hist.size() - kMax : cut + 1); }
    return add;
}
/** The CW histories after a cw_signals list: each listed signal's window joined on; a signal no longer listed is
 *  forgotten (the one being read is kept — its log stays readable after it fades). */
inline void decCwHistories() {
    DecUi& u = decUi();
    std::unordered_map<long long, std::string> keep;
    for (auto& g : u.sigs) {
        const long long id = (long long)g.id;
        auto it = u.sigHist.find(id);
        std::string h = it != u.sigHist.end() ? std::move(it->second) : std::string();
        const std::string add = decHistMerge(h, g.text);
        if (id == u.readSig && !add.empty()) u.sigLog.push(add, false);
        keep[id] = std::move(h);
    }
    if (u.readSig && !keep.count(u.readSig)) { auto it = u.sigHist.find(u.readSig); if (it != u.sigHist.end()) keep[u.readSig] = std::move(it->second); }
    u.sigHist.swap(keep);
}
/** Open (id ≠ 0) or close the reader of one CW signal. */
inline void decReadSig(long long id) {
    DecUi& u = decUi();
    u.readSig = id;
    u.sigLog.clear();
    if (!id) return;
    auto it = u.sigHist.find(id);
    if (it != u.sigHist.end()) u.sigLog.push(it->second, false);
}
/** CLR pressed on a list decoder (dec_box.h). */
inline void decClearLists() {
    DecUi& u = decUi();
    u.clrFor = u.active;
    u.clrText.clear();
    for (auto& g : u.sigs) u.clrText[(long long)g.id] = g.raw;
    u.sigs.clear();
    u.sigHist.clear(); u.sigLog.clear();   // ★ the reader stays open on its signal, empty, and fills with what comes next
    u.aprsClrAt = nowSec();
}

/** The engine's listener session id, so the decoders hear THIS listener's audio on a per-VFO radio. Engine has no
 *  accessor today: with `const std::string& sessionId() const { return sid_; }` added to engine.h it is used; without
 *  it the engine falls back to the one listener at this address (decoderSessionByAddress — the handheld's own UI). */
template <class E> auto decSidOf(const E& e, int) -> decltype(std::string(e.sessionId())) { return e.sessionId(); }
template <class E> std::string decSidOf(const E&, long) { return ""; }

// ── decoders provided by OTHER files (the registry) ──────────────────────────────────────────────────────────────────
/** ★ A decoder this file does not run itself — Advanced RDS (adv_rds*.h) is the first. It is offered in the picker under
 *  its band + group (src/utils/decoderBands.ts: rds = VHF · BROADCAST), toggled like every other key, drawn in the box
 *  by its own `draw`, and is EXCLUSIVE with the text decoders (the engine's rule: attaching anything on a listener's
 *  decoder socket replaces what it had — Adv RDS stops a listener's RTTY there too). Its transport is its own.
 *  Register once at start-up (decRegister); every member but id / label / offered / setOn may be left empty. */
struct DecExternal {
    std::string id, label;                                   // "rds", "ADV RDS"
    int band = 1;                                            // the picker tab: 0 HF, 1 VHF
    std::string group = "BROADCAST";                         // the heading it sits under (made if missing)
    std::function<bool(const RadioState&)> offered;          // draw the key at all (hwinfo / vibeserver.json / blocked[])
    std::function<void(App&, const RadioState&, bool)> setOn;  // start (true) / stop (false) it
    std::function<void(App&, const RadioState&)> tick;        // once a loop while it is on (after the socket work)
    std::function<std::string()> status;                     // the header's status line while it is on
    std::function<bool()> live;                              // the header's LED
    std::function<void(App&, Ctx&, const RadioState&, Rect)> draw;   // the box body (register stops in it as usual)
    bool tall = false;                                       // ★ the box as the tall panel (y 44 →) instead of over the waterfall
    std::function<void(App&, const RadioState&, std::vector<std::string>&, std::vector<std::function<void()>>&)> keys;   // extra header keys (labels + presses)
    std::string hint;                                        // under its key in the picker, while on
    std::function<void()> clear;                             // ★ CLR's action — no CLR key without one (AGENTS.md: no no-op controls)
};
inline std::vector<DecExternal>& decExternals() { static std::vector<DecExternal> v; return v; }
inline void decRegister(DecExternal e) { for (auto& x : decExternals()) if (x.id == e.id) { x = std::move(e); return; } decExternals().push_back(std::move(e)); }
inline DecExternal* decExt(const std::string& id) { for (auto& x : decExternals()) if (x.id == id) return &x; return nullptr; }
inline void decExternalEnded(const std::string& id);   // (below) — the provider says its decoder stopped by itself
// ★ the picture decoders (dec_pic.h — included by dec_box.h after this file)
inline bool decIsPicture(const std::string& id);
inline void decPicBinary(App&, const RadioState&, const std::string&);
inline bool decPicJson(const std::string& type, const Json& d);
inline void decPicOpen(const RadioState&, const std::string&);
inline std::string decPicParams(const std::string&);
inline void decPicTick(App&, const RadioState&);
inline void decPicCanned(App&, const RadioState&, int);
inline void galTick(App&);

inline bool decBlocked(const RadioState& s, const std::string& id) {
    for (auto& b : s.blockedModes) if (b == id) return true;
    return false;
}
/** Offered here: built by this UI, the server has it (or it is out of reach with a reason — then greyed), not blocked. */
inline bool decOffered(const RadioState& s, const DecDef& d) {
    if (!d.built || decBlocked(s, d.id)) return false;
    DecUi& u = decUi();
    std::lock_guard<std::mutex> lk(u.capMtx);
    if (!*d.cap) return true;
    if (u.why.count(d.id)) return true;
    auto it = u.caps.find(d.cap);
    if (std::string(d.id) == "mfsk") return it != u.caps.end() && it->second && !u.mfskModes.empty();
    return it != u.caps.end() && it->second;
}
/** The server's reason a fixed-frequency decoder cannot run on this radio ("" = it can). */
inline std::string decWhyNot(const std::string& id) { DecUi& u = decUi(); std::lock_guard<std::mutex> lk(u.capMtx); auto it = u.why.find(id); return it == u.why.end() ? "" : it->second; }

/** main.ts anyDecoderLeft, for what THIS UI offers: is there anything behind the DECODERS… door? */
inline bool decExtOffered(const RadioState& s, const DecExternal& e) { return e.offered && e.offered(s) && !decBlocked(s, e.id); }
inline bool decAnyOffered(const RadioState& s) {
    for (auto& d : kDecs) if (decOffered(s, d)) return true;
    for (auto& e : decExternals()) if (decExtOffered(s, e)) return true;
    return false;
}
/** main.ts soleDecoder: the spots blocked and ONE decoder key left — it goes on the mode sheet itself. */
inline const DecDef* decSole(const RadioState& s) {
    if (!decBlocked(s, "spots")) return nullptr;
    const DecDef* one = nullptr; int n = 0;
    for (auto& d : kDecs) if (decOffered(s, d) && std::string(d.id) != "spots") { one = &d; n++; }
    for (auto& e : decExternals()) if (decExtOffered(s, e)) n++;   // (an external key counts; then DECODERS… stays the door)
    return n == 1 ? one : nullptr;
}
inline bool decSkimSegment(int& lo, int& hi) {
    const DecUi& u = decUi();
    const int w = u.active == "cw" && u.cwSkim ? u.cwWidth : u.active == "psk" && u.pskSkim ? u.pskWidth : 0;
    if (w <= 0) return false;
    lo = -w / 2; hi = w / 2;
    return true;
}
inline bool decRunning() { const DecUi& u = decUi(); return !u.active.empty() || u.spotsOn; }
inline bool decIsOn(const std::string& id) { const DecUi& u = decUi(); return id == "spots" ? u.spotsOn : u.active == id; }

/** The vibeserver.json read (worker thread), at most once a minute while the decoders are being used. */
inline void decFetchCaps(int port) {
    DecUi& u = decUi();
    { std::lock_guard<std::mutex> lk(u.capMtx); if (u.capFetching) return; u.capFetching = true; u.capAt = nowSec(); }
    std::thread([port] {
        DecUi& u = decUi();
        const HttpResult r = httpRequest(port, "GET", "/vibeserver.json", "", 4);
        std::lock_guard<std::mutex> lk(u.capMtx);
        u.capFetching = false;
        if (r.status != 200) return;
        const Json j = Json::parse(r.body);
        u.caps.clear(); u.why.clear();
        for (const char* k : {"cw", "psk", "aprs", "dsc", "sitora", "same", "selcal", "navtex", "mfsk", "jtty", "hell"}) {
            u.caps[k] = j[k].boolean();
            const std::string& w = j[std::string(k) + "Why"].str();
            if (j[k].kind == Json::Bool && !j[k].b && !w.empty()) u.why[k] = w.substr(0, 200);
        }
        u.skimOk = !(j["skim"].kind == Json::Bool && !j["skim"].b);
        if (j["aprsHz"].num(0) > 0) u.aprsHz = j["aprsHz"].num(0);
        u.mfskModes.clear();
        for (auto& m : j["mfskModes"].a) {
            DecGMode g{m["id"].str(), m["label"].str(), m["group"].str()};
            if (!g.id.empty() && g.id.size() <= 24 && !g.label.empty() && g.label.size() <= 24 && u.mfskModes.size() < 48) u.mfskModes.push_back(g);
        }
        u.spotModes.clear();
        for (auto& m : j["spotModes"].a) if (!m.str().empty() && u.spotModes.size() < 12) u.spotModes.push_back(m.str().substr(0, 12));
        u.capFetched = true;
    }).detach();
}

// ── what the person did ───────────────────────────────────────────────────────────────────────────────────────────────

/** The middle of the passband, audio Hz — ONE SIGNAL's cursor for PSK (USB 200–2800 → 1500). */
inline int decPassbandCentre(const RadioState& s) {
    const double c = std::fabs((s.bwLow + s.bwHigh) / 2.0);
    return c >= 300 && c <= 3500 ? (int)std::lround(c) : 1500;
}
inline std::string decExtFor(const std::string& id) {
    const DecUi& u = decUi();
    if (id == "cw") return u.cwSkim ? "cwskim" : "cw";
    if (id == "psk") return u.pskSkim ? "pskskim" : "psk";
    if (id == "rtty") return "fsk";   // ★ the shim's name for it
    return id;
}
/** src/utils/rttySpec.ts rttyFraming — what the server parses (<data><parity><stop>, or SITOR-B's 4/7). */
inline std::string decRttyFraming(const DecUi::Rtty& r) {
    if (r.enc == "CCIR476") return "4/7";
    if (r.enc == "ASCII") return std::to_string(r.bits == 8 ? 8 : 7) + std::string(1, r.parity) + (r.stop == 2 ? "2" : "1");
    return r.stop == 1 ? "5N1" : r.stop == 2 ? "5N2" : "5N1.5";
}
inline std::string decNum(double v) { char b[32]; std::snprintf(b, sizeof b, "%.6g", v); return b; }
/** main.ts decParams + the attach message (decoders.ts _sendAttach). */
inline std::string decAttachJson(const RadioState& s, const std::string& id) {
    const DecUi& u = decUi();
    std::string j = "{\"type\":\"audio_extension_attach\",\"extension_name\":" + jsonQuote(decExtFor(id));
    if (id == "rtty") {
        const auto& r = u.rtty;
        j += ",\"center_frequency\":1000,\"shift\":" + decNum(r.shift) + ",\"baud_rate\":" + decNum(r.baud) + ",\"encoding\":" + jsonQuote(r.enc)
           + ",\"inverted\":" + (r.inv ? "true" : "false") + ",\"framing\":" + jsonQuote(decRttyFraming(r));
        if (!r.autoOn && r.usos && r.enc == "ITA2") j += ",\"usos\":true";
        if (r.autoOn && r.enc == "ITA2") j += ",\"auto\":true";
    } else if (id == "time") j += ",\"station\":" + jsonQuote(u.timeStation);
    else if (id == "cw") { if (u.cwSkim) j += ",\"width\":" + std::to_string(u.cwWidth); }
    else if (id == "psk") j += u.pskSkim ? ",\"width\":" + std::to_string(u.pskWidth) : ",\"centre\":" + std::to_string(decPassbandCentre(s));
    else if (id == "aprs") j += ",\"baud_rate\":" + std::to_string(u.aprsBaud);
    else if (id == "dsc") {
        const std::string& m = s.mode;
        if (m == "nfm" || m == "fm" || std::fabs(s.vfoHz - 156525000) < 25000) j += ",\"band\":\"vhf\"";
        else j += ",\"band\":\"hf\",\"center_frequency\":" + std::to_string(decDscCentre(s.vfoHz, m));
    } else if (id == "sitora") j += ",\"center_frequency\":1000";
    else if (id == "mfsk") j += ",\"mode\":" + jsonQuote(u.mfskMode);
    else if (decIsPicture(id)) j += decPicParams(id);
    return j + "}";
}

inline void decSend(const std::string& j) { DecUi& u = decUi(); if (u.ws && u.ws->alive()) u.ws->sendText(j); }

/** The status line a decoder opens with (before its first state). */
inline std::string decIdleStatus(const std::string& id) {
    const DecUi& u = decUi();
    if (id == "mfsk") return u.mfskMode == "auto" ? "listening — AUTO (MFSK, OLIVIA, CONTESTIA or THOR, from the signal)" : "listening — " + u.mfskMode;
    if (id == "same") return "listening for alert headers";
    if (id == "selcal") return "listening for SELCAL";
    if (id == "sitora") return "NO SIGNAL";
    return "listening…";
}

/** Start a decoder (one text decoder at a time, as the web; the spots run beside it). */
inline void decStop(App& a, const std::string& id);
inline void decStart(App& a, const RadioState& s, const std::string& id) {
    DecUi& u = decUi();
    if (DecExternal* e = decExt(id)) {
        if (u.active == id) return;
        if (!u.active.empty()) decStop(a, u.active);   // ★ exclusive with the text decoders
        u.refusal.clear(); u.boxOpen = true;
        u.active = id; u.ext.clear(); u.view = id; u.sub = 0; u.status.clear(); u.live = false;
        u.clrFor.clear(); u.clrText.clear(); u.aprsClrAt = 0;
        if (e->setOn) e->setOn(a, s, true);
        std::fprintf(stderr, "vibe-hhui: decoder %s on (external)\n", id.c_str());
        return;
    }
    const DecDef* d = decDef(id);
    if (!d) return;
    const std::string why = decWhyNot(id);
    if (!why.empty()) { a.toastLong(std::string(d->label) + " unavailable — " + why); return; }
    u.refusal.clear();
    u.boxOpen = true;
    if (id == "spots") {
        if (u.spotsOn) return;
        u.spotsOn = true;
        decSend("{\"type\":\"subscribe_digital_spots\"}");
        u.view = "spots";
        std::fprintf(stderr, "vibe-hhui: decoder spots on\n");
        return;
    }
    if (u.active == id) return;
    if (DecExternal* e = decExt(u.active)) { if (e->setOn) e->setOn(a, s, false); u.active.clear(); }   // ★ an external one gives way
    u.active = id; u.ext = decExtFor(id);
    u.clrFor.clear(); u.clrText.clear(); u.aprsClrAt = 0;   // ★ a decoder started afresh: no CLR in force
    // ★ the first sub-pane: TEXT in ONE SIGNAL, SIGNALS in the skimmer (it was 1 — CALLS — once the skimmer has no TEXT)
    u.view = id; u.sub = 0;
    u.sigHist.clear(); decReadSig(0);
    u.log.clear(); u.sitor = Json(); u.sigs.clear(); u.calls.clear(); u.dsc.clear(); u.aprs.clear(); u.aprsLog.clear();
    u.status = decIdleStatus(id); u.tuneHint.clear(); u.live = false;
    u.navtexHz = s.vfoHz;
    if (decIsPicture(id)) decPicOpen(s, id);   // ★ a clean picture box (resetDecImages), this kHz's WEFAX slant
    u.cannedN = 0;
    decSend(decAttachJson(s, id));
    std::fprintf(stderr, "vibe-hhui: decoder %s attach %s\n", id.c_str(), decAttachJson(s, id).c_str());
}
/** Stop one (the box closes when nothing is left running and the person is not choosing). */
inline void decStop(App& a, const std::string& id) {
    DecUi& u = decUi();
    (void)a;
    if (id == "spots") {
        if (!u.spotsOn) return;
        u.spotsOn = false;
        decSend("{\"type\":\"unsubscribe_digital_spots\"}");
        if (u.view == "spots") u.view = u.active;
    } else if (DecExternal* e = decExt(id)) {
        if (u.active != id) return;
        if (e->setOn) e->setOn(a, a.eng_->state(), false);
        u.active.clear();
        if (u.view == id) u.view = u.spotsOn ? "spots" : "";
    } else {
        if (u.active != id) return;
        decSend("{\"type\":\"audio_extension_detach\"}");
        u.active.clear(); u.ext.clear();
        if (u.view == id) u.view = u.spotsOn ? "spots" : "";
    }
    std::fprintf(stderr, "vibe-hhui: decoder %s stopped\n", id.c_str());
}
inline void decToggle(App& a, const RadioState& s, const std::string& id) { if (decIsOn(id)) decStop(a, id); else decStart(a, s, id); }
/** A setting changed under a running decoder: re-attach (main.ts reattachIf) — once, 350 ms after the last press. */
inline void decReattachSoon(const std::string& id) { DecUi& u = decUi(); if (u.active == id) u.reattachAt = nowSec() + 0.35; }

/** Tune for a decoder (a row, a channel key, a station preset): the mode first if it changes, through the dial's
 *  ranges (clampTune). Nothing in DAB (the dial is the multiplex's). */
inline void decTune(App& a, const RadioState& s, double hz, const std::string& mode) {
    if (hz <= 0 || dabActive()) return;
    const double got = a.clampTune(std::round(hz));
    a.tuneTo(got);
    if (!mode.empty() && mode != s.mode) { a.eng_->setMode(mode); a.num_["bw.sync"] = -1; }
}

// ── the socket ───────────────────────────────────────────────────────────────────────────────────────────────────────

inline uint32_t decU32(const std::string& b, size_t o) { return (uint32_t)(uint8_t)b[o] << 24 | (uint32_t)(uint8_t)b[o + 1] << 16 | (uint32_t)(uint8_t)b[o + 2] << 8 | (uint8_t)b[o + 3]; }

inline void decOnText(App& a, const RadioState& s, const std::string& t) {
    DecUi& u = decUi();
    if (u.active == "navtex" && s.vfoHz > 0) {
        // ★★ A RETUNE STARTS THE NEXT MESSAGE (NickB, 2026-10-12): a move of 3 kHz or more is a different broadcast
        if (u.navtexHz > 0 && std::fabs(s.vfoHz - u.navtexHz) >= 3000) {
            char b[96]; std::snprintf(b, sizeof b, "— retuned to %.1f kHz —", s.vfoHz / 1e3);
            u.log.line(b);
        }
        u.navtexHz = s.vfoHz;
    }
    if (u.active == "dsc" || u.active == "jtty") return;   // ★ their rows are drawn from the JSON; the line is the same text
    u.log.push(t, u.active == "time");
    u.live = true; u.liveAt = nowSec();
    (void)a;
}
/** src/services/sitora.ts sitorStatusText: "TRAFFIC · printing PEARDBY (364775427), caller · RQ 12 · 2 lost". */
inline std::string decSitorText(const Json& d, int link) {
    static const char* links[] = {"NO SIGNAL", "SEARCHING", "PHASING", "IDENTIFICATION", "TRAFFIC", "IDLE", "CHANGE-OVER", "ENDED"};
    link = std::max(0, std::min(7, link));
    std::string t = links[link];
    if (d.kind != Json::Obj) return t;
    const int iss = (int)std::lround(d["iss"].num(-1)), master = (int)std::lround(d["master"].num(-1));
    auto clean = [](const std::string& v, size_t n) { std::string o; for (char c : v) if (std::isalnum((unsigned char)c)) o += (char)std::toupper((unsigned char)c); return o.substr(0, std::min(n, o.size())); };
    if (iss >= 0 && iss <= 1 && link >= 2 && link <= 6) {
        const std::string id = clean(d["ids"][(size_t)iss].str(), 7), nb = clean(d["numbers"][(size_t)iss].str(), 9);
        std::string nm = id.empty() ? "station " + std::to_string(iss + 1) : (!nb.empty() && nb != id ? id + " (" + nb + ")" : id);
        if (master == iss) nm += ", caller"; else if (master == 1 - iss) nm += ", called";
        t += " · printing " + nm;
    }
    if (d["blocks"].num(0) > 0 || d["repeats"].num(0) > 0) t += " · RQ " + std::to_string((long)d["repeats"].num(0));
    if (d["lost"].num(0) > 0) t += " · " + std::to_string((long)d["lost"].num(0)) + " lost";
    if (d["idChecked"].boolean()) t += " · identity confirmed";
    return t;
}
inline void decOnState(const RadioState& s, int st) {
    DecUi& u = decUi();
    (void)s;
    const std::string& m = u.active;
    if (m == "sitora") { u.status = decSitorText(u.sitor, st); u.live = st >= 2 && st <= 6; return; }
    if (m == "mfsk") { if (!st) u.status = decIdleStatus("mfsk"); u.live = st != 0; return; }
    if (m == "same") { u.status = st ? "reading an alert header…" : "listening for alert headers"; u.live = st != 0; return; }
    if (m == "selcal") { u.status = st ? "tones…" : "listening for SELCAL"; u.live = st != 0; return; }
    if (m == "dsc") u.status = st ? "receiving a call…" : "listening…";
    else if (m == "jtty") u.status = st ? "decoding…" : "listening…";
    else if (m == "cw" || m == "psk") u.status = st >= 3 ? "decoding…" : st ? "carriers found…" : "listening…";
    else if (m == "navtex") u.status = st >= 3 ? "receiving…" : st ? "syncing…" : "standing by";
    else u.status = st >= 3 ? "decoding…" : st ? "syncing…" : "listening…";
    u.live = st != 0;
}
/** src/utils/tuneHint.ts — RTTY AUTO's guide: which way to turn the DIAL (< lower, > higher), from the audio move. */
inline std::string decTuneHint(int audioHz, const std::string& mode) {
    if (!audioHz) return "";
    const bool lower = mode == "lsb" || mode == "cwl";
    const int dial = lower ? audioHz : -audioHz, hz = std::abs(dial);
    char amt[32];
    if (hz >= 1000) std::snprintf(amt, sizeof amt, "%.1f kHz", hz / 1000.0); else std::snprintf(amt, sizeof amt, "%d Hz", hz);
    // ★ ASCII arrows: Atkinson Hyperlegible has no ◀ ▶ (Stuart's own words for it were "< 100Hz or 500Hz >")
    return dial < 0 ? std::string("tune < ") + amt : std::string("tune ") + amt + " >";
}

inline void decOnBinary(App& a, const RadioState& s, const std::string& b) {
    DecUi& u = decUi();
    if (b.empty() || u.active.empty()) return;   // ★ frames mean something only against the decoder attached (0x01 is ambiguous)
    const DecDef* d = decDef(u.active);
    if (!d || d->kind == DK_MAP) return;
    if (d->kind == DK_PICTURE) { decPicBinary(a, s, b); return; }
    const uint8_t op = (uint8_t)b[0];
    if (op == 0x01 && b.size() >= 13) {
        const uint32_t len = decU32(b, 9);
        const std::string text = b.substr(13, std::min<size_t>(len, b.size() - 13));
        if (!text.empty()) decOnText(a, s, text);
    } else if (op == 0x03 && b.size() >= 2) decOnState(s, (uint8_t)b[1]);
    else if (op == 0x06 && b.size() >= 3 && u.active == "rtty") u.tuneHint = decTuneHint((int16_t)((uint8_t)b[1] << 8 | (uint8_t)b[2]), s.mode);
    else if (op == 0x07 && b.size() >= 13 && u.active == "navtex") {
        // ★ the server's own FEC count for the message its NNNN just closed (2026-10-05)
        char t[128]; std::snprintf(t, sizeof t, "— message: %u clean · %u repaired · %u lost —", decU32(b, 1), decU32(b, 5), decU32(b, 9));
        u.log.line(t);
    }
}

template <class T> inline void decCap(std::deque<T>& d, size_t n) { while (d.size() > n) d.pop_back(); }
/** ★ Arrows Atkinson Hyperlegible does not have (the server's DSC / APRS lines say "A → B"), as ASCII; then cut. */
inline std::string decAscii(std::string s) {
    static const std::pair<const char*, const char*> m[] = {{"\xe2\x86\x92", "->"}, {"\xe2\x86\x90", "<-"}, {"\xe2\x96\xb6", ">"}, {"\xe2\x97\x80", "<"}, {"\xe2\x96\xbc", "v"}, {"\xe2\x96\xb2", "^"}};
    for (auto& p : m) for (size_t i; (i = s.find(p.first)) != std::string::npos;) s.replace(i, 3, p.second);
    return s;
}
inline std::string decClip(const std::string& s, size_t n) { const std::string a = decAscii(s); return a.size() > n ? a.substr(0, n) : a; }

inline void decOnJson(App& a, const RadioState& s, const std::string& raw) {
    DecUi& u = decUi();
    const Json m = Json::parse(raw);
    const std::string& type = m["type"].str();
    const Json& d = m["data"];
    const double now = nowSec();
    if (decPicJson(type, d)) return;   // hell_status
    if (type == "decoder_refused") {
        // ★★ FORGET WHAT WAS REFUSED, and say so in the server's words where the output would have been
        const std::string msg = m["message"].str().empty() ? "The server could not start that decoder." : decClip(m["message"].str(), 300);
        if (m["what"].str() == "spots") { u.spotsOn = false; if (u.view == "spots") u.view = u.active; }
        else { u.active.clear(); u.ext.clear(); }
        u.refusal = msg; u.status = "not started"; u.live = false;
        if (u.view.empty()) u.view = "refused";
        a.toastLong(msg);
        std::fprintf(stderr, "vibe-hhui: decoder refused — %s\n", msg.c_str());
        return;
    }
    if (type == "audio_extension_attached") { std::fprintf(stderr, "vibe-hhui: decoder attached (%s)\n", u.ext.c_str()); return; }
    if (type == "time_sync") {
        // ★★ THE CLOCK FEED — whichever decoder is open (main.ts onTimeSync → handheld.ts noteTimeSync)
        const std::string st = decClip(d["station"].str(), 12);
        if (!st.empty() && d["utcMs"].kind == Json::Num && d["hostMs"].kind == Json::Num) {
            hwNoteTimeSync(st, d["utcMs"].num(), d["rxDelayMs"].num(0), d["hostMs"].num());
            u.timeSynced = st; u.timeSyncedAt = now;
            std::fprintf(stderr, "vibe-hhui: time_sync %s → the clock key\n", st.c_str());
        }
        return;
    }
    if (type == "digital_spot" && d.kind == Json::Obj) {
        if (!u.spotsOn) return;
        DecSpot sp;
        for (char c : d["mode"].str()) if (std::isalnum((unsigned char)c) || c == '+' || c == '-') sp.mode += c;
        if (sp.mode.empty()) sp.mode = "FT8";
        sp.mode = decClip(sp.mode, 12);
        sp.call = decClip(d["callsign"].str(), 16); sp.band = decClip(d["band"].str(), 8); sp.grid = decClip(d["grid"].str(), 8);
        sp.country = decClip(d["country"].str(), 40); sp.how = decClip(d["how"].str(), 8);
        sp.snr = d["snr"].num(0); sp.hz = d["frequency"].num(0); sp.at = d["timestamp"].num(0) > 0 ? d["timestamp"].num(0) / 1000.0 : (double)std::time(nullptr);
        if (d["dt"].kind == Json::Num) { sp.dt = d["dt"].num(); sp.hasDt = true; }
        // ★★ THE CLOCK FEED: every FT8 spot's DT (handheld.ts noteSpot)
        if (sp.mode == "FT8" && sp.hasDt) hwNoteFt8Dt(sp.dt);
        for (auto it = u.spots.begin(); it != u.spots.end(); ++it)
            if (it->call == sp.call && it->band == sp.band && it->mode == sp.mode) { u.spots.erase(it); break; }
        u.spots.push_front(sp);
        decCap(u.spots, 300);
        u.spotsSeq++;
        u.spotsMin++;
        if (u.view == "spots") { u.live = true; u.liveAt = now; }
        return;
    }
    if ((type == "cw_signals" || type == "psk_signals") && d["signals"].kind == Json::Arr) {
        const bool psk = type == "psk_signals";
        if (u.active != (psk ? "psk" : "cw")) return;
        u.sigs.clear();
        bool any = false;
        for (auto& x : d["signals"].a) {
            if (u.sigs.size() >= 80) break;
            DecSig g;
            g.id = x["id"].num(0); g.hz = x["hz"].num(0); g.audio = x["audio"].num(0); g.wpm = x["wpm"].num(0); g.snr = x["snr"].num(0);
            g.wpmLo = x["wpmLo"].num(0); g.wpmHi = x["wpmHi"].num(0); g.q = x["q"].str(); g.active = x["active"].boolean(); g.last = x["last"].num(0);
            g.call = decClip(x["call"].str(), 16); g.country = decClip(x["country"].str(), 40); g.text = decClip(x["text"].str(), 160);
            g.baud = psk ? (x["baud"].num(0) > 40 ? 63 : 31) : 0; g.imd = x["imd"].num(0);
            any = any || g.active;
            u.sigs.push_back(std::move(g));
        }
        decApplyClr(u.sigs);
        decSortSigs(u.sigs);   // ★★ the app's order (cwList sortCwSignals) — the server's order moves every update
        if (!psk) decCwHistories();
        u.live = any; if (any) u.liveAt = now;
        return;
    }
    if ((type == "cw_spot" || type == "psk_spot" || type == "jtty_spot") && d["callsign"].kind == Json::Str && !d["callsign"].str().empty()) {
        const std::string want = type == "cw_spot" ? "cw" : type == "psk_spot" ? "psk" : "jtty";
        if (u.active != want) return;
        DecCall c;
        c.call = decClip(d["callsign"].str(), 16); c.country = decClip(d["country"].str(), 40); c.how = decClip(d["how"].str(), 8);
        c.hz = d["frequency"].num(0); c.snr = d["snr"].num(0); c.wpm = d["wpm"].num(0); c.mode = decClip(d["mode"].str(), 8); c.at = (double)std::time(nullptr);
        for (auto it = u.calls.begin(); it != u.calls.end(); ++it)
            if (it->call == c.call) { if (want == "jtty" && it->how == "CQ") c.how = "CQ"; u.calls.erase(it); break; }   // ★ once a CQ, a CQ spot
        u.calls.push_front(c);
        decCap(u.calls, 200);
        return;
    }
    if (type == "jtty_signals" && d["signals"].kind == Json::Arr) {
        if (u.active != "jtty") return;
        u.sigs.clear();
        bool any = false;
        for (auto& x : d["signals"].a) {
            if (u.sigs.size() >= 80) break;
            DecSig g;
            g.id = x["id"].num(0); g.hz = x["hz"].num(0); g.audio = x["audio"].num(0); g.snr = x["snr"].num(0); g.q = x["q"].str();
            g.active = x["active"].boolean(); g.frames = x["frames"].num(0);
            g.call = decClip(x["call"].str(), 16); g.country = decClip(x["country"].str(), 40); g.text = decClip(x["text"].str(), 600);
            any = any || g.active;
            u.sigs.push_back(std::move(g));
        }
        decApplyClr(u.sigs);
        decSortSigs(u.sigs);   // ★ lowest first
        u.live = any; if (any) u.liveAt = now;
        return;
    }
    if (type == "dsc_call" && d["line"].kind == Json::Str) {
        if (u.active != "dsc") return;
        mapDecFeed(a, type, d);   // ★ the DSC map's distress positions (map_modes.h)
        DecDsc c;
        c.id = d["id"].num(0); c.rf = d["rf"].num(0); c.utc = d["utc"].num(0); c.repeat = std::max(1, (int)d["repeat"].num(1));
        c.level = std::max(0, std::min(3, (int)d["level"].num(0))); c.line = decClip(d["line"].str(), 600);
        for (auto it = u.dsc.begin(); it != u.dsc.end(); ++it) if (it->id == c.id && it->rf == c.rf) { u.dsc.erase(it); break; }
        u.dsc.push_front(c);
        decCap(u.dsc, 300);
        u.live = true; u.liveAt = now;
        return;
    }
    if (type == "aprs_list" && d.kind == Json::Obj) {
        if (u.active != "aprs") return;
        mapDecFeed(a, type, d);   // ★ the APRS map's positions (map_modes.h)
        u.aprs.clear();
        bool recent = false;
        for (auto& x : d["stations"].a) {
            if (u.aprs.size() >= 80) break;
            DecAprsSt st;
            st.key = decClip(x["key"].str(), 40); st.name = decClip(!x["name"].str().empty() ? x["name"].str() : x["from"].str(), 24);
            std::string line = x["line"].str();
            const size_t dot = line.find("·");
            if (dot != std::string::npos && dot < 40) { line = line.substr(dot + 2); while (!line.empty() && line[0] == ' ') line.erase(0, 1); }   // (the web drops the name before ·)
            st.line = decClip(line.empty() ? x["kind"].str() : line, 200); st.kind = decClip(x["kind"].str(), 14);
            st.seen = x["seen"].num(0); st.pos = x["lat"].kind == Json::Num;
            recent = recent || st.seen < 60;
            if (u.clrFor == "aprs" && u.aprsClrAt > 0 && st.seen >= nowSec() - u.aprsClrAt) continue;   // ★ CLR: not heard since
            u.aprs.push_back(std::move(st));
        }
        u.live = recent;
        return;
    }
    if (type == "aprs_packet" && d.kind == Json::Obj) {
        if (u.active != "aprs" || d["key"].str().empty()) return;
        u.aprsLog.push_front({(double)std::time(nullptr), decClip(!d["line"].str().empty() ? d["line"].str() : d["raw"].str(), 300)});
        decCap(u.aprsLog, 60);
        u.live = true; u.liveAt = now;
        return;
    }
    if (type == "textmode_status" && d.kind == Json::Obj) {
        if (u.active != "mfsk") return;
        const std::string det = decClip(d["detected"].str(), 24);
        if (!det.empty()) u.status = det + " · " + std::to_string((int)std::lround(std::max(0.0, std::min(5000.0, d["hz"].num(0))))) + " Hz";
        else u.status = decIdleStatus("mfsk");
        return;
    }
    if (type == "sitora_status" && d.kind == Json::Obj) {
        if (u.active != "sitora") return;
        u.sitor = d;
        const int link = std::max(0, std::min(7, (int)std::lround(d["link"].num(0))));
        u.status = decSitorText(d, link);
        u.live = link >= 2 && link <= 6;
        return;
    }
    (void)s;
}

/** The decoder socket's path: this listener's session, so a per-VFO radio decodes THIS listener's audio. */
inline std::string decPath(App& a) {
    const std::string sid = decSidOf(*a.eng_, 0);
    return sid.empty() ? std::string("/ws/dxcluster?client=vibe-hhui") : "/ws/dxcluster?user_session_id=" + sid + "&client=vibe-hhui";
}
inline void decDrop() {
    DecUi& u = decUi();
    if (!u.ws) return;
    std::shared_ptr<WsClient> w; w.swap(u.ws);
    std::thread([w] { w->close(); }).detach();   // ★ the reader thread is joined off the UI thread
}

inline void decCannedTick(App& a, const RadioState& s);

/** ★ Once a loop, after eng.pump (dabTick's neighbour): the socket kept to what runs, its messages drained and read,
 *  a settled option re-attached, the server's offer refreshed. Cheap when nothing runs. */
inline void decTick(App& a, const RadioState& s) {
    DecUi& u = decUi();
    const double now = nowSec();
    const bool wanted = s.session == "running" && ((!u.active.empty() && !decExt(u.active)) || u.spotsOn);   // (an external decoder brings its own transport)
    // the server's offer: at the session's start, then at most once a minute while the decoders are in use
    {
        static std::string lastSess;
        if (s.session != lastSess) { lastSess = s.session; if (s.session == "running") { std::lock_guard<std::mutex> lk(u.capMtx); u.capAt = -1e9; } }
        bool due;
        { std::lock_guard<std::mutex> lk(u.capMtx); due = s.session == "running" && !u.capFetching && (now - u.capAt > (u.capFetched ? 60 : 5)); }
        if (due && (u.boxOpen || !u.capFetched)) decFetchCaps(a.eng_->port());
    }
    if (s.session != "running" && decRunning()) { u.active.clear(); u.spotsOn = false; u.view.clear(); u.boxOpen = false; }
    // ★★ APRS MUTES THE SPEAKER (Stuart, 2026-11-10): packets are a screech nobody listens to. Muted when it opens, put
    //    back as it was when it closes, however it closes; the speaker key still unmutes in between (the mute is the
    //    AUDIO sheet's own, so the deck's glyph shows it). This listener's speaker only — the server and anyone else on
    //    the dial hear what they heard.
    {
        static bool aprsWas = false, muteWas = false;
        const bool aprs = u.active == "aprs";
        if (aprs != aprsWas) {
            aprsWas = aprs;
            aud::State& A = aud::st();
            if (aprs) { muteWas = A.mute; A.mute = true; } else A.mute = muteWas;
            aud::sendLevel(a);
            // ★★ …AND NFM AUDIO GOES TO ALL (Stuart, 2026-11-10: "when APRS is open the NFM needs to switch to all and then
            //    switch back to voice afterwards"). Voice's steep edges smear 1200/2200 (100 synthetic packets, 11 dB, space
            //    tone 6 dB down: ALL 70, VOICE 18). Silent, so the harsher hiss costs nothing. A press in between stands.
            const bool was = A.aprsNfmRaw;
            A.aprsNfmRaw = aprs;
            if (aprs || was) {
                const int w = aud::nfmWant(A);
                a.eng_->send(std::string("{\"type\":\"nfmvoice\",\"on\":") + (w == 0 ? "false" : "true") + "}");
            }
            std::fprintf(stderr, "vibe-hhui: speaker %s (APRS %s)\n", A.mute ? "muted" : "on", aprs ? "open" : "closed");
        }
    }
    // ★ tests: VIBE_HHUI_DEC_START=time,spots starts those once the session runs and the offer is in (the screenshots)
    {
        static bool started = false;
        const char* st = std::getenv("VIBE_HHUI_DEC_START");
        bool fetched; { std::lock_guard<std::mutex> lk(u.capMtx); fetched = u.capFetched; }
        if (!started && st && *st && s.session == "running" && fetched) {
            started = true;
            std::string list = st, one;
            for (size_t i = 0; i <= list.size(); i++) {
                if (i < list.size() && list[i] != ',') { one += list[i]; continue; }
                if (!one.empty()) decStart(a, s, one);
                one.clear();
            }
        }
    }
    if (DecExternal* e = decExt(u.active)) { if (e->tick) e->tick(a, s); if (e->status) u.status = e->status(); if (e->live) u.live = e->live(); }
    if (u.canned) { decCannedTick(a, s); }
    else {
        // the socket: opened while wanted (a worker does the connect), closed 8 s after the last decoder stops
        if (u.ws && !u.ws->alive()) { decDrop(); u.retryAt = now + 3; }
        {
            std::shared_ptr<WsClient> f;
            { std::lock_guard<std::mutex> lk(u.netMtx); f.swap(u.fresh); }
            if (f) {
                if (wanted && !u.ws) {
                    u.ws = f;
                    // ★ re-asserted WITH the parameters (the web's lesson: a reconnect must never revert to the defaults)
                    if (!u.active.empty() && !decExt(u.active)) u.ws->sendText(decAttachJson(s, u.active));
                    if (u.spotsOn) u.ws->sendText("{\"type\":\"subscribe_digital_spots\"}");
                    std::fprintf(stderr, "vibe-hhui: decoder socket open\n");
                } else std::thread([f] { f->close(); }).detach();
            }
        }
        bool opening;
        { std::lock_guard<std::mutex> lk(u.netMtx); opening = u.opening; }
        if (wanted) {
            u.idleSince = 0;
            if (!u.ws && !opening && now >= u.retryAt) {
                { std::lock_guard<std::mutex> lk(u.netMtx); u.opening = true; }
                std::thread([port = a.eng_->port(), path = decPath(a)] {
                    auto w = std::make_shared<WsClient>();
                    w->maxBinaryQueued = 1024;   // ★ text frames are binary here: a slow frame must not drop decoded text
                    const bool ok = w->open(port, path);
                    DecUi& u2 = decUi();
                    std::lock_guard<std::mutex> lk(u2.netMtx);
                    u2.opening = false;
                    if (ok) u2.fresh = w; else u2.retryAt = nowSec() + 3;
                }).detach();
            }
        } else if (u.ws) {
            if (u.idleSince == 0) u.idleSince = now;
            else if (now - u.idleSince > 8) { decDrop(); u.idleSince = 0; }
        }
        if (u.ws) {
            std::vector<WsMessage> msgs;
            u.ws->drain(msgs);
            for (auto& m : msgs) { if (m.text) decOnJson(a, s, m.data); else decOnBinary(a, s, m.data); }
        }
    }
    /* ★ ONE LINE A MINUTE while the spots run (Stuart, 2026-11-10: with the screen off "it appears as if a lot of decodes
     *  were missing"): the list keeps ONE row per call, so a repeat never shows as new — this count, in the debug log,
     *  says whether spots kept arriving through a dark screen. */
    if (u.spotsOn) {
        if (u.spotsMinAt == 0) u.spotsMinAt = now;
        else if (now - u.spotsMinAt >= 60) {
            std::fprintf(stderr, "vibe-hhui: spots: %d in the last minute (screen %s)\n", u.spotsMin, a.screenOff_ ? "off" : "on");
            u.spotsMin = 0; u.spotsMinAt = now;
        }
    } else { u.spotsMin = 0; u.spotsMinAt = 0; }
    decPicTick(a, s);   // the pictures: the WEFAX worker's answers, the textures' mipmaps, the LED
    galTick(a);         // the gallery's list, thumbnails and picture
    // a burst of option presses: the one re-attach
    if (u.reattachAt > 0 && now >= u.reattachAt) {
        u.reattachAt = 0;
        if (!u.active.empty() && !decExt(u.active)) {
            u.ext = decExtFor(u.active); u.tuneHint.clear();
            decSend(decAttachJson(s, u.active));
            std::fprintf(stderr, "vibe-hhui: decoder %s re-attach %s\n", u.active.c_str(), decAttachJson(s, u.active).c_str());
        }
    }
    // the LED: quiet for 20 s = not live (a list decoder's own flag stands)
    if (u.live && u.liveAt > 0 && now - u.liveAt > 20 && (u.active == "rtty" || u.view == "spots")) u.live = false;
}

/** The provider's decoder stopped by itself (refused, the radio left FM …): the key goes dark, the box follows. */
inline void decExternalEnded(const std::string& id) {
    DecUi& u = decUi();
    if (u.active != id) return;
    u.active.clear();
    if (u.view == id) u.view = u.spotsOn ? "spots" : "";
    if (!decRunning() && u.pane == DecUi::P_OUT) u.boxOpen = false;
}

// ── tests: canned frames through the real parse path ─────────────────────────────────────────────────────────────────

inline std::string decTextFrame(const std::string& t) {
    std::string b(13, '\0');
    b[0] = 0x01;
    const uint32_t n = (uint32_t)t.size();
    b[9] = (char)(n >> 24); b[10] = (char)(n >> 16); b[11] = (char)(n >> 8); b[12] = (char)n;
    return b + t;
}
inline void decCannedTick(App& a, const RadioState& s) {
    DecUi& u = decUi();
    const double now = nowSec();
    if (now - u.cannedAt < 0.25) return;
    u.cannedAt = now;
    const int k = u.cannedN++;
    const std::string& m = u.active;
    if (k == 0 && !m.empty()) decOnJson(a, s, "{\"type\":\"audio_extension_attached\"}");
    if (m == "rtty") {
        static const std::string txt = "RYRYRYRY CQ CQ CQ DE DL0ABC DL0ABC DL0ABC PSE K\r\r\nDL0ABC DE G4XYZ UR 599 599 IN LONDON NAME JOHN JOHN HW? BK\r\r\n"
                                       "WX REPORT 0600 UTC: NORTH SEA WIND SW 5 TO 7 BACKING S 6 TO GALE 8 LATER. RAIN, GOOD BECOMING MODERATE.\r\r\n";
        if (k == 1) decOnBinary(a, s, std::string("\x03\x03", 2));
        const size_t at = (size_t)(k * 9) % txt.size();
        decOnBinary(a, s, decTextFrame(txt.substr(at, 9)));
        if (k == 3) decOnBinary(a, s, std::string("\x06\x00\x64", 3));
        if (k == 20) decOnBinary(a, s, std::string("\x06\x00\x00", 3));
    } else if (m == "time") {
        if (k % 4 == 0) {
            const int sec = (k / 4) % 60;
            char b[160]; std::snprintf(b, sizeof b, "\r[MSF] second %d/59 · minute marker ok · date 2026-11-07 · 14:%02d", sec, 32 + (k / 240));
            decOnBinary(a, s, decTextFrame(b));
        }
        if (k == 2) decOnBinary(a, s, decTextFrame("[MSF] locked — carrier +13 dB\n"));
        if (k == 40) decOnBinary(a, s, decTextFrame("\n[MSF] 14:33 UTC Sat 7 Nov 2026 (BST no) — confirmed by the minute before\n"));
        if (k == 41) {
            const double host = (double)std::time(nullptr) * 1000.0;
            char j[200]; std::snprintf(j, sizeof j, "{\"type\":\"time_sync\",\"data\":{\"station\":\"MSF\",\"utcMs\":%.0f,\"hostMs\":%.0f,\"rxDelayMs\":12}}", host + 230, host);
            decOnJson(a, s, j);
        }
        if (k == 1) decOnBinary(a, s, std::string("\x03\x03", 2));
    } else if (m == "navtex") {
        static const std::string txt = "ZCZC GA42\nWZ 1534\nNORTH SEA. HUMBER. FISHING VESSEL ADRIFT 53-41N 000-52E.\nVESSELS IN VICINITY KEEP SHARP LOOKOUT.\nNNNN\n";
        const size_t at = (size_t)(k * 7) % txt.size();
        decOnBinary(a, s, decTextFrame(txt.substr(at, 7)));
        if (at + 7 >= txt.size()) { std::string f(13, '\0'); f[0] = 0x07; f[4] = 44; f[8] = 2; decOnBinary(a, s, f); }
        if (k == 1) decOnBinary(a, s, std::string("\x03\x03", 2));
    } else if (m == "cw" || m == "psk") {
        if (k % 4 == 0) {
            const bool psk = m == "psk";
            char j[1400];
            std::snprintf(j, sizeof j, "{\"type\":\"%s_signals\",\"data\":{\"skimmer\":false,\"primary\":1,\"signals\":["
                "{\"id\":1,\"hz\":7023500,\"audio\":700,\"wpm\":22,\"wpmLo\":20,\"wpmHi\":24,\"snr\":18,\"q\":\"high\",\"active\":true,\"call\":\"G4ABC\",\"country\":\"England\",\"text\":\"CQ CQ DE G4ABC G4ABC K\",\"baud\":31},"
                "{\"id\":2,\"hz\":7024100,\"audio\":1300,\"wpm\":16,\"snr\":9,\"q\":\"medium\",\"active\":true,\"call\":\"DL1XYZ\",\"country\":\"Germany\",\"text\":\"TU 5NN DL1XYZ\",\"baud\":63},"
                "{\"id\":3,\"hz\":7022900,\"audio\":100,\"wpm\":28,\"snr\":3,\"q\":\"low\",\"active\":false,\"call\":\"\",\"country\":\"\",\"text\":\"EE TNX FER\",\"baud\":31}]}}", psk ? "psk" : "cw");
            decOnJson(a, s, j);
        }
        if (k == 6) decOnJson(a, s, std::string("{\"type\":\"") + (m == "psk" ? "psk" : "cw") + "_spot\",\"data\":{\"callsign\":\"G4ABC\",\"frequency\":7023500,\"snr\":18,\"wpm\":22,\"how\":\"CQ\",\"country\":\"England\"}}");
        static const std::string txt = "CQ CQ DE G4ABC G4ABC K  G4ABC DE DL1XYZ TNX FER CALL UR RST 599 5NN NAME HANS QTH BERLIN HW? AR ";
        decOnBinary(a, s, decTextFrame(txt.substr((size_t)(k * 3) % txt.size(), 3)));
        if (k == 1) decOnBinary(a, s, std::string("\x03\x03", 2));
    } else if (m == "dsc") {
        if (k == 4) decOnJson(a, s, "{\"type\":\"dsc_call\",\"data\":{\"id\":7,\"rf\":8414500,\"repeat\":1,\"level\":0,\"line\":\"ROUTINE · MMSI 002320004 (UK coast station, Falmouth) → MMSI 235012345 (United Kingdom) · test call · ack requested\"}}");
        if (k == 10) decOnJson(a, s, "{\"type\":\"dsc_call\",\"data\":{\"id\":8,\"rf\":8414500,\"repeat\":5,\"level\":3,\"line\":\"DISTRESS · MMSI 235099999 (United Kingdom) · Sinking · 50°12'N 004°10'W · 14:32 UTC\"}}");
        if (k == 1) decOnBinary(a, s, std::string("\x03\x00", 2));
    } else if (m == "jtty") {
        if (k % 4 == 0) decOnJson(a, s, "{\"type\":\"jtty_signals\",\"data\":{\"signals\":[{\"id\":1,\"hz\":14091200,\"audio\":1200,\"snr\":-8,\"q\":\"high\",\"active\":true,\"frames\":12,\"call\":\"K1ABC\",\"country\":\"United States\",\"text\":\"CQ TEST K1ABC FN42\"},{\"id\":2,\"hz\":14091650,\"audio\":1650,\"snr\":-15,\"q\":\"low\",\"active\":true,\"frames\":3,\"call\":\"\",\"country\":\"\",\"text\":\"K1ABC G4XYZ 599 IO91\"}]}}");
        if (k == 8) decOnJson(a, s, "{\"type\":\"jtty_spot\",\"data\":{\"callsign\":\"K1ABC\",\"frequency\":14091200,\"snr\":-8,\"how\":\"CQ\",\"country\":\"United States\"}}");
    } else if (m == "aprs") {
        if (k == 4) decOnJson(a, s, "{\"type\":\"aprs_packet\",\"data\":{\"key\":\"M0ABC-9\",\"from\":\"M0ABC-9\",\"kind\":\"position\",\"line\":\"M0ABC-9 · 51°30'N 0°07'W · car, 48 km/h · via MB7UH\",\"lat\":51.5,\"lon\":-0.12}}");
        if (k % 8 == 5) decOnJson(a, s, "{\"type\":\"aprs_list\",\"data\":{\"stations\":[{\"key\":\"M0ABC-9\",\"from\":\"M0ABC-9\",\"kind\":\"position\",\"line\":\"M0ABC-9 · car, 48 km/h · via MB7UH\",\"seen\":4,\"lat\":51.5,\"lon\":-0.12},{\"key\":\"GB7WX\",\"from\":\"GB7WX\",\"kind\":\"weather\",\"line\":\"GB7WX · 12 °C · wind SW 14 km/h · 1013 hPa\",\"seen\":95}]}}");
    } else if (m == "sitora") {
        if (k == 2) decOnJson(a, s, "{\"type\":\"sitora_status\",\"data\":{\"link\":4,\"iss\":0,\"master\":0,\"ids\":[\"PEARDBY\",\"\"],\"numbers\":[\"364775427\",\"\"],\"blocks\":120,\"repeats\":12,\"lost\":2,\"idChecked\":true}}");
        static const std::string txt = "[1 · PEARDBY] GA+? QRU? MSG FOR MASTER: ETA ROTTERDAM 0600 UTC 08 NOV. CARGO DOCS FOLLOW. ";
        decOnBinary(a, s, decTextFrame(txt.substr((size_t)(k * 5) % txt.size(), 5)));
    } else if (decIsPicture(m)) {
        decPicCanned(a, s, k);
    } else if (m == "same" || m == "selcal" || m == "mfsk") {
        if (k == 6) decOnBinary(a, s, decTextFrame(m == "same" ? "ZCZC-WXR-TOR-029095-029101+0045-3121453-KEAX/NWS- Tornado Warning · Jackson MO, Cass MO · until 15:30\n"
                                                    : m == "selcal" ? "14:32:07 SELCAL AB-CD (4 tones, 1.0 s)\n" : "[OLIVIA 8/250 · 1500 Hz]\nCQ CQ DE PA3ABC PA3ABC OLIVIA 8/250 K\n"));
        if (k == 5 && m == "mfsk") decOnJson(a, s, "{\"type\":\"textmode_status\",\"data\":{\"detected\":\"OLIVIA 8/250\",\"hz\":1500}}");
    }
    if (u.spotsOn && k % 3 == 1) {
        static const char* calls[] = {"G4ABC", "DL1XYZ", "K1ABC", "VK2DEF", "JA1GHI", "EA8JKL", "PY2MNO", "ZS6PQR"};
        static const char* grids[] = {"IO91", "JO62", "FN42", "QF56", "PM95", "IL18", "GG66", "KG33"};
        static const char* ctry[] = {"England", "Germany", "United States", "Australia", "Japan", "Canary Is.", "Brazil", "South Africa"};
        const int i = (k / 3) % 8;
        char j[400];
        std::snprintf(j, sizeof j, "{\"type\":\"digital_spot\",\"data\":{\"mode\":\"FT8\",\"callsign\":\"%s\",\"snr\":%d,\"frequency\":%d,\"band\":\"20m\",\"grid\":\"%s\",\"country\":\"%s\",\"dt\":%.1f}}",
                      calls[i], -20 + (i * 7) % 30, 14074000 + 300 + i * 211, grids[i], ctry[i], 0.1 + 0.1 * (i % 3));
        decOnJson(a, s, j);
    }
}

}  // namespace hh
