# src/kb — the controller keyboard's model, ported

A C++17 port of the app's controller keyboard **model** (no drawing): `src/utils/kbLayouts.ts` (→ `kb_layouts.h`),
`src/utils/controllerKeyboard.ts` (→ `controller_keyboard.h`), and the open-keyboard logic of
`web/client/src/padKeyboard.ts` (→ `kb::Session`). It behaves exactly like the TS: `test/run_kb_golden.sh` runs the TS
and the C++ through the same inputs and compares every line.

- `kbKeys(mode, layer, opts)` gives the keys. Each has an `id`, a UTF-8 `label`, an `act` (`KeyActT::ch` plus its character,
  or a system action) and a box `x, y, w, h` in key units (numeric is 4.75 × 4, QWERTY is 10 × 5).
  `kbMove`, `kbStartKey`, `layoutFor` and `KB_SYSTEM_GLYPHS` / `kbHasSystemGlyph` are as in the TS.
- `KbState` holds the value as UTF-16, the caret in UTF-16 units, the layer, the mode, the unit and the reveal flag.
  `kbInit`, `kbReduce(state, action, &replaced)` and `kbDisplay` do the editing. `kbParseNumber` and `kbFormatNumber`
  handle numbers (JS `toFixed` rounding, ties up).
- `Session` is the keyboard while it is open:
  - Inputs: `open(opts)`, then `move(dir)`, `press(commit)`, `back()`, `edit(EditAct)` and `submit(commit)`. Each one
    returns an `Outcome`: `none`, `changed`, `submit`, `refused` or `cancel`.
  - What to draw: `keys()`, `focus()`, `keyLit(i)`, `displayBefore()` / `displayAfter()` (the text either side of the
    caret, already dotted when `secure`), `unitText()`, `err()`, `state().layer`, `secure()` (grey ring, no press flash).
  - Pad mapping (padKeyboard.ts): D-pad → `move`, ✕ → `press`, ○ → `back`, □ → `edit(bs)`, △ → `edit(space)`,
    L1/R1 → `edit(shift)` / `edit(symbols)` (numeric: `edit(unit)` on both), START tap → `submit`, the left stick
    turned → `edit(left/right)`.

```cpp
#include "kb/controller_keyboard.h"
kb::Session kbd;
kb::SessionOpts o;
o.mode = kb::KbMode::frequency;  o.value = "9.410";  o.unit = kb::KbUnit::mhz;
o.unitSettable = true;           // the unit key (kHz ↔ MHz) shows, and L1/R1 switch it
kbd.open(o);
// each pad press:
auto commit = [&](const kb::KbState& st) -> std::string {   // ↵: try to tune; return a refusal to stay open
  const double n = kb::kbParseNumber(st.valueUtf8());
  const double hz = n * (*st.unit == kb::KbUnit::mhz ? 1e6 : 1e3);
  if (!(hz >= rangeLo && hz <= rangeHi)) return "Out of range";
  tuneTo(hz); return "";
};
switch (kbd.press(commit)) {                // or kbd.move(kb::Dir::up), kbd.edit(kb::EditAct::unit), kbd.back() …
  case kb::Outcome::submit:  /* closed, tuned */ break;
  case kb::Outcome::cancel:  /* closed, field unchanged */ break;
  default: break;                           // changed / refused / none: redraw from kbd.keys(), kbd.focus(), …
}
// draw: "FREQUENCY" box = kbd.displayBefore() + caret + kbd.displayAfter() + " " + kbd.unitText()   → "9.410| MHz"
```
