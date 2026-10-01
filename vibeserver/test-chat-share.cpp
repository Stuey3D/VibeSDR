// test-chat-share.cpp — "share a station" in the canned chat: what the server reads, refuses,
// names and sends (vibe_chat_share.h).
//
// Proves:
//   • THE SENDER'S LABEL NEVER REACHES THE ROOM — a request carrying name/label/text fields has them
//     ignored, and the line the room receives names the station from the SERVER'S knowledge only;
//   • the vocabulary stays closed: an unknown mode, a junk id, an unknown block = dropped;
//   • the range gate reads the EFFECTIVE tunable set, the owner's mode block, and DAB capability;
//   • names: RDS-learned and the owner's bookmarks by nearest frequency, provisional labels never,
//     EiBi only below 30 MHz and only on air now, DAB by service id on the block (ensemble id must
//     agree when both sides know it);
//   • the line keeps the top-level hz/mode an older client reads, and carries a server-built text.
#include "vibe_chat_share.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace vibechat;
static int fails = 0, passes = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } else ++passes; } while (0)

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static Caps rtlCaps(bool dab = true) {
    Caps c;
    c.ranges = { {500000.0, 1766000000.0} };          // an unrestricted RTL-SDR V4
    c.modeBlocked = [](const std::string& m) { return m == "wefax"; };
    c.dabCapable = dab;
    return c;
}

int main() {
    printf("test-chat-share\n");

    // ── Reading the request ────────────────────────────────────────────────────────────────────
    {
        ShareReq r;
        CHECK(parseRequest(R"({"type":"say","id":"check_out","kind":"bookmark","hz":96600000,"mode":"wfm","bwLo":-100000,"bwHi":100000})", r),
              "a plain bookmark share parses");
        CHECK(!r.dab && r.hz == 96600000 && r.mode == "wfm" && r.hasBw && r.bwLo == -100000 && r.bwHi == 100000,
              "  …with its frequency, mode and passband");
        CHECK(parseRequest(R"({"type":"say","id":"check_out","hz":96600000,"mode":"wfm"})", r) && !r.dab,
              "the old composer's share (no kind) is an analogue share");
        CHECK(parseRequest(R"({"type":"say","id":"check_out","kind":"dab","block":"12b","sid":"C0D2","eid":"0xCC15"})", r),
              "a DAB share with hex ids parses");
        CHECK(r.dab && r.block >= 0 && std::strcmp(vibedab::kBandIII[r.block].name, "12B") == 0 && r.sid == 0xC0D2 && r.eid == 0xCC15,
              "  …block 12B, sid C0D2, eid CC15");
        CHECK(parseRequest(R"({"type":"say","id":"check_out","kind":"dab","block":"12B","sid":49362})", r) && r.sid == 49362,
              "a numeric sid is read as a number (the shim's jsonStr would have read the next key)");
        CHECK(parseRequest(R"({"type":"say","id":"check_out","sid":49362,"eid":52245,"block":"11D"})", r)
              && r.sid == 49362 && r.eid == 52245, "numbers followed by more keys read correctly");
        CHECK(!parseRequest(R"({"type":"say","id":"check_out","kind":"dab","block":"12Z","sid":"C0D2"})", r),
              "an unknown block is dropped");
        CHECK(!parseRequest(R"({"type":"say","id":"check_out","kind":"dab","block":"12B","sid":"you idiot"})", r),
              "a sid that is not an id is dropped — no text through an id field");
        CHECK(!parseRequest(R"({"type":"say","id":"check_out","kind":"rude words","hz":96600000})", r),
              "an unknown kind is dropped");
        CHECK(!parseRequest(R"({"type":"say","id":"check_out","hz":"ninety six"})", r),
              "a frequency must be a number");
        CHECK(!parseRequest(R"({"type":"say","id":"check_out","kind":"dab","block":"12B","sid":0})", r),
              "sid 0 is not a service");
    }

    // ── ★★★ The label never travels ───────────────────────────────────────────────────────────
    {
        const char* req = R"({"type":"say","id":"check_out","kind":"bookmark","hz":96600000,"mode":"wfm",)"
                          R"("name":"SOMETHING OFFENSIVE","label":"SOMETHING OFFENSIVE","text":"SOMETHING OFFENSIVE"})";
        ShareReq r;
        CHECK(parseRequest(req, r), "a share with smuggled text fields still parses (they are ignored)");
        Caps c = rtlCaps();
        CHECK(validate(r, c) == Verdict::Ok, "  …and validates on its numbers");
        // Nothing known at 96.6: the room hears the bare frequency.
        std::vector<Known> none;
        const std::string line = saidJson(4, false, r, resolveName(r, none, nullptr, 600));
        CHECK(!has(line, "OFFENSIVE"), "the smuggled label is NOT in the line the room receives");
        CHECK(has(line, "\"text\":\"shared 96.600 MHz WFM\""), "nothing known → the bare frequency and mode");
        // Something known: the SERVER'S name, not the sender's.
        std::vector<Known> k = { {"Heart", 96600000, "wfm", -1, -1, 2} };
        const std::string l2 = saidJson(4, false, r, resolveName(r, k, nullptr, 600));
        CHECK(!has(l2, "OFFENSIVE") && has(l2, "\"name\":\"Heart\""), "the name is the receiver's own (RDS learnt)");
        CHECK(has(l2, "\"text\":\"shared 96.600 MHz WFM \xE2\x80\x94 Heart\""), "fallback text: shared 96.600 MHz WFM — Heart");
    }

    // ── Validation: range, mode, DAB ─────────────────────────────────────────────────────────
    {
        ShareReq r; Caps c = rtlCaps();
        parseRequest(R"({"hz":96600000,"mode":"Advanced RDS"})", r);
        CHECK(validate(r, c) == Verdict::BadPayload, "a mode outside the closed list is refused (was echoed before)");
        parseRequest(R"({"hz":96600000,"mode":"WFM"})", r);
        CHECK(validate(r, c) == Verdict::Ok && r.mode == "wfm", "mode is case-folded");
        parseRequest(R"({"hz":100000,"mode":"am"})", r);
        CHECK(validate(r, c) == Verdict::OutOfRange, "below the radio's coverage is refused");
        parseRequest(R"({"hz":2400000000,"mode":"nfm"})", r);
        CHECK(validate(r, c) == Verdict::OutOfRange, "above the radio's coverage is refused");
        parseRequest(R"({"hz":7800000,"mode":"wefax"})", r);
        CHECK(validate(r, c) == Verdict::ModeBlocked, "a mode the owner switched off is refused");
        parseRequest(R"({"hz":96600000,"mode":"wfm","bwLo":5000,"bwHi":-5000})", r);
        CHECK(validate(r, c) == Verdict::Ok && !r.hasBw, "an inverted passband is dropped, the share kept");

        Caps hf; hf.ranges = { {1000.0, 31000000.0} }; hf.dabCapable = false;   // Airspy HF+
        parseRequest(R"({"hz":96600000,"mode":"wfm"})", r);
        CHECK(validate(r, hf) == Verdict::OutOfRange, "FM on an HF-only radio is refused");
        parseRequest(R"({"kind":"dab","block":"12B","sid":"C0D2"})", r);
        CHECK(validate(r, hf) == Verdict::NoDab, "DAB on a radio that cannot do DAB is refused");
        parseRequest(R"({"kind":"dab","block":"12B","sid":"C0D2"})", r);
        CHECK(validate(r, rtlCaps(false)) == Verdict::NoDab, "DAB on a build/owner without DAB is refused");

        parseRequest(R"({"hz":225650000,"mode":"dab"})", r);
        CHECK(validate(r, c) == Verdict::Ok && r.dab && std::strcmp(vibedab::kBandIII[r.block].name, "12B") == 0
              && r.hz == 225648000, "an old client's 'check out 225.65 DAB' becomes block 12B at its table centre");
        parseRequest(R"({"hz":226500000,"mode":"dab"})", r);
        CHECK(validate(r, c) == Verdict::BadPayload, "a DAB frequency that is not a block is refused");
        parseRequest(R"({"kind":"dab","block":"12B","sid":"C0D2","hz":1})", r);
        CHECK(validate(r, c) == Verdict::Ok && r.hz == 225648000, "a DAB share's hz is the TABLE's, never the client's");
    }

    // ── Naming ───────────────────────────────────────────────────────────────────────────────
    {
        std::vector<Known> k = {
            {"PI4322 93.7MHz", 93700000, "wfm", -1, -1, 0},       // provisional — never a name
            {"Heart",          96600000, "wfm", -1, -1, 2},       // RDS heard
            {"Hrt?",           96610000, "wfm", -1, -1, 1},       // a guess, nearer to nothing
            {"Owner's beacon", 7040000,  "usb", -1, -1, 2},       // the owner's bookmark
            {"Heart",          225648000, "dab", 0xC0D2, 0xCC15, 2},
            {"Capital",        225648000, "dab", 0xC0D3, 0xCC15, 2},
        };
        ShareReq r; Caps c = rtlCaps();
        parseRequest(R"({"hz":93700000,"mode":"wfm"})", r); validate(r, c);
        CHECK(resolveName(r, k, nullptr, 0).name.empty(), "a provisional PI label is not a name");
        parseRequest(R"({"hz":96605000,"mode":"wfm"})", r); validate(r, c);
        CHECK(resolveName(r, k, nullptr, 0).name == "Heart", "nearest within ±20 kHz on FM wins");
        parseRequest(R"({"hz":96700000,"mode":"wfm"})", r); validate(r, c);
        CHECK(resolveName(r, k, nullptr, 0).name.empty(), "the next FM channel is not that station");
        parseRequest(R"({"hz":7040200,"mode":"usb"})", r); validate(r, c);
        CHECK(resolveName(r, k, nullptr, 0).name == "Owner's beacon", "the owner's bookmark names an SSB share");

        // DAB: by sid on the block.
        parseRequest(R"({"kind":"dab","block":"12B","sid":"C0D2"})", r); validate(r, c);
        Named n = resolveName(r, k, nullptr, 0, "D1 National");
        CHECK(n.name == "Heart" && n.ensemble == "D1 National", "DAB: the service label this receiver decoded");
        const std::string line = saidJson(2, true, r, n);
        CHECK(has(line, "\"kind\":\"dab\"") && has(line, "\"block\":\"12B\"") && has(line, "\"sid\":49362")
              && has(line, "\"mode\":\"dab\"") && has(line, "\"hz\":225648000") && has(line, "\"admin\":true"),
              "DAB line: kind, block, sid, mode and the old clients' hz");
        CHECK(has(line, "\"text\":\"shared DAB Heart \xE2\x80\x94 12B (225.648 MHz)\""), "DAB fallback text");
        parseRequest(R"({"kind":"dab","block":"12B","sid":"C0D2","eid":"1234"})", r); validate(r, c);
        CHECK(resolveName(r, k, nullptr, 0).name.empty(), "a different ensemble id = a different multiplex: no name");
        parseRequest(R"({"kind":"dab","block":"12B"})", r); validate(r, c);
        n = resolveName(r, k, nullptr, 0, "D1 National");
        CHECK(n.name.empty() && fallbackText(r, n) == "shared DAB 12B (225.648 MHz) \xE2\x80\x94 D1 National",
              "a multiplex share is named by its ensemble");
        parseRequest(R"({"kind":"dab","block":"11D","sid":"C0D2"})", r); validate(r, c);
        CHECK(resolveName(r, k, nullptr, 0).name.empty(), "the same sid on another block is not that service");
    }

    // ── EiBi: on air now, below 30 MHz, only when nothing better is known ─────────────────────
    {
        const std::string eibi =
            R"([{"name":"BBC World Service","frequency":5875000,"source":"eibi","comment":"0500-0700 · E","mode":"am"},)"
            R"({"name":"Radio Romania Int","frequency":5875000,"source":"eibi","comment":"2000-2100 · E","mode":"am"},)"
            R"({"name":"Overnight","frequency":6000000,"source":"eibi","comment":"2300-0100 · E","mode":"am"},)"
            R"({"name":"Allday","frequency":9410000,"source":"eibi","mode":"am"}])";
        ShareReq r; Caps c = rtlCaps();
        std::vector<Known> none;
        parseRequest(R"({"hz":5875000,"mode":"am"})", r); validate(r, c);
        CHECK(resolveName(r, none, &eibi, 6 * 60).name == "BBC World Service", "the entry on air at 06:00");
        CHECK(resolveName(r, none, &eibi, 20 * 60 + 30).name == "Radio Romania Int", "the entry on air at 20:30");
        CHECK(resolveName(r, none, &eibi, 12 * 60).name.empty(), "nothing on air at noon = no name");
        CHECK(std::strcmp(resolveName(r, none, &eibi, 6 * 60).source, "eibi") == 0, "  …sourced eibi");
        parseRequest(R"({"hz":6000000,"mode":"am"})", r); validate(r, c);
        CHECK(resolveName(r, none, &eibi, 30).name == "Overnight", "a window that crosses midnight");
        CHECK(resolveName(r, none, &eibi, 120).name.empty(), "  …and ends after it");
        parseRequest(R"({"hz":9410500,"mode":"am"})", r); validate(r, c);
        CHECK(resolveName(r, none, &eibi, 999).name == "Allday", "an entry with no time is not 'off air'");
        std::vector<Known> k = { {"Owner's name", 5875000, "am", -1, -1, 2} };
        parseRequest(R"({"hz":5875000,"mode":"am"})", r); validate(r, c);
        CHECK(resolveName(r, k, &eibi, 6 * 60).name == "Owner's name", "the receiver's own bookmark outranks the timetable");
    }

    // ── Escaping: a name from the air cannot break the line ───────────────────────────────────
    {
        ShareReq r; Caps c = rtlCaps();
        parseRequest(R"({"hz":96600000,"mode":"wfm"})", r); validate(r, c);
        std::vector<Known> k = { {"Q\"uote\\Back", 96600000, "wfm", -1, -1, 2} };
        const std::string line = saidJson(1, false, r, resolveName(r, k, nullptr, 0));
        CHECK(has(line, "Q\\\"uote\\\\Back"), "quotes and backslashes in a station name are escaped");
    }

    // ── Formatting ───────────────────────────────────────────────────────────────────────────
    CHECK(freqText(96600000) == "96.600 MHz", "96.600 MHz");
    CHECK(freqText(198000) == "198 kHz", "198 kHz");
    CHECK(freqText(7500) == "7.5 kHz", "7.5 kHz");
    CHECK(freqText(1000000) == "1.000 MHz", "1.000 MHz");

    printf("  %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
