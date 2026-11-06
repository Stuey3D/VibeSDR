// kb_layouts.h — THE CONTROLLER KEYBOARD'S LAYOUTS, AS DATA. A C++17 port of the app's src/utils/kbLayouts.ts (and of
// pickNext from src/utils/gamepadNav.ts, which kbMove uses), bit-exact in behaviour: every key's id, label, action
// and box, and every highlight move, match the TS — checked by handheld-ui/test/kb_golden_test.cpp against the TS
// itself. The model only; drawing is the caller's.
//
//   numeric                         qwerty (lower)
//   [7][8][9]   [⌫]                 [1][2][3][4][5][6][7][8][9][0]
//   [4][5][6]   [kHz/MHz]           [q][w][e][r][t][y][u][i][o][p]
//   [1][2][3]   [Clear]             [a][s][d][f][g][h][j][k][l][⌫]
//   [.][0][↵]   [Back]              [⇧][z][x][c][v][b][n][m][-][.]
//                                   [Back][#+=][ÀÉ][  space  ][ ↵ ]
//
// Boxes are in KEY UNITS (numeric 4.75 × 4, QWERTY 10 × 5): x, y, w, h. Labels and characters are UTF-8.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace kb {

enum class KbLayoutName { numeric, qwerty };
enum class KbMode { frequency, pin, search, text, password, url };
enum class KbLayer { lower, upper, caps, symbols, accents, accentsUp };
enum class Dir { up, down, left, right };

/** What a key does (TS KeyAct). `ch` keys carry their character (UTF-8) in KeyAct::ch. */
enum class KeyActT { ch, bs, enter, back, shift, symbols, accents, abc, space, clear, unit, reveal };
struct KeyAct { KeyActT t = KeyActT::ch; std::string ch; };

struct Box { double x = 0, y = 0, w = 0, h = 0; };

struct KbKey {
  std::string id;      // stable across layers where the key persists (the highlight follows the id)
  std::string label;   // UTF-8
  KeyAct act;
  double x = 0, y = 0, w = 0, h = 0;
  Box box() const { return {x, y, w, h}; }
};

struct KbKeysOpts {
  /** frequency: the receiver locks the unit — no unit key. */
  bool unitLocked = false;
  /** frequency: the unit key's label (the unit it switches TO, e.g. "MHz"). Default "MHz". */
  std::optional<std::string> unitLabel;
  /** password: SHOW / HIDE's current label. Default "SHOW". */
  std::optional<std::string> revealLabel;
};

KbLayoutName layoutFor(KbMode mode);

/** Every key on screen for this mode and layer. */
std::vector<KbKey> kbKeys(KbMode mode, KbLayer layer = KbLayer::lower, const KbKeysOpts& o = {});

/** gamepadNav.ts pickNext: the box to move to from `cur` in `dir`, or -1. cur == nullptr → nearest the top-left. */
int pickNext(const Box* cur, const std::vector<Box>& boxes, Dir dir);

/** Where the highlight goes from key `cur` in `dir` — rows for left/right, pickNext for up/down, wrapping at the
 *  edges. -1 when there are no keys; 0 when cur is out of range. */
int kbMove(const std::vector<KbKey>& keys, int cur, Dir dir);

/** The key id the highlight starts on ('5' numeric, 'lg' QWERTY): ↵ is never the start. */
std::string kbStartKey(KbMode mode);

/** The keycap glyphs neither keycap font carries (UTF-8 "⌫↵⇧⇪"): draw these characters in a system font. */
extern const char* const KB_SYSTEM_GLYPHS;
/** True when any character of `label` is one of KB_SYSTEM_GLYPHS. */
bool kbHasSystemGlyph(const std::string& label);

// Names, as the TS spells them ("frequency", "accentsUp", "up", "bs", …), and back.
const char* name(KbMode m);
const char* name(KbLayer l);
const char* name(KbLayoutName l);
const char* name(Dir d);
const char* name(KeyActT t);
std::optional<KbMode> modeFromName(const std::string& s);
std::optional<KbLayer> layerFromName(const std::string& s);
std::optional<Dir> dirFromName(const std::string& s);

}  // namespace kb
