/**
 * ★★★ WORKLETS AS THE UI THREAD RUNS THEM — the LED VU crash of 11 B7.
 *
 * Choosing SIGNAL METER → LED VU killed the app instantly, and because the setting persists it killed
 * it on every entry into a receiver. The error (Android logcat, release build):
 *
 *     ReferenceError: Property 'PEAK_HOLD_MS' doesn't exist
 *         at peakStep_metersTs3 … at LedVuTsx3 (the frame callback)
 *
 * `peakStep(p, top, nowMs, holdMs = PEAK_HOLD_MS)` — a DEFAULT PARAMETER naming a module constant. The
 * worklets Babel plugin captures the constant correctly, but it unpacks the capture INSIDE the body
 * (`const { PEAK_HOLD_MS } = this.__closure;`), and a default initialiser runs BEFORE the body. On the
 * JS thread the module scope is there and it works, which is why every plain-JS test of meters.ts
 * passed; on the UI thread there is no module scope and it throws, inside a frame callback, which is a
 * native abort.
 *
 * So this test runs the PLUGIN'S OUTPUT, not our source:
 *   1. every file under src/ that has worklets is compiled with the app's own plugin
 *      (react-native-reanimated/plugin, as babel.config.js), and every worklet it emits is checked:
 *      a default parameter may not name anything the worklet captures;
 *   2. the meters' worklets and the LED VU's / analogue meter's FRAME CALLBACKS are rebuilt from the
 *      emitted code in an empty context (no module scope — only `this.__closure`, as on the UI
 *      thread) and run for real: first frame, steady LEDs, squelch muting, silence to full scale.
 *
 * Run: node --no-warnings scripts/test_worklet_defaults.mjs   (run-tests.sh does)
 */
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(path.join(ROOT, 'package.json'));
const babel = require('@babel/core');
const parser = require('@babel/parser');
const traverse = require('@babel/traverse').default;

let fails = 0, passes = 0;
const fail = (msg) => { fails++; console.error(`FAIL ${msg}`); };
const pass = () => { passes++; };

function compile(file, commonjs = false) {
  const src = fs.readFileSync(file, 'utf8');
  return babel.transformSync(src, {
    filename: file, babelrc: false, configFile: false, sourceMaps: false,
    presets: [[require.resolve('@babel/preset-typescript'), { isTSX: true, allExtensions: true }]],
    plugins: [
      [require.resolve('@babel/plugin-syntax-jsx')],
      require.resolve('react-native-reanimated/plugin'),
      ...(commonjs ? [require.resolve('@babel/plugin-transform-modules-commonjs')] : []),
    ],
  }).code;
}

/** Every worklet the plugin emitted: its `code` string (what the UI runtime evaluates). */
function workletCodes(compiled) {
  const ast = parser.parse(compiled, { sourceType: 'module', plugins: ['jsx'] });
  const out = [];
  traverse(ast, {
    VariableDeclarator(p) {
      const id = p.node.id;
      if (id.type !== 'Identifier' || !/^_worklet_\d+_init_data$/.test(id.name)) return;
      const init = p.node.init;
      if (init?.type !== 'ObjectExpression') return;
      const code = init.properties.find(q => q.key?.name === 'code' || q.key?.value === 'code');
      if (code?.value?.type === 'StringLiteral') out.push(code.value.value);
    },
  });
  return out;
}

/** The worklet's own function, its captured names, and the identifiers each default initialiser reads. */
function analyse(code) {
  const ast = parser.parse(`(${code})`, { sourceType: 'script' });
  let fn = null;
  traverse(ast, { Function(p) { if (!fn) { fn = p; p.stop(); } } });
  const name = fn.node.id?.name ?? '(anonymous)';
  const captured = new Set();
  for (const st of fn.node.body.body ?? []) {
    if (st.type !== 'VariableDeclaration') continue;
    for (const d of st.declarations) {
      const isClosure = d.init?.type === 'MemberExpression' && d.init.object.type === 'ThisExpression'
        && d.init.property.name === '__closure';
      if (isClosure && d.id.type === 'ObjectPattern') {
        for (const pr of d.id.properties) if (pr.value?.type === 'Identifier') captured.add(pr.value.name);
      }
    }
  }
  const defaults = [];
  fn.get('params').forEach(pp => {
    if (!pp.isAssignmentPattern()) return;
    const reads = new Set();
    pp.get('right').traverse({
      Identifier(ip) { if (ip.isReferencedIdentifier()) reads.add(ip.node.name); },
    });
    if (pp.get('right').isIdentifier()) reads.add(pp.node.right.name);
    defaults.push({ param: pp.node.left.name ?? '?', reads: [...reads] });
  });
  return { name, captured, defaults };
}

// ── 1. Every worklet in src/: no default parameter may read a capture ─────────
const HOOKS = /'worklet'|"worklet"|useFrameCallback|useDerivedValue|useAnimated(Style|Props|Reaction|ScrollHandler|GestureHandler)|runOnUI|scheduleOnUI|Gesture\./;
function* walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    else if (/\.(ts|tsx)$/.test(e.name) && !e.name.endsWith('.d.ts')) yield p;
  }
}
let scanned = 0, worklets = 0;
for (const file of walk(path.join(ROOT, 'src'))) {
  if (!HOOKS.test(fs.readFileSync(file, 'utf8'))) continue;
  scanned++;
  let codes;
  try { codes = workletCodes(compile(file)); }
  catch (e) { fail(`${path.relative(ROOT, file)} did not compile with the worklets plugin: ${e.message}`); continue; }
  for (const code of codes) {
    worklets++;
    const { name, captured, defaults } = analyse(code);
    for (const d of defaults) {
      const bad = d.reads.filter(r => captured.has(r));
      if (bad.length) {
        fail(`${path.relative(ROOT, file)} worklet ${name}: default parameter '${d.param}' reads ${bad.join(', ')} — `
          + `a capture, unpacked only inside the body. On the UI thread this THROWS (ReferenceError). `
          + `Take the parameter optional and write \`x ?? ${bad[0]}\` in the body.`);
      } else pass();
    }
  }
}
if (scanned < 4 || worklets < 10) fail(`the scan found only ${scanned} files / ${worklets} worklets — is the plugin being applied?`);
else pass();

// ── 2. Run them as the UI thread does ─────────────────────────────────────────
/** Load meters.ts (compiled with the plugin) on the JS side, to get each worklet's code + closure. */
const metersFile = path.join(ROOT, 'src/constants/meters.ts');
const mod = { exports: {} };
vm.runInThisContext(`(function (exports, require, module) {${compile(metersFile, true)}\n})`)(mod.exports, require, mod);
const M = mod.exports;

/** Rebuild a worklet from its emitted code in a context with NO module scope, bound to its closure —
 *  nested worklets in the closure rebuilt the same way (what the worklets runtime does). */
const uiContext = vm.createContext({});
function onUi(fn, closureOverride) {
  const code = fn.__initData?.code;
  if (!code) throw new Error('not a worklet');
  const f = new vm.Script(`(${code})`).runInContext(uiContext);
  const closure = {};
  for (const [k, v] of Object.entries(closureOverride ?? fn.__closure ?? {})) {
    closure[k] = typeof v === 'function' && v.__initData ? onUi(v) : v;
  }
  return (...a) => f.apply({ __closure: closure }, a);
}

function tryRun(what, f) {
  try { f(); pass(); }
  catch (e) { fail(`${what} threw on the UI thread: ${e.name}: ${e.message}`); }
}

// Each meters worklet, with the arguments the meters actually pass (no optional ones).
const CALLS = {
  vuPos:          f => f(0.5),
  ringSegment:    f => f(0.43),
  peakStep:       f => f({ idx: -1, at: 0 }, 3, 16),
  phi:            f => f(0.3),
  edgeBrightness: f => f(20, 22, 3),
  eyeStep:        f => f(0.2, 0.8, 16),
  steadyLit:      f => f(false, 20, 22),
  segmentTarget:  f => { f(4, 5.2, 0.3, true, false, false); f(4, 5.2, 0.3, false, false, false); f(4, 5.2, 0.3, false, true, true); },
  needleX:        f => f(4.2, 200),
  peakNeedleStep: f => f({ pos: 3, heldMs: 0 }, 2, 16),
};
for (const [name, v] of Object.entries(M)) {
  if (typeof v !== 'function' || !v.__initData) continue;
  if (!CALLS[name]) { fail(`meters.ts worklet ${name} is not exercised here — add it to CALLS`); continue; }
  tryRun(`meters.${name}`, () => CALLS[name](onUi(v)));
}

/** A frame callback from a component, rebuilt from the plugin's output and given fake shared values. */
function frameCallback(file, marker) {
  const code = workletCodes(compile(path.join(ROOT, file))).find(c => c.includes(marker));
  if (!code) { fail(`${file}: no worklet containing ${marker} — has the frame callback moved?`); return null; }
  return { code, captured: [...analyse(code).captured] };
}
const sv = (value) => ({ value });
/** The meters catch their own throw and hand it to `onFault` via scheduleOnRN (ControlsBar MeterHousing)
 *  — so a fault here is a CALL, not a throw, and it fails the test just the same. */
function runFrames(what, fc, shared, extra = {}) {
  if (!fc) return;
  shared.faulted ??= sv(0);
  extra = { ...extra,
    onFault: (m) => fail(`${what} reported a fault (the app would fall back to the bar): ${m}`),
    scheduleOnRN: (fn, ...a) => fn(...a) };
  const closure = {};
  for (const k of fc.captured) {
    if (k in shared) closure[k] = shared[k];
    else if (k in extra) closure[k] = extra[k];
    else if (k in M) closure[k] = M[k];
    else { fail(`${what}: captures '${k}', which this harness does not know — teach it`); return; }
  }
  const cb = onUi({ __initData: { code: fc.code }, __closure: closure });
  let t = 1000;
  tryRun(`${what} first frame (no timeSincePreviousFrame)`, () => cb({ timestamp: t, timeSincePreviousFrame: null }));
  return (level, frames) => {
    for (let n = 0; n < frames; n++) {
      t += 16.7;
      shared.__set?.(level, n);
      cb({ timestamp: t, timeSincePreviousFrame: 16.7 });
    }
  };
}

// The LED VU (LedVu.tsx): every mode it can be in.
{
  const S = {
    muPos: sv(0), sigma: sv(0), steadySv: sv(0), muting: sv(0),
    bright: sv(new Array(M.VU_SEGMENTS).fill(0)), litState: sv(new Array(M.VU_SEGMENTS).fill(0)),
    peakIdx: sv(-1), peakAt: sv(0),
  };
  S.__set = (level) => { S.muPos.value = M.vuPos(level); S.sigma.value = 0.3; };
  const fc = frameCallback('src/components/LedVu.tsx', 'segmentTarget(');
  const run = runFrames('LedVu frame callback', fc, S, { thresholds: M.VU_THRESHOLDS });
  if (run) {
    for (const [steady, muting] of [[0, 0], [1, 0], [0, 1], [1, 1]]) {
      S.steadySv.value = steady; S.muting.value = muting;
      tryRun(`LedVu frames steady=${steady} muting=${muting}`, () => {
        for (const lvl of [0, 0.35, 0.8, 1, 0.1, 0]) run(lvl, 40);
      });
    }
    // …and the LEDs actually light: full scale for a second lights all ten.
    S.steadySv.value = 0; S.muting.value = 0;
    tryRun('LedVu frames at full scale', () => run(1, 60));
    const lit = S.bright.value.filter(b => b > 0.9).length;
    if (lit === M.VU_SEGMENTS) pass(); else fail(`LedVu at full scale lit ${lit} of ${M.VU_SEGMENTS}`);
  }
}

// The analogue meter (EdgeMeter.tsx): the peak needle's frame callback.
{
  const S = { needle: sv(0), peak: sv(0), held: sv(0) };
  S.__set = (level) => { S.needle.value = M.vuPos(level); };
  const fc = frameCallback('src/components/EdgeMeter.tsx', 'peakNeedleStep(');
  const run = runFrames('EdgeMeter frame callback', fc, S);
  if (run) tryRun('EdgeMeter frames', () => { for (const lvl of [0, 0.9, 0.2, 0]) run(lvl, 120); });
}

console.log(`worklets as the UI thread runs them: ${passes} passed, ${fails} failed (${scanned} files, ${worklets} worklets)`);
process.exit(fails ? 1 : 0);
