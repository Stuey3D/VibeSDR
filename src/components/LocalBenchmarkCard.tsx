/* ★★★ WHAT THIS PHONE CAN RUN — the device benchmark in LOCAL HARDWARE (Android: a radio plugged into the phone, the phone
 *  itself the receiver). Stuart, 2026-11-20: "we should offer the benchmark to the local hardware mode too so people on
 *  weaker devices know exactly what they are getting".
 *
 *  ★★ The SAME measurement as the server screen's (one result per device — a run from either shows in both); read here for
 *     someone listening on their own radio: one plain verdict per thing they might do (src/services/localBench.ts), only
 *     for what this radio can do here, the figure as small print, when it was measured, and RUN AGAIN.
 *  ★★ It lives in the hardware sheet, with this phone's other radio settings — never on the control deck (house rule: the
 *     deck stays minimal). It CHANGES NOTHING: no switch is turned off by it here (the server screen's defaults are for
 *     what a server offers its listeners; Local Hardware has none of those switches).
 *  ★ The run belongs to localBenchRun.ts, not to this card: closing the sheet mid-run must not lose the resume. */
import React, { useEffect, useSyncExternalStore } from 'react';
import { Text, TouchableOpacity, View, type TextStyle, type ViewStyle } from 'react-native';
import { appAlert } from './AppAlert';
import { useKeyboardMode, useNavFocus } from './PanelNav';
import { meaningText } from '../constants/decoderTokens';
import { localBenchLines, measuredWhen, pauseMinutes, type LocalReach, type Verdict } from '../services/localBench';
import { loadLocalBench, localBenchState, runLocalBench, subscribeLocalBench } from '../services/localBenchRun';

export interface LocalBenchProps {
  reach: LocalReach;
  /** Stop this app's own listening for the run (the waterfall) — and bring it back. */
  onPause: () => void;
  onResume: () => void;
}

interface Styles { section: TextStyle; note: TextStyle; noteStrong: TextStyle; noteBad: TextStyle;
                   adminBtn: ViewStyle; adminBtnTxt: TextStyle; toggleLabel: TextStyle }
interface Meaning { good: string; warn: string; bad: string; badBold: boolean }

export default function LocalBenchmarkCard({ bench, styles, m, fill, track, slot }: {
  bench: LocalBenchProps; styles: Styles; m: Meaning; fill: string; track: string;
  slot: (run: () => void) => boolean;
}) {
  const st = useSyncExternalStore(subscribeLocalBench, localBenchState);
  const kbNav = useKeyboardMode();
  const focusC = useNavFocus();
  // ★ Every time the sheet opens: a run made from the server screen since then shows here too.
  useEffect(() => { loadLocalBench(); }, []);

  const lines = localBenchLines(st.result, bench.reach);
  const when = measuredWhen(st.result?.at);
  const mins = pauseMinutes(st.result);
  const start = () => {
    if (st.busy) return;
    const pause = mins ? `about ${mins} minute${mins === 1 ? '' : 's'}` : 'a few minutes';
    appAlert('Measure this phone?',
      `The radio pauses for ${pause} while this phone is measured, then carries on where it was. `
      + (st.result ? '' : 'The first run also downloads the test recordings it measures with. ')
      + 'Leave the phone be until it finishes.',
      [{ text: 'Not now', style: 'cancel' },
       { text: 'Measure', onPress: () => { runLocalBench({ pause: bench.onPause, resume: bench.onResume }, bench.reach.dab); } }]);
  };
  const tone = (v: Verdict) => meaningText(m, v === 'good' ? m.good : v === 'heavy' ? m.warn : v === 'too-heavy' ? m.bad : undefined);
  const ring = slot(start) && kbNav ? { borderColor: focusC.ring, borderWidth: 2 } : null;

  return (
    <View>
      <Text style={styles.section}>WHAT THIS PHONE CAN RUN</Text>
      {!st.busy && !lines.length && (
        <Text style={styles.note}>
          {st.result
            ? 'The last measurement on this phone has nothing to say about what this radio can do here. Measure it again to find out.'
            : 'Find out how well this phone copes with each thing this radio can do — FM, DAB, the digital voice modes and the decoders — measured on this phone, not guessed from its name.'}
        </Text>
      )}
      {!st.busy && lines.map((l) => (
        <View key={l.key} accessible accessibilityLabel={`${l.title}: ${l.words}${l.small ? ', ' + l.small : ''}`}
              style={{ marginTop: 8 }}>
          <Text style={styles.toggleLabel}>{l.title}</Text>
          <Text style={[styles.toggleLabel, { fontSize: 13 }, tone(l.verdict)]}>{l.words}</Text>
          {!!l.small && <Text style={[styles.note, { marginTop: 1 }]}>{l.small}</Text>}
        </View>
      ))}
      {!st.busy && !!when && lines.length > 0 && <Text style={styles.note}>{when}.</Text>}
      {st.busy && (
        <View style={{ marginTop: 8 }}>
          {st.prog && st.prog.steps > 0 ? (<>
            <View style={{ height: 6, borderRadius: 3, backgroundColor: track, overflow: 'hidden' }}>
              <View style={{ height: 6, borderRadius: 3, backgroundColor: fill,
                             width: `${Math.max(2, Math.min(100, (st.prog.step / st.prog.steps) * 100))}%` }} />
            </View>
            <Text style={styles.note} numberOfLines={1}>
              {`${st.prog.step} of ${st.prog.steps}${st.prog.label ? ' — ' + st.prog.label : ''}`}
            </Text>
          </>) : (
            <Text style={styles.note}>Getting ready — fetching the test recordings if this phone does not have them yet.</Text>
          )}
          <Text style={styles.note}>The radio is paused while this phone is measured, and comes back by itself when it finishes.</Text>
        </View>
      )}
      {!!st.error && !st.busy && <Text style={[styles.note, styles.noteBad]}>{st.error}</Text>}
      <TouchableOpacity style={[styles.adminBtn, { alignSelf: 'flex-start', marginTop: 10 }, ring, st.busy && { opacity: 0.5 }]}
                        disabled={st.busy} onPress={start}
                        accessibilityRole="button" accessibilityState={{ disabled: st.busy }}>
        <Text style={styles.adminBtnTxt}>{st.busy ? 'MEASURING…' : st.result ? 'RUN AGAIN' : 'MEASURE THIS PHONE'}</Text>
      </TouchableOpacity>
    </View>
  );
}
