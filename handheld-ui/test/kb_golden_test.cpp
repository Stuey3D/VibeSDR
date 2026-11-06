// kb_golden_test.cpp — replays gen_kb_golden.ts's inputs (the app's own kbLayouts.ts / controllerKeyboard.ts, and
// padKeyboard.ts's session logic) through the C++ port (src/kb) and compares every output line exactly: every
// layout's keys and every kbMove, the number parse / format / init, the bare reducer, and the full session state
// after every input. Build/run: handheld-ui/test/run_kb_golden.sh
#include "../src/kb/controller_keyboard.h"
#include "../src/kb/kb_layouts.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace kb;

static std::string hexOf(const std::string& s) {
  static const char* H = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) { o += H[c >> 4]; o += H[c & 15]; }
  return o;
}
static std::string unhex(const std::string& h) {
  std::string o;
  for (size_t i = 0; i + 1 < h.size(); i += 2) o += (char)std::stoi(h.substr(i, 2), nullptr, 16);
  return o;
}
static std::string bitsOf(double d) {
  uint64_t u; std::memcpy(&u, &d, 8);
  char b[17]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)u);
  return b;
}
static double fromBits(const std::string& h) {
  uint64_t u = std::stoull(h, nullptr, 16); double d; std::memcpy(&d, &u, 8); return d;
}
static std::vector<std::string> split(const std::string& s, char sep = ' ') {
  std::vector<std::string> o; std::string cur;
  for (char c : s) { if (c == sep) { o.push_back(cur); cur.clear(); } else cur += c; }
  o.push_back(cur);
  return o;
}
static std::optional<int> optInt(const std::string& s) { if (s == "-") return std::nullopt; return std::stoi(s); }
static std::optional<KbUnit> optUnit(const std::string& s) {
  if (s == "hz") return KbUnit::hz; if (s == "khz") return KbUnit::khz; if (s == "mhz") return KbUnit::mhz; return std::nullopt;
}

static std::string keysSer(const std::vector<KbKey>& keys) {
  std::string o;
  for (size_t i = 0; i < keys.size(); i++) {
    const KbKey& k = keys[i];
    if (i) o += ';';
    o += k.id + "," + hexOf(k.label) + "," + name(k.act.t) + "," + (k.act.t == KeyActT::ch ? hexOf(k.act.ch) : "") + "," +
         bitsOf(k.x) + "," + bitsOf(k.y) + "," + bitsOf(k.w) + "," + bitsOf(k.h);
  }
  return o;
}
static std::string stSer(const KbState& s) {
  return "v=" + hexOf(s.valueUtf8()) + " c=" + std::to_string(s.caret) + " L=" + name(s.layer) + " m=" + name(s.mode) +
         " u=" + (s.unit ? name(*s.unit) : "-") + " r=" + (s.reveal ? "1" : "0");
}

// The test's field commit — the same rule as gen_kb_golden.ts's commit().
static std::string commitRule(const KbState& st) {
  if (st.mode == KbMode::frequency) {
    const KbUnit u = st.unit ? *st.unit : KbUnit::mhz;
    const double hz = kbParseNumber(st.valueUtf8()) * (u == KbUnit::mhz ? 1e6 : u == KbUnit::khz ? 1e3 : 1);
    if (!std::isfinite(hz) || hz < 10e3 || hz > 2e9)
      return "Out of range: " + st.valueUtf8() + " " + name(u) + " \xE2\x80\x94 10 kHz\xE2\x80\x93" "2 GHz";
    return "";
  }
  if (st.mode == KbMode::pin) return st.value.size() < 4 ? "PIN too short" : "";
  return st.valueUtf8() == "bad" ? "Nope \xE2\x9C\x97" : "";
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "kb_golden.txt";
  std::ifstream in(path);
  if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
  std::map<std::string, long> cases, fails;
  std::string firstFail;
  long lineNo = 0;
  auto check = [&](const char* kind, const std::string& got, const std::string& want) {
    cases[kind]++;
    if (got != want) {
      fails[kind]++;
      if (firstFail.empty()) firstFail = "line " + std::to_string(lineNo) + " (" + kind + ")\n  C++: " + got + "\n  TS:  " + want;
    }
  };

  Session sess;
  bool everOpened = false;
  std::string wantKeys;
  std::vector<KbKey> ksKeys;
  KbState red;
  std::string line;
  bool redReplaced = false;
  Outcome lastOut = Outcome::none;
  bool pendingState = false;

  auto sessionState = [&](Outcome out) {
    const auto& ks = sess.keys();
    std::string lit;
    for (int i = 0; i < (int)ks.size(); i++) lit += sess.keyLit(i) ? '1' : '0';
    const KbKey* f = sess.focusedKey();
    return std::string("S open=") + (sess.isOpen() ? "1" : "0") + " out=" + name(out) + " " + stSer(sess.state()) +
           " f=" + std::to_string(sess.focus()) + " fid=" + (f ? f->id : "") + " err=" + hexOf(sess.err()) +
           " disp=" + hexOf(sess.displayText()) + " bef=" + hexOf(sess.displayBefore()) + " aft=" + hexOf(sess.displayAfter()) +
           " ut=" + sess.unitText() + " lit=" + lit + " orig=" + hexOf(sess.origValue()) + " lay=" + name(sess.layout());
  };

  while (std::getline(in, line)) {
    lineNo++;
    if (line.empty()) continue;
    const size_t sp = line.find(' ');
    const std::string tag = line.substr(0, sp);
    const std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
    const auto t = split(rest);
    if (tag == "KS") {
      KbKeysOpts o;
      o.unitLocked = t[2] == "1";
      if (t[3] != "-") o.unitLabel = unhex(t[3]);
      if (t[4] != "-") o.revealLabel = unhex(t[4]);
      ksKeys = kbKeys(*modeFromName(t[0]), *layerFromName(t[1]), o);
      check("layout", keysSer(ksKeys), t[5]);
    } else if (tag == "MV") {
      check("kbMove", std::to_string(kbMove(ksKeys, std::stoi(t[0]), *dirFromName(t[1]))), t[2]);
    } else if (tag == "SK") {
      std::string o;
      for (KbMode m : {KbMode::frequency, KbMode::pin, KbMode::search, KbMode::text, KbMode::password, KbMode::url}) {
        if (!o.empty()) o += ',';
        o += kbStartKey(m) + ":" + name(layoutFor(m));
      }
      check("startKey", o, rest);
    } else if (tag == "PN") {
      check("parseNumber", bitsOf(kbParseNumber(unhex(t[0]))), t[1]);
    } else if (tag == "FN") {
      check("formatNumber", hexOf(kbFormatNumber(fromBits(t[0]), std::stoi(t[1]))), t[2]);
    } else if (tag == "IN") {
      const KbState s = kbInit(*modeFromName(t[0]), unhex(t[1]), optUnit(t[2]), optInt(t[3]));
      check("kbInit", stSer(s), rest.substr(rest.find(' ', rest.find(' ', rest.find(' ', rest.find(' ') + 1) + 1) + 1) + 1));
    } else if (tag == "R") {
      red = kbInit(*modeFromName(t[0]), unhex(t[1]), optUnit(t[2]), optInt(t[3]));
    } else if (tag == "RA") {
      KbAction a;
      static const std::map<std::string, KbActT> M = {
        {"ch", KbActT::ch}, {"bs", KbActT::bs}, {"left", KbActT::left}, {"right", KbActT::right}, {"shift", KbActT::shift},
        {"symbols", KbActT::symbols}, {"accents", KbActT::accents}, {"abc", KbActT::abc}, {"space", KbActT::space},
        {"clear", KbActT::clear}, {"unit", KbActT::unit}, {"reveal", KbActT::reveal}};
      a.t = M.at(t[0]);
      if (a.t == KbActT::ch) a.ch = unhex(t[1]);
      red = kbReduce(red, a, &redReplaced);
    } else if (tag == "RS") {
      check("reducer", std::string(redReplaced ? "1" : "0") + " " + stSer(red) + " d=" + hexOf(kbDisplay(red)), rest);
    } else if (tag == "O") {
      SessionOpts o;
      o.mode = *modeFromName(t[0]);
      o.unit = optUnit(t[1]);
      o.caret = optInt(t[2]);
      o.unitSettable = t[3] == "1";
      o.secure = t[4] == "1";
      o.value = unhex(t[5]);
      sess.open(o);
      everOpened = true;
      lastOut = Outcome::none;
      pendingState = true;
    } else if (tag == "A") {
      Outcome out = Outcome::none;
      if (t[0] == "move") out = sess.move(*dirFromName(t[1]));
      else if (t[0] == "press") out = sess.press(commitRule);
      else if (t[0] == "back") out = sess.back();
      else if (t[0] == "submit") out = sess.submit(commitRule);
      else if (t[0] == "edit") {
        static const std::map<std::string, EditAct> E = {{"bs", EditAct::bs}, {"space", EditAct::space}, {"shift", EditAct::shift},
          {"symbols", EditAct::symbols}, {"unit", EditAct::unit}, {"left", EditAct::left}, {"right", EditAct::right}};
        out = sess.edit(E.at(t[1]));
      }
      lastOut = out;
      pendingState = true;
    } else if (tag == "K") {
      wantKeys = rest;
    } else if (tag == "S") {
      if (!everOpened || !pendingState) { std::fprintf(stderr, "line %ld: S with no input\n", lineNo); return 2; }
      check("session keys", keysSer(sess.keys()), wantKeys);
      check("session state", sessionState(lastOut), line);
      pendingState = false;
    } else {
      std::fprintf(stderr, "line %ld: unknown tag %s\n", lineNo, tag.c_str());
      return 2;
    }
  }

  long total = 0, totalFail = 0;
  for (auto& [k, n] : cases) {
    std::printf("  %-15s %7ld compared, %ld failed\n", k.c_str(), n, fails[k]);
    total += n; totalFail += fails[k];
  }
  if (totalFail) {
    std::printf("FAIL: %ld of %ld differ — first: %s\n", totalFail, total, firstFail.c_str());
    return 1;
  }
  std::printf("PASS: %ld cases, all identical to the TS\n", total);
  return 0;
}
