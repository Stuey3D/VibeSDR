/**
 * ★★ V12 THE SCANNER — the body of the DECODER BOX when SCANNER is the decoder (docs/v12/SCANNER.md). Stuart, 2026-10-27:
 *    "scanner is going to be another decoder box item." So it lives in DecoderPanel's box — same open / minimise / BIG,
 *    same pad and keyboard rules (the box's list zone walks the items below, Enter presses) — and the header carries the
 *    title SCANNER and the three source keys BAND · BOOKMARKS · MANUAL (DecoderPanel draws those). This is the body:
 *
 *    status   ● STOPPED  128.590 MHz – London South Information · S5 auto
 *    live     START/STOP · HOLD · SKIP · IGNORE   (+ TAG in MANUAL)       ← exactly three live controls, plus start
 *    source   BAND: Lo / Hi / step / mode (+ a band list that fills them) · BOOKMARKS: the groups · MANUAL: the tags
 *    found    128.590 MHz – London South Information      [IGNORE]       ← Ignore is THE row action; tune / → tag /
 *             (Ignore? on a repeat offender)                                → bookmark are secondary (tap the row)
 *    settings ☑ Until the signal stops (hang) · ☐ Time limit per signal · threshold AUTO / manual · ☐ Record on stop
 *    presets  SAVE AS SCAN… · the saved ones · the bundled starters (CLONE)
 *
 * ★★ VOICEOVER FROM DAY ONE (brief §7.5): every key has a label and a hint, the steppers are `adjustable` (swipe up /
 *    down), each row reads as a sentence, and the engine announces every state change (useScanner → AccessibilityInfo).
 * ★ The decoder font throughout (DecoderShell) — the display font is for the main controls only.
 * ★ Pure logic lives in services/scanner.ts; this file only draws it.
 */
import React, { useEffect, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { formatReading } from '../constants/meters';
import { usePadField } from './ControllerKeyboard';
import {
  type ScannerController, type BandChoice, type ScanPreset, type FoundSignal, type MeterMode, type ScanTag,
  freqText, modeText, nameTolerance, sMeterDetents, snapSMeter, setTrigger, HANG_MIN_MS, HANG_MAX_MS, LIMIT_MIN_MS,
  LIMIT_MAX_MS, defaultTagLabel, scanRowText, type RangeUnit,
} from '../services/scanner';
import type { SDRMode } from '../services/sdrProtocol';

export interface ScannerNav { len: number; press: (i: number) => void }

export interface ScannerPanelProps {
  ctl: ScannerController;
  /** Bumped by the hook on every engine change — the panel re-reads `ctl`. */
  rev: number;
  meterMode: MeterMode;
  bands: BandChoice[];
  bookmarkGroups: string[];
  bookmarkGroup: string;
  onBookmarkGroup: (g: string) => void;
  presets: ScanPreset[];
  onLoadPreset: (p: ScanPreset) => void;
  onSavePreset: (name: string) => void;
  onDeletePreset: (id: string) => void;
  onClonePreset: (p: ScanPreset) => void;
  /** Snapshot the current VFO into a tag (frequency, mode, passband, noise floor). */
  onTagHere: () => void;
  onTune: (hz: number, mode: SDRMode) => void;
  onBookmark: (hz: number, mode: SDRMode, name?: string) => void;
  onTagFound: (f: FoundSignal) => void;
  nameFor: (hz: number, tolHz: number) => string | undefined;
  /** The audio engine can record here (record-on-stop is offered only then). */
  canRecord: boolean;
  bodyStyle?: object;
  /** The decoder box's list focus (-1 = none) and the nav it walks. */
  focusIdx: number;
  navRef: React.MutableRefObject<ScannerNav>;
}

const MODES: SDRMode[] = ['am', 'nfm', 'usb', 'lsb', 'cwu', 'wfm'];
const STEPS = [500, 1000, 2500, 5000, 6250, 8333, 9000, 10000, 12500, 25000, 100000];


function stateWord(s: string): string {
  return s === 'SETTLING' || s === 'SAMPLING' ? 'SCANNING' : s;
}

export default function ScannerPanel(p: ScannerPanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const c = p.ctl;
  const [showSettings, setShowSettings] = useState(false);
  const [showBands, setShowBands] = useState(false);
  const [openRow, setOpenRow] = useState<number | null>(null);
  const [saving, setSaving] = useState<string | null>(null);
  const [undo, setUndo] = useState<{ text: string; fn: () => void } | null>(null);
  useEffect(() => { if (!undo) return; const t = setTimeout(() => setUndo(null), 6000); return () => clearTimeout(t); }, [undo]);
  // ★ The Lo / Hi fields are text while typed; the engine only ever sees a parsed number.
  const [loTxt, setLoTxt] = useState(''); const [hiTxt, setHiTxt] = useState('');
  // ★ 5 decimals: an 8.33 kHz channel (118.00833) must read back as itself, not as 118.0083.
  const mhzTxt = (hz: number) => (hz > 0 ? (hz / 1e6).toFixed(5).replace(/0+$/, '').replace(/\.$/, '') : '');
  const syncText = () => { setLoTxt(c.band ? mhzTxt(c.band.loHz) : ''); setHiTxt(c.band ? mhzTxt(c.band.hiHz) : ''); };
  useEffect(() => {
    syncText();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [c.band?.loHz, c.band?.hiHz]);

  /* ★ A PAD: Enter on Lo / Hi / the name opens the controller keyboard (frequency in MHz, text); keyboard or touch: the
   *  cursor goes in the field as always (ControllerKeyboard usePadField). */
  const loRef = useRef<TextInput | null>(null), hiRef = useRef<TextInput | null>(null), nameRef = useRef<TextInput | null>(null);
  const live = useRef({ loTxt, hiTxt, saving }); live.current = { loTxt, hiTxt, saving };
  const loField = usePadField({ ref: loRef, mode: 'frequency', title: 'Scan from (MHz)', unit: () => 'mhz', unitLocked: true,
    getValue: () => live.current.loTxt, setValue: setLoTxt, onSubmit: (v, u) => { applyRange(v, live.current.hiTxt, { lo: u }); } });
  const hiField = usePadField({ ref: hiRef, mode: 'frequency', title: 'Scan to (MHz)', unit: () => 'mhz', unitLocked: true,
    getValue: () => live.current.hiTxt, setValue: setHiTxt, onSubmit: (v, u) => { applyRange(live.current.loTxt, v, { hi: u }); } });
  const nameField = usePadField({ ref: nameRef, mode: 'text', title: 'Name this scan',
    getValue: () => live.current.saving ?? '', setValue: (v) => setSaving(v),
    onSubmit: (v) => { if (!v.trim()) return 'Enter a name.'; p.onSavePreset(v.trim()); setSaving(null); } });

  const running = c.running();
  const cfg = c.config;
  const step = c.band?.stepHz;
  const cur = c.current;
  const curName = cur ? p.nameFor(cur.frequency, nameTolerance(cur, c.source === 'band' ? step : undefined)) : undefined;
  const fam = p.meterMode === 'snr' ? 'snr' : 'dbfs';
  const thr = c.effectiveThreshold();
  const thrText = Number.isFinite(thr) ? formatReading(p.meterMode, thr) : '—';
  const found = c.foundRows();

  // ── The nav list (the decoder box's list zone walks this; Enter presses) ─────────────────────────────────────
  const items: { key: string; press: () => void }[] = [];
  const add = (key: string, press: () => void) => { items.push({ key, press }); return key; };
  const hl = (key: string) => (items[p.focusIdx]?.key === key ? { borderColor: tk.focus, borderWidth: 2 } : null);

  /* ★★ ONE COMMIT (scanner.ts commitRange / parseScanRange). A field losing the cursor commits quietly; START commits
   *  out loud, from the fields' LIVE text — on the Mac a click on START never ends the field's editing, so START used to
   *  scan the last band committed rather than the one on screen (Stuart, 2026-10-28: fields 110 / 130, scan refused as
   *  "130.000 MHz–110000.000 MHz"). The fields are rewritten from the band afterwards, so a swapped Lo / Hi reads back
   *  in order. */
  const applyRange = (loT: string, hiT: string, o: { lo?: RangeUnit; hi?: RangeUnit; quiet?: boolean } = { quiet: true }) => {
    const done = c.commitRange(loT, hiT, o);
    if (done) syncText();
    return done;
  };
  const startScan = (reachableOnly = false) => {
    if (c.source === 'band' && !applyRange(live.current.loTxt, live.current.hiTxt, {})) return;
    c.start(reachableOnly ? { reachableOnly: true } : {});
  };

  const doIgnore = () => {
    const was = c.current;
    const u = c.ignoreCurrent();
    if (u && was) setUndo({ text: `Ignoring ${freqText(was.frequency)}`, fn: u });
  };

  // ── Rows ─────────────────────────────────────────────────────────────────────────────────────────────────────
  const key = (k: string, label: string, onPress: () => void, o: { on?: boolean; a11y?: string; hint?: string; big?: boolean; dim?: boolean } = {}) => {
    add(k, onPress);
    return (
      <TouchableOpacity key={k} onPress={onPress} activeOpacity={0.7}
        style={[o.big ? s.bigKey : s.chip, o.on && s.chipOn, o.dim && s.dim, hl(k)]}
        accessibilityRole="button" accessibilityLabel={o.a11y ?? label} accessibilityHint={o.hint}
        accessibilityState={o.on !== undefined ? { selected: !!o.on } : undefined}>
        <Text style={[o.big ? s.bigKeyTxt : s.chipTxt, o.on && s.chipTxtOn]}>{label}</Text>
      </TouchableOpacity>
    );
  };
  /** A −/+ stepper that VoiceOver reads as one adjustable control. */
  const stepper = (k: string, label: string, value: string, dec: () => void, inc: () => void, a11y: string) => {
    add(`${k}-`, dec); add(`${k}+`, inc);
    return (
      <View style={s.stepRow} accessible accessibilityRole="adjustable" accessibilityLabel={a11y} accessibilityValue={{ text: value }}
            accessibilityActions={[{ name: 'increment' }, { name: 'decrement' }]}
            onAccessibilityAction={(e) => (e.nativeEvent.actionName === 'increment' ? inc() : dec())}>
        <Text style={s.lbl}>{label}</Text>
        <TouchableOpacity style={[s.chip, s.stepKey, hl(`${k}-`)]} onPress={dec}><Text style={s.chipTxt}>−</Text></TouchableOpacity>
        <Text style={s.val}>{value}</Text>
        <TouchableOpacity style={[s.chip, s.stepKey, hl(`${k}+`)]} onPress={inc}><Text style={s.chipTxt}>+</Text></TouchableOpacity>
      </View>
    );
  };
  const tick = (k: string, label: string, on: boolean, flip: () => void, hint: string) => {
    add(k, flip);
    return (
      <TouchableOpacity key={k} style={[s.tickRow, hl(k)]} onPress={flip} activeOpacity={0.7}
        accessibilityRole="checkbox" accessibilityState={{ checked: on }} accessibilityLabel={label} accessibilityHint={hint}>
        <Text style={[s.tickBox, on && s.tickOn]}>{on ? '☑' : '☐'}</Text>
        <Text style={s.tickTxt}>{label}</Text>
      </TouchableOpacity>
    );
  };

  // ── Status ───────────────────────────────────────────────────────────────────────────────────────────────────
  const stopped = c.state === 'STOPPED' || c.state === 'HOLD';
  const statusMain = !running ? 'READY'
    : c.state === 'PAUSED' ? 'PAUSED'
    : `${stateWord(c.state)}${cur ? '  ' : ''}`;
  const statusFreq = cur && running && c.state !== 'PAUSED' ? scanRowText(cur.frequency, curName) : '';
  const statusThr = `${c.needsFloor && cfg.thresholdMode === 'auto' && fam === 'dbfs' ? 'measuring the noise floor…' : thrText}${cfg.thresholdMode === 'auto' ? ' auto' : ''}`;

  const body = (
    <>
      <View style={s.statusRow} accessible accessibilityLiveRegion="polite"
            accessibilityLabel={`${statusMain}${statusFreq ? `, ${statusFreq}` : ''}. Threshold ${statusThr}`}>
        <View style={[s.dot, stopped && s.dotOn, c.state === 'PAUSED' && s.dotWarn]} />
        <Text style={s.state}>{statusMain}</Text>
        <Text style={s.statusFreq} numberOfLines={1} ellipsizeMode="tail">{statusFreq}</Text>
        <Text style={s.thr} numberOfLines={1}>{statusThr}</Text>
      </View>
      {c.state === 'PAUSED' && (
        <Text style={s.banner} accessibilityLiveRegion="assertive">
          {`${c.pauseWhy}${c.unreachable.length ? ` — out of reach: ${c.unreachable.join(', ')}` : ''}. It carries on by itself when they are back.`}
        </Text>
      )}
      {!running && !!c.refusal && (
        <View style={s.bannerBox}>
          <Text style={s.banner}>{`${c.refusal.why}${c.refusal.out.length ? `: ${c.refusal.out.join(', ')}` : ''}`}</Text>
          {c.refusal.okCount > 0 && key('reach', `START WITH ${c.refusal.okCount} REACHABLE`, () => startScan(true),
            { a11y: `Start with the ${c.refusal.okCount} reachable channels`, hint: 'Scans only what this receiver can reach now' })}
        </View>
      )}
      {!!undo && (
        <View style={s.undoRow}>
          <Text style={s.undoTxt}>{undo.text}</Text>
          {key('undo', 'UNDO', () => { undo.fn(); setUndo(null); }, { a11y: 'Undo ignore' })}
        </View>
      )}

      {/* ── the live controls: exactly three, large, plus Start/Stop (and Tag in Manual) ── */}
      <View style={s.liveRow}>
        {key('start', running ? 'STOP' : 'START', () => (running ? c.stop() : startScan()),
          { big: true, on: running, a11y: running ? 'Stop scanning' : 'Start scanning' })}
        {key('hold', c.state === 'HOLD' ? 'RESUME' : 'HOLD', () => c.hold(),
          { big: true, on: c.state === 'HOLD', dim: !running, a11y: c.state === 'HOLD' ? 'Resume scanning' : 'Hold on this frequency',
            hint: 'Stays here; timers freeze. Also the play/pause key.' })}
        {key('skip', 'SKIP', () => c.skip(),
          { big: true, dim: !running, a11y: 'Skip', hint: 'Moves on now and passes this frequency over for a few minutes. Also the next-track key.' })}
        {key('ignore', 'IGNORE', doIgnore,
          { big: true, dim: !running, a11y: 'Ignore this frequency', hint: 'The scan passes it over from now on. Undo is offered.' })}
        {c.source === 'list' && key('tag', 'TAG', p.onTagHere,
          { big: true, a11y: 'Tag this frequency', hint: 'Adds where you are tuned — frequency, mode and passband — to the list' })}
      </View>

      {/* ── the source ── */}
      {c.source === 'band' && (
        <View style={s.section}>
          <View style={s.fieldRow}>
            <Text style={s.lbl}>LO</Text>
            {void add('lo', () => loField.open())}
            <TextInput ref={loRef} {...loField.fieldProps} style={[s.input, hl('lo')]} value={loTxt} keyboardType="decimal-pad" placeholder="MHz" placeholderTextColor={tk.muted}
              onChangeText={setLoTxt} onEndEditing={() => applyRange(live.current.loTxt, live.current.hiTxt)} accessibilityLabel="Lowest frequency, megahertz" />
            {/* ★ The unit stays on screen once a number is in — the placeholder alone vanished, and "110000" (kHz) became 110 GHz. */}
            <Text style={s.unit}>MHz</Text>
            <Text style={s.lbl}>HI</Text>
            {void add('hi', () => hiField.open())}
            <TextInput ref={hiRef} {...hiField.fieldProps} style={[s.input, hl('hi')]} value={hiTxt} keyboardType="decimal-pad" placeholder="MHz" placeholderTextColor={tk.muted}
              onChangeText={setHiTxt} onEndEditing={() => applyRange(live.current.loTxt, live.current.hiTxt)} accessibilityLabel="Highest frequency, megahertz" />
            <Text style={s.unit}>MHz</Text>
            {key('bands', showBands ? 'BANDS ▴' : 'BANDS ▾', () => setShowBands((v) => !v),
              { on: showBands, a11y: 'Band list', hint: 'Fills the range, step and mode for a band in one tap' })}
          </View>
          {stepper('step', 'STEP', step ? `${(step / 1000).toFixed(step % 1000 ? 2 : 0)} kHz` : '—',
            () => { const i = STEPS.findIndex((x) => x >= (step ?? 25000)); const n = STEPS[Math.max(0, i - 1)]; c.setBand({ ...(c.band ?? { loHz: 0, hiHz: 0, mode: 'am' as SDRMode, stepHz: n }), stepHz: n }); },
            () => { const i = STEPS.findIndex((x) => x > (step ?? 25000)); const n = STEPS[i < 0 ? STEPS.length - 1 : i]; c.setBand({ ...(c.band ?? { loHz: 0, hiHz: 0, mode: 'am' as SDRMode, stepHz: n }), stepHz: n }); },
            'Scan step')}
          <View style={s.chipRow}>
            {MODES.map((m) => key(`mode:${m}`, modeText(m), () => c.setBand({ ...(c.band ?? { loHz: 0, hiHz: 0, stepHz: 25000, mode: m }), mode: m }),
              { on: c.band?.mode === m, a11y: `Mode ${modeText(m)}` }))}
          </View>
          {showBands && (
            <View style={s.chipWrap}>
              {p.bands.map((b) => key(`band:${b.name}`, b.name, () => { c.setBand(b.spec); setShowBands(false); },
                { a11y: `${b.name}, ${freqText(b.spec.loHz)} to ${freqText(b.spec.hiHz)}, ${modeText(b.spec.mode)}` }))}
            </View>
          )}
        </View>
      )}
      {c.source === 'bookmarks' && (
        <View style={s.section}>
          {p.bookmarkGroups.length ? (
            <View style={s.chipWrap}>
              {p.bookmarkGroups.map((g) => key(`grp:${g}`, g || 'UNGROUPED', () => p.onBookmarkGroup(g),
                { on: p.bookmarkGroup === g, a11y: `Bookmark group ${g || 'ungrouped'}` }))}
            </View>
          ) : <Text style={s.note}>No bookmarks on this receiver yet — save some, or use MANUAL to tag as you go.</Text>}
          {tagRows(c.tags, false)}
        </View>
      )}
      {c.source === 'list' && (
        <View style={s.section}>
          {c.tags.length ? tagRows(c.tags, true)
            : <Text style={s.note}>Tune to a conversation and press TAG. Tag another — any band, any mode — then START: the scanner hops between them and dwells on whichever is talking.</Text>}
        </View>
      )}

      {/* ── found (band) ── */}
      {c.source === 'band' && (found.live.length > 0 || found.ignored.length > 0) && (
        <View style={s.section}>
          <Text style={s.head}>FOUND</Text>
          {found.live.map((f, i) => foundRow(f, i, false))}
          {found.ignored.length > 0 && <Text style={s.head}>IGNORED</Text>}
          {found.ignored.map((f, i) => foundRow(f, 1000 + i, true))}
        </View>
      )}

      {/* ── settings ── */}
      <View style={s.section}>
        {key('settings', showSettings ? 'SETTINGS ▴' : 'SETTINGS ▾', () => setShowSettings((v) => !v), { on: showSettings, a11y: 'Scanner settings' })}
        {showSettings && (
          <>
            {tick('until', 'Until the signal stops', cfg.untilStops, () => c.setConfig(setTrigger(cfg, 'untilStops', !cfg.untilStops)),
              'Stays while the signal is there, then waits the hang time before moving on')}
            {cfg.untilStops && stepper('hang', 'HANG', `${(cfg.hangMs / 1000).toFixed(0)} s`,
              () => c.setConfig({ hangMs: Math.max(HANG_MIN_MS, cfg.hangMs - 1000) }),
              () => c.setConfig({ hangMs: Math.min(HANG_MAX_MS, cfg.hangMs + 1000) }), 'Hang time')}
            {tick('limit', 'Time limit per signal', cfg.timeLimit, () => c.setConfig(setTrigger(cfg, 'timeLimit', !cfg.timeLimit)),
              'Moves on after this long even if the signal is still there')}
            {cfg.timeLimit && stepper('lim', 'LIMIT', `${Math.round(cfg.timeLimitMs / 1000)} s`,
              () => c.setConfig({ timeLimitMs: Math.max(LIMIT_MIN_MS, cfg.timeLimitMs - (cfg.timeLimitMs > 60000 ? 30000 : 5000)) }),
              () => c.setConfig({ timeLimitMs: Math.min(LIMIT_MAX_MS, cfg.timeLimitMs + (cfg.timeLimitMs >= 60000 ? 30000 : 5000)) }), 'Time limit per signal')}
            {tick('auto', `Auto threshold (noise + ${cfg.marginDb} dB)`, cfg.thresholdMode === 'auto',
              () => c.setConfig({ thresholdMode: cfg.thresholdMode === 'auto' ? 'manual' : 'auto' }),
              'Stops on anything clearly above the noise. Untick to set your own level in your meter\'s units')}
            {cfg.thresholdMode === 'manual' && stepper('thr', 'STOP AT', formatReading(p.meterMode, cfg.threshold[fam]),
              () => bumpThr(-1), () => bumpThr(1), 'Stop threshold')}
            {p.canRecord && tick('rec', 'Record on stop', cfg.recordOnStop, () => c.setConfig({ recordOnStop: !cfg.recordOnStop }),
              'Each stop is recorded to the Recordings list')}
          </>
        )}
      </View>

      {/* ── presets ── */}
      <View style={s.section}>
        <Text style={s.head}>SCANS</Text>
        {saving !== null ? (
          <View style={s.fieldRow}>
            {void add('name', () => nameField.open())}
            <TextInput ref={nameRef} {...nameField.fieldProps} style={[s.input, s.grow, hl('name')]} value={saving} onChangeText={setSaving} autoFocus placeholder="Name"
              placeholderTextColor={tk.muted} accessibilityLabel="Name for this scan"
              onSubmitEditing={() => { if (saving.trim()) p.onSavePreset(saving.trim()); setSaving(null); }} />
            {key('save-ok', 'SAVE', () => { if (saving.trim()) p.onSavePreset(saving.trim()); setSaving(null); }, { a11y: 'Save this scan' })}
            {key('save-x', 'CANCEL', () => setSaving(null))}
          </View>
        ) : key('save', 'SAVE AS SCAN…', () => setSaving(''), { a11y: 'Save as scan', hint: 'Keeps the mode, list, ignores and settings under a name' })}
        {p.presets.map((pr) => (
          <View key={pr.id} style={s.presetRow}>
            <Text style={s.presetName} numberOfLines={1}>{pr.name}{pr.bundled ? '  ·  starter' : ''}</Text>
            {key(`load:${pr.id}`, 'LOAD', () => p.onLoadPreset(pr), { a11y: `Load ${pr.name}` })}
            {pr.bundled ? key(`clone:${pr.id}`, 'CLONE', () => p.onClonePreset(pr), { a11y: `Copy ${pr.name} to edit` })
              : key(`del:${pr.id}`, 'DELETE', () => p.onDeletePreset(pr.id), { a11y: `Delete ${pr.name}` })}
          </View>
        ))}
      </View>
    </>
  );

  function bumpThr(d: number) {
    if (p.meterMode === 'smeter') {
      const det = sMeterDetents();
      const cur0 = snapSMeter(cfg.threshold.dbfs);
      const i = det.indexOf(cur0);
      const n = det[Math.max(0, Math.min(det.length - 1, i + d))];
      c.setConfig({ threshold: { ...cfg.threshold, dbfs: n } });
    } else {
      c.setConfig({ threshold: { ...cfg.threshold, [fam]: cfg.threshold[fam] + d } });
    }
  }

  function tagRows(tags: ScanTag[], editable: boolean) {
    return tags.map((t) => {
      const nm = p.nameFor(t.frequency, nameTolerance(t)) ?? t.label;
      const live = c.current?.tagId === t.id && stopped;
      const text = `${scanRowText(t.frequency, nm && nm !== defaultTagLabel(t) ? nm : undefined)} ${modeText(t.mode)}`;
      const sk = `tag:${t.id}`;
      add(sk, () => p.onTune(t.frequency, t.mode));
      return (
        <View key={t.id} style={[s.row, live && s.rowLive, t.lockedOut && s.dim]}>
          {/* ★ The coverage dot (§7.2): amber = checked periodically (probe). M2/M3 turn it green ("watched live"). */}
          <View style={[s.cov]} accessibilityElementsHidden />
          <TouchableOpacity style={[s.rowMain, hl(sk)]} onPress={() => p.onTune(t.frequency, t.mode)} activeOpacity={0.7}
            accessibilityRole="button"
            accessibilityLabel={`${text}${t.priority ? ', priority' : ''}${t.lockedOut ? ', ignored' : ''}, checked periodically`}
            accessibilityHint="Tunes to it">
            <Text style={s.rowTxt} numberOfLines={1} ellipsizeMode="tail">{text}</Text>
          </TouchableOpacity>
          {key(`star:${t.id}`, t.priority ? '★' : '☆', () => c.toggleStar(t.id),
            { on: !!t.priority, a11y: t.priority ? 'Priority on' : 'Priority off', hint: 'A starred channel interrupts whatever is playing when it goes live' })}
          {key(`ign:${t.id}`, t.lockedOut ? 'UN-IGNORE' : 'IGNORE', () => c.toggleTagIgnored(t.id),
            { on: !!t.lockedOut, a11y: t.lockedOut ? 'Un-ignore' : 'Ignore' })}
          {editable && key(`del:${t.id}`, '✕', () => c.removeTag(t.id), { a11y: 'Remove from the list' })}
        </View>
      );
    });
  }

  function foundRow(f: FoundSignal, i: number, ignored: boolean) {
    const nm = p.nameFor(f.frequency, nameTolerance({ bwLow: 0, bwHigh: 0 }, step));
    const text = scanRowText(f.frequency, nm);
    const hint = !ignored && c.ignoreHint(f);
    const open = openRow === i;
    const rk = `found:${f.frequency}`;
    add(rk, () => setOpenRow(open ? null : i));
    return (
      <View key={rk + (ignored ? 'i' : '')}>
        <View style={[s.row, ignored && s.dim, c.current?.frequency === f.frequency && stopped && s.rowLive]}>
          <TouchableOpacity style={[s.rowMain, hl(rk)]} onPress={() => setOpenRow(open ? null : i)} activeOpacity={0.7}
            accessibilityRole="button"
            accessibilityLabel={`${text}, heard ${f.hits} ${f.hits === 1 ? 'time' : 'times'}${hint ? ', keeps stopping the scan' : ''}${ignored ? ', ignored' : ''}`}
            accessibilityHint="Shows tune, tag and bookmark">
            {/* ★★ THE NAME TRUNCATES, IGNORE NEVER MOVES (Stuart's layout): flex 1 + minWidth 0 + one line with an ellipsis
                here, and the key beside it does not shrink — so at 390 pt a long name ends "…" and Ignore stays. */}
            <Text style={s.rowTxt} numberOfLines={1} ellipsizeMode="tail">{text}</Text>
          </TouchableOpacity>
          {ignored
            ? key(`unign:${f.frequency}`, 'UN-IGNORE', () => c.unignore(f.frequency), { a11y: `Un-ignore ${freqText(f.frequency)}` })
            : key(`ignf:${f.frequency}`, hint ? 'IGNORE?' : 'IGNORE', () => {
                const u = c.ignoreTarget({ frequency: f.frequency });
                setUndo({ text: `Ignoring ${freqText(f.frequency)}`, fn: u });
              }, { on: hint, a11y: `Ignore ${freqText(f.frequency)}`, hint: 'The scan passes it over from now on' })}
        </View>
        {open && (
          <View style={s.subRow}>
            <Text style={s.meta}>{`${f.hits}× · ${ago(f.lastHeard)}`}</Text>
            {key(`ftune:${f.frequency}`, 'TUNE', () => p.onTune(f.frequency, f.mode), { a11y: 'Tune to it' })}
            {key(`ftag:${f.frequency}`, '→ TAG', () => p.onTagFound(f), { a11y: 'Add to the manual list' })}
            {key(`fbm:${f.frequency}`, '→ BOOKMARK', () => p.onBookmark(f.frequency, f.mode, nm), { a11y: 'Save as a bookmark' })}
          </View>
        )}
      </View>
    );
  }

  // ★ The nav list is rebuilt on every render (the items above register themselves in draw order); the box reads it at key time.
  p.navRef.current = { len: items.length, press: (i: number) => items[i]?.press() };

  return (
    <ScrollView style={[s.body, p.bodyStyle]} contentContainerStyle={[s.bodyIn, scrollLane]}
                keyboardShouldPersistTaps="handled">
      {body}
    </ScrollView>
  );
}

function ago(t: number): string {
  const sec = Math.max(0, Math.round((Date.now() - t) / 1000));
  if (sec < 10) return 'now';
  if (sec < 90) return `${sec} s ago`;
  return `${Math.round(sec / 60)} min ago`;
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  body:      { flexGrow: 0 },
  bodyIn:    { paddingHorizontal: 10, paddingBottom: 10, gap: 6 },
  statusRow: { flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 26 },
  dot:       { width: 8, height: 8, borderRadius: 4, backgroundColor: T.dotIdle },
  dotOn:     { backgroundColor: T.dotOn },
  dotWarn:   { backgroundColor: T.warn },
  state:     { fontFamily: F, fontSize: 12, letterSpacing: 1, color: T.label },
  statusFreq: { flex: 1, minWidth: 0, fontFamily: F, fontSize: 13, color: T.value },
  thr:       { flexShrink: 0, fontFamily: F, fontSize: 11, color: T.muted },
  banner:    { fontFamily: F, fontSize: 12, lineHeight: 17, color: T.warn },
  bannerBox: { gap: 6 },
  undoRow:   { flexDirection: 'row', alignItems: 'center', gap: 8 },
  undoTxt:   { flex: 1, fontFamily: F, fontSize: 12, color: T.value },
  liveRow:   { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  // ★ The live controls are LARGE targets (brief §7.1) — 44 pt, the platform minimum.
  bigKey:    { minHeight: 44, minWidth: 64, flexGrow: 1, justifyContent: 'center', alignItems: 'center', paddingHorizontal: 10,
               borderRadius: 8, borderWidth: 1, borderColor: T.chipBorder },
  bigKeyTxt: { fontFamily: F, fontSize: 13, letterSpacing: 1, color: T.keyText },
  section:   { gap: 6, paddingTop: 4, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: T.divider },
  head:      { fontFamily: F, fontSize: 11, letterSpacing: 1.5, color: T.label, marginTop: 2 },
  note:      { fontFamily: F, fontSize: 12, lineHeight: 17, color: T.muted },
  fieldRow:  { flexDirection: 'row', alignItems: 'center', gap: 6 },
  lbl:       { fontFamily: F, fontSize: 11, letterSpacing: 1, color: T.rowLabel },
  unit:      { fontFamily: F, fontSize: 11, color: T.rowLabel, marginLeft: -2 },
  val:       { fontFamily: F, fontSize: 13, color: T.value, minWidth: 64, textAlign: 'center' },
  input:     { minWidth: 72, flexShrink: 1, height: 34, paddingHorizontal: 8, borderRadius: 6, borderWidth: 1, borderColor: T.chipBorder,
               fontFamily: F, fontSize: 13, color: T.value },
  grow:      { flex: 1 },
  stepRow:   { flexDirection: 'row', alignItems: 'center', gap: 6 },
  stepKey:   { minWidth: 40, alignItems: 'center' },
  chipRow:   { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  chipWrap:  { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  chip:      { minHeight: 32, justifyContent: 'center', paddingHorizontal: 10, borderRadius: 6, borderWidth: 1, borderColor: T.chipBorder, flexShrink: 0 },
  chipOn:    { borderColor: T.chipBorderAct, backgroundColor: T.chipBgAct },
  chipTxt:   { fontFamily: F, fontSize: 11, letterSpacing: 1, color: T.keyText },
  chipTxtOn: { color: T.keyTextAct },
  dim:       { opacity: 0.45 },
  tickRow:   { flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 36, borderRadius: 6, borderWidth: 1, borderColor: 'transparent' },
  tickBox:   { fontSize: 18, color: T.keyText },
  tickOn:    { color: T.keyTextAct },
  tickTxt:   { flex: 1, fontFamily: F, fontSize: 13, color: T.value },
  row:       { flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 36 },
  rowLive:   { backgroundColor: T.selectedBg },
  // ★ minWidth 0 is what lets the text shrink inside a row (RN's flex default would push the keys off the edge).
  rowMain:   { flex: 1, minWidth: 0, justifyContent: 'center', minHeight: 32, borderRadius: 6, borderWidth: 1, borderColor: 'transparent', paddingHorizontal: 4 },
  rowTxt:    { fontFamily: F, fontSize: 13, color: T.value },
  cov:       { width: 7, height: 7, borderRadius: 4, backgroundColor: T.warn },
  subRow:    { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6, paddingLeft: 12, paddingBottom: 4 },
  meta:      { fontFamily: F, fontSize: 11, color: T.muted, marginRight: 4 },
  presetRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  presetName: { flex: 1, minWidth: 0, fontFamily: F, fontSize: 13, color: T.value },
});
