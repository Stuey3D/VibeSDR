// Render-error boundary — companion to services/crashGuard.ts. A flaky server
// can feed our components malformed state that throws during render; without a
// boundary RN unmounts the whole tree (white screen) or aborts. This catches
// the render throw, records it (same breadcrumb as the global handler), and
// remounts the navigation tree fresh — which lands back on the instance picker
// (initialRouteName) — then shows an HONESTLY ATTRIBUTED message: see
// classifyFault in crashGuard. A render throw is very often our own bug, and this
// used to report every one of them as the server having stopped responding.

import React from 'react';
import { Alert, View } from 'react-native';
import { recordCrash, faultMessage } from '../services/crashGuard';

type Props = { children: React.ReactNode };
type State = { hasError: boolean };

export default class CrashBoundary extends React.Component<Props, State> {
  state: State = { hasError: false };

  static getDerivedStateFromError(): State {
    return { hasError: true };
  }

  componentDidCatch(error: any, errorInfo: React.ErrorInfo) {
    // ★★ TAKE THE COMPONENT STACK. React hands it here and nowhere else, and for a render error it
    // is the ONLY thing that names the culprit: the JS stack ends inside React's own work loop with
    // no application frames (React #327, 2026-07-31 — `performWorkOnRoot` and nothing else).
    const info = recordCrash(error, 'render', errorInfo?.componentStack ?? undefined);
    // Remount the tree on the next tick (back to the picker), then warn.
    setTimeout(() => {
      this.setState({ hasError: false });
      const m = faultMessage(error, info.message);
      Alert.alert(m.title, m.body);
    }, 50);
  }

  render() {
    if (this.state.hasError) return <View style={{ flex: 1, backgroundColor: '#0A0A12' }} />;
    return this.props.children;
  }
}
