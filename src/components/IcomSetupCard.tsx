/**
 * IcomSetupCard — the OWNER's view of an Icom transceiver served by a VibeServer (receive only), drawn inside
 * the radio's controls sheet for a SIGNED-IN ADMIN. It mirrors the server setup page's Icom card
 * (vibe_setup_page.h, icomRender) and saves through the SAME admin endpoint (GET/POST /vibeserver/config at
 * the front door, with the admin ticket the app already holds), so the two can never disagree.
 *
 * ★★ FACTS FIRST, from the radio itself (hwinfo.radio): model, CI-V address, baud — with WHY the baud matters
 *    (the scope shares the serial line: 115200 ≈ 19 lines a second, 19200 ≈ 3) — the transport (DTR/RTS held
 *    low or not), the preflight with the exact radio MENU item to change, the audio device and its problem,
 *    and "only one program can hold the radio's USB port".
 * ★★ THEN THE SETTINGS, and saving them RESTARTS the server (the config is read at start; the setup page says
 *    the same). Admin-only by construction: this card is only rendered for an admin, and the endpoint refuses
 *    without the credential anyway.
 * ★ A server that stores no configuration (a phone serving its own radio) answers 501: the card then says the
 *   settings live in that app's server screen and shows the facts only.
 */
import React, { useCallback, useEffect, useState } from 'react';
import { View, Text, TextInput, TouchableOpacity, Switch, ActivityIndicator, type TextInputProps } from 'react-native';
import {
  type IcomRadio, type IcomSettings, ICOM_BAUDS, ICOM_SCOPE_SPEED_NAMES, baudNote, transportText,
  preflightLines, icomSettingsFrom, parseCivHex, fmtCivHex, pickIcomSerial, withIcomSettings,
} from '../services/icomRadio';

type Styles = Record<string, any>;
interface Props {
  icom: IcomRadio;
  /** The panel's own styles (LocalHardwarePanel makeLhpStyles) — this card is a section of that sheet. */
  styles: Styles;
  K: { gold: string; abtn: string; fill: string };
  /** The FRONT DOOR's base URL (the setup endpoints live there, not on a radio behind it). */
  doorBase: string;
  /** The radio's own base (…/r/<serial> behind a door) — names which radio in the config this is. */
  connectBase: string;
  /** The admin credential as a query string (`vs_admin_ticket=…`); '' = none yet. */
  authQ: string;
  /** Open the server's own setup page (the full form), when the app cannot edit this radio itself. */
  onOpenSetupPage?: () => void;
}

export default function IcomSetupCard({ icom, styles, K, doorBase, connectBase, authQ, onOpenSetupPage }: Props) {
  const [cfg, setCfg] = useState<Record<string, unknown> | null>(null);
  const [serial, setSerial] = useState<string | null>(null);
  const [form, setForm] = useState<IcomSettings | null>(null);
  const [civTxt, setCivTxt] = useState('');
  const [state, setState] = useState<'idle' | 'loading' | 'saving' | 'saved' | 'unsupported' | 'error'>('idle');
  const [msg, setMsg] = useState('');
  const base = doorBase.replace(/\/+$/, '');

  const load = useCallback(async () => {
    if (!authQ) return;
    setState('loading'); setMsg('');
    try {
      const r = await fetch(`${base}/vibeserver/config?${authQ.replace(/^&/, '')}`, { cache: 'no-store' });
      if (r.status === 501) { setState('unsupported'); return; }
      if (!r.ok) { setState('error'); setMsg(r.status === 401 ? 'The server did not accept the admin sign-in for its settings.' : `The server answered ${r.status}.`); return; }
      const j = await r.json();
      const sn = pickIcomSerial(j, connectBase);
      setCfg(j); setSerial(sn);
      if (sn) {
        const entry = (j.radios as Record<string, unknown>[]).find((x) => x && x.serial === sn);
        const s = icomSettingsFrom(entry);
        setForm(s); setCivTxt(fmtCivHex(s.icomCiv));
      }
      setState('idle');
    } catch {
      setState('error'); setMsg('Could not reach the server for its settings.');
    }
  }, [authQ, base, connectBase]);
  useEffect(() => { void load(); }, [load]);

  const save = async () => {
    if (!cfg || !serial || !form) return;
    const civ = parseCivHex(civTxt);
    if (civ === null) return;   // ★ IcomSettingsFields already says why, under the box
    const body = withIcomSettings(cfg, serial, { ...form, icomCiv: civ });
    if (!body) { setMsg('This radio is no longer in the server’s settings.'); return; }
    setState('saving'); setMsg('');
    try {
      const r = await fetch(`${base}/vibeserver/config?${authQ.replace(/^&/, '')}`, { method: 'POST', body: JSON.stringify(body) });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) { setState('error'); setMsg(j?.error ? `Not saved: ${j.error}` : `Not saved (${r.status}).`); return; }
      setState('saved');
      setMsg('Saved. VibeServer is restarting to apply it — the app reconnects by itself.');
    } catch {
      // ★ An unreachable server is not a rejected save (the setup page learned this the hard way): say we
      //   cannot tell, rather than claim it failed.
      setState('error'); setMsg('The server did not answer — it may already be restarting. Check again in a moment.');
    }
  };

  const set = <Kk extends keyof IcomSettings>(k: Kk, v: IcomSettings[Kk]) => setForm((f) => (f ? { ...f, [k]: v } : f));
  const ink: IcomSwitchInk = { trackOn: K.abtn, trackOff: '#444', thumbOn: K.gold, thumbOff: '#ccc' };

  return (<>
    <Text style={styles.section}>ICOM SETUP (OWNER)</Text>
    <IcomFacts icom={icom} styles={styles} />

    {state === 'loading' && <ActivityIndicator color={K.gold} style={{ marginTop: 8 }} />}
    {state === 'unsupported' && (
      <Text style={styles.note}>This server keeps its radio settings in its own app &mdash; change them there.</Text>
    )}
    {form && serial ? (<>
      <IcomSettingsFields form={form} civTxt={civTxt} onCivTxt={setCivTxt} onSet={set} styles={styles} ink={ink} />
      <TouchableOpacity style={[styles.adminBtn, { marginTop: 14, alignSelf: 'flex-start' }]} disabled={state === 'saving'} onPress={save}>
        <Text style={styles.adminBtnTxt}>{state === 'saving' ? 'SAVING…' : 'SAVE AND RESTART'}</Text>
      </TouchableOpacity>
      <Text style={styles.note}>Saving restarts VibeServer, so listeners reconnect.</Text>
    </>) : (state === 'idle' && cfg && !serial && (
      <Text style={styles.note}>This server has more than one Icom &mdash; change its settings on the setup page.</Text>
    ))}
    {!!msg && <Text style={[styles.note, state === 'error' && styles.noteBad]}>{msg}</Text>}
    {!!onOpenSetupPage && (
      <TouchableOpacity style={[styles.adminBtn, { marginTop: 10, alignSelf: 'flex-start' }]} onPress={onOpenSetupPage}>
        <Text style={styles.adminBtnTxt}>OPEN THE SETUP PAGE</Text>
      </TouchableOpacity>
    )}
  </>);
}

/* ── The two halves, shared with the app's OWN server screen ────────────────────────────────────────────────────
 * ★★ ONE CARD, TWO HOMES (2026-10-22): this card (a remote VibeServer, saved through /vibeserver/config) and
 *    ServerModeScreen's Icom card (a phone serving its own radio, saved into the start config) draw the same facts
 *    and the same settings from these two components, so the words and the order cannot drift apart. Each home
 *    passes its own styles under the same names (section, note, noteBad, noteStrong, fieldLbl, segRow, seg,
 *    segActive, segTxt, segTxtActive, convInput, toggleRow, toggleLabel) and its own switch colours. */

/** Switch colours — each home keeps its own look. */
export interface IcomSwitchInk { trackOn: string; trackOff: string; thumbOn: string; thumbOff: string; placeholder?: string }

/** A row of keys, one lit. Only values the caller lists are offered. */
export function IcomChoice<T extends string | number>({ opts, value, fmt, onPick, styles }:
  { opts: readonly T[]; value: T; fmt: (v: T) => string; onPick: (v: T) => void; styles: Styles }) {
  return (
    <View style={styles.segRow}>
      {opts.map((o) => (
        <TouchableOpacity key={String(o)} style={[styles.seg, o === value && styles.segActive]} onPress={() => onPick(o)}>
          <Text style={[styles.segTxt, o === value && styles.segTxtActive]}>{fmt(o)}</Text>
        </TouchableOpacity>
      ))}
    </View>
  );
}

/**
 * The radio's own facts (hwinfo.radio): model, CI-V, baud and why it matters, transport, audio, refused frames, the
 * transmit-safety preflight with each menu path, and who may tune.
 * @param audio false = the caller shows the audio state itself (the phone knows more than hwinfo: the permission and
 *              the USB input Android picked), so it is not said twice.
 * @param portNote false = leave out "close wfview / WSJT-X / your logger": on a phone nothing else is on that cable.
 */
export function IcomFacts({ icom, styles, audio = true, portNote = true }:
  { icom: IcomRadio; styles: Styles; audio?: boolean; portNote?: boolean }) {
  const pf = icom.preflight;
  return (<>
    <Text style={styles.toggleLabel}>
      {icom.model}{icom.civ ? ` · CI-V ${icom.civ}` : ''}{icom.baud ? ` · ${icom.baud} baud` : ''}
      {icom.scope.fps > 0 ? ` · scope ${icom.scope.fps.toFixed(1)} lines/s` : ''}
    </Text>
    <Text style={styles.note}>{transportText(icom)}{icom.transport ? ` (${icom.transport})` : ''}.</Text>
    <Text style={styles.note}>{baudNote(icom.baud)}</Text>
    {audio && !!icom.audio && <Text style={styles.note}>Audio: {icom.audio}</Text>}
    {audio && !!icom.audioProblem && <Text style={[styles.note, styles.noteBad]}>Audio: {icom.audioProblem}</Text>}
    {icom.refusedFrames > 0 && (
      <Text style={[styles.note, styles.noteBad]}>
        VibeServer refused {icom.refusedFrames} command{icom.refusedFrames === 1 ? '' : 's'} that were not on its receive-only list.
        Nothing was sent to the radio; please report it.
      </Text>
    )}

    <Text style={styles.fieldLbl}>Transmit safety check</Text>
    {pf ? (<>
      <Text style={[styles.note, pf.passed ? styles.noteStrong : styles.noteBad]}>{pf.summary}</Text>
      {preflightLines(pf).map((l, i) => (
        <Text key={i} style={[styles.note, !l.ok && styles.noteBad]}>{l.ok ? '✓' : '✗'}  {l.text}</Text>
      ))}
      {!pf.verified && (
        <Text style={styles.note}>
          The menu numbers for these items are not yet confirmed on a real radio. VibeServer counts an item it
          cannot read as unsafe and will not start the radio.
        </Text>
      )}
    </>) : <Text style={styles.note}>Not reported.</Text>}
    <Text style={styles.note}>
      VibeServer reads these every time it connects and never changes them itself. It never transmits.
    </Text>
    {portNote && (
      <Text style={[styles.note, styles.noteStrong]}>
        Only one program can hold the radio&rsquo;s USB port. Close wfview, wfweb, WSJT-X&rsquo;s rig control and any
        logger that talks to the radio, or VibeServer cannot reach it.
      </Text>
    )}

    <Text style={styles.fieldLbl}>Who may tune</Text>
    <Text style={styles.note}>
      {icom.ownerLock ? 'Listeners follow the radio — only you tune it.' : 'Listeners may tune the radio.'}{' '}
      {icom.rfOpen ? 'Listeners may change its RF settings.' : 'Its RF settings are yours alone.'}
    </Text>
  </>);
}

/**
 * The owner's settings, in the setup page's names and order.
 * @param phone true = a phone serving its own radio: only the five keys Android's server reads (VibeIcom.prepareStart —
 *              icomBaud, icomCiv, icomScopeSpeed, icomOwnerLock, icomRfOpen). Its audio is Android's AudioRecord on the
 *              radio's USB input and its port is the USB device the owner allowed, so audio/port/serial-device are
 *              not settings there and are not drawn (AGENTS.md: no control that does nothing).
 */
export function IcomSettingsFields({ form, civTxt, onCivTxt, onSet, styles, ink, phone = false, Input = TextInput }: {
  form: IcomSettings; civTxt: string; onCivTxt: (t: string) => void;
  onSet: <Kk extends keyof IcomSettings>(k: Kk, v: IcomSettings[Kk]) => void;
  styles: Styles; ink: IcomSwitchInk; phone?: boolean;
  /** ★ The server screen passes TvTextInput, so a TV remote can move PAST the box (Lite on a TV box). */
  Input?: React.ComponentType<TextInputProps>;
}) {
  const civBad = parseCivHex(civTxt) === null;
  return (<>
    <Text style={styles.fieldLbl}>CI-V baud rate (must match the radio)</Text>
    <IcomChoice styles={styles} opts={ICOM_BAUDS} value={form.icomBaud as (typeof ICOM_BAUDS)[number]} fmt={(b) => String(b)} onPick={(b) => onSet('icomBaud', b)} />
    <Text style={styles.note}>
      On the radio: MENU &gt; SET &gt; Connectors &gt; CI-V &gt; CI-V USB Baud Rate, and CI-V USB Port to
      &ldquo;Unlink from [REMOTE]&rdquo;.
    </Text>
    <Text style={styles.fieldLbl}>CI-V address (empty = the radio&rsquo;s own)</Text>
    <Input value={civTxt} onChangeText={onCivTxt} maxLength={2} autoCapitalize="characters" autoCorrect={false}
               placeholder="94" placeholderTextColor={ink.placeholder ?? '#777'} style={styles.convInput} />
    {civBad && (
      <Text style={[styles.note, styles.noteBad]}>
        The CI-V address is two hex digits (94 on an IC-7300), or empty for the radio&rsquo;s own.
      </Text>
    )}
    <Text style={styles.fieldLbl}>Scope speed</Text>
    <IcomChoice styles={styles} opts={[0, 1, 2] as const} value={form.icomScopeSpeed as 0 | 1 | 2} fmt={(v) => ICOM_SCOPE_SPEED_NAMES[v]} onPick={(v) => onSet('icomScopeSpeed', v)} />
    <View style={styles.toggleRow}>
      <Text style={[styles.toggleLabel, { flex: 1 }]}>Listeners follow the radio &mdash; only you tune it</Text>
      <Switch value={form.icomOwnerLock} onValueChange={(v) => onSet('icomOwnerLock', v)}
              trackColor={{ true: ink.trackOn, false: ink.trackOff }} thumbColor={form.icomOwnerLock ? ink.thumbOn : ink.thumbOff} />
    </View>
    <Text style={styles.note}>This only protects the radio once an admin password is set.</Text>
    <View style={styles.toggleRow}>
      <Text style={[styles.toggleLabel, { flex: 1 }]}>Listeners may change ATT, preamp, AGC, RF gain, NB, NR and ANF</Text>
      <Switch value={form.icomRfOpen} onValueChange={(v) => onSet('icomRfOpen', v)}
              trackColor={{ true: ink.trackOn, false: ink.trackOff }} thumbColor={form.icomRfOpen ? ink.thumbOn : ink.thumbOff} />
    </View>
    {!phone && (<>
      <Text style={styles.fieldLbl}>Receive audio</Text>
      <Input value={form.icomAudio} onChangeText={(t) => onSet('icomAudio', t)} autoCapitalize="none" autoCorrect={false}
                 placeholder="auto" placeholderTextColor={ink.placeholder ?? '#777'} style={styles.convInput} />
      <Text style={styles.note}>auto finds the radio&rsquo;s own USB audio; none serves the waterfall and dial only.</Text>
      <Text style={styles.fieldLbl}>Port</Text>
      <Input value={form.icomPort} onChangeText={(t) => onSet('icomPort', t)} autoCapitalize="none" autoCorrect={false}
                 placeholder="usb" placeholderTextColor={ink.placeholder ?? '#777'} style={styles.convInput} />
      <View style={styles.toggleRow}>
        <Text style={[styles.toggleLabel, { flex: 1 }]}>Allow a serial device (/dev/&hellip;) as the port</Text>
        <Switch value={form.icomTtyOk} onValueChange={(v) => onSet('icomTtyOk', v)}
                trackColor={{ true: ink.trackOn, false: ink.trackOff }} thumbColor={form.icomTtyOk ? ink.thumbOn : ink.thumbOff} />
      </View>
      {form.icomTtyOk && (
        <Text style={[styles.note, styles.noteBad]}>
          Opening a serial device raises its DTR and RTS lines for an instant. If the radio&rsquo;s USB SEND or USB
          Keying is set to DTR or RTS, that keys the transmitter. Only allow this if those are OFF.
        </Text>
      )}
    </>)}
  </>);
}
