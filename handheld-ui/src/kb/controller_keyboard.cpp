// controller_keyboard.cpp — port of src/utils/controllerKeyboard.ts + the session logic of
// web/client/src/padKeyboard.ts. See controller_keyboard.h. ★★ Logs nothing, ever (password mode).
#include "controller_keyboard.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>

namespace kb {

// ── UTF-8 / UTF-16 ───────────────────────────────────────────────────────────────────────────────────────────────────

std::u16string utf8ToUtf16(const std::string& s) {
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char c = (unsigned char)s[i];
    char32_t cp = 0xFFFD;
    int len = 1;
    if (c < 0x80) cp = c;
    else {
      int need = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
      if (need > 0) {
        char32_t v = need == 1 ? (c & 0x1F) : need == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= need; k++) {
          if (i + k >= n || ((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
          v = (v << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        const char32_t minV = need == 1 ? 0x80 : need == 2 ? 0x800 : 0x10000;
        if (ok && v >= minV && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) { cp = v; len = need + 1; }
      }
    }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += (char16_t)(0xD800 + (cp >> 10));
      out += (char16_t)(0xDC00 + (cp & 0x3FF));
    } else {
      out += (char16_t)cp;
    }
  }
  return out;
}

static void putU8(std::string& o, char32_t c) {
  if (c < 0x80) o += (char)c;
  else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
  else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
}

std::string utf16ToUtf8(const std::u16string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;   // a lone surrogate (⌫ can split a pair, as in JS) has no UTF-8 form
    }
    putU8(o, c);
  }
  return o;
}

std::string KbState::valueUtf8() const { return utf16ToUtf8(value); }

// ── Character classes (JS regexes) ───────────────────────────────────────────────────────────────────────────────────

namespace {

bool isDigit16(char16_t c) { return c >= u'0' && c <= u'9'; }

// JS /\s/: WhiteSpace + LineTerminator.
bool isJsSpace(char16_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
         c == 0xFEFF;
}

// /\p{L}/u for the scripts a handheld keyboard can produce. Every character on the layouts is covered exactly (ASCII
// and Latin-1 letters); beyond them this is the main alphabetic blocks — an approximation of Unicode's Letter category.
bool isLetterCp(char32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
  if (c < 0x80) return false;
  if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
  if (c >= 0xC0 && c <= 0x24F) return c != 0xD7 && c != 0xF7;
  if (c >= 0x250 && c <= 0x2C1) return true;                     // IPA, modifier letters
  if (c >= 0x370 && c <= 0x3FF) return c == 0x370 || c == 0x371 || c == 0x372 || c == 0x373 || c == 0x376 || c == 0x377 ||
                                       (c >= 0x37B && c <= 0x37D) || c == 0x37F || c == 0x386 || (c >= 0x388 && c <= 0x38A) ||
                                       c == 0x38C || (c >= 0x38E && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3F5) || (c >= 0x3F7);
  if (c >= 0x400 && c <= 0x52F) return !(c >= 0x482 && c <= 0x489);  // Cyrillic
  if (c >= 0x531 && c <= 0x556) return true;
  if (c >= 0x560 && c <= 0x588) return true;                     // Armenian
  if (c >= 0x5D0 && c <= 0x5EA) return true;                     // Hebrew
  if (c >= 0x620 && c <= 0x64A) return true;                     // Arabic
  if (c >= 0x1E00 && c <= 0x1FFF) return true;                   // Latin / Greek extended (approx.)
  if (c >= 0x3041 && c <= 0x3096) return true;                   // Hiragana
  if (c >= 0x30A1 && c <= 0x30FA) return true;                   // Katakana
  if (c >= 0x4E00 && c <= 0x9FFF) return true;                   // CJK
  if (c >= 0xAC00 && c <= 0xD7A3) return true;                   // Hangul
  return false;
}

bool anyLetter(const std::u16string& s) {
  for (size_t i = 0; i < s.size(); i++) {
    char32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    if (isLetterCp(c)) return true;
  }
  return false;
}

bool anySpace(const std::u16string& s) {
  for (char16_t c : s) if (isJsSpace(c)) return true;
  return false;
}

bool isOneDigit(const std::u16string& c) { return c.size() == 1 && isDigit16(c[0]); }

// The decimal point snprintf wrote, whatever the C locale: the first character that is neither digit nor sign.
size_t pointAt(const std::string& s) {
  for (size_t i = 0; i < s.size(); i++) if (!(s[i] >= '0' && s[i] <= '9') && s[i] != '-') return i;
  return std::string::npos;
}

double parseC(const std::string& s) {
  std::istringstream is(s);
  is.imbue(std::locale::classic());
  double v = NAN;
  is >> v;
  return v;
}

// JS Number.prototype.toString for a finite number (used by toFixed past 1e21): the shortest round-trip digits.
std::string jsNumberToString(double x) {
  if (x == 0) return "0";
  std::string sign = x < 0 ? "-" : "";
  const double a = std::fabs(x);
  char buf[64];
  int p = 1;
  for (; p <= 17; p++) {
    std::snprintf(buf, sizeof buf, "%.*e", p - 1, a);
    std::string t(buf);
    const size_t pt = pointAt(t);
    if (pt != std::string::npos && t[pt] != 'e') t[pt] = '.';
    if (parseC(t) == a) break;
  }
  std::snprintf(buf, sizeof buf, "%.*e", p - 1, a);
  std::string t(buf);
  // digits and exponent
  std::string digits;
  size_t e = t.find('e');
  for (size_t i = 0; i < e; i++) if (t[i] >= '0' && t[i] <= '9') digits += t[i];
  int exp10 = std::atoi(t.c_str() + e + 1);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  const int k = (int)digits.size(), n = exp10 + 1;
  std::string r;
  if (k <= n && n <= 21) r = digits + std::string(n - k, '0');
  else if (0 < n && n <= 21) r = digits.substr(0, n) + "." + digits.substr(n);
  else if (-6 < n && n <= 0) r = "0." + std::string(-n, '0') + digits;
  else {
    r = digits.substr(0, 1);
    if (k > 1) r += "." + digits.substr(1);
    r += "e";
    r += (n - 1 >= 0 ? "+" : "-");
    r += std::to_string(std::abs(n - 1));
  }
  return sign + r;
}

// JS Number.prototype.toFixed(dec): the exact value rounded half-up (on the magnitude), as the spec's "larger n".
std::string jsToFixed(double x, int dec) {
  if (std::isnan(x)) return "NaN";
  if (std::fabs(x) >= 1e21) return jsNumberToString(x);
  std::string sign;
  if (x < 0) { sign = "-"; x = -x; }
  if (x == 0) x = 0.0;   // -0 is not < 0: JS prints it unsigned
  char buf[1200];
  std::snprintf(buf, sizeof buf, "%.1100f", x);   // the exact binary value, in full
  std::string s(buf);
  const size_t pt = pointAt(s);
  std::string ip = s.substr(0, pt), fp = s.substr(pt + 1);
  std::string digits = ip + fp.substr(0, dec);
  const bool up = fp[dec] >= '5';
  if (up) {
    int i = (int)digits.size() - 1;
    for (; i >= 0; i--) {
      if (digits[i] == '9') digits[i] = '0';
      else { digits[i]++; break; }
    }
    if (i < 0) digits.insert(digits.begin(), '1');
  }
  const size_t intLen = digits.size() - dec;
  std::string out = digits.substr(0, intLen);
  if (dec > 0) out += "." + digits.substr(intLen);
  // A "-0.000" stays signed in JS only when x < 0 exactly, which is what the sign records.
  return sign + out;
}

bool numericInsertOk(const std::u16string& value, int caret, const std::u16string& c, KbUnit unit) {
  const UnitDigits ud = unitDigits(unit);
  if (c == u".") {
    if (ud.dec == 0) return false;
    return value.find(u'.') == std::u16string::npos;
  }
  if (!isOneDigit(c)) return false;
  const std::u16string next = value.substr(0, caret) + c + value.substr(caret);
  // next.split('.') → [i, d = '']: up to the first point, and between the first and the second.
  const size_t p1 = next.find(u'.');
  const size_t iLen = p1 == std::u16string::npos ? next.size() : p1;
  size_t dLen = 0;
  if (p1 != std::u16string::npos) {
    const size_t p2 = next.find(u'.', p1 + 1);
    dLen = (p2 == std::u16string::npos ? next.size() : p2) - p1 - 1;
  }
  return (int)iLen <= ud.intDigits && (int)dLen <= ud.dec;
}

// insert(): `same` = the TS returns s itself.
KbState insert(const KbState& s, const std::u16string& c, bool& same) {
  same = false;
  const std::u16string& value = s.value;
  const int caret = s.caret;
  if (s.mode == KbMode::frequency) { if (!numericInsertOk(value, caret, c, s.unit ? *s.unit : KbUnit::mhz)) { same = true; return s; } }
  else if (s.mode == KbMode::pin) { if (!isOneDigit(c) || (int)value.size() >= PIN_MAX) { same = true; return s; } }
  else {
    if ((int)value.size() >= TEXT_MAX) { same = true; return s; }
    if (s.mode == KbMode::url && anySpace(c)) { same = true; return s; }
  }
  KbState n = s;
  n.value = value.substr(0, caret) + c + value.substr(caret);
  n.caret = caret + (int)c.size();
  // One shifted letter, then back to lower case (caps lock stays).
  if (s.layer == KbLayer::upper && anyLetter(c)) n.layer = KbLayer::lower;
  return n;
}

}  // namespace

const char* name(KbUnit u) { return u == KbUnit::hz ? "hz" : u == KbUnit::khz ? "khz" : "mhz"; }

UnitDigits unitDigits(KbUnit u) {
  switch (u) {
    case KbUnit::hz: return {10, 0};
    case KbUnit::khz: return {7, 3};
    case KbUnit::mhz: return {5, 6};
  }
  return {5, 6};
}

KbState kbInit(KbMode mode, const std::string& value, std::optional<KbUnit> unit, std::optional<int> caret) {
  std::u16string v = utf8ToUtf16(value);
  if (mode == KbMode::frequency) {
    std::u16string t;
    for (char16_t c : v) { if (c == u',') c = u'.'; if (isDigit16(c) || c == u'.') t += c; }
    v = t;
  }
  if (mode == KbMode::pin) {
    std::u16string t;
    for (char16_t c : v) if (isDigit16(c)) t += c;
    if ((int)t.size() > PIN_MAX) t.resize(PIN_MAX);
    v = t;
  }
  KbState s;
  s.value = v;
  const int len = (int)v.size();
  s.caret = !caret ? len : std::max(0, std::min(len, *caret));
  s.layer = KbLayer::lower;
  s.mode = mode;
  if (mode == KbMode::frequency) s.unit = unit ? *unit : KbUnit::mhz;
  s.reveal = false;
  return s;
}

double kbParseNumber(const std::string& v) {
  // /^\d*\.?\d*$/, and not '' or '.'
  size_t i = 0;
  while (i < v.size() && v[i] >= '0' && v[i] <= '9') i++;
  if (i < v.size() && v[i] == '.') i++;
  while (i < v.size() && v[i] >= '0' && v[i] <= '9') i++;
  if (i != v.size() || v.empty() || v == ".") return NAN;
  return parseC(v[0] == '.' ? "0" + v : v);
}

std::string kbFormatNumber(double n, int dec) {
  if (!std::isfinite(n)) return "";
  std::string s = jsToFixed(n, dec);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

KbState kbReduce(const KbState& s, const KbAction& a, bool* replaced) {
  bool same = false;
  KbState r = s;
  switch (a.t) {
    case KbActT::ch: r = insert(s, utf8ToUtf16(a.ch), same); break;
    case KbActT::space:
      if (s.mode == KbMode::frequency || s.mode == KbMode::pin || s.mode == KbMode::url) same = true;
      else r = insert(s, u" ", same);
      break;
    case KbActT::bs:
      if (s.caret <= 0) { same = true; break; }
      r.value = s.value.substr(0, s.caret - 1) + s.value.substr(s.caret);
      r.caret = s.caret - 1;
      break;
    case KbActT::left: r.caret = std::max(0, s.caret - 1); break;
    case KbActT::right: r.caret = std::min((int)s.value.size(), s.caret + 1); break;
    case KbActT::clear: r.value.clear(); r.caret = 0; break;
    case KbActT::shift:
      if (s.mode == KbMode::url || s.layer == KbLayer::symbols) { same = true; break; }
      if (s.layer == KbLayer::accents || s.layer == KbLayer::accentsUp)
        r.layer = s.layer == KbLayer::accents ? KbLayer::accentsUp : KbLayer::accents;
      else r.layer = s.layer == KbLayer::lower ? KbLayer::upper : s.layer == KbLayer::upper ? KbLayer::caps : KbLayer::lower;
      break;
    case KbActT::symbols: r.layer = s.layer == KbLayer::symbols ? KbLayer::lower : KbLayer::symbols; break;
    case KbActT::accents:
      r.layer = s.layer == KbLayer::accents || s.layer == KbLayer::accentsUp ? KbLayer::lower : KbLayer::accents;
      break;
    case KbActT::abc: r.layer = KbLayer::lower; break;
    case KbActT::reveal:
      if (s.mode == KbMode::password) r.reveal = !s.reveal; else same = true;
      break;
    case KbActT::unit: {
      if (s.mode != KbMode::frequency) { same = true; break; }
      const KbUnit from = s.unit ? *s.unit : KbUnit::mhz;
      const KbUnit to = from == KbUnit::mhz ? KbUnit::khz : KbUnit::mhz;
      const double n = kbParseNumber(utf16ToUtf8(s.value));
      r.unit = to;
      if (!std::isfinite(n)) break;
      const double hz = n * (from == KbUnit::mhz ? 1e6 : from == KbUnit::khz ? 1e3 : 1);
      const std::string v = kbFormatNumber(hz / (to == KbUnit::mhz ? 1e6 : 1e3), unitDigits(to).dec);
      r.value = utf8ToUtf16(v);
      r.caret = (int)r.value.size();
      break;
    }
  }
  if (replaced) *replaced = !same;
  return same ? s : r;
}

static std::string dots(size_t n) {
  std::string o;
  o.reserve(n * 3);
  for (size_t i = 0; i < n; i++) o += "\xE2\x80\xA2";   // •
  return o;
}

std::string kbDisplay(const KbState& s) {
  return s.mode == KbMode::password && !s.reveal ? dots(s.value.size()) : s.valueUtf8();
}

// ── Session ──────────────────────────────────────────────────────────────────────────────────────────────────────────

const char* name(Outcome o) {
  switch (o) {
    case Outcome::none: return "none"; case Outcome::changed: return "changed"; case Outcome::submit: return "submit";
    case Outcome::refused: return "refused"; case Outcome::cancel: return "cancel";
  }
  return "?";
}

std::vector<KbKey> Session::keysFor() const {
  KbKeysOpts o;
  o.unitLocked = !unitSettable_;
  o.unitLabel = (st_.unit ? *st_.unit : KbUnit::mhz) == KbUnit::mhz ? "kHz" : "MHz";
  o.revealLabel = st_.reveal ? "HIDE" : "SHOW";
  return kbKeys(mode_, st_.layer, o);
}

void Session::open(const SessionOpts& o) {
  mode_ = o.mode;
  unitSettable_ = o.unitSettable;
  secure_ = o.secure;
  st_ = kbInit(o.mode, o.value, o.unit, o.caret);
  orig_ = o.value;
  err_.clear();
  keys_ = keysFor();
  focus_ = 0;
  const std::string start = kbStartKey(mode_);
  for (int i = 0; i < (int)keys_.size(); i++) if (keys_[i].id == start) { focus_ = i; break; }
  open_ = true;
}

void Session::render() {
  // A layer change swaps the keys: keep the highlight on the same key where it still exists, else in place.
  const std::string was = focus_ >= 0 && focus_ < (int)keys_.size() ? keys_[focus_].id : std::string();
  const bool hadWas = focus_ >= 0 && focus_ < (int)keys_.size();
  keys_ = keysFor();
  int same = -1;
  if (hadWas && !was.empty())
    for (int i = 0; i < (int)keys_.size(); i++) if (keys_[i].id == was) { same = i; break; }
  focus_ = same >= 0 ? same : std::max(0, std::min(focus_, (int)keys_.size() - 1));
}

Outcome Session::apply(const KbAction& a) {
  if (!open_) return Outcome::none;
  bool replaced = false;
  KbState next = kbReduce(st_, a, &replaced);
  if (!replaced) return Outcome::none;
  st_ = std::move(next);
  err_.clear();
  render();
  return Outcome::changed;
}

Outcome Session::submit(const Commit& commit) {
  if (!open_) return Outcome::none;
  const std::string refused = commit ? commit(st_) : std::string();
  if (!refused.empty()) { err_ = refused; render(); return Outcome::refused; }
  open_ = false;
  return Outcome::submit;
}

Outcome Session::back() {
  if (!open_) return Outcome::none;
  if (st_.layer != KbLayer::lower) return apply({KbActT::abc, {}});
  open_ = false;
  return Outcome::cancel;
}

Outcome Session::move(Dir d) {
  if (!open_) return Outcome::none;
  const int n = kbMove(keys_, focus_, d);
  if (n >= 0 && n != focus_) { focus_ = n; render(); return Outcome::changed; }
  return Outcome::none;
}

Outcome Session::press(const Commit& commit) {
  if (!open_) return Outcome::none;
  const KbKey* k = focusedKey();
  if (!k) return Outcome::none;
  switch (k->act.t) {
    case KeyActT::enter: return submit(commit);
    case KeyActT::back: return back();
    case KeyActT::ch: return apply({KbActT::ch, k->act.ch});
    case KeyActT::bs: return apply({KbActT::bs, {}});
    case KeyActT::shift: return apply({KbActT::shift, {}});
    case KeyActT::symbols: return apply({KbActT::symbols, {}});
    case KeyActT::accents: return apply({KbActT::accents, {}});
    case KeyActT::abc: return apply({KbActT::abc, {}});
    case KeyActT::space: return apply({KbActT::space, {}});
    case KeyActT::clear: return apply({KbActT::clear, {}});
    case KeyActT::unit: return apply({KbActT::unit, {}});
    case KeyActT::reveal: return apply({KbActT::reveal, {}});
  }
  return Outcome::none;
}

Outcome Session::edit(EditAct a) {
  if (!open_) return Outcome::none;
  switch (a) {
    case EditAct::symbols: return apply({st_.layer == KbLayer::symbols ? KbActT::abc : KbActT::symbols, {}});
    case EditAct::bs: return apply({KbActT::bs, {}});
    case EditAct::space: return apply({KbActT::space, {}});
    case EditAct::shift: return apply({KbActT::shift, {}});
    case EditAct::unit: return apply({KbActT::unit, {}});
    case EditAct::left: return apply({KbActT::left, {}});
    case EditAct::right: return apply({KbActT::right, {}});
  }
  return Outcome::none;
}

bool Session::keyLit(int i) const {
  if (i < 0 || i >= (int)keys_.size()) return false;
  const KeyActT t = keys_[i].act.t;
  return t == KeyActT::enter ||
         (t == KeyActT::shift && (st_.layer == KbLayer::upper || st_.layer == KbLayer::caps || st_.layer == KbLayer::accentsUp)) ||
         (t == KeyActT::reveal && st_.reveal);
}

std::string Session::displayText() const {
  if (mode_ == KbMode::pin && secure_ && !st_.reveal) return dots(st_.value.size());
  return kbDisplay(st_);
}

// shown.slice(0, caret) / shown.slice(caret), in UTF-16 units as the TS cuts it.
std::string Session::displayBefore() const {
  const std::u16string shown = utf8ToUtf16(displayText());
  return utf16ToUtf8(shown.substr(0, std::min<size_t>(st_.caret, shown.size())));
}
std::string Session::displayAfter() const {
  const std::u16string shown = utf8ToUtf16(displayText());
  return utf16ToUtf8(shown.substr(std::min<size_t>(st_.caret, shown.size())));
}

std::string Session::unitText() const {
  if (mode_ != KbMode::frequency) return "";
  const KbUnit u = st_.unit ? *st_.unit : KbUnit::mhz;
  return u == KbUnit::mhz ? "MHz" : u == KbUnit::khz ? "kHz" : "Hz";
}

}  // namespace kb
