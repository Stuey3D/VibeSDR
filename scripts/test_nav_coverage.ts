// ★★★ NAV COVERAGE (controller audit root cause C, 2026-10-20): a control on a surface a pad drives that never joins the
// focus order is SILENTLY skipped — nothing warns, and it reaches Stuart's sofa as "the pad cannot press that". This scan
// looks at every file that uses PanelNav / ScreenNav and every touchable in it (TouchableOpacity, Pressable, Switch,
// Slider), and counts the ones with no sign of a nav registration near them.
//
// ★ Static, so approximate: a touchable counts as REGISTERED when its opening tag (or the three lines above it) carries
//   one of the registration marks below — a ref from useNavButton / screenNavRef, a `focused`/ring style, a
//   NavSlot / NavWrap / CardKey wrapper, a picker slot — or an explicit `nav-skip: <reason>` comment for a control that
//   is deliberately left out (a third-party page, a touch-only gesture).
// ★ The audit's SHOULD list is not done yet, so each file has a BASELINE of known unregistered touchables. The test fails
//   when a file goes ABOVE its baseline (a new silent skip) or when a new nav file appears with any; when a file drops
//   BELOW its baseline it says so, so the number is lowered and cannot creep back.
import { readFileSync, readdirSync } from 'node:fs';
import path from 'node:path';

let fails = 0;
const ok = (c: boolean, what: string, extra = '') => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}${extra ? '\n' + extra : ''}`); if (!c) fails++; };
const root = new URL('../', import.meta.url).pathname;

const MARK = /rowFocus\(|splashKb|viewRef|screenNavRef|focused[=:{ ]|focused\b|Ring\(|NavRing|NavSlot|NavWrap|CardKey|ChooserKey|ChooserRow|chooserSlot|slotRing|subRing|subOn\(|navOn|tuneOn\(|hdrFocus|refusalHl|pickItems\[pickFocus\]|nav\.(key|field)\(|keyHl\(|bmSlot|slot\??\.on|\bhl\b|isFocused|kbFocus|navFocus|focusIdx|listIdx|nav-skip:|nav-registered:/;
const TAG = /<(TouchableOpacity|Pressable|Switch|Slider|AnimatedPressable)\b/;

function files(dir: string): string[] {
  const out: string[] = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) out.push(...files(p));
    else if (/\.tsx$/.test(e.name)) out.push(p);
  }
  return out;
}

const all = [...files(path.join(root, 'src')), path.join(root, 'App.tsx')];
const navFiles = all.filter((f) => {
  const t = readFileSync(f, 'utf8');
  if (/PanelNav\.tsx$|ScreenNav\.tsx$/.test(f)) return false;
  return /from '\.{1,2}\/(components\/)?(PanelNav|ScreenNav|NavCard|FieldPopupNav)'|from '\.\/src\/components\/PanelNav'/.test(t);
});

function unregistered(file: string): string[] {
  const lines = readFileSync(file, 'utf8').split('\n');
  const out: string[] = [];
  for (let i = 0; i < lines.length; i++) {
    if (!TAG.test(lines[i]) || /^\s*(\/\/|\*)/.test(lines[i])) continue;
    // The element: from its opening line to its own closing tag (or a self-closing `/>`), at most 14 lines — a ring
    // drawn as a CHILD (NavRing) counts as much as a focus style on the tag.
    const name = (TAG.exec(lines[i]) as RegExpExecArray)[1];
    let tag = '';
    for (let j = i; j < Math.min(lines.length, i + 14); j++) {
      tag += lines[j] + '\n';
      if (lines[j].includes(`</${name}>`) || (j > i && /^\s*\/>\s*\)?;?\s*$/.test(lines[j])) || (j === i && /\/>\s*\)?;?\s*$/.test(lines[j]))) break;
    }
    const ctx = lines.slice(Math.max(0, i - 3), i).join('\n') + '\n' + tag;
    if (!MARK.test(ctx)) out.push(`${path.relative(root, file)}:${i + 1}  ${lines[i].trim().slice(0, 90)}`);
  }
  return out;
}

// Known unregistered touchables per file — the audit's SHOULD list (2026-10-20). Lower these as controls are registered;
// never raise one without a `nav-skip:` reason on the control instead.
const BASELINE: Record<string, number> = {
  'src/components/AboutOverlay.tsx': 0,
  'src/components/ChatDrawer.tsx': 0,
  'src/components/DecoderPanel.tsx': 4,         // decoder spot rows etc. (SHOULD)
  'src/components/FreqModal.tsx': 12,           // bookmarks-pane extras: magic key, DAB rows, EiBi, sync, delete, import (SHOULD)
  'src/components/LocalHardwarePanel.tsx': 35,  // to be rebuilt on usePanelNav (SHOULD)
  'src/components/MenuSheet.tsx': 9,            // sideways selector nudges, sub-page back row, drop-downs (SHOULD)
  'src/components/RecordingsOverlay.tsx': 5,    // share / delete / seek (SHOULD)
  'src/screens/SDRScreen.tsx': 2,               // owner notice + its ✕ (NICE)
  'src/screens/TunerScreen.tsx': 10,            // ♥, KEYS/DRUMS, PAUSED, band and AF chips (SHOULD)
};

console.log('test_nav_coverage');
const report: Record<string, string[]> = {};
for (const f of navFiles) report[path.relative(root, f)] = unregistered(f);
const PRINT = process.argv.includes('--list');
for (const [f, list] of Object.entries(report).sort()) {
  const base = BASELINE[f] ?? 0;
  if (PRINT && list.length) console.log(list.map((l) => '      ' + l).join('\n'));
  ok(list.length <= base, `${f}: ${list.length} unregistered (baseline ${base})`,
     list.length > base ? list.map((l) => '      ' + l).join('\n') : '');
  if (list.length < base) console.log(`      ↓ below its baseline — lower BASELINE['${f}'] to ${list.length}`);
}
// The surfaces this phase made pad-complete must STAY complete.
for (const f of ['src/components/NavCard.tsx', 'src/components/FieldPopupNav.tsx', 'src/components/PasswordModal.tsx',
                 'src/components/IdentModal.tsx', 'src/components/VibePinPrompt.tsx', 'src/components/Coachmark.tsx',
                 'src/components/AppAlert.tsx', 'src/components/DabPanel.tsx']) {
  const list = report[f] ?? unregistered(path.join(root, f));
  ok(list.length === 0, `${f}: every touchable registered`, list.map((l) => '      ' + l).join('\n'));
}
process.exit(fails ? 1 : 0);
