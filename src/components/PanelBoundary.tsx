// PanelBoundary — ONE PANEL THAT THROWS CLOSES ONE PANEL.
//
// ★★★ WHY. The only boundary used to be CrashBoundary, around the whole navigator. So an
// AdvRdsPanel render throw (one wrong-typed field in one RDS message) unmounted the ENTIRE SDR
// screen and dumped the listener back at the directory (1adcd21b) — while the radio underneath was
// perfectly healthy: audio flowing, spectrum arriving. Stuart, 2026-09-28: "prevent a bad packet
// from taking down the whole app like the broken RDS used to".
//
// So every independently mountable panel, sheet and overlay on SDRScreen sits in one of these.
// When its subtree throws during render (or in a lifecycle/effect), ONLY that subtree is replaced,
// by a one-line notice — "Advanced RDS hit an error and was closed — the radio is still playing" —
// with RETRY (remount it fresh) and CLOSE (tell the parent it is shut, via onClose). Everything
// else on the screen keeps rendering: the socket, the audio and the spectrum are not its children.
//
// ★★ LOGGED, NEVER SILENT. The error goes to faultLog (console.error with the panel name and the
//    stack, rate-limited, and counted into the Diagnostics report) AND to crashGuard.recordCrash,
//    which keeps the COMPONENT STACK — the only thing that names the culprit for a render error in
//    a minified build (the JS stack ends inside React's work loop).
// ★★ CrashBoundary stays around the whole tree as the last resort. This is the first line.
// ★ `resetKey`: a panel that is always mounted and shown by a prop (a Modal with `visible`) passes
//   that prop here, so closing and reopening it gives a fresh mount rather than the old notice.
// ★ `autoRetry`: for the parts that ARE the receiver's face — the spectrum, the controls, the audio
//   component — a remount is tried by itself (at most three times a minute, so a fault that recurs
//   on every render cannot become a remount loop), and the notice says it is restarting.

import React from 'react';
import { Text, TouchableOpacity, View } from 'react-native';
import { noteFault } from '../services/faultLog';
import { recordCrash } from '../services/crashGuard';

type Props = {
  /** Human name, used in the notice and the log: "Advanced RDS", "Spectrum". */
  name: string;
  children: React.ReactNode;
  /** Parent's "this panel is now shut" — the CLOSE button and the auto-close call it. */
  onClose?: () => void;
  /** Any value whose change means "a fresh open" — resets a caught error. */
  resetKey?: unknown;
  /** Remount by itself after a short pause (rate-limited). */
  autoRetry?: boolean;
  /** The notice's reassurance. Default "the radio is still playing" — which is a lie for the audio
   *  component itself, so that one says what is actually happening instead. */
  tail?: string;
  /** Where the notice floats; defaults to near the top of the screen. */
  noticeTop?: number;
};
type State = { error: string | null; dismissed: boolean; gen: number; restarting: boolean };

const AUTO_RETRY_MS = 1500;
const AUTO_RETRY_MAX_PER_MIN = 3;
/** A notice nobody touches goes away by itself, closing the panel, so it cannot sit over the dial. */
const NOTICE_TTL_MS = 12_000;

export default class PanelBoundary extends React.Component<Props, State> {
  state: State = { error: null, dismissed: false, gen: 0, restarting: false };
  private retries: number[] = [];
  private timer: ReturnType<typeof setTimeout> | null = null;

  static getDerivedStateFromError(error: unknown): Partial<State> {
    const msg = error instanceof Error ? error.message : String(error);
    return { error: msg || 'unknown error', dismissed: false };
  }

  componentDidCatch(error: unknown, info: React.ErrorInfo) {
    const where = (info?.componentStack ?? '').trim().split('\n').slice(0, 4)
      .map((l) => l.trim()).join(' <- ');
    // ★ console.error with the panel name + stack (rate-limited) and a Diagnostics count.
    noteFault('panel:' + this.props.name, 'render', error, where);
    // ★ And the persisted "last JS error", with the component stack that names the culprit.
    recordCrash(error, 'panel:' + this.props.name, info?.componentStack ?? undefined);
    this.clearTimer();
    // ★ Decided HERE, not in render: render runs before this, so it cannot know yet.
    if (this.props.autoRetry && this.mayAutoRetry()) {
      this.timer = setTimeout(() => this.retry(), AUTO_RETRY_MS);
      this.setState({ restarting: true });
    } else {
      this.timer = setTimeout(() => this.close(), NOTICE_TTL_MS);
      this.setState({ restarting: false });
    }
  }

  componentDidUpdate(prev: Props) {
    if (prev.resetKey !== this.props.resetKey && (this.state.error || this.state.dismissed)) {
      this.clearTimer();
      this.setState((s) => ({ error: null, dismissed: false, gen: s.gen + 1 }));
    }
  }

  componentWillUnmount() { this.clearTimer(); }

  private clearTimer() { if (this.timer) { clearTimeout(this.timer); this.timer = null; } }

  private mayAutoRetry(): boolean {
    const now = Date.now();
    this.retries = this.retries.filter((t) => now - t < 60_000);
    if (this.retries.length >= AUTO_RETRY_MAX_PER_MIN) return false;
    this.retries.push(now);
    return true;
  }

  private retry = () => {
    this.clearTimer();
    // ★ A new `key` on the children is what makes this a FRESH mount, not a re-render of the
    //   state that threw.
    this.setState((s) => ({ error: null, dismissed: false, gen: s.gen + 1 }));
  };

  private close = () => {
    this.clearTimer();
    this.setState({ dismissed: true });
    // ★ The parent's own close, so its open-flag is false and reopening from its button remounts.
    //   Guarded: a throwing close handler must not turn a contained fault into a screen fault.
    try { this.props.onClose?.(); }
    catch (e) { noteFault('panel:' + this.props.name, 'onClose', e); }
  };

  render() {
    const { error, dismissed, gen } = this.state;
    if (!error && !dismissed) {
      return <React.Fragment key={gen}>{this.props.children}</React.Fragment>;
    }
    if (dismissed) return null;
    const { restarting } = this.state;
    return (
      <View pointerEvents="box-none"
            style={{ position: 'absolute', left: 12, right: 12, top: this.props.noticeTop ?? 96,
                     zIndex: 999, elevation: 30, alignItems: 'center' }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', backgroundColor: '#1A1420F0',
                       borderColor: '#FFB833', borderWidth: 1, borderRadius: 10,
                       paddingVertical: 6, paddingHorizontal: 10, maxWidth: 520 }}>
          <Text style={{ color: '#FFD27A', fontSize: 12, flexShrink: 1 }} numberOfLines={2}
                accessibilityRole="alert">
            {restarting
              ? `${this.props.name} hit an error and is restarting — ${this.props.tail ?? 'the radio is still playing'}.`
              : `${this.props.name} hit an error and was closed — ${this.props.tail ?? 'the radio is still playing'}.`}
          </Text>
          <TouchableOpacity onPress={this.retry} hitSlop={8} accessibilityLabel={`Retry ${this.props.name}`}
                            style={{ marginLeft: 10 }}>
            <Text style={{ color: '#FFB833', fontSize: 12, fontWeight: '700' }}>RETRY</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={this.close} hitSlop={8} accessibilityLabel={`Close ${this.props.name}`}
                            style={{ marginLeft: 10 }}>
            <Text style={{ color: '#9AA', fontSize: 12, fontWeight: '700' }}>CLOSE</Text>
          </TouchableOpacity>
        </View>
      </View>
    );
  }
}
