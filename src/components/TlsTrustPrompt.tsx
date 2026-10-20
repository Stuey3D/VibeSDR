/**
 * TlsTrustPrompt — "Trust this server?" for a receiver with its own certificate (GitHub #52). Mounted ONCE in App, so
 * whichever screen's connection failed (the picker's probe, the receiver screen's socket, the FM-DX tuner) the same
 * question appears the same way. The logic is services/tlsTrust.ts; the rules are utils/tlsPin.ts.
 *
 * Three faces:
 *  - FIRST USE   — not from a recognised authority: the certificate's name, dates and SHA-256, CANCEL / TRUST THIS SERVER.
 *  - CHANGED     — a different certificate from the one trusted: both fingerprints, CANCEL / TRUST NEW CERTIFICATE.
 *  - REFUSED     — expired, or a public certificate for another name: why, and OK. Trust on first use never covers these.
 *
 * ★ Built from PasswordModal's parts (PopupShell): the default chassis in the user's colours, the brushed plate and
 *   dome keys on silver / black, the data in a recessed window, no dim with Transparency off. A width cap keeps it a
 *   dialog on a tablet or a Mac window; it scrolls on an SE in Display Zoom.
 * ★ Not dismissible by the system back gesture: the answer is the point. CANCEL is always there.
 * ★★ V12 CONTROLLERS (docs/v12/CONTROLLER-AUDIT.md §7): an overlay over every screen (useFieldPopupNav), so arrows and
 *   Enter never reach the screen or list beneath. The highlight STARTS ON CANCEL — the safe answer; TRUST is never the
 *   default — and Back = CANCEL. A refusal has one key, OK, which is where it starts.
 */
import React, { useEffect, useState } from 'react';
import { Modal, Platform, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import {
  usePopupStyles, usePopupTheme, usePopupFrame, onMetal, engraveText,
  PopupKey, PopupPlate, PopupWindow, type PopupTokens,
} from './PopupShell';
import { useSurface } from '../contexts/FaceplateContext';
import { useFieldPopupNav, NavWrap } from './FieldPopupNav';
import { scrollLane } from '../constants/popupTokens';
import { subscribeTlsPrompt, type TlsPrompt } from '../services/tlsTrust';
import { certNamesHost, fingerprintLines } from '../utils/tlsPin';

const MONO = Platform.select({ ios: 'Courier', default: 'monospace' });

function day(d: Date | null | undefined): string {
  if (!d) return '';
  try { return d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }); }
  catch { return d.toISOString().slice(0, 10); }
}

export default function TlsTrustPrompt() {
  const [p, setP] = useState<TlsPrompt | null>(null);
  useEffect(() => subscribeTlsPrompt(setP), []);
  const styles = usePopupStyles(makeStyles);
  const pt = usePopupTheme();
  const dim = useSurface().scrimOpacity > 0;
  const metalFrame = usePopupFrame(16, false);
  // ★ The keys, left to right, exactly as drawn: [CANCEL, TRUST] when trust is on offer, [OK] on a refusal.
  const offerKeys = !!p && p.decision.kind !== 'refuse';
  const nav = useFieldPopupNav(!!p, {
    name: 'tls trust', fields: 0, startAt: 0,
    keys: offerKeys ? [() => p?.answer(false), () => p?.answer(true)] : [() => p?.answer(false)],
    onField: () => {},
    onBack: () => p?.answer(false),
  });
  if (!p) return null;

  const d = p.decision;
  const where = `${p.host.includes(':') ? `[${p.host}]` : p.host}:${p.port}`;
  const c = p.cert;
  const namesIt = c ? certNamesHost(c, p.host) : true;
  const issuedTo = c ? (c.subjectCN || c.names[0] || '') : '';

  const title = d.kind === 'offer' ? 'Untrusted certificate'
              : d.kind === 'changed' ? 'Certificate has changed'
              : d.why === 'expired' ? 'Certificate out of date'
              : 'Certificate is for another address';
  const body = d.kind === 'offer'
    ? `This server's certificate isn't from a recognised authority — usual for a receiver that uses its own self-signed certificate. Only trust it if this is your server, or you know its owner.`
    : d.kind === 'changed'
    ? `This server now has a different certificate from the one you trusted. If its owner replaced it, trust the new one. If you weren't expecting a change, cancel — someone could be intercepting the connection.`
    : d.why === 'expired'
    ? `This server's certificate ${c?.notAfter && c.notAfter.getTime() < Date.now() ? `expired on ${day(c.notAfter)}` : 'is not valid yet'}. VibeSDR won't connect with an out-of-date certificate, even one you trusted before — its owner needs to renew it. (If the date looks wrong, check this device's clock.)`
    : `This server's certificate is from a recognised authority, but for a different name${c?.names.length ? ` (${c.names.slice(0, 3).join(', ')})` : issuedTo ? ` (${issuedTo})` : ''}. Check the address — the name in the certificate is usually the one to use.`;

  const offerable = d.kind !== 'refuse';
  const primary = d.kind === 'changed' ? 'TRUST NEW CERTIFICATE' : 'TRUST THIS SERVER';

  const row = (label: string, value: string, mono = false) => (
    <View style={styles.row} key={label}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text style={[styles.rowValue, mono && styles.mono]} selectable>{value}</Text>
    </View>
  );

  return (
    <Modal visible transparent animationType="fade" supportedOrientations={['portrait', 'landscape']}
           onRequestClose={() => {/* intentionally not dismissible via system gesture */}}>
      <View style={[styles.overlay, dim && styles.overlayDim]}>
        <View style={[styles.box, metalFrame]}>
          <PopupPlate radius={16} />
          <ScrollView contentContainerStyle={[{ gap: 12 }, scrollLane]} bounces={false}>
            <Text style={styles.title}>{title}</Text>
            <Text style={styles.sub} selectable>{where}</Text>
            <Text style={styles.body}>{body}</Text>
            <PopupWindow style={styles.win} metalStyle={styles.winMetal}>
              {!!issuedTo && row('Issued to', namesIt ? issuedTo : `${issuedTo} — not this address`)}
              {!!c && row('Issued by', c.selfSigned ? 'itself (self-signed)' : (c.issuerCN || 'unknown'))}
              {!!c?.notAfter && row('Expires', day(c.notAfter))}
              {d.kind === 'changed' && row('Trusted before (SHA-256)', fingerprintLines(d.previous, 8).join('\n'), true)}
              {row(d.kind === 'changed' ? 'Now (SHA-256)' : 'SHA-256 fingerprint', fingerprintLines(d.sha256, 8).join('\n'), true)}
            </PopupWindow>
            {offerable && (
              <Text style={styles.note}>
                VibeSDR will accept this exact certificate for {where} only. To forget it later: the pencil (✎) on a saved
                server, or a long press on a custom server.
              </Text>
            )}
          </ScrollView>
          {pt.metal ? (
            <View style={styles.keys}>
              {offerable && <PopupKey label="CANCEL" focused={nav.key(0)} onPress={() => p.answer(false)} height={40} fontSize={12} style={{ minWidth: 96 }} />}
              <PopupKey label={offerable ? primary : 'OK'} primary focused={nav.key(offerable ? 1 : 0)} onPress={() => p.answer(offerable)} height={40} fontSize={12}
                        style={{ minWidth: offerable ? 150 : 96 }} />
            </View>
          ) : (
            <View style={styles.keys}>
              {offerable && (
                <NavWrap on={nav.key(0)} color={nav.ring}>
                  <TouchableOpacity style={styles.btn} onPress={() => p.answer(false)}>
                    <Text style={styles.btnTxtCancel}>Cancel</Text>
                  </TouchableOpacity>
                </NavWrap>
              )}
              <NavWrap on={nav.key(offerable ? 1 : 0)} color={nav.ring}>
                <TouchableOpacity style={[styles.btn, styles.btnPrimary]} onPress={() => p.answer(offerable)}>
                  <Text style={styles.btnTxtPrimary}>{offerable ? (d.kind === 'changed' ? 'Trust new certificate' : 'Trust this server') : 'OK'}</Text>
                </TouchableOpacity>
              </NavWrap>
            </View>
          )}
        </View>
      </View>
    </Modal>
  );
}

/* ★ PasswordModal's styles, with a data window: text in the TEXT colour, keys in the CONTROLS colour (default), engraved
 *   on the plate and lit inside the window (silver / black). */
const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  overlay:     { flex: 1, justifyContent: 'center', alignItems: 'center', padding: 16 },
  overlayDim:  { backgroundColor: 'rgba(0,0,0,0.75)' },
  box:         { width: '100%', maxWidth: 460, maxHeight: '92%', backgroundColor: '#0A0804', borderRadius: 10, borderWidth: 1,
                 borderColor: pt.ink.border, padding: 20, gap: 14 },
  title:       onMetal(pt, { fontFamily: 'Courier', fontSize: 16, fontWeight: 'bold', color: pt.ink.text, letterSpacing: 1 }, { ...engraveText(pt), letterSpacing: 1.5 }),
  sub:         onMetal(pt, { fontFamily: 'Courier', fontSize: 12, color: pt.ink.textDim }, engraveText(pt, pt.note)),
  body:        onMetal(pt, { fontFamily: 'Courier', fontSize: 12.5, lineHeight: 18, color: pt.ink.text }, { ...engraveText(pt), fontSize: 13, lineHeight: 19 }),
  note:        onMetal(pt, { fontFamily: 'Courier', fontSize: 11, lineHeight: 16, color: pt.ink.textDim }, { ...engraveText(pt, pt.note), fontSize: 11.5, lineHeight: 16 }),
  win:         { borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6, padding: 10, gap: 8, backgroundColor: 'rgba(20,10,0,0.80)' },
  winMetal:    { padding: 10, gap: 8 },
  row:         { gap: 2 },
  rowLabel:    onMetal(pt, { fontFamily: 'Courier', fontSize: 10.5, color: pt.ink.textDim, letterSpacing: 0.5 }, { fontFamily: 'Atkinson Hyperlegible', fontSize: 11, color: pt.winDim }),
  rowValue:    onMetal(pt, { fontFamily: 'Courier', fontSize: 13, color: pt.ink.text }, { fontFamily: 'Atkinson Hyperlegible', fontSize: 13, color: pt.readout }),
  mono:        { fontFamily: MONO, fontSize: 12, lineHeight: 17, letterSpacing: 0.3 },
  keys:        { flexDirection: 'row', flexWrap: 'wrap', gap: 10, justifyContent: 'flex-end' },
  btn:         { paddingHorizontal: 18, paddingVertical: 10, borderRadius: 6, borderWidth: 1, borderColor: pt.ink.keyBorder },
  btnPrimary:  { borderColor: pt.gold.selBorder, backgroundColor: pt.ink.keyBgAct },
  btnTxtCancel:  { fontFamily: 'Courier', fontSize: 13, color: pt.ink.key },
  btnTxtPrimary: { fontFamily: 'Courier', fontSize: 13, color: pt.gold.sel, fontWeight: 'bold' },
});
