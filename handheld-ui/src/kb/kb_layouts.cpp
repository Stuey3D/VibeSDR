// kb_layouts.cpp — port of src/utils/kbLayouts.ts (+ pickNext from src/utils/gamepadNav.ts). See kb_layouts.h.
#include "kb_layouts.h"
#include <cmath>

namespace kb {

const char* const KB_SYSTEM_GLYPHS = "\xE2\x8C\xAB\xE2\x86\xB5\xE2\x87\xA7\xE2\x87\xAA";   // ⌫↵⇧⇪

namespace {

// UTF-8 → code points and back (the layouts' strings are all valid UTF-8, BMP).
std::vector<char32_t> cps(const std::string& s) {
  std::vector<char32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    char32_t v = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n && i + k < s.size(); k++) v = (v << 6) | ((unsigned char)s[i + k] & 0x3F);
    out.push_back(v);
    i += n;
  }
  return out;
}
std::string u8(char32_t c) {
  std::string o;
  if (c < 0x80) o += (char)c;
  else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
  else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
  return o;
}
// String.prototype.toUpperCase for the characters the layouts carry: ASCII and the Latin-1 lower-case letters.
// (ß is never passed: the TS keeps it as it is.)
char32_t upperCp(char32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
  return c;
}

const char* const LETTERS[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
const char* const SYMBOLS[3] = {"-/:;()$&@\"", ".,?!'#%*+", "=_\\|~<>[]"};
const char* const ACCENTS[3] = {"àáâäãåæçèé", "êëìíîïñòó", "ôöõøùúûüß"};

KeyAct chAct(const std::string& c) { return {KeyActT::ch, c}; }
KeyAct act(KeyActT t) { return {t, std::string()}; }

void numericKeys(std::vector<KbKey>& out, KbMode mode, const KbKeysOpts& o) {
  auto at = [&](const std::string& id, const std::string& label, KeyAct a, int col, int row) {
    KbKey k; k.id = id; k.label = label; k.act = std::move(a);
    k.x = col < 3 ? (double)col : 3.25; k.y = row; k.w = col < 3 ? 1 : 1.5; k.h = 1;
    out.push_back(std::move(k));
  };
  const char* const rows[3] = {"789", "456", "123"};
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++) { const std::string d(1, rows[row][col]); at(d, d, chAct(d), col, row); }
  if (mode == KbMode::frequency) at(".", ".", chAct("."), 0, 3);
  at("0", "0", chAct("0"), 1, 3);
  at("enter", "\xE2\x86\xB5", act(KeyActT::enter), 2, 3);
  at("bs", "\xE2\x8C\xAB", act(KeyActT::bs), 3, 0);
  if (mode == KbMode::frequency && !o.unitLocked) at("unit", o.unitLabel ? *o.unitLabel : "MHz", act(KeyActT::unit), 3, 1);
  at("clear", "Clear", act(KeyActT::clear), 3, 2);
  at("back", "Back", act(KeyActT::back), 3, 3);
}

void qwertyKeys(std::vector<KbKey>& out, KbMode mode, KbLayer layer, const KbKeysOpts& o) {
  auto put = [&](const std::string& id, const std::string& label, KeyAct a, double x, double y, double w = 1) {
    KbKey k; k.id = id; k.label = label; k.act = std::move(a); k.x = x; k.y = y; k.w = w; k.h = 1;
    out.push_back(std::move(k));
  };
  const bool upper = (layer == KbLayer::upper || layer == KbLayer::caps || layer == KbLayer::accentsUp) && mode != KbMode::url;
  const bool accents = layer == KbLayer::accents || layer == KbLayer::accentsUp;
  auto cased = [&](char32_t c) { return u8(upper ? upperCp(c) : c); };
  const std::string bsL = "\xE2\x8C\xAB";
  // Row 0: the digits, on every layer.
  const char* digits = "1234567890";
  for (int i = 0; i < 10; i++) { const std::string d(1, digits[i]); put("d" + d, d, chAct(d), i, 0); }
  if (layer == KbLayer::symbols) {
    { auto r = cps(SYMBOLS[0]); for (size_t i = 0; i < r.size(); i++) put("s0" + std::to_string(i), u8(r[i]), chAct(u8(r[i])), (double)i, 1); }
    { auto r = cps(SYMBOLS[1]); for (size_t i = 0; i < r.size(); i++) put("s1" + std::to_string(i), u8(r[i]), chAct(u8(r[i])), (double)i, 2); }
    put("bs", bsL, act(KeyActT::bs), 9, 2);
    put("abc", "ABC", act(KeyActT::abc), 0, 3);
    { auto r = cps(SYMBOLS[2]); for (size_t i = 0; i < r.size(); i++) put("s2" + std::to_string(i), u8(r[i]), chAct(u8(r[i])), (double)(i + 1), 3); }
  } else if (accents) {
    { auto r = cps(ACCENTS[0]); for (size_t i = 0; i < r.size(); i++) put("a0" + std::to_string(i), cased(r[i]), chAct(cased(r[i])), (double)i, 1); }
    { auto r = cps(ACCENTS[1]); for (size_t i = 0; i < r.size(); i++) put("a1" + std::to_string(i), cased(r[i]), chAct(cased(r[i])), (double)i, 2); }
    put("bs", bsL, act(KeyActT::bs), 9, 2);
    put("shift", upper ? "\xE2\x87\xAA" : "\xE2\x87\xA7", act(KeyActT::shift), 0, 3);
    auto r = cps(ACCENTS[2]);
    for (size_t i = 0; i < r.size(); i++) {
      const std::string k = r[i] == 0xDF ? u8(r[i]) : cased(r[i]);   // ß has no single-letter capital here
      put("a2" + std::to_string(i), k, chAct(k), (double)(i + 1), 3);
    }
  } else {
    for (int row = 0; row < 3; row++) {
      const char* L = LETTERS[row];
      for (int i = 0; L[i]; i++) {
        const std::string id = std::string("l") + L[i];
        const std::string c = cased((char32_t)L[i]);
        if (row == 2) {
          if (i == 0) {
            if (mode == KbMode::url) put("slash", "/", chAct("/"), 0, 3);
            else put("shift", layer == KbLayer::caps ? "\xE2\x87\xAA" : "\xE2\x87\xA7", act(KeyActT::shift), 0, 3);
          }
          put(id, c, chAct(c), i + 1, 3);
        } else {
          put(id, c, chAct(c), i, row + 1);
        }
      }
      if (row == 1) put("bs", bsL, act(KeyActT::bs), 9, 2);
    }
    if (mode == KbMode::url) { put("colon", ":", chAct(":"), 8, 3); put("dot", ".", chAct("."), 9, 3); }
    else { put("dash", "-", chAct("-"), 8, 3); put("dot", ".", chAct("."), 9, 3); }
  }
  // Row 4: Back, #+=, (ÀÉ), (SHOW), space, ↵ — ten units in all.
  double x = 0;
  put("back", "Back", act(KeyActT::back), x, 4, 1.5); x += 1.5;
  const bool sym = layer == KbLayer::symbols;
  put("sym", sym ? "ABC" : "#+=", act(sym ? KeyActT::abc : KeyActT::symbols), x, 4, 1.5); x += 1.5;
  const bool accentsOk = mode == KbMode::text || mode == KbMode::password || mode == KbMode::search;
  if (accentsOk) { put("acc", accents ? "abc" : "\xC3\x80\xC3\x89", act(accents ? KeyActT::abc : KeyActT::accents), x, 4, 1); x += 1; }
  if (mode == KbMode::password) { put("reveal", o.revealLabel ? *o.revealLabel : "SHOW", act(KeyActT::reveal), x, 4, 1); x += 1; }
  const bool spaceOk = mode != KbMode::url;
  const double spaceW = 10 - 2 - x;
  if (spaceOk) put("space", "space", act(KeyActT::space), x, 4, spaceW);
  put("enter", "\xE2\x86\xB5", act(KeyActT::enter), spaceOk ? x + spaceW : 8, 4, spaceOk ? 2 : 10 - x);
}

double cxOf(const Box& k) { return k.x + k.w / 2; }
double cyOf(const Box& k) { return k.y + k.h / 2; }
bool overlapsRow(const Box& a, const Box& b) { return a.y < b.y + b.h && b.y < a.y + a.h; }
bool overlapsCol(const Box& a, const Box& b) { return a.x < b.x + b.w && b.x < a.x + a.w; }

}  // namespace

KbLayoutName layoutFor(KbMode mode) {
  return mode == KbMode::frequency || mode == KbMode::pin ? KbLayoutName::numeric : KbLayoutName::qwerty;
}

std::vector<KbKey> kbKeys(KbMode mode, KbLayer layer, const KbKeysOpts& o) {
  std::vector<KbKey> out;
  out.reserve(56);
  if (layoutFor(mode) == KbLayoutName::numeric) numericKeys(out, mode, o);
  else qwertyKeys(out, mode, layer, o);
  return out;
}

int pickNext(const Box* cur, const std::vector<Box>& boxes, Dir dir) {
  if (boxes.empty()) return -1;
  if (!cur) {
    int best = 0;
    for (int i = 1; i < (int)boxes.size(); i++)
      if (boxes[i].y + boxes[i].x * 0.5 < boxes[best].y + boxes[best].x * 0.5) best = i;
    return best;
  }
  int best = -1;
  double bestScore = INFINITY;
  for (int i = 0; i < (int)boxes.size(); i++) {
    const Box& b = boxes[i];
    if (b.x == cur->x && b.y == cur->y && b.w == cur->w && b.h == cur->h) continue;
    const double dx = cxOf(b) - cxOf(*cur), dy = cyOf(b) - cyOf(*cur);
    const double along = dir == Dir::right ? dx : dir == Dir::left ? -dx : dir == Dir::down ? dy : -dy;
    const double across = dir == Dir::left || dir == Dir::right ? std::fabs(dy) : std::fabs(dx);
    if (along <= 1) continue;
    const double score = along + across * 2.5;
    if (score < bestScore) { bestScore = score; best = i; }
  }
  return best;
}

int kbMove(const std::vector<KbKey>& keys, int cur, Dir dir) {
  const int n = (int)keys.size();
  if (!n) return -1;
  if (cur < 0 || cur >= n) return 0;
  const Box here = keys[cur].box();
  // LEFT / RIGHT STAY ON THE ROW: the nearest key that way on it, else the wrap below.
  if (dir == Dir::left || dir == Dir::right) {
    int near = -1;
    for (int j = 0; j < n; j++) {
      const Box k = keys[j].box();
      if (j == cur || !overlapsRow(k, here)) continue;
      const bool ahead = dir == Dir::right ? cxOf(k) > cxOf(here) : cxOf(k) < cxOf(here);
      if (ahead && (near < 0 || std::fabs(cxOf(k) - cxOf(here)) < std::fabs(cxOf(keys[near].box()) - cxOf(here)))) near = j;
    }
    if (near >= 0) return near;
  }
  // pickNext works in POINTS (it ignores a move of a point or less), so key units are scaled up first.
  if (dir == Dir::up || dir == Dir::down) {
    const double S = 100;
    auto px = [&](const Box& k) { return Box{k.x * S, k.y * S, k.w * S, k.h * S}; };
    std::vector<Box> all;
    all.reserve(n);
    for (const auto& k : keys) all.push_back(px(k.box()));
    const Box h = px(here);
    const int i = pickNext(&h, all, dir);
    if (i >= 0) return i;
  }
  // Wrap: the far end of this row (left / right) or of this column (up / down).
  int best = -1;
  for (int j = 0; j < n; j++) {
    if (j == cur) continue;
    const Box k = keys[j].box();
    if (dir == Dir::left || dir == Dir::right) {
      if (!overlapsRow(k, here)) continue;
      if (best < 0 || (dir == Dir::left ? cxOf(k) > cxOf(keys[best].box()) : cxOf(k) < cxOf(keys[best].box()))) best = j;
    } else {
      if (!overlapsCol(k, here)) continue;
      if (best < 0 || (dir == Dir::up ? cyOf(k) > cyOf(keys[best].box()) : cyOf(k) < cyOf(keys[best].box()))) best = j;
    }
  }
  return best >= 0 ? best : cur;
}

std::string kbStartKey(KbMode mode) { return layoutFor(mode) == KbLayoutName::numeric ? "5" : "lg"; }

bool kbHasSystemGlyph(const std::string& label) {
  const auto g = cps(KB_SYSTEM_GLYPHS);
  for (char32_t c : cps(label))
    for (char32_t s : g) if (c == s) return true;
  return false;
}

const char* name(KbMode m) {
  switch (m) {
    case KbMode::frequency: return "frequency"; case KbMode::pin: return "pin"; case KbMode::search: return "search";
    case KbMode::text: return "text"; case KbMode::password: return "password"; case KbMode::url: return "url";
  }
  return "?";
}
const char* name(KbLayer l) {
  switch (l) {
    case KbLayer::lower: return "lower"; case KbLayer::upper: return "upper"; case KbLayer::caps: return "caps";
    case KbLayer::symbols: return "symbols"; case KbLayer::accents: return "accents"; case KbLayer::accentsUp: return "accentsUp";
  }
  return "?";
}
const char* name(KbLayoutName l) { return l == KbLayoutName::numeric ? "numeric" : "qwerty"; }
const char* name(Dir d) {
  switch (d) { case Dir::up: return "up"; case Dir::down: return "down"; case Dir::left: return "left"; case Dir::right: return "right"; }
  return "?";
}
const char* name(KeyActT t) {
  switch (t) {
    case KeyActT::ch: return "ch"; case KeyActT::bs: return "bs"; case KeyActT::enter: return "enter";
    case KeyActT::back: return "back"; case KeyActT::shift: return "shift"; case KeyActT::symbols: return "symbols";
    case KeyActT::accents: return "accents"; case KeyActT::abc: return "abc"; case KeyActT::space: return "space";
    case KeyActT::clear: return "clear"; case KeyActT::unit: return "unit"; case KeyActT::reveal: return "reveal";
  }
  return "?";
}
std::optional<KbMode> modeFromName(const std::string& s) {
  for (KbMode m : {KbMode::frequency, KbMode::pin, KbMode::search, KbMode::text, KbMode::password, KbMode::url})
    if (s == name(m)) return m;
  return std::nullopt;
}
std::optional<KbLayer> layerFromName(const std::string& s) {
  for (KbLayer l : {KbLayer::lower, KbLayer::upper, KbLayer::caps, KbLayer::symbols, KbLayer::accents, KbLayer::accentsUp})
    if (s == name(l)) return l;
  return std::nullopt;
}
std::optional<Dir> dirFromName(const std::string& s) {
  for (Dir d : {Dir::up, Dir::down, Dir::left, Dir::right}) if (s == name(d)) return d;
  return std::nullopt;
}

}  // namespace kb
