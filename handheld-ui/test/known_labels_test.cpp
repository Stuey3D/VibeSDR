// The handheld SIGNAL LABELS port's golden test: replays known_labels_inputs.json through src/known_labels.h and requires
// gen_known_labels_golden.ts's lines (the app's src/utils/knownSignals.ts) exactly — the curated table, EiBi's utility
// rows at four times of day, the receiver's bookmarks, the merged list and seven layouts. Then the R36S's own rules:
// the cap (HH_LABEL_MAX), no label over the needle or the dB axis, no two labels overlapping, and the layout cache.
// Silent: no GL drawing, no audio.   handheld-ui/test/run_known_labels.sh
#include "../src/json.h"
#include "../src/known_labels.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hh;

static std::string jsNum(double v) {
    char b[48];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}
static std::string sigLine(const char* tag, const KnownSig& s) {
    return std::string(tag) + " " + jsNum(s.hz) + " " + jsNum(s.lo) + " " + jsNum(s.hi) + " " + jsNum(s.dialHz) + " " + s.mode + " " +
           std::to_string(s.prio) + " " + (s.dec.empty() ? "-" : s.dec) + " " + s.src + " |" + s.label;
}
static std::string readAll(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t cps(const std::string& s) { return kl::cpLen(s); }

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: known_labels_test <inputs.json> <golden.txt>\n"); return 2; }
    const Json in = Json::parse(readAll(argv[1]));
    if (in.isNull()) { std::fprintf(stderr, "inputs do not parse\n"); return 2; }
    std::vector<std::string> got;

    const auto& table = knownTable();
    got.push_back("T " + std::to_string(table.size()));
    for (auto& s : table) got.push_back(sigLine("K", s));
    std::vector<EibiIn> rows;
    for (auto& r : in["eibi"].a) rows.push_back({r["name"].str(), r["comment"].str(), r["frequency"].num()});
    for (auto& m : in["nowMins"].a) {
        const auto e = eibiKnownSignals(rows, (int)m.num());
        got.push_back("E@" + jsNum(m.num()) + " " + std::to_string(e.size()));
        for (auto& s : e) got.push_back(sigLine("E", s));
    }
    std::vector<BookmarkIn> bms;
    for (auto& b : in["bookmarks"].a) {
        BookmarkIn o;
        o.name = b["name"].str(); o.mode = b["mode"].str(); o.frequency = b["frequency"].num();
        o.manual = b["manual"].kind == Json::Bool ? (b["manual"].b ? 1 : 0) : -1;
        o.nameSrc = b["nameSrc"].kind == Json::Num ? (int)b["nameSrc"].n : -1;
        o.sid = b["sid"].kind == Json::Num ? (int)b["sid"].n : -1;
        o.repeater = b["repeater"].boolean();
        bms.push_back(o);
    }
    const auto bm = bookmarkSignals(bms);
    got.push_back("B " + std::to_string(bm.size()));
    for (auto& s : bm) got.push_back(sigLine("B", s));
    const auto list = knownSignalList(eibiKnownSignals(rows, (int)in["layoutNowMin"].num()), bm);
    got.push_back("A " + std::to_string(list.size()));
    auto measure = [](const std::string& t) { return (double)(cps(t) * 7 + 6); };
    for (auto& v : in["views"].a) {
        std::vector<BlockedBox> bl;
        for (auto& b : v["blocked"].a) bl.push_back({(int)b["row"].num(), b["x0"].num(), b["x1"].num()});
        const double dial = v["dial"].kind == Json::Num ? v["dial"].n : NAN;
        const auto p = labelLayout(list, v["lo"].num(), v["span"].num(), v["width"].num(), measure, dial, bl, v["pad"].num(3));
        got.push_back("V " + v["name"].str() + " " + std::to_string(p.size()));
        for (auto& q : p) {
            char b[96];
            std::snprintf(b, sizeof b, "P %d %.3f %.3f %s %s |", q.row, q.x, q.lx, jsNum(q.w).c_str(), jsNum(q.sig->hz).c_str());
            got.push_back(b + q.sig->label);
        }
    }

    // ── against the app ──
    std::vector<std::string> want;
    { std::istringstream ss(readAll(argv[2])); std::string l; while (std::getline(ss, l)) want.push_back(l); }
    int bad = 0;
    const size_t n = std::max(got.size(), want.size());
    for (size_t i = 0; i < n; i++) {
        const std::string g = i < got.size() ? got[i] : "<none>", w = i < want.size() ? want[i] : "<none>";
        if (g != w) { if (bad < 12) std::printf("  line %zu\n    app:  %s\n    port: %s\n", i + 1, w.c_str(), g.c_str()); bad++; }
    }
    std::printf("  golden: %zu lines, %d differ\n", want.size(), bad);

    // ── the R36S's own rules (its cap, the needle, the axis, no overlaps) on a crowded synthetic view ──
    int fails = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { std::printf("  FAIL %s\n", what); fails++; } };
    {
        // 40 labels across 640 px, all one priority, the dial in the middle with a signal right on it
        std::vector<KnownSig> crowd;
        for (int i = 0; i < 40; i++) { KnownSig s; s.hz = 10e6 + i * 2500; s.label = "SIG " + std::to_string(i); s.prio = PRIO_TABLE; s.src = "table"; crowd.push_back(s); }
        const double lo = 10e6, span = 100e3, W = 640, dial = 10e6 + 20 * 2500, nx = (dial - lo) / span * W;
        std::vector<BlockedBox> bl = {{0, 0, KnownLabels::kAxisW}, {1, 0, KnownLabels::kAxisW}, {0, nx - 1, nx + 1}, {1, nx - 1, nx + 1}};
        const auto p = labelLayout(crowd, lo, span, W, measure, dial, bl, KnownLabels::kMarkPad, LABEL_ROWS, KnownLabels::kGap, HH_LABEL_MAX, 1);
        check((int)p.size() == HH_LABEL_MAX, "the cap: exactly HH_LABEL_MAX placed from 40");
        check(!p.empty() && p[0].sig->hz == dial, "the station you are on is placed first");
        for (size_t i = 0; i < p.size(); i++) {
            check(!(p[i].lx < nx + 1 + 1 && p[i].lx + p[i].w > nx - 1 - 1), "no box over the needle");
            check(p[i].lx >= KnownLabels::kAxisW + 1, "no box over the dB axis");
            check(p[i].lx >= 0 && p[i].lx + p[i].w <= W, "inside the spectrum");
            for (size_t j = 0; j < i; j++)
                if (p[i].row == p[j].row) check(p[i].lx + p[i].w + KnownLabels::kGap <= p[j].lx || p[i].lx >= p[j].lx + p[j].w + KnownLabels::kGap, "no two boxes overlap");
        }
        // collision culling: a higher priority beats a nearer one when they collide
        std::vector<KnownSig> two(2);
        two[0].hz = 10.050e6; two[0].label = "NEAR EiBi"; two[0].prio = PRIO_EIBI;
        two[1].hz = 10.0505e6; two[1].label = "SAFETY"; two[1].prio = PRIO_SAFETY;
        auto q = labelLayout(two, lo, span, W, measure, 10.050e6, {}, 3, 1, 4, HH_LABEL_MAX, 1);
        check(q.size() == 1 && q[0].sig->label == "SAFETY", "one row: the higher priority wins the collision");
        q = labelLayout(two, lo, span, W, measure, 10.050e6, {}, 3, 2, 4, HH_LABEL_MAX, 1);
        check(q.size() == 2 && q[0].row == 0 && q[1].row == 1, "two rows: the loser drops to the second row");
    }
    std::printf("  R36S rules: %s\n", fails ? "FAILED" : "ok (cap, needle, axis, overlaps, priority)");

    // ── cost: building the list from a full-size schedule, and one layout ──
    {
        std::vector<EibiIn> big;
        for (int i = 0; i < 9000; i++) {
            EibiIn r;
            r.frequency = 150e3 + (i * 2917) % 29850000;
            r.name = i % 40 == 0 ? "Station " + std::to_string(i) + " Meteo Fax" : "Radio Broadcaster " + std::to_string(i);
            r.comment = i % 60 == 0 ? "0000-2400 · -TY" : "0600-0800 · E,F";
            big.push_back(r);
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto e = eibiKnownSignals(big, 750);
        const auto l = knownSignalList(e, bm);
        const auto t1 = std::chrono::steady_clock::now();
        int reps = 2000; size_t placed = 0;
        for (int i = 0; i < reps; i++) placed += labelLayout(l, 7.0e6 + i, 200e3, 640, measure, 7.074e6, {}, 3, 2, 4, HH_LABEL_MAX, 1).size();
        const auto t2 = std::chrono::steady_clock::now();
        std::printf("  cost: list from 9000 EiBi rows %.2f ms (%zu utility labels) · one layout %.1f us (%zu placed)\n",
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), e.size(),
                    std::chrono::duration<double, std::micro>(t2 - t1).count() / reps, placed / reps);
    }
    return bad || fails ? 1 : 0;
}
