// test-shared-decoders.cpp — ★★★ A DECODER ON A SHARED DIAL IS THE ROOM'S (vibe_shared_decoders.h, Stuart 2026-11-24).
//
// The book of who holds the shared host's decoder: a start makes its listener the only holder, a join adds one, a release
// lets ONE listener go and the decoder stops only when the LAST lets go; the room's view (`dec_shared`) per listener; a
// SATELLITE pick's decoder held by the pick and never listed; the parameters rebuilt so a joiner's attach JOINS the run
// (the same DecoderHost key) — never a client's JSON passed through.
#include "vibe_shared_decoders.h"
#include "vibe_decoder_host.h"
#include <cstdio>
#include <string>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main() {
    using R = SharedDecoderRoom;
    std::printf("── the parameters a joiner sends back ──\n");
    {
        // the web page spreads them at the top level; the app nests them
        const std::string web = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\",\"center_frequency\":1000,\"shift\":170,"
                                "\"baud_rate\":45.45,\"encoding\":\"ITA2\",\"inverted\":false,\"framing\":\"5N1.5\",\"auto\":true}";
        const std::string app = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\",\"params\":{\"center_frequency\":1000,"
                                "\"shift\":170,\"baud_rate\":45.45,\"inverted\":false,\"framing\":\"5N1.5\",\"encoding\":\"ITA2\",\"auto\":true},\"vfo\":\"A\"}";
        const std::string pw = R::paramsJson(web), pa = R::paramsJson(app);
        std::printf("    web → %s\n    app → %s\n", pw.c_str(), pa.c_str());
        ok(!pw.empty() && !pa.empty(), "both shapes give parameters");
        ok(!has(pw, "type") && !has(pw, "extension_name") && !has(pa, "vfo"), "the envelope is not part of them");
        // ★★ the joiner's attach (either client shape) has the SAME key as the run it joins
        ok(DecoderHost::startKeyFor("fsk", pw) == DecoderHost::startKeyFor("fsk", web), "web: the rebuilt parameters JOIN the run (same key)");
        ok(DecoderHost::startKeyFor("fsk", pa) == DecoderHost::startKeyFor("fsk", app), "app: the rebuilt parameters JOIN the run (same key)");
        const std::string appJoin = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\",\"params\":" + pw + "}";
        const std::string webJoin = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\"," + pa.substr(1);
        ok(DecoderHost::startKeyFor("fsk", appJoin) == DecoderHost::startKeyFor("fsk", web), "the app joins a run the web page started");
        ok(DecoderHost::startKeyFor("fsk", webJoin) == DecoderHost::startKeyFor("fsk", app), "the web page joins a run the app started");
        // APRS as the clients attach it today (packet.ts packetParams)
        const std::string aprs = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"aprs\",\"baud_rate\":1200,\"packet\":\"auto\",\"fx25\":true,\"il2p\":true}";
        const std::string pp = R::paramsJson(aprs);
        ok(pp == "{\"baud_rate\":1200,\"packet\":\"auto\",\"fx25\":true,\"il2p\":true}", "APRS: every field kept, as written (" + pp + ")");
        ok(R::paramsJson("{\"type\":\"audio_extension_attach\",\"extension_name\":\"sstv\"}") == "{}", "no parameters = {}");
        // nothing a client could use to inject into other listeners' pages
        ok(!has(R::paramsJson("{\"extension_name\":\"fsk\",\"encoding\":\"a\\\"}],\\\"x\\\":\\\"\"}"), "\\"), "a string with escapes is dropped");
        ok(!has(R::paramsJson("{\"extension_name\":\"fsk\",\"encoding\":\"<script>\"}"), "script"), "a string with markup is dropped");
        ok(R::paramsJson("{\"extension_name\":\"fsk\",\"shift\":1e999x}").empty(), "a malformed number = not shareable");
        ok(R::paramsJson("{\"extension_name\":\"fsk\",\"shift\":170") .empty(), "a truncated message = not shareable");
        ok(R::paramsJson("{\"extension_name\":\"fsk\",\"nested\":{\"a\":[1,2,{\"b\":3}]},\"shift\":170}") == "{\"shift\":170}", "nested values are skipped");
        std::string many = "{\"extension_name\":\"fsk\"";
        for (int i = 0; i < 40; ++i) many += ",\"f" + std::to_string(i) + "\":" + std::to_string(i);
        ok(R::paramsJson(many + "}").empty(), "more than kMaxFields fields = not shareable (bounded)");
        ok(!has(R::paramsJson("{\"extension_name\":\"fsk\",\"bad-name\":1,\"ok\":2}"), "bad-name"), "a field with an odd name is dropped");
    }

    std::printf("── who holds it ──\n");
    {
        R room;
        const uintptr_t s1 = 0x1000, s2 = 0x2000, s3 = 0x3000;
        const std::string K = "aprs|key-1";
        ok(room.started(R::Text, s1, "sess-1", "aprs", "{\"baud_rate\":1200}", K, false), "listener 1 starts APRS: the room changed");
        std::string v1 = room.listJson("sess-1", K, ""), v2 = room.listJson("sess-2", K, "");
        std::printf("    to 1: %s\n    to 2: %s\n", v1.c_str(), v2.c_str());
        ok(has(v1, "\"ext\":\"aprs\"") && has(v1, "\"held\":true") && has(v1, "\"mine\":true"), "listener 1 is told it is theirs and held");
        ok(has(v2, "\"ext\":\"aprs\"") && has(v2, "\"held\":false") && has(v2, "\"mine\":false") && has(v2, "\"params\":{\"baud_rate\":1200}"),
           "listener 2 is told what runs, with the parameters to join it");
        ok(room.started(R::Text, s2, "sess-2", "aprs", "{\"baud_rate\":1200}", K, true), "listener 2 joins: the room changed");
        ok(has(room.listJson("sess-2", K, ""), "\"held\":true") && has(room.listJson("sess-2", K, ""), "\"n\":2"), "…and now holds it (2 holders)");
        bool ch = false;
        ok(room.release(R::Text, s2, K, &ch) == SharedDecoderRoom::Release::Kept && ch, "listener 2 closes: KEPT for listener 1");
        ok(has(room.listJson("sess-2", K, ""), "\"held\":false"), "listener 2 no longer holds it — still listed for them");
        ok(room.release(R::Text, s3, K) == SharedDecoderRoom::Release::Kept, "a socket that never held it cannot stop it");
        ok(room.release(R::Text, s1, K) == SharedDecoderRoom::Release::Last, "listener 1 closes: the LAST — stopped");
        ok(room.listJson("sess-1", K, "") == "{\"type\":\"dec_shared\",\"list\":[]}", "the room is told: nothing runs");
    }
    {
        R room;
        const std::string K1 = "fsk|a", K2 = "navtex|b";
        room.started(R::Text, 0x10, "a", "fsk", "{}", K1, false);
        room.started(R::Text, 0x20, "b", "fsk", "{}", K1, true);
        ok(room.started(R::Text, 0x20, "b", "navtex", "{}", K2, false), "listener b opens NAVTEX: it replaces the room's RTTY");
        const std::string va = room.listJson("a", K2, "");
        ok(has(va, "\"ext\":\"navtex\"") && has(va, "\"held\":false") && room.holderCount(R::Text) == 1, "…a is told and no longer holds anything");
        ok(room.by(R::Text) == "b", "the admin credit is b's");
        room.started(R::Text, 0x10, "a", "navtex", "{}", K2, true);
        ok(room.release(R::Text, 0x20, K2) == SharedDecoderRoom::Release::Kept && room.by(R::Text) == "a", "b leaves: the credit moves to a, who still holds it");
        // the host stopped it some other way (a hold expired, a whole-radio mode): the book is not believed
        ok(room.listJson("a", "", "") == "{\"type\":\"dec_shared\",\"list\":[]}", "a decoder the host no longer runs is never listed");
        ok(room.release(R::Text, 0x10, "") == SharedDecoderRoom::Release::Untracked, "…and a release then is Untracked (the caller's old rule)");
        ok(room.key(R::Text).empty(), "…and the stale book is forgotten");
    }
    {
        R room;
        const std::string K = "lrpt|sat";
        room.started(R::Text, R::kSatHolder, "", "lrpt", "{\"symbol_rate\":72000}", K, false, true);
        ok(room.listJson("x", K, "") == "{\"type\":\"dec_shared\",\"list\":[]}", "a SATELLITE pick's decoder is not listed (the pick tells the room)");
        room.started(R::Text, 0x99, "x", "lrpt", "{\"symbol_rate\":72000}", K, true);
        ok(room.release(R::Text, 0x99, K) == SharedDecoderRoom::Release::Kept, "a listener closing the pick's LRPT box leaves the pick's decoder running");
        ok(room.clear(R::Text) && room.key(R::Text).empty(), "the pick dropped: cleared");
    }
    {
        R room;
        room.started(R::Spots, 0x10, "a", "spots", "{\"scope\":\"band\"}", "spots|band", false);
        room.started(R::Text, 0x10, "a", "fsk", "{}", "fsk|x", false);
        const std::string v = room.listJson("b", "fsk|x", "spots|band");
        ok(has(v, "\"slot\":\"decoder\"") && has(v, "\"slot\":\"spots\"") && has(v, "\"scope\":\"band\""), "the spots run is listed beside the decoder");
        ok(room.release(R::Spots, 0x10, "spots|band") == SharedDecoderRoom::Release::Last && has(room.listJson("b", "fsk|x", ""), "fsk"),
           "the spots end on their own; the decoder is untouched");
        room.started(R::Text, 0x77, "p", "paging", "{}", "paging|", false);
        ok(!has(room.listJson("b", "paging|", ""), "paging"), "PAGING is never listed");
    }
    std::printf("── one set of settings for the room ──\n");
    {
        R room;
        room.started(R::Text, 0x10, "a", "fsk", "{\"shift\":170}", "fsk|170", false);
        room.started(R::Text, 0x20, "b", "fsk", "{\"shift\":170}", "fsk|170", true);
        ok(room.started(R::Text, 0x20, "b", "fsk", "{\"shift\":850}", "fsk|850", false), "b (a holder) changes the shift: the room changed");
        ok(room.holderCount(R::Text) == 2 && room.key(R::Text) == "fsk|850", "…ONE run with the new settings, and BOTH still hold it");
        const std::string va = room.listJson("a", "fsk|850", "");
        ok(has(va, "\"held\":true") && has(va, "\"shift\":850") && has(va, "\"mine\":false"), "a is told the new settings (held — its panel follows, nothing re-attached)");
        ok(room.by(R::Text) == "b", "the credit is b's (the last to set it)");
        ok(room.started(R::Text, 0x10, "a", "rttyskim", "{\"width\":48000}", "rttyskim|48", false) && room.holderCount(R::Text) == 2,
           "one signal → skimmer is the same box: still both");
        ok(room.started(R::Text, 0x10, "a", "navtex", "{}", "navtex|x", false) && room.holderCount(R::Text) == 1, "a different decoder replaces it (b must follow)");
        room.started(R::Text, 0x30, "c", "navtex", "{}", "navtex|y", false);
        ok(room.holderCount(R::Text) == 1 && room.by(R::Text) == "c", "a NON-holder's attach with other settings replaces the run (the others follow it)");
        R t;
        t.started(R::Text, 0x10, "a", "msf", "{}", "msf|", false);
        t.started(R::Text, 0x20, "b", "msf", "{}", "msf|", true);
        ok(t.started(R::Text, 0x20, "b", "time", "{\"station\":\"dcf77\"}", "time|dcf77", false) && t.holderCount(R::Text) == 2, "a time station change is the same box");
        R s;
        s.started(R::Text, R::kSatHolder, "", "lrpt", "{}", "lrpt|a", false, true);
        s.started(R::Text, 0x10, "a", "lrpt", "{}", "lrpt|a", true);
        s.started(R::Text, 0x10, "a", "lrpt", "{\"symbol_rate\":80000}", "lrpt|b", false);
        ok(!s.holds(R::Text, R::kSatHolder), "changing a SATELLITE pick's decoder makes it the listener's own (not the pick's)");
    }
    {
        R room;
        int added = 0;
        for (uintptr_t i = 0; i < 200; ++i) if (room.started(R::Text, 0x100 + i, "s" + std::to_string(i), "fsk", "{}", "fsk|k", i != 0)) added++;
        ok(room.holderCount(R::Text) == SharedDecoderRoom::kMaxHolders, "holders bounded at kMaxHolders");
    }
    std::printf("%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
