import React, { useMemo, useState } from 'react';
import { View, Text, TextInput, TouchableOpacity, ScrollView, StyleSheet } from 'react-native';
import Slider from '@react-native-community/slider';

/**
 * ★★★ THE BAND LISTS, AS A TABLE — the same shape the browser's setup page has always had.
 *
 * The phone offered a single free-text box ("e.g. all:25dB, fm:15dB") for settings the browser
 * builds by picking a band, typing a figure and pressing Add, with what you have added listed
 * underneath. Same server, same strings on the wire, two completely different jobs for the owner:
 * one of them has to remember a syntax, and gets no feedback until the server refuses it (Stuart,
 * 2026-08-21: "needs to be formatted the same way as the web page as a proper table").
 *
 * ★★ THE BANDS COME FROM THE SERVER, never from a copy here — `vibe_bands.h` is what actually
 *    resolves "fm" or "airband" when a limit is applied, so a second list in TypeScript would
 *    drift, and the copy that drifts is the one that quietly stops matching. Fetched through
 *    VibeLocalSdr.getBands().
 *
 * ★★ AND IT STILL SPEAKS THE SAME STRING. The value in and out is exactly what the server parses
 *    ("fm:15dB, all:25dB" or "fm, 87.5M-108M"), so nothing downstream knows this changed —
 *    the editor is a way of WRITING that string, not a new format.
 *
 * ★ Chips rather than a dropdown: a <select> is a desktop idiom, and on a phone a row you can
 *   thumb through is both faster and shows what is available without a modal.
 */

export type Band = { id: string; label: string; lo?: number; hi?: number };

type Props = {
  C: Record<string, string>;
  F: string;
  bands: Band[];
  /** The server string being edited. */
  value: string;
  onChange: (next: string) => void;
  /** 'gain' takes a band AND a ceiling; 'range' takes a band OR a typed frequency range. */
  kind: 'gain' | 'range';
  /** Shown when the list is empty — says what happens with nothing set. */
  emptyText: string;
  /** Placeholder for the free-text half. */
  placeholder: string;
  /** True when "All bands" is a sensible target (gain ceilings), false for allow/block lists. */
  allowAll?: boolean;
  /**
   * ★★★ THE TUNER'S REAL GAIN STEPS, in tenths of a dB — and a ceiling is chosen ON them, never
   *     typed. An R820T offers 29 fixed positions and nothing between them, so a typing box
   *     invites a number the radio cannot take and then snaps it somewhere else without saying so.
   *     The Pi's setup page made this mistake and was fixed for the same reason (Stuart,
   *     2026-08-17: "we had this issue before on the Pi"; again 2026-08-21: "not a type box for
   *     the gain").
   */
  gainSteps?: number[];
  /* ★★★ THE LOCK IS PER BAND — the phone's half of the setup page's "Lock this band". A ceiling can
   *   be a limit (the listener keeps the control and cannot go past it) or the SETTING (the gain
   *   sits there and there are no controls at all), and which of the two is a question about the
   *   BAND: Stuart, 2026-08-28 — "I can lock the gain on FM but allow it to be unlocked but limited
   *   for HF." Stored in its own parallel list, keyed by band, exactly as the server stores it.
   * ★ Absent props = no lock UI at all, which is what the allow/block lists want. */
  lockable?: boolean;
  locks?: string;
  onLocksChange?: (next: string) => void;
  /** HackRF only: when a band is LOCKED, the LNA's share of that band's total, 0-100. A total does
   *  not determine two stages, so a ceiling is enough to limit with and not enough to set with. */
  splittable?: boolean;
  splits?: string;
  onSplitsChange?: (next: string) => void;
};

/** A parallel per-band list ("fm:1, hf:0") as a map. The band syntax and the parser are the
 *  server's own, so nothing here has to know what a band is. */
function sideMap(value?: string): Record<string, number> {
  const out: Record<string, number> = {};
  for (const e of parse(value ?? '')) {
    const c = e.lastIndexOf(':');
    if (c >= 0) out[e.slice(0, c).trim().toLowerCase()] = parseInt(e.slice(c + 1), 10);
  }
  return out;
}
function sideWrite(m: Record<string, number>): string {
  return Object.keys(m).map(k => `${k}:${m[k]}`).join(', ');
}

/** Split the server string into entries, tolerating the spacing a person actually types. */
function parse(value: string): string[] {
  return value.split(',').map(s => s.trim()).filter(Boolean);
}

export default function BandLimitEditor(p: Props) {
  const entries = useMemo(() => parse(p.value), [p.value]);
  // ★★★ IN FREQUENCY ORDER, because that is the only order a radio person reads a band list in.
  //     The server returns them grouped by KIND — broadcast bands together, amateur bands
  //     together — so medium wave sat nowhere near long wave and the eye had to hunt (Stuart,
  //     2026-08-21: "for some reason MW was nowhwere near LW band"). Sorted here rather than in
  //     the server: the grouping is right for other callers, and this is a presentation choice.
  //  ★ A band with no edges sorts last rather than to zero — an unknown is not "at DC".
  const bands = useMemo(
    () => [...p.bands].sort((a, b) => (a.lo ?? Infinity) - (b.lo ?? Infinity)),
    [p.bands]);
  const [band, setBand] = useState('');
  const [text, setText] = useState('');
  /** ★ A range is TWO numbers, so it gets two boxes — see the note where they are drawn. */
  const [loText, setLoText] = useState('');
  const [hiText, setHiText] = useState('');
  /** Index into gainSteps for the ceiling being added. ★ Starts at the TOP: a ceiling nobody has
   *  moved should not quietly be the lowest one on the list. */
  const steps = p.gainSteps && p.gainSteps.length ? p.gainSteps : [];
  const [gainIdx, setGainIdx] = useState(Math.max(0, steps.length - 1));
  /** ★ Belongs to the ENTRY being added, not to the radio — so it starts clear each time rather
   *  than inheriting whatever the last band was given. */
  const [lockNext, setLockNext] = useState(false);
  const [splitNext, setSplitNext] = useState(50);
  const locks = useMemo(() => sideMap(p.locks), [p.locks]);

  const label = (entry: string) => {
    // ★ Show the band's real NAME, not its id — "FM broadcast" rather than "fm". The id is what
    //   travels; the label is what the owner recognised when they picked it.
    const [head, tail] = entry.split(':');
    const b = bands.find(x => x.id === head.trim().toLowerCase());
    const name = b ? b.label : head.trim();
    if (!tail) return name;
    /* ★★★ TWO STATES THAT BEHAVE DIFFERENTLY MUST NOT READ THE SAME. A locked band shows the figure
     *   and a padlock; an unlocked one says "up to" in words, rather than being the absence of a
     *   symbol. Same wording as the setup page's chips, so an owner who runs both meets one idea. */
    if (!p.lockable) return `${name} · ${tail.trim()}`;
    const locked = locks[head.trim().toLowerCase()] > 0;
    return `${name} · ${locked ? '' : 'up to '}${tail.trim()}${locked ? ' \u{1F512}' : ''}`;
  };

  const add = () => {
    const t = text.trim();
    let entry = '';
    if (p.kind === 'gain') {
      // ★ The slider decides the figure when the tuner's list is known; the box is only the
      //   fallback for a radio whose steps we have not been told.
      if (!band) return;
      const db = steps.length ? (steps[gainIdx] / 10).toFixed(1) : t;
      if (!db) return;
      entry = `${band}:${/db$/i.test(db) ? db : `${db}dB`}`;
    } else {
      // ★ A band OR a typed range — the two ways the server accepts a limit, and the owner should
      //   not have to know which one they are using.
      const lo = loText.trim(), hi = hiText.trim();
      entry = band || (lo && hi ? `${lo}-${hi}` : '');
      if (!entry) return;
    }
    // ★ Replacing rather than appending a duplicate: two ceilings for one band is a contradiction
    //   the server would resolve silently, and silently is the problem.
    const head = entry.split(':')[0].trim().toLowerCase();
    const kept = entries.filter(e => e.split(':')[0].trim().toLowerCase() !== head);
    p.onChange([...kept, entry].join(', '));
    if (p.lockable && p.onLocksChange) {
      const m = sideMap(p.locks);
      if (lockNext) m[head] = 1; else delete m[head];
      p.onLocksChange(sideWrite(m));
    }
    if (p.splittable && p.onSplitsChange) {
      const m = sideMap(p.splits);
      // ★ Only a LOCKED band has a split to keep: as a limiter the listener still chooses it, so
      //   storing one would be a figure nothing reads.
      if (lockNext) m[head] = splitNext; else delete m[head];
      p.onSplitsChange(sideWrite(m));
    }
    setBand(''); setText(''); setLoText(''); setHiText(''); setLockNext(false);
  };

  const remove = (entry: string) => {
    p.onChange(entries.filter(e => e !== entry).join(', '));
    // ★★ AND ITS COMPANIONS. A lock or a split for a band with no ceiling is a figure nothing
    //    reads — invisible here and still in the config, which is how a setting comes back from
    //    the dead when the band is added again later.
    const head = entry.split(':')[0].trim().toLowerCase();
    if (p.onLocksChange)  { const m = sideMap(p.locks);  delete m[head]; p.onLocksChange(sideWrite(m)); }
    if (p.onSplitsChange) { const m = sideMap(p.splits); delete m[head]; p.onSplitsChange(sideWrite(m)); }
  };

  const chip = (active: boolean) => ({
    borderWidth: 1, borderRadius: 8, paddingVertical: 8, paddingHorizontal: 12,
    borderColor: active ? p.C.green : p.C.border,
    backgroundColor: active ? `${p.C.green}18` : 'transparent',
  });

  return (
    <View style={{ marginTop: 8 }}>
      {/* ── pick a band ───────────────────────────────────────────────────
          ★★★ TWO LINES THAT WRAP, NOT ONE THAT SCROLLS SIDEWAYS. A horizontal strip cut the last
              chip through the middle of its word ("75 m br…"), and a sliced label reads as a
              layout bug rather than as an invitation to scroll — nothing on screen says there is
              more to the right. Wrapped, every band is either visible or plainly below the fold.
           ★ Capped at roughly two rows and scrolled vertically past that: the band list runs to
             thirty-odd entries, and letting it grow unbounded would push the value and the Add
             button off the screen on a phone. */}
      <ScrollView style={{ maxHeight: 96 }} nestedScrollEnabled
                  showsVerticalScrollIndicator={false}
                  contentContainerStyle={{ flexDirection: 'row', flexWrap: 'wrap',
                                           gap: 8, paddingVertical: 2 }}>
        {p.allowAll && (
          <TouchableOpacity onPress={() => setBand(band === 'all' ? '' : 'all')}
                            style={chip(band === 'all')}>
            <Text style={{ color: band === 'all' ? p.C.green : p.C.gold, fontFamily: p.F, fontSize: 12 }}>
              All bands
            </Text>
          </TouchableOpacity>
        )}
        {bands.map(b => (
          <TouchableOpacity key={b.id} onPress={() => setBand(band === b.id ? '' : b.id)}
                            style={chip(band === b.id)}>
            <Text style={{ color: band === b.id ? p.C.green : p.C.gold, fontFamily: p.F, fontSize: 12 }}>
              {b.label}
            </Text>
          </TouchableOpacity>
        ))}
      </ScrollView>

      {/* ── the value, and Add ──────────────────────────────────────────── */}
      <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center', marginTop: 8 }}>
        {p.kind === 'gain' && steps.length ? (
          <>
            <Slider style={{ flex: 1 }} minimumValue={0} maximumValue={steps.length - 1} step={1}
              value={gainIdx} onValueChange={(v: number) => setGainIdx(Math.round(v))}
              minimumTrackTintColor={p.C.gold} maximumTrackTintColor="rgba(255,255,255,0.25)"
              thumbTintColor={p.C.gold} />
            <Text style={{ color: p.C.amber, fontFamily: p.F, fontSize: 13, minWidth: 62,
                           textAlign: 'right' }}>
              {(steps[gainIdx] / 10).toFixed(1)} dB
            </Text>
          </>
        ) : p.kind === 'range' ? (
          /* ★★★ TWO BOXES, BECAUSE A RANGE IS TWO NUMBERS. One field holding "87.5M-108M" asks the
                 owner to know that the separator is a hyphen, that both halves take a unit suffix,
                 and that a space is or is not allowed — none of which is on screen, and none of
                 which is told to them until the server refuses the whole string. A pair of boxes
                 with "to" between them cannot be typed wrong in that way (Stuart, 2026-08-21).
              ★ The string on the wire is unchanged: they are joined with a hyphen, which is what
                the server has always parsed. */
          <>
            <TextInput value={loText} onChangeText={setLoText}
              placeholder="from, e.g. 87.5M" placeholderTextColor={p.C.goldDim}
              autoCapitalize="none" autoCorrect={false}
              style={[styles.input, { flex: 1, color: p.C.amber, borderColor: p.C.border,
                                      fontFamily: p.F }]} />
            <Text style={{ color: p.C.textDim, fontFamily: p.F, fontSize: 12 }}>to</Text>
            <TextInput value={hiText} onChangeText={setHiText}
              placeholder="to, e.g. 108M" placeholderTextColor={p.C.goldDim}
              autoCapitalize="none" autoCorrect={false}
              style={[styles.input, { flex: 1, color: p.C.amber, borderColor: p.C.border,
                                      fontFamily: p.F }]} />
          </>
        ) : (
        <TextInput value={text} onChangeText={setText}
          placeholder={p.placeholder} placeholderTextColor={p.C.goldDim}
          autoCapitalize="none" autoCorrect={false}
          keyboardType="numbers-and-punctuation"
          style={[styles.input, { flex: 1, color: p.C.amber, borderColor: p.C.border, fontFamily: p.F }]} />
        )}
        <TouchableOpacity onPress={add} style={chip(false)}>
          <Text style={{ color: p.C.gold, fontFamily: p.F, fontSize: 13 }}>Add</Text>
        </TouchableOpacity>
      </View>

      {/* ★★ THE LOCK, AND THE SPLIT IT IMPLIES ON A HACKRF — both belong to the entry being added,
             which is why they sit between the value and the list rather than over the section. */}
      {p.lockable && (
        <TouchableOpacity onPress={() => setLockNext(v => !v)}
          style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 10 }}>
          <View style={{ width: 18, height: 18, borderRadius: 4, borderWidth: 1,
                         borderColor: lockNext ? p.C.green : p.C.border,
                         backgroundColor: lockNext ? `${p.C.green}55` : 'transparent' }} />
          <Text style={{ color: p.C.gold, fontFamily: p.F, fontSize: 12, flex: 1 }}>
            Lock this band — the ceiling is the SETTING, not a limit
          </Text>
        </TouchableOpacity>
      )}
      {p.lockable && p.splittable && lockNext && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8 }}>
          <Text style={{ color: p.C.textDim, fontFamily: p.F, fontSize: 12 }}>Split</Text>
          <Slider style={{ flex: 1 }} minimumValue={0} maximumValue={100} step={5}
            value={splitNext} onValueChange={(v: number) => setSplitNext(Math.round(v))}
            minimumTrackTintColor={p.C.gold} maximumTrackTintColor="rgba(255,255,255,0.25)"
            thumbTintColor={p.C.gold} />
          <Text style={{ color: p.C.amber, fontFamily: p.F, fontSize: 12, minWidth: 96,
                         textAlign: 'right' }}>
            {splitNext}% LNA / {100 - splitNext}% VGA
          </Text>
        </View>
      )}

      {/* ── what is set ─────────────────────────────────────────────────── */}
      {entries.length === 0 ? (
        <Text style={{ color: p.C.textDim, fontFamily: p.F, fontSize: 12, marginTop: 8 }}>
          {p.emptyText}
        </Text>
      ) : (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 8 }}>
          {entries.map(e => (
            <TouchableOpacity key={e} onPress={() => remove(e)}
              style={{ borderWidth: 1, borderRadius: 8, paddingVertical: 8, paddingHorizontal: 12,
                       borderColor: p.C.green, backgroundColor: `${p.C.green}18`,
                       flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <Text style={{ color: p.C.green, fontFamily: p.F, fontSize: 12 }}>{label(e)}</Text>
              {/* ★ The × is the whole hit target's job — see se_display_zoom_narrowest_layout:
                    a clipped × is a trap, so the row itself removes rather than a tiny glyph. */}
              <Text style={{ color: p.C.textDim, fontFamily: p.F, fontSize: 12 }}>×</Text>
            </TouchableOpacity>
          ))}
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  input: {
    borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, fontSize: 16,
  },
});
