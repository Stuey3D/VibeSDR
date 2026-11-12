// controller_keyboard.h — THE CONTROLLER KEYBOARD'S EDITING RULES. A C++17 port of the app's
// src/utils/controllerKeyboard.ts (kbInit, kbReduce, kbDisplay, kbParseNumber, kbFormatNumber), plus `Session`: the
// open-keyboard logic of web/client/src/padKeyboard.ts without its DOM (keys for the state, the highlight that
// follows a key's id across layers, press / Back-one-layer / ↵ with a refusal that keeps it open, the pad's own edit
// buttons). Bit-exact in behaviour against the TS — handheld-ui/test/kb_golden_test.cpp.
//
// ★ STRINGS: the value is held as UTF-16 (std::u16string) and the caret counts UTF-16 units, exactly as the TS's JS
//   strings do, so ⌫ and the caret behave identically even on a character outside the BMP. Every accessor a renderer
//   needs comes out as UTF-8 (valueUtf8, displayBefore/After…).
// ★★ PASSWORD MODE: nothing here logs, ever.
#pragma once
#include "kb_layouts.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kb {

enum class KbUnit { hz, khz, mhz };
const char* name(KbUnit u);   // "hz" "khz" "mhz"

struct KbState {
  std::u16string value;
  int caret = 0;               // in UTF-16 units, 0..value.size()
  KbLayer layer = KbLayer::lower;
  KbMode mode = KbMode::text;
  std::optional<KbUnit> unit;  // frequency only
  bool reveal = false;         // password: the characters are shown
  std::string valueUtf8() const;
};

/** TS KbAction. `ch` is UTF-8. */
enum class KbActT { ch, bs, left, right, shift, symbols, accents, abc, space, clear, unit, reveal };
struct KbAction { KbActT t = KbActT::ch; std::string ch; };

constexpr int PIN_MAX = 12;
constexpr int TEXT_MAX = 256;
struct UnitDigits { int intDigits, dec; };
UnitDigits unitDigits(KbUnit u);   // hz 10/0, khz 7/3, mhz 5/6

/** kbInit(mode, value, {unit, caret}). value is UTF-8. */
KbState kbInit(KbMode mode, const std::string& value = "", std::optional<KbUnit> unit = std::nullopt,
               std::optional<int> caret = std::nullopt);
/** The reducer. `replaced` (optional) is false exactly where the TS returns the SAME state object (a refused key,
 *  ⌫ at the start, …) — the session uses it as the TS does (no redraw, an error message stays). */
KbState kbReduce(const KbState& s, const KbAction& a, bool* replaced = nullptr);
/** The value as the field's text should show it (password: • per UTF-16 unit unless revealed). UTF-8. */
std::string kbDisplay(const KbState& s);
/** Our own number parse: digits and one '.', nothing locale-aware. NaN when not a number. (UTF-8 in.) */
double kbParseNumber(const std::string& v);
/** A number back to text without float noise: JS toFixed(dec), trailing zeros trimmed. '' for NaN / ±Inf. */
std::string kbFormatNumber(double n, int dec);

// UTF-8 ↔ UTF-16 (invalid input → U+FFFD).
std::u16string utf8ToUtf16(const std::string& s);
std::string utf16ToUtf8(const std::u16string& s);

// ── Session: one open keyboard (padKeyboard.ts) ──────────────────────────────────────────────────────────────────────

struct SessionOpts {
  KbMode mode = KbMode::text;
  std::string value;                 // the field's value when it opens (UTF-8)
  std::optional<KbUnit> unit;        // frequency: the field's unit now (default MHz)
  std::optional<int> caret;          // default: the end
  bool unitSettable = false;         // frequency: the field can switch unit (TS: a setUnit) — else no unit key
  bool secure = false;               // the field is a password / secret PIN: draw dots, grey ring, no press flash
};

/** What an input did. */
enum class Outcome {
  none,      // nothing changed
  changed,   // redraw
  submit,    // ↵ accepted: closed — read state() (value, unit) and act on it
  refused,   // ↵ refused: still open, err() says why
  cancel,    // Back past the last layer: closed — the field keeps the value it had (origValue)
};
const char* name(Outcome o);

/** The pad's own editing buttons (padKbEdit). On the numeric pad the shoulders send `unit`. */
enum class EditAct { bs, space, shift, symbols, unit, left, right };

/** ↵: write the value and do the field's commit (tune…). Return a refusal message to keep the keyboard open, or "". */
using Commit = std::function<std::string(const KbState&)>;

class Session {
 public:
  void open(const SessionOpts& o);
  bool isOpen() const { return open_; }

  Outcome move(Dir d);                          // D-pad: the highlight
  Outcome press(const Commit& commit = {});     // press the highlighted key (↵ → submit, Back → back)
  Outcome back();                               // ○ / Back: one layer at a time, then cancel
  Outcome edit(EditAct a);                      // ⌫ space ⇧ #+= unit caret ← →
  Outcome submit(const Commit& commit = {});    // START tap / ↵

  // ── What a renderer draws ──
  const KbState& state() const { return st_; }
  KbMode mode() const { return mode_; }
  KbLayoutName layout() const { return layoutFor(mode_); }
  bool secure() const { return secure_; }
  const std::vector<KbKey>& keys() const { return keys_; }
  int focus() const { return focus_; }
  const KbKey* focusedKey() const { return focus_ >= 0 && focus_ < (int)keys_.size() ? &keys_[focus_] : nullptr; }
  /** Drawn lit: ↵ always, ⇧ while upper/caps/accentsUp, SHOW while revealed. */
  bool keyLit(int i) const;
  /** The value as drawn (dots for a password, or for a secure PIN), and split at the caret. UTF-8. */
  std::string displayText() const;
  std::string displayBefore() const;
  std::string displayAfter() const;
  /** frequency: "MHz" / "kHz" / "Hz" after the value; "" otherwise. */
  std::string unitText() const;
  /** The refusal message under the value ("" when none). UTF-8. */
  const std::string& err() const { return err_; }
  /** The field's value when the keyboard opened (Back leaves the field there). */
  const std::string& origValue() const { return orig_; }

 private:
  std::vector<KbKey> keysFor() const;
  void render();
  Outcome apply(const KbAction& a);
  bool open_ = false;
  KbMode mode_ = KbMode::text;
  bool unitSettable_ = false, secure_ = false;
  KbState st_;
  std::vector<KbKey> keys_;
  int focus_ = 0;
  std::string orig_, err_;
};

}  // namespace kb
