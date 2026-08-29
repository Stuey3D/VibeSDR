// Render-error boundary — companion to services/crashGuard.ts. A flaky server
// can feed our components malformed state that throws during render; without a
// boundary RN unmounts the whole tree (white screen) or aborts. This catches
// the render throw, records it (same breadcrumb as the global handler), and
// remounts the navigation tree fresh — which lands back on the instance picker
// (initialRouteName) — then shows an HONESTLY ATTRIBUTED message: see
// classifyFault in crashGuard. A render throw is very often our own bug, and this
// used to report every one of them as the server having stopped responding.

import React from 'react';
import { Alert, Text, TouchableOpacity, View } from 'react-native';
import { recordCrash, faultMessage } from '../services/crashGuard';

type Props = { children: React.ReactNode };
type State = { hasError: boolean; what: string; where: string; loops: number };

export default class CrashBoundary extends React.Component<Props, State> {
  state: State = { hasError: false, what: '', where: '', loops: 0 };

  static getDerivedStateFromError(error: any): Partial<State> {
    return { hasError: true, what: String(error?.message ?? error ?? 'unknown error') };
  }

  componentDidCatch(error: any, errorInfo: React.ErrorInfo) {
    // ★★ TAKE THE COMPONENT STACK. React hands it here and nowhere else, and for a render error it
    // is the ONLY thing that names the culprit: the JS stack ends inside React's own work loop with
    // no application frames (React #327, 2026-07-31 — `performWorkOnRoot` and nothing else).
    const info = recordCrash(error, 'render', errorInfo?.componentStack ?? undefined);
    /* ★★★ NAME WHERE IT THREW, ON SCREEN. The component stack is the only thing that identifies a
     *   render error (the JS stack ends inside React's own work loop), and it was being recorded
     *   and then shown to nobody. First few frames only — that is the culprit and its parents. */
    const stack = (errorInfo?.componentStack ?? '').trim().split('\n').slice(0, 4)
      .map((l) => l.trim()).join('  ←  ');
    this.setState((st) => ({ where: stack, loops: st.loops + 1 }));
    /* ★★★ SELF-HEAL ONCE, THEN STOP AND SAY SO. Remounting is right for a one-off — a flaky server
     *   feeding one bad frame — and WRONG for an error that recurs: it throws again on the next
     *   render, is caught again, and the app sits in a loop showing a blank rectangle while its
     *   Alert is raised over and over at a screen nobody is looking at. Stuart, 2026-08-29, on an
     *   app that was still playing audio and still feeding the watch: "the black screen when
     *   opening the main app ... is still present."
     * ★ THREE strikes, because the first remount genuinely does fix the transient case that this
     *   boundary was written for. */
    if (this.state.loops < 2) {
      setTimeout(() => {
        this.setState({ hasError: false });
        const m = faultMessage(error, info.message);
        Alert.alert(m.title, m.body);
      }, 50);
    }
  }

  /* ★★★ A DARK RECTANGLE IS INDISTINGUISHABLE FROM A BROKEN APP. This rendered exactly that —
   *   #0A0A12, full screen, no text — so a render error that recurred looked like the app failing
   *   to start, on a process that was otherwise healthy and still streaming. It cost most of a
   *   night of debugging in which the one fact that would have ended it (the error's own message)
   *   was sitting in this component's state.
   * ★★ SAY WHAT AND WHERE, and give a way out. The same rule as every other surface in this app:
   *   an unexplained dead screen reads as "the product is broken" rather than "this bit failed". */
  render() {
    if (!this.state.hasError) return this.props.children;
    return (
      <View style={{ flex: 1, backgroundColor: '#0A0A12', padding: 18, justifyContent: 'center' }}>
        <Text style={{ color: '#FFB833', fontSize: 16, fontWeight: '700', marginBottom: 10 }}>
          Something went wrong on this screen
        </Text>
        <Text style={{ color: '#EEE', fontSize: 13, marginBottom: 10 }}>{this.state.what}</Text>
        {!!this.state.where && (
          <Text style={{ color: '#9AA', fontSize: 11, marginBottom: 16 }}>{this.state.where}</Text>
        )}
        <TouchableOpacity
          onPress={() => this.setState({ hasError: false, loops: 0 })}
          style={{ borderWidth: 1, borderColor: '#FFB833', borderRadius: 8,
                   paddingVertical: 10, alignItems: 'center' }}>
          <Text style={{ color: '#FFB833', fontSize: 14, fontWeight: '600' }}>Try again</Text>
        </TouchableOpacity>
        <Text style={{ color: '#667', fontSize: 10, marginTop: 14 }}>
          The receiver keeps playing while this is on screen — audio and the watch are unaffected.
        </Text>
      </View>
    );
  }
}
