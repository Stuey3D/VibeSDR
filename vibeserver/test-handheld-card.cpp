// test-handheld-card.cpp — the R36S handheld's card: names, FAT safety, the one way anything is written, and the
// gallery's card layout (handheld_card.h, vibe_gallery.h CardLayout; docs/v12/R36S-HANDHELD-IMAGE.md §0.1 "The card").
//
// ★ What this pins:
//   · NAMES: "YYYY-MM-DD HHMM - <kHz to 3 dp> kHz <MODE detail>.ext" in UTC; none of : \ / * ? " < > |, no control
//     characters, no trailing dot or space; a clash becomes " (2)", never an overwrite; folders [A-Z0-9-].
//   · ATOMIC WRITES: the name only ever points at a whole file — the temp is hidden, ends ".part", and a sweep removes
//     what a cut-off write left; a rewrite replaces whole.
//   · THE JSON READER: nested objects, escapes and \u surrogates, a value's RAW text kept exactly, junk refused.
//   · THE GALLERY ON THE CARD: an item lands at Gallery/NAVTEX/<name>.txt with the index NOT on the card; append and
//     replace are temp + rename (no ".part" left, no write in place); list / resolve / readItem work on the new names;
//     a name from the network with "..", a second '/', a lowercase folder or a FAT-illegal character is refused;
//     eviction removes the card file; with the layout OFF the store is exactly the server's flat directory.
#include "handheld_card.h"
#include "handheld_json.h"
#include "vibe_gallery.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/hhcard-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static void rmTree(const std::string& d) { std::string c = "rm -rf '" + d + "'"; if (std::system(c.c_str())) {} }
static bool exists(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; }
static std::string slurp(const std::string& p) { std::string s; hhcard::readFile(p, s); return s; }
static int countParts(const std::string& d) {
    std::string c = "find '" + d + "' -name '*.part' | wc -l";
    FILE* f = ::popen(c.c_str(), "r"); int n = 0; if (f) { if (std::fscanf(f, "%d", &n) != 1) n = -1; ::pclose(f); } return n;
}

int main() {
    std::printf("test-handheld-card — card names, FAT safety, atomic writes, the gallery's card layout\n");
    const int64_t t0 = 1793888580;   // 2026-11-05 14:23:00 UTC

    std::printf("── names ──\n");
    ok(hhcard::itemName(t0, 518000, "NAVTEX", "txt") == "2026-11-05 1423 - 518.000 kHz NAVTEX.txt", "a NAVTEX message: " + hhcard::itemName(t0, 518000, "NAVTEX", "txt"));
    ok(hhcard::itemName(t0, 100700000, "WFM stereo", "wav") == "2026-11-05 1423 - 100700.000 kHz WFM stereo.wav", "kHz to 3 dp at VHF");
    ok(hhcard::itemName(t0, 7074123.4, "USB", "wav") == "2026-11-05 1423 - 7074.123 kHz USB.wav", "a fractional kHz");
    ok(hhcard::itemName(t0, 0, "ADS-B", "json") == "2026-11-05 1423 - ADS-B.json", "no frequency: the stamp and the mode");
    const std::string nasty = hhcard::itemName(t0, 1e6, "a:b\\c/d*e?f\"g<h>i|j\x01k. ", "w/a:v");
    bool clean = true; for (char c : nasty) if (std::strchr(":\\/*?\"<>|", c) || (unsigned char)c < 0x20) clean = false;
    ok(clean && nasty.find(" .") == std::string::npos && nasty.back() != ' ', "every FAT-illegal character gone: \"" + nasty + "\"");
    ok(hhcard::fatSafe(" ..hidden name.. ") == "hidden name", "no leading dot (a hidden file) and no trailing dot or space");
    ok(hhcard::fatSafe("12:30") == "12.30", "a colon reads as a dot, not as nothing");
    ok(hhcard::fatSafe(std::string(300, 'x')).size() == 120, "capped");
    ok(hhcard::fatSafe("caf\xc3\xa9", 4) == "caf", "a cap never cuts a UTF-8 character in half");
    ok(hhcard::folderName("navtex") == "NAVTEX" && hhcard::folderName("DAB slides") == "DAB-SLIDES" && hhcard::folderName("") == "OTHER", "folders");
    ok(hhcard::serialFile("IC-7300 03001234") == "IC-7300_03001234" && hhcard::serialFile("../x") == "_.._x", "a serial as a file name");
    ok(!hhcard::safeSegment("../x") && !hhcard::safeSegment(".hidden") && !hhcard::safeSegment("a/b") && !hhcard::safeSegment("a:b")
       && hhcard::safeSegment("2026-11-05 1423 - 518.000 kHz NAVTEX (2).txt"), "safeSegment: ours yes, traversal / hidden / separators no");

    std::printf("── atomic writes ──\n");
    {
        const std::string d = tmpDir("atomic");
        ok(hhcard::atomicWrite(d + "/a.txt", "one") && slurp(d + "/a.txt") == "one", "written whole");
        ok(hhcard::atomicWrite(d + "/a.txt", "two two") && slurp(d + "/a.txt") == "two two", "rewritten whole");
        ok(countParts(d) == 0, "no temp left behind");
        ok(hhcard::uniqueIn(d, "a.txt") == "a (2).txt", "a clash is \" (2)\"");
        hhcard::atomicWrite(d + "/a (2).txt", "x");
        ok(hhcard::uniqueIn(d, "a.txt") == "a (3).txt", "…then \" (3)\"");
        // a write cut off: the temp exists, the real name does not
        const std::string cut = hhcard::tempFor(d + "/sub/cut.wav");
        hhcard::ensureDir(d + "/sub");
        { FILE* f = std::fopen(cut.c_str(), "w"); std::fputs("half", f); std::fclose(f); }
        ok(cut.find("/sub/.cut.wav.") != std::string::npos && cut.size() > 5 && cut.compare(cut.size() - 5, 5, ".part") == 0, "the temp is hidden and ends .part: " + cut);
        ok(!exists(d + "/sub/cut.wav"), "a cut-off write never shows under its real name");
        ok(hhcard::sweepParts(d) == 1 && !exists(cut), "the start-up sweep removes it");
        ok(hhcard::listFiles(d).size() == 2, "listFiles: plain files only, no dot files, no folders");
        rmTree(d);
    }

    std::printf("── the JSON reader ──\n");
    {
        hhjson::Value v;
        const std::string src = "{\"serial\":\"A\\u00e9\\ud83d\\ude00\",\"prefs\":{\"tuned\":{\"x\":{\"hz\":7074000.5}},\"a\":[1,2,{\"b\":null}]},\"t\":true}";
        ok(hhjson::parse(src, v), "parses a nested object");
        ok(v.str("serial") == "A\xc3\xa9\xf0\x9f\x98\x80", "escapes and a surrogate pair decode to UTF-8");
        ok(v.get("prefs") && v.get("prefs")->raw == "{\"tuned\":{\"x\":{\"hz\":7074000.5}},\"a\":[1,2,{\"b\":null}]}", "a value's RAW text is kept exactly");
        ok(v.boolean("t"), "booleans");
        ok(!hhjson::parse("{\"a\":1} junk", v) && !hhjson::parse("{\"a\":}", v) && !hhjson::parse("[1,2", v), "junk refused");
        std::string deep; for (int i = 0; i < 100; i++) deep += "["; for (int i = 0; i < 100; i++) deep += "]";
        ok(!hhjson::parse(deep, v), "nesting past 32 refused");
        ok(hhjson::quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"", "quote() escapes");
    }

    std::printf("── the gallery on the card ──\n");
    {
        const std::string idx = tmpDir("idx"), cardRoot = tmpDir("card");
        const std::string files = cardRoot + "/Gallery";
        vibe::gallery::cardLayout().files = files;
        vibe::gallery::cardLayout().on = true;
        vibe::gallery::Store g;
        g.setDir(idx);
        g.setLimitBytes(1u << 20);
        vibe::gallery::Meta m; m.kind = "navtex"; m.origin = "live"; m.freqHz = 518000; m.mode = "usb"; m.decoder = "navtex";
        m.utc = t0; m.title = "Station G";
        const std::string n1 = g.add(m, "txt", "ZCZC GA01\nhello\nNNNN\n");
        ok(n1 == "NAVTEX/2026-11-05 1423 - 518.000 kHz NAVTEX.txt", "a NAVTEX message's name: " + n1);
        ok(exists(files + "/" + n1) && slurp(files + "/" + n1).find("hello") != std::string::npos, "its file is on the card");
        ok(exists(idx + "/index.jsonl") && !exists(files + "/index.jsonl"), "the index is in the data dir, NOT on the card");
        const std::string n2 = g.add(m, "txt", "second");
        ok(n2 == "NAVTEX/2026-11-05 1423 - 518.000 kHz NAVTEX (2).txt", "the same minute twice: \" (2)\", never an overwrite");
        vibe::gallery::Meta r = m; r.kind = "rtty"; r.decoder = "rtty"; r.freqHz = 147300; r.profile = "Profile 1";
        const std::string run = g.create(r, "txt", "RYRY ");
        ok(run == "RTTY/2026-11-05 1423 - 147.300 kHz RTTY Profile 1.txt", "a run, in its own folder: " + run);
        ok(g.append(run, "more text") && slurp(files + "/" + run) == "RYRY more text", "an append is the whole file renamed over itself");
        ok(countParts(cardRoot) == 0, "no .part left by add / create / append");
        ok(g.replace(n2, m, "replaced") && slurp(files + "/" + n2) == "replaced", "a replace is whole");
        ok(countParts(cardRoot) == 0, "…and leaves no .part");
        std::string path, ctype, body;
        ok(g.resolve(n1, path, ctype) && path == files + "/" + n1 && ctype.rfind("text/plain", 0) == 0, "resolve() maps the name to the card");
        ok(g.readItem(run, body) && body == "RYRY more text", "readItem()");
        const std::string list = g.listJson("", 50);
        ok(list.find("NAVTEX/2026-11-05 1423 - 518.000 kHz NAVTEX.txt") != std::string::npos && list.find("RTTY/") != std::string::npos, "listed");
        for (const char* bad : { "../x.txt", "NAVTEX/../../etc", "NAVTEX/a/b.txt", "navtex/a.txt", "NAVTEX/a:b.txt", "NAVTEX/.hidden",
                                 "a.txt", "/NAVTEX/a.txt" })
            ok(!vibe::gallery::Store::safeName(bad) && !g.resolve(bad, path, ctype), std::string("refused: ") + bad);
        // eviction takes the card file
        g.setLimitBytes(1);
        vibe::gallery::Meta w = m; w.kind = "wefax"; w.decoder = "wefax"; w.freqHz = 4610000; w.utc = t0 + 60;
        const std::string pic = g.add(w, "png", std::string(5000, 'p'));
        ok(!pic.empty() && !exists(files + "/" + n1), "over the limit the oldest card file is removed");
        rmTree(idx); rmTree(cardRoot);
        vibe::gallery::cardLayout().on = false;
        vibe::gallery::cardLayout().files.clear();
    }
    {
        std::printf("── the layout OFF: a server's flat gallery, unchanged ──\n");
        const std::string d = tmpDir("flat");
        vibe::gallery::Store g; g.setDir(d); g.setLimitBytes(1u << 20);
        vibe::gallery::Meta m; m.kind = "navtex"; m.origin = "live"; m.freqHz = 518000; m.utc = t0;
        const std::string n = g.add(m, "txt", "x");
        ok(n.rfind("20261105-142300-navtex-518k0-", 0) == 0 && n.find('/') == std::string::npos && exists(d + "/" + n), "the server's own name, beside the index: " + n);
        ok(!vibe::gallery::Store::safeName("NAVTEX/2026-11-05 1423 - 518.000 kHz NAVTEX.txt"), "a card name is not a server name");
        rmTree(d);
    }

    std::printf(fails ? "test-handheld-card: %d FAILED\n" : "test-handheld-card: all passed\n", fails);
    return fails ? 1 : 0;
}
