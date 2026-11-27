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

const MARK = /rowFocus\(|splashKb|viewRef|screenNavRef|focused[=:{ ]|focused\b|Ring\(|NavRing|NavSlot|NavWrap|CardKey|ChooserKey|ChooserRow|chooserSlot|slotRing|subRing|subOn\(|navOn|tuneOn\(|hdrFocus|refusalHl|pickItems\[pickFocus\]|nav\.(key|field)\(|keyHl\(|bmSlot|slot\??\.on|\bhl\b|isFocused|kbFocus|navFocus|focusIdx|listIdx|nav-skip:|nav-registered:|\bslot\(\(\) =>/;
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
  'src/components/DecoderPanel.tsx': 3,         // header run arrows + the popup's options; the spot rows joined beta92 (spotsNav)
  'src/components/FreqModal.tsx': 12,           // bookmarks-pane extras: magic key, DAB rows, EiBi, sync, delete, import (SHOULD)
  'src/components/LocalHardwarePanel.tsx': 32,  // to be rebuilt on usePanelNav (SHOULD); 35 → 32 when slot(() => …) — the panel's own D-pad bank — was recognised (11-18)
  'src/components/MenuSheet.tsx': 9,            // sideways selector nudges, sub-page back row, drop-downs (SHOULD)
  'src/components/RecordingsOverlay.tsx': 5,    // share / delete / seek (SHOULD)
  'src/screens/SDRScreen.tsx': 2,               // owner notice + its ✕ (NICE)
  'src/screens/TunerScreen.tsx': 8,             // ♥, KEYS/DRUMS, PAUSED, band and AF chips (SHOULD)
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
                 'src/components/AppAlert.tsx', 'src/components/DabPanel.tsx',
                 // ★ main's "Trust this server?" (GitHub #52), joined to the overlay rule in the 11.0.9 merge (audit §7).
                 'src/components/TlsTrustPrompt.tsx',
                 // ★★★ beta96 the server entry — made for TVs: every key a ServerKey (TV remote focus + the pad bus + a ring)
                 'src/components/ServerKey.tsx', 'src/screens/ServerChoiceScreen.tsx', 'src/screens/SimpleServerScreen.tsx',
                 'src/screens/SetupElsewhereScreen.tsx', 'src/components/ServerTopBar.tsx',
                 'src/components/DeviceCredentials.tsx', 'src/components/DeviceReset.tsx']) {
  const list = report[f] ?? unregistered(path.join(root, f));
  ok(list.length === 0, `${f}: every touchable registered`, list.map((l) => '      ' + l).join('\n'));
}

// ★★ beta96 HALF-OPEN BIG (iPhone Duo): the list decoders draw into TWO panels either side of the fold. Both halves must
//    stay on the pad: every row-bearing half is handed the box's list focus (so the pad's index walks on across the fold
//    and the half that draws the row brings it into view), the scanner's split keeps its one nav list, and the box's own
//    entry (screenNavRef 'decoderBox') stays in the upper panel, which is always there.
{
  const dp = readFileSync(path.join(root, 'src/components/DecoderPanel.tsx'), 'utf8');
  const lowerAt = dp.indexOf('const lowerBody = !splitOn'), upperAt = dp.indexOf('const upperBody = !splitOn');
  const upperSrc = dp.slice(upperAt, lowerAt), lowerSrc = dp.slice(lowerAt, dp.indexOf('\n  return (', lowerAt));
  const FOCUS = "focusIdx={kbZone === 'list' ? listIdx : -1}";
  for (const v of ['CwView', 'JttyView', 'DscView', 'AprsView']) {
    const tag = (src: string) => { const i = src.indexOf(`<${v} `); return i < 0 ? '' : src.slice(i, src.indexOf('/>', i)); };
    ok(tag(upperSrc).includes(FOCUS), `half-open: ${v}'s upper half takes the pad's list focus`);
    if (v !== 'AprsView') ok(tag(lowerSrc).includes(FOCUS), `half-open: ${v}'s lower half takes the pad's list focus (rows continue across the fold)`);
  }
  ok(/<ScannerPanel [^>]*navRef=\{scanNav\}[^>]*split=\{scanSplit\}/.test(upperSrc) && lowerSrc.includes('<PanelSlotOutlet slot={scanSlot}'),
     'half-open: the scanner keeps ONE nav list across both panels (one instance, the lower half through its slot)');
  ok(/screenNavRef\('decoderBox'/.test(dp.slice(dp.indexOf('<DecoderShell bottom'))), 'half-open: the box\'s pad entry is in the upper panel (the shell\'s children)');
  // The halves' own touchables carry the focus ring like the one box's.
  for (const f of ['src/components/CwView.tsx', 'src/components/JttyView.tsx', 'src/components/DscView.tsx', 'src/components/AprsView.tsx']) {
    const t = readFileSync(path.join(root, f), 'utf8');
    ok(/focusIdx/.test(t) && /part|linked/.test(t), `${f}: drawn by half, the focus ring by the pad's index`);
  }
}
// ★★★ beta96 b96-setupux THE SERVER SCREEN ON A TV REMOTE (Stuart 11-26: "scrolling the long screen with a remote took ages").
//    Every section's header is ONE D-pad stop with its own focus ring; a closed section mounts nothing, so with the sections
//    closed the remote steps header to header; OK opens one and down walks into it. OPEN ALL / CLOSE ALL are stops too.
{
  const f = 'src/components/SetupSection.tsx';
  const list = unregistered(path.join(root, f));
  ok(list.length === 0, `${f}: every touchable registered (the header, OPEN ALL, CLOSE ALL)`, list.map((l) => '      ' + l).join('\n'));
  const t = readFileSync(path.join(root, f), 'utf8');
  ok((t.match(/focusable=\{Platform\.OS === 'android'\}/g) || []).length >= 2, `${f}: the header and the all-buttons are focusable on Android (the TV)`);
  ok(/\{open && <View style=\{s\.body\}>\{children\}<\/View>\}/.test(t), `${f}: a closed section mounts nothing — the D-pad steps header to header`);
  ok(/onFocus=\{\(\) => setFocused\(true\)\}/.test(t) && /focused && \{ backgroundColor: C\.keySelBg \}/.test(t), `${f}: the header draws its focus ring`);
  const sm = readFileSync(path.join(root, 'src/screens/ServerModeScreen.tsx'), 'utf8');
  // ★★★ beta96 FULL IS TABBED: START (the top bar) → the SERVER | RADIO tabs → OPEN ALL / CLOSE ALL → the tab's headers
  const iTop = sm.indexOf('{serverChoiceTop}'), iTabs = sm.indexOf('<SetupTabs'), iAll = sm.indexOf('<SectionAllRow'),
        iSrv = sm.indexOf("{tabNow === 'server' && secRender(serverSecs, SERVER_ORDER)}"),
        iRad = sm.indexOf("{tabNow === 'radio' && secRender(radioSecs, RADIO_ORDER)}");
  ok(iTop > 0 && iTop < iTabs && iTabs < iAll && iAll < iSrv && iSrv < iRad,
     'ServerModeScreen: D-pad order — START, the SERVER | RADIO tabs, OPEN ALL / CLOSE ALL, then the chosen tab\'s headers');
  ok((t.match(/focusable=\{Platform\.OS === 'android'\}/g) || []).length >= 3 && /accessibilityRole="tab"/.test(t)
     && /onFocus=\{\(\) => setFocused\(t\.id\)\}/.test(t) && /borderWidth: foc \? 2 : 1/.test(t),
     `${f}: each tab is a focusable D-pad stop with its own focus ring`);
  ok(/tabs\.map\(\(t\) =>/.test(t) && /onPress=\{\(\) => onTab\(t\.id\)\}/.test(t), `${f}: a tab press switches the tab`);
  ok(/setFullTab\(ref\.first\.group\)/.test(sm), 'ServerModeScreen: a refused START selects the offending tab');
  ok(/<SetupSection key=\{s\.key\} sectionKey=\{s\.key\}/.test(sm), 'ServerModeScreen: every section is drawn through SetupSection (one header each)');
}
// ★★★ beta96 maps2 THE AMATEUR MAP's TABS (SPOTS · APRS · MESH · BEACONS): TrackLiveList over the 'digi' page — drawn by React
//    Native so the pad reaches it — and the window keys (DigiMapKeys) above the map. ONE pad order for both: the top nav owner
//    takes the pad, so two owners would leave the keys unreachable while the list is up. The tab keys and the rows are
//    TrackLiveList's (RangeKey / Row — useNavButton + NavRing, counted above).
{
  const mo = readFileSync(path.join(root, 'src/components/MapOverlay.tsx'), 'utf8');
  const dk = readFileSync(path.join(root, 'src/components/DigiMapKeys.tsx'), 'utf8');
  // ★★★ beta97 the AMATEUR map is on the MAP CARDS: ONE pad order (MapChrome's) over its chips, cards and LIST — the old list
  //     (TrackLiveList, DELETED) and the window keys' shared order are gone
  ok(/\{amCards && !!amList && !!pageUri && !packError && \(\s*<MapChrome /.test(mo) && !/amNav|amKeysNav/.test(mo), 'AMATEUR: the MAP CARDS own its one pad order (no second owner)');
  ok(/tabs: amList\.tabs, tab: amList\.tab, onTab: \(id\) => setAmPick\(id as AmLayer\)/.test(mo), 'AMATEUR: the tabs and rows are the LIST drawer\'s (pad keys)');
  ok(/usePanelNav\(true, \{ flat: true \}\)/.test(dk) && /function Key[^]*?useNavButton\(onPress\)/.test(dk), 'DigiMapKeys (the maps off the cards): its panel keys are pad keys');
  ok(/route: amSel && amateur\?\.mesh\.some/.test(mo) && /function ListDrawer/.test(readFileSync(path.join(root, 'src/components/MapChrome.tsx'), 'utf8')), 'the mesh ROUTE key is a pad key in the LIST drawer (ISM and AMATEUR maps)');
  // ★★ beta96 (Stuart 11-26) the NODES list's NEAREST | LATEST key: RangeKeys in the tabs' NavRow, handed through by MapOverlay,
  //    given by the ISM live map and the gallery's map
  const sd = readFileSync(path.join(root, 'src/screens/SDRScreen.tsx'), 'utf8');
  // ★★★ beta97 the track maps' LIST is the MAP CONTROL CARDS' drawer (MapChrome): ONE sort toggle in its foot bar, a pad key
  const mc = readFileSync(path.join(root, 'src/components/MapChrome.tsx'), 'utf8');
  ok(/pane === 'list' && !!L\.sort && \(\s*<ChipKey /.test(mc) && /L\.onSort\?\.\(L\.sort === 'nearest' \? 'latest' : 'nearest'\)/.test(mc),
     'MapChrome LIST: the sort (NEAREST | LATEST) is ONE toggle, a pad key in the foot bar');
  ok(/sort: t\.sort \?\? null, onSort: t\.onSort/.test(mo), 'MapOverlay: the drawer is handed the sort key');
  ok(/const nav = usePanelNav\(p\.visible, \{ flat: true, onBack:/.test(mc) && /if \(card\) \{ setCard\(null\); return true; \}/.test(mc) && /if \(listMax\) \{ setListMax\(false\)/.test(mc),
     'MapChrome: one pad order; BACK folds a card, then un-maximises LIST, then passes on (the map\'s exit)');
  ok(/addKeyListener\('down'/.test(mc) && /k === 'ZoomIn'/.test(mc) && /p\.onZoom\(1\)/.test(mc), 'MapChrome: the jog / right stick zooms the map (tuning only through TUNE)');
  ok((sd.match(/sort: meshSortOffered\(meshSort, (trkRx|rx)\), onSort: meshSortSet, sortWhat: 'nodes'/g) || []).length === 2,
     'SDRScreen: the ISM live map and the gallery map both give the NODES sort key');
}
process.exit(fails ? 1 : 0);
