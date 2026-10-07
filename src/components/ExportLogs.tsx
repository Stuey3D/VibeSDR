/**
 * ★★★ EXPORT LOGS — ONE BUTTON, ONE BEHAVIOUR, THREE PLACES (2026-10-07).
 *
 * Stuart asked NickB for the crash logs "from About" — and About lives in the LISTENING half of the app.
 * Nick's Pixel 6 hosts the VibeServer that keeps crashing; he sent the diagnostics from his Pixel 8
 * running the old 10.5 app as a listener, which knew nothing about the crash. Stuart: *"this does
 * highlight an issue with the placement of the crash log exporter as it is in the listen section."*
 *
 * So the same small link now sits where server owners actually are — and nowhere else (Stuart, same day):
 *   • the server screen (main app server mode AND VibeServer Lite, which renders that same screen);
 *   • the home screen's server list (its footer);
 *   • About (the original).
 *
 * ★ SHARE… AND COPY. The report is long — Discord's 2000-character limit made Nick email it — so the
 *  dialog says how long it is and offers both: Share… hands it to the system share sheet (email, a notes
 *  app, Files/Drive to keep it as a file), Copy puts it on the clipboard for anything else.
 * ★ LOCAL ONLY, as before: assembled on demand, SHOWN first, sent only by the user. See diagnostics.ts.
 */
import React from 'react';
import { Alert, Share, Text, TouchableOpacity } from 'react-native';
import type { StyleProp, TextStyle, ViewStyle } from 'react-native';
import { buildDiagnostics } from '../services/diagnostics';

export const EXPORT_LOGS_LABEL = 'Export logs…';
const DISCORD_LIMIT = 2000;

function copyText(text: string): boolean {
  /* ★ React Native's own Clipboard (RCTClipboard / ClipboardModule) — present in the main app's RN AND in
   *  Lite's RN 0.73, so the one helper works in both bundles with no extra package. Read lazily: the
   *  getter prints a deprecation note, which belongs here and not at import time. */
  try {
    const cb = require('react-native').Clipboard;
    cb?.setString?.(text);
    return !!cb?.setString;
  } catch { return false; }
}

/** Build the report and offer Share… / Copy. `server`: called from the server screen (see buildDiagnostics). */
export async function exportLogs(opts?: { server?: boolean }): Promise<void> {
  let report: string;
  try { report = await buildDiagnostics(undefined, { server: !!opts?.server }); }
  catch (e: any) { Alert.alert('Export logs', `The report could not be put together: ${String(e?.message ?? e)}`); return; }
  const n = report.length;
  const size = n > DISCORD_LIMIT
    ? `${n.toLocaleString()} characters — too long for one Discord message, so Share… it to email or a file.`
    : `${n.toLocaleString()} characters.`;
  Alert.alert(
    'Export logs',
    `${size} No PINs, passwords or location are included.\n\n`
      + (n > 900 ? report.slice(0, 900) + '\n…' : report),
    [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Copy',
        onPress: () => {
          const ok = copyText(report);
          Alert.alert(ok ? 'Copied' : 'Could not copy',
            ok ? 'The whole report is on the clipboard — paste it into an email or a file.'
               : 'This device would not take it — use Share… instead.');
        },
      },
      { text: 'Share…', onPress: () => { Share.share({ message: report, title: 'VibeSDR diagnostics' }).catch(() => {}); } },
    ],
  );
}

/** ★ The small link itself — deliberately quiet (dim text, no box), the same words everywhere. */
export default function ExportLogsLink({ color, fontFamily, fontSize = 13, server, style, textStyle }: {
  color: string; fontFamily?: string; fontSize?: number; server?: boolean;
  style?: StyleProp<ViewStyle>; textStyle?: StyleProp<TextStyle>;
}) {
  return (
    <TouchableOpacity onPress={() => { void exportLogs({ server }); }} accessibilityRole="button"
                      accessibilityLabel="Export logs for a crash report"
                      hitSlop={{ top: 10, bottom: 10, left: 12, right: 12 }}
                      style={[{ alignSelf: 'center', paddingVertical: 8, paddingHorizontal: 12 }, style]}>
      <Text style={[{ color, fontFamily, fontSize, textDecorationLine: 'underline' }, textStyle]}>{EXPORT_LOGS_LABEL}</Text>
    </TouchableOpacity>
  );
}
