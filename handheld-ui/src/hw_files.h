// hw_files.h — MENU → R36S HARDWARE → FILES: IMPORT, EXPORT BOOKMARKS, EXPORT DIAGNOSTICS (handheld.ts runImport /
// exportBookmarks / exportDiagnostics), on worker threads.
// ★ IMPORT parses each file the way the app does (src/services/userBookmarks.ts parseBookmarksAny: JSON — a plain array
//   or {bookmarks:[…]} — else UberSDR's flat YAML), then sends the rows to the RECEIVER's list (POST /bookmarks/import,
//   batched under 55 KB as main.ts does) and tells the supervisor how it went (POST /handheld/import/result — the file
//   moves to import/done/ with a .result.txt).
// ★ EXPORT BOOKMARKS writes the RECEIVER's list (GET /bookmarks) in the app's UberSDR-importable JSON
//   (exportBookmarksJSON). The web page exports its browser-local list instead (inventory §4c ⚠️); the native UI has no
//   such list, and the receiver's is the one IMPORT fills.
#pragma once
#include "hw_state.h"

#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace hh {

struct HwBookmark { std::string name, mode; double frequency = 0; bool hasBw = false; double bwLo = 0, bwHi = 0; };

inline std::string hwTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
inline std::string hwLower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

/** parseBookmarksJSON. */
inline bool hwParseBookmarksJson(const std::string& text, std::vector<HwBookmark>& out) {
    const Json d = Json::parse(text);
    if (d.isNull()) return false;
    const Json& arr = d.kind == Json::Arr ? d : d["bookmarks"];
    if (arr.kind != Json::Arr) return false;
    auto pick = [](const Json& b, std::initializer_list<const char*> ks) -> const Json& {
        static const Json none;
        for (const char* k : ks) if (b.has(k) && !b[k].isNull()) return b[k];
        return none;
    };
    for (auto& b : arr.a) {
        if (b.kind != Json::Obj) continue;
        const Json& fj = pick(b, {"frequency", "frequencyHz", "freq", "f"});
        double f = fj.kind == Json::Num ? fj.n : fj.kind == Json::Str ? std::atof(fj.s.c_str()) : 0;
        if (!(f > 0) || !std::isfinite(f)) continue;
        if (f < 30000) f *= 1000;                       // the kHz heuristic (skin normBm parity)
        HwBookmark o;
        o.frequency = std::round(f);
        const Json& mj = pick(b, {"mode", "modulation", "m"});
        o.mode = hwLower(mj.kind == Json::Str ? mj.s : "usb");
        const Json& nj = pick(b, {"name", "label", "title"});
        o.name = nj.kind == Json::Str ? nj.s : "Unnamed";
        const Json& lo = b.has("bandwidth_low") ? b["bandwidth_low"] : b["bandwidthLow"];
        const Json& hi = b.has("bandwidth_high") ? b["bandwidth_high"] : b["bandwidthHigh"];
        if (lo.kind == Json::Num && hi.kind == Json::Num) { o.hasBw = true; o.bwLo = lo.n; o.bwHi = hi.n; }
        out.push_back(o);
    }
    return true;
}

/** parseBookmarksYAML: only UberSDR's flat shape ("- name: …" items with key: value lines). */
inline void hwParseBookmarksYaml(const std::string& yaml, std::vector<HwBookmark>& out) {
    auto splitKV = [](const std::string& s, std::string& k, std::string& v) {
        const size_t i = s.find(':');
        if (i == std::string::npos) return false;
        k = hwTrim(s.substr(0, i));
        v = hwTrim(s.substr(i + 1));
        if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
            const size_t e = v.find(v[0], 1);
            v = e != std::string::npos ? v.substr(1, e - 1) : v.substr(1);
        } else {
            const size_t h = v.find(" #");
            if (h != std::string::npos) v = hwTrim(v.substr(0, h));
        }
        return true;
    };
    std::vector<std::map<std::string, std::string>> items;
    size_t p = 0;
    while (p <= yaml.size()) {
        size_t e = yaml.find('\n', p);
        if (e == std::string::npos) e = yaml.size();
        std::string line = yaml.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (auto& c : line) if (c == '\t') c = ' ';
        const std::string t = hwTrim(line);
        if (t.empty() || t[0] == '#') { if (e == yaml.size()) break; continue; }
        std::string k, v;
        if (t[0] == '-') {
            items.emplace_back();
            const std::string rest = hwTrim(t.substr(1));
            if (!rest.empty() && splitKV(rest, k, v)) items.back()[k] = v;
        } else if (!items.empty() && splitKV(t, k, v)) items.back()[k] = v;
        if (e == yaml.size()) break;
    }
    for (auto& b : items) {
        const std::string fs = b.count("frequency") ? b["frequency"] : b.count("freq") ? b["freq"] : "";
        double f = std::atof(fs.c_str());
        if (!(f > 0)) continue;
        if (f < 30000) f *= 1000;
        HwBookmark o;
        o.frequency = std::round(f);
        o.name = b.count("name") ? b["name"] : "Unnamed";
        o.mode = hwLower(b.count("mode") ? b["mode"] : "usb");
        if (b.count("bandwidth_low") && b.count("bandwidth_high") && !b["bandwidth_low"].empty() && !b["bandwidth_high"].empty()) {
            o.hasBw = true; o.bwLo = std::atof(b["bandwidth_low"].c_str()); o.bwHi = std::atof(b["bandwidth_high"].c_str());
        }
        out.push_back(o);
    }
}

/** parseBookmarksAny: JSON when it looks like JSON, else YAML (falling back to JSON). */
inline std::vector<HwBookmark> hwParseBookmarksAny(const std::string& text) {
    std::vector<HwBookmark> out;
    const std::string t = hwTrim(text);
    if (!t.empty() && (t[0] == '{' || t[0] == '[')) { hwParseBookmarksJson(text, out); return out; }
    hwParseBookmarksYaml(text, out);
    if (out.empty()) hwParseBookmarksJson(text, out);
    return out;
}

inline std::string hwBookmarkJson(const HwBookmark& b) {
    char f[40]; std::snprintf(f, sizeof f, "%.0f", b.frequency);
    std::string name = b.name;
    if (name.size() > 64) name.resize(64);
    std::string o = "{\"name\":" + jsonQuote(name) + ",\"frequency\":" + f;
    if (!b.mode.empty()) o += ",\"mode\":" + jsonQuote(b.mode);
    if (b.hasBw) { char w[64]; std::snprintf(w, sizeof w, ",\"bandwidth_low\":%.0f,\"bandwidth_high\":%.0f", b.bwLo, b.bwHi); o += w; }
    return o + "}";
}

/** host.importBookmarks (main.ts:3151): the receiver's list, in batches under its 60 KB ceiling. */
inline bool hwImportText(int port, const std::string& text, int& imported, std::string& message) {
    std::vector<HwBookmark> rows;
    for (auto& b : hwParseBookmarksAny(text)) if (!b.name.empty() && b.frequency > 0) rows.push_back(b);
    if (rows.empty()) { message = "No bookmarks found in that file"; return false; }
    imported = 0;
    std::string batch;
    auto send = [&]() -> bool {
        if (batch.empty()) return true;
        const HttpResult r = httpRequest(port, "POST", "/bookmarks/import", "[" + batch + "]", 15);
        const Json j = Json::parse(r.body);
        if (r.status != 200) {
            message = !j["error"].str().empty() ? j["error"].str() : "the receiver refused it (" + std::to_string(r.status) + ")";
            return false;
        }
        imported += (int)j["imported"].num(0);
        batch.clear();
        return true;
    };
    for (auto& b : rows) {
        const std::string o = hwBookmarkJson(b);
        if (batch.size() + o.size() + 3 > 55000 && !send()) return false;
        if (!batch.empty()) batch += ",";
        batch += o;
    }
    if (!send()) return false;
    message = "Imported " + std::to_string(imported) + " bookmark" + (imported == 1 ? "" : "s") + " into the receiver's list";
    return true;
}

/** runImport (handheld.ts): every file waiting in the card's import/. `announceNone`: say so when there is nothing. */
inline void hwRunImport(int port, bool announceNone) {
    {
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        if (h.importBusy) return;
        h.importBusy = true;
    }
    hwAsync([port, announceNone] {
        const HttpResult r = httpRequest(port, "GET", "/handheld/import", "", 10);
        const Json j = Json::parse(r.body);
        const Json& files = j["files"];
        if (files.kind != Json::Arr || files.a.empty()) {
            if (announceNone) hwToast("Nothing to import — put bookmark backups in the card's import folder.");
        } else {
            int total = 0, okFiles = 0;
            for (auto& f : files.a) {
                bool ok = false; int n = 0;
                std::string msg = "too big to import (over 1 MB)";
                if (!f["tooBig"].boolean() && f["text"].kind == Json::Str) ok = hwImportText(port, f["text"].s, n, msg);
                if (ok) { okFiles++; total += n; }
                httpRequest(port, "POST", "/handheld/import/result",
                            "{\"name\":" + jsonQuote(f["name"].str()) + ",\"ok\":" + (ok ? "true" : "false") + ",\"imported\":" +
                            std::to_string(n) + ",\"message\":" + jsonQuote(msg) + "}", 5);
            }
            const size_t nf = files.a.size();
            hwToast("Imported " + std::to_string(total) + " bookmark" + (total == 1 ? "" : "s") + " · " + std::to_string(okFiles) +
                    " of " + std::to_string(nf) + " file" + (nf == 1 ? "" : "s") + " — results in import/done/");
        }
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        h.importBusy = false;
    });
}

/** EXPORT BOOKMARKS: the receiver's list as the app's UberSDR-importable JSON (exportBookmarksJSON), to export/. */
inline void hwExportBookmarks(int port) {
    {
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        if (h.exportBusy) return;
        h.exportBusy = true;
    }
    hwAsync([port] {
        const HttpResult g = httpRequest(port, "GET", "/bookmarks", "", 10);
        const Json list = Json::parse(g.body);
        if (g.status != 200 || list.kind != Json::Arr) hwToast("Export failed — the receiver did not send its bookmarks");
        else {
            std::string out = "[";
            bool first = true;
            for (auto& b : list.a) {
                if (b.kind != Json::Obj || !(b["frequency"].num() > 0)) continue;
                char f[40]; std::snprintf(f, sizeof f, "%.0f", std::round(b["frequency"].num()));
                std::string mode = hwLower(b["mode"].str().empty() ? "usb" : b["mode"].str());
                std::string bwl = "null", bwh = "null";
                if (b["bandwidth_low"].kind == Json::Num) bwl = std::to_string((long long)b["bandwidth_low"].n);
                if (b["bandwidth_high"].kind == Json::Num) bwh = std::to_string((long long)b["bandwidth_high"].n);
                out += std::string(first ? "\n" : ",\n") + "  {\n    \"name\": " + jsonQuote(b["name"].str()) + ",\n    \"frequency\": " + f +
                       ",\n    \"mode\": " + jsonQuote(mode) + ",\n    \"group\": null,\n    \"comment\": null,\n    \"extension\": null,\n" +
                       "    \"bandwidth_low\": " + bwl + ",\n    \"bandwidth_high\": " + bwh + "\n  }";
                first = false;
            }
            out += first ? "]" : "\n]";
            const HttpResult r = httpRequest(port, "POST", "/handheld/export?kind=bookmarks", out, 10);
            const Json j = Json::parse(r.body);
            if (r.status == 0) hwToast("Export failed — the engine did not answer");
            else if (j["ok"].boolean()) hwToast("Bookmarks exported · " + j["name"].str());
            else hwToast("Export failed" + (j["error"].str().empty() ? std::string() : " — " + j["error"].str()));
        }
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        h.exportBusy = false;
    });
}

/** EXPORT DIAGNOSTICS: the supervisor writes the logs to the card's "Diagnostic logs/". */
inline void hwExportDiagnostics(int port) {
    {
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        if (h.diagBusy) return;
        h.diagBusy = true;
    }
    hwAsync([port] {
        const HttpResult r = httpRequest(port, "POST", "/handheld/diagnostics", "", 30);
        const Json j = Json::parse(r.body);
        if (r.status == 0) hwToast("Diagnostics failed — the engine did not answer");
        else if (j["ok"].boolean()) hwToast("Diagnostics saved · " + j["name"].str());
        else hwToast("Diagnostics failed" + (j["error"].str().empty() ? std::string() : " — " + j["error"].str()));
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        h.diagBusy = false;
    });
}

}  // namespace hh
