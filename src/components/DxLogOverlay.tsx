/**
 * DxLogOverlay — a VibeServer's DX LOG in the app (V12, 2026-11-04 — docs/v12/DX-SCANNER.md). Opened from the DX LOG key,
 * which sits beside GALLERY (Stuart: "The DX Log should live next to the gallery") and is drawn only where the server's
 * hwinfo says `dxLog`.
 *
 * What the server's idle DX scanner heard on Broadcast FM (by RDS PI) and DAB (by multiplex) while nobody was listening.
 * The web client's DX LOG, the same rules (utils/dxLog.ts): DX only by default, SHOW LOCALS / SHOW SUSPECTS, ALL / FM / DAB,
 * sort by last heard / first heard / peak signal / times heard, LIVE NOW, 25 a page. A row opens its openings; TUNE goes
 * there through the screen's ordinary tune path (the server's own shared-dial rules apply); BOOKMARK (the owner) copies it
 * into the server's bookmarks.
 * ★ Text from the air is shown as text. Alerts (per user, opt-in) are the screen's — see SDRScreen dxAlerts.
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { ActivityIndicator, FlatList, Modal, ScrollView, StyleSheet, Switch, Text, TouchableOpacity, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { usePopupTheme, scrollLane, type PopupTokens } from './PopupShell';
import { withReadAuth } from '../services/vibeAuth';
import { DX_SORTS, dxEmptyLine, dxFooter, dxPageCount, dxQuery, dxSubline, dxSuspectLine, dxTags, dxTitle, dxTuneTarget,
         dxWhen, dxWhere, type DxEntry, type DxPage, type DxSort, type DxTune, type DxTuneDab } from '../utils/dxLog';

export default function DxLogOverlay({ visible, base, readAuth = '', ownerQuery = '', alertsOn, onAlerts, onTune, onPromoted, onClose }: {
  visible: boolean; base: string; readAuth?: string;
  /** The owner's signed query (the screen's admin auth) — '' when not signed in; the server also says `canPromote`. */
  ownerQuery?: string;
  /** ★ This device's "tell me about new DX here" (opt-in), and its switch. */
  alertsOn: boolean; onAlerts: (on: boolean) => void;
  onTune: (t: DxTune | DxTuneDab, name: string) => void;
  /** The server's bookmark list changed (a promote) — the screen re-reads it. */
  onPromoted?: () => void;
  onClose: () => void;
}) {
  const pt = usePopupTheme();
  const s = useMemo(() => makeStyles(pt), [pt]);
  const b = base.replace(/\/+$/, '');
  const [band, setBand] = useState<'' | 'FM' | 'DAB'>('');
  const [showLocal, setShowLocal] = useState(false);
  const [showSuspect, setShowSuspect] = useState(false);
  const [sort, setSort] = useState<DxSort>('last');
  const [page, setPage] = useState(1);
  const [data, setData] = useState<DxPage | null>(null);
  const [loading, setLoading] = useState(false);
  const [err, setErr] = useState('');
  const [open, setOpen] = useState<DxEntry | null>(null);
  const [promoteNote, setPromoteNote] = useState('');
  const listRef = React.useRef<FlatList<DxEntry>>(null);

  const load = useCallback(async () => {
    setLoading(true); setErr('');
    try {
      const r = await fetch(withReadAuth(`${b}/vibeserver/dxlog.json?${dxQuery({ band, showLocal, showSuspect, sort, page })}`, readAuth), { cache: 'no-store' } as any);
      const j = await r.json() as DxPage;
      if (!r.ok || j.error) { setData(null); setErr(String(j.error || 'This server keeps no DX log.')); return; }
      const n = dxPageCount(j.total);
      if (page > n && j.total > 0) { setPage(n); return; }
      setData(j);
      listRef.current?.scrollToOffset({ offset: 0, animated: false });
    } catch { setData(null); setErr('Could not reach the server.'); }
    finally { setLoading(false); }
  }, [b, band, showLocal, showSuspect, sort, page, readAuth]);
  useEffect(() => { if (visible) void load(); }, [visible, load]);
  useEffect(() => { if (!visible) { setOpen(null); setPage(1); } }, [visible]);

  const openEntry = async (e: DxEntry) => {
    setOpen(e); setPromoteNote('');
    try {
      const r = await fetch(withReadAuth(`${b}/vibeserver/dxlog/entry?id=${e.id}`, readAuth), { cache: 'no-store' } as any);
      const j = await r.json() as { entry?: DxEntry };
      if (j.entry) setOpen(j.entry);
    } catch { /* the row's own figures stay */ }
  };
  const promote = async (e: DxEntry) => {
    setPromoteNote('');
    try {
      const r = await fetch(`${b}/vibeserver/dxlog/promote?id=${e.id}${ownerQuery ? `&${ownerQuery}` : ''}`, { method: 'POST' });
      const j = await r.json().catch(() => ({})) as { ok?: boolean; promoted?: string; error?: string };
      if (r.ok && j.ok) { setOpen({ ...e, promoted: j.promoted || 'yes' }); onPromoted?.(); setPromoteNote('Added to the server’s bookmarks.'); }
      else setPromoteNote(j.error || 'Only the server owner can add bookmarks.');
    } catch { setPromoteNote('Could not reach the server.'); }
  };
  const pages = data ? dxPageCount(data.total) : 1;
  const canPromote = !!(data?.canPromote || ownerQuery);
  const sel = (on: boolean) => on ? { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg } : null;
  const selT = (on: boolean) => on ? { color: pt.ink.selected } : null;
  const close = () => { setOpen(null); onClose(); };

  return (
    <Modal visible={visible} animationType="fade" transparent onRequestClose={() => (open ? setOpen(null) : close())}>
      <View style={s.scrim}>
        <SafeAreaView style={s.panel} edges={['top', 'bottom']}>
          <View style={s.head}>
            <Text style={s.title}>DX LOG</Text>
            <TouchableOpacity onPress={open ? () => setOpen(null) : close} style={s.key} accessibilityRole="button">
              <Text style={s.keyText}>{open ? '‹ BACK' : 'CLOSE'}</Text>
            </TouchableOpacity>
          </View>
          {open && (() => {
            const tgt = dxTuneTarget(open);
            return (
              <ScrollView style={{ flex: 1 }} contentContainerStyle={scrollLane}>
                <Text style={s.big}>{`${dxWhere(open)}${open.band === 'FM' ? ' MHz' : ''} · ${dxTitle(open)}`}</Text>
                <Text style={s.dim}>{dxSubline(open)}</Text>
                {!!dxSuspectLine(open) && <Text style={s.warn}>{dxSuspectLine(open)}</Text>}
                {open.band === 'FM' && (open.psVariants?.length ?? 0) > 1 && <Text style={s.dim}>{`PS seen: ${open.psVariants!.join(' · ')}`}</Text>}
                {open.band === 'DAB' && (open.services?.length ?? 0) > 0 && <Text style={s.dim}>{`Services: ${open.services!.map((x) => x.label).join(' · ')}`}</Text>}
                <Text style={s.dim}>{`First heard ${dxWhen(open.firstHeard)} · present in ${Math.round((open.presence || 0) * 100)} % of its sweeps this week`}</Text>
                <View style={s.actions}>
                  {!!tgt && (
                    <TouchableOpacity style={s.key} accessibilityRole="button" accessibilityLabel={`Tune to ${dxTitle(open)}`}
                                      onPress={() => { onTune(tgt, dxTitle(open)); close(); }}>
                      <Text style={s.keyText}>TUNE</Text>
                    </TouchableOpacity>
                  )}
                  {canPromote && (
                    <TouchableOpacity style={[s.key, !!open.promoted && s.off]} disabled={!!open.promoted} accessibilityRole="button"
                                      accessibilityLabel="Copy this station into the server's bookmarks"
                                      onPress={() => { void promote(open); }}>
                      <Text style={s.keyText}>{open.promoted ? 'BOOKMARKED' : 'BOOKMARK'}</Text>
                    </TouchableOpacity>
                  )}
                </View>
                {!!promoteNote && <Text style={s.dim}>{promoteNote}</Text>}
                <Text style={s.section}>OPENINGS</Text>
                {(open.openingList || []).map((o, i) => (
                  <View key={i} style={s.oRow}>
                    <Text style={s.oCell}>{dxWhen(o.start)}</Text>
                    <Text style={s.oCell}>{`to ${dxWhen(o.end)}`}</Text>
                    <Text style={s.oNum}>{`${o.detections}×`}</Text>
                    <Text style={s.oNum}>{`${Math.round(o.peakSnrDb)} dB`}</Text>
                  </View>
                ))}
              </ScrollView>
            );
          })()}
          <View style={open ? { display: 'none' } : { flex: 1 }}>
            <View style={s.filters}>
              {(['', 'FM', 'DAB'] as const).map((k) => (
                <TouchableOpacity key={k || 'all'} onPress={() => { setBand(k); setPage(1); }} style={[s.key, sel(band === k)]}
                                  accessibilityRole="button" accessibilityState={{ selected: band === k }}>
                  <Text style={[s.keyText, selT(band === k)]}>{k || 'ALL'}</Text>
                </TouchableOpacity>
              ))}
              <TouchableOpacity onPress={() => { setShowLocal((v) => !v); setPage(1); }} style={[s.key, sel(showLocal)]}
                                accessibilityRole="switch" accessibilityState={{ checked: showLocal }}>
                <Text style={[s.keyText, selT(showLocal)]}>SHOW LOCALS</Text>
              </TouchableOpacity>
              <TouchableOpacity onPress={() => { setShowSuspect((v) => !v); setPage(1); }} style={[s.key, sel(showSuspect)]}
                                accessibilityRole="switch" accessibilityState={{ checked: showSuspect }}>
                <Text style={[s.keyText, selT(showSuspect)]}>SHOW SUSPECTS</Text>
              </TouchableOpacity>
            </View>
            <View style={s.filters}>
              {DX_SORTS.map((o) => (
                <TouchableOpacity key={o.id} onPress={() => { setSort(o.id); setPage(1); }} style={[s.key, sel(sort === o.id)]}
                                  accessibilityRole="button" accessibilityState={{ selected: sort === o.id }}>
                  <Text style={[s.keyText, selT(sort === o.id)]}>{o.label}</Text>
                </TouchableOpacity>
              ))}
            </View>
            {loading && !data ? <ActivityIndicator style={{ marginTop: 24 }} /> : (
              <FlatList ref={listRef} data={data?.entries || []} keyExtractor={(e) => String(e.id)} contentContainerStyle={scrollLane}
                ListEmptyComponent={<Text style={s.dim}>{err || dxEmptyLine(data, showLocal)}</Text>}
                ListFooterComponent={pages > 1 ? (
                  <View style={s.pager} accessibilityRole="toolbar">
                    <TouchableOpacity onPress={() => setPage(Math.max(1, page - 1))} disabled={page <= 1} style={[s.key, page <= 1 && s.off]}
                                      accessibilityRole="button" accessibilityLabel="Newer page">
                      <Text style={s.keyText}>{'‹ Newer'}</Text>
                    </TouchableOpacity>
                    <Text style={s.pageLab}>{`Page ${page} of ${pages}`}</Text>
                    <TouchableOpacity onPress={() => setPage(Math.min(pages, page + 1))} disabled={page >= pages} style={[s.key, page >= pages && s.off]}
                                      accessibilityRole="button" accessibilityLabel="Older page">
                      <Text style={s.keyText}>{'Older ›'}</Text>
                    </TouchableOpacity>
                  </View>
                ) : null}
                renderItem={({ item: e }) => (
                  <TouchableOpacity onPress={() => { void openEntry(e); }} accessibilityRole="button"
                                    accessibilityLabel={`${dxWhere(e)} ${dxTitle(e)}, ${dxTags(e).join(', ')}`}
                                    style={[s.row, e.class === 'suspect' && { opacity: 0.55 }]}>
                    <Text style={s.freq}>{dxWhere(e)}</Text>
                    <View style={{ flex: 1 }}>
                      <Text style={s.name}>{dxTitle(e)}
                        {dxTags(e).map((t) => (
                          <Text key={t} style={t === 'LIVE NOW' ? s.live : s.tag}>{`  ${t}`}</Text>
                        ))}
                      </Text>
                      <Text style={s.dim}>{dxSubline(e, data?.now)}</Text>
                      {!!dxSuspectLine(e) && <Text style={s.dim}>{dxSuspectLine(e)}</Text>}
                    </View>
                  </TouchableOpacity>
                )} />
            )}
            {!!data && <Text style={s.dim}>{dxFooter(data, data.entries.length)}</Text>}
            <View style={s.alertRow}>
              <Switch value={alertsOn} onValueChange={onAlerts} accessibilityLabel="Tell me about new DX on this server" />
              <Text style={[s.dim, { marginTop: 0, marginLeft: 8, flex: 1 }]}>Tell me about new DX here (this device)</Text>
            </View>
          </View>
        </SafeAreaView>
      </View>
    </Modal>
  );
}

function makeStyles(pt: PopupTokens) {
  return StyleSheet.create({
    scrim: { flex: 1, backgroundColor: 'rgba(0,0,0,0.6)', alignItems: 'center', justifyContent: 'center' },
    panel: { width: '94%', maxWidth: 640, height: '86%', backgroundColor: 'rgba(8,6,2,0.98)', borderRadius: 12,
             borderWidth: 1, borderColor: pt.ink.border, padding: 14 },
    head: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 },
    title: { color: pt.ink.section, fontSize: 15, letterSpacing: 2, fontWeight: '600' },
    section: { color: pt.ink.section, fontSize: 12, letterSpacing: 2, marginTop: 12, marginBottom: 4 },
    key: { borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginRight: 6, marginBottom: 6 },
    keyText: { color: pt.ink.key, fontSize: 13, letterSpacing: 1 },
    filters: { flexDirection: 'row', flexWrap: 'wrap', marginBottom: 4 },
    actions: { flexDirection: 'row', flexWrap: 'wrap', marginTop: 10 },
    row: { flexDirection: 'row', paddingVertical: 8, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: pt.ink.divider },
    freq: { color: pt.gold.readout, width: 72, fontVariant: ['tabular-nums'] },
    name: { color: pt.ink.text, fontSize: 14 },
    big: { color: pt.ink.text, fontSize: 16, marginBottom: 4 },
    tag: { color: pt.ink.textFaint, fontSize: 10, letterSpacing: 1 },
    live: { color: pt.ink.selected, fontSize: 10, letterSpacing: 1, fontWeight: '700' },
    dim: { color: pt.ink.textDim, fontSize: 12, marginTop: 4 },
    warn: { color: pt.ink.selected, fontSize: 12, marginTop: 4 },
    oRow: { flexDirection: 'row', paddingVertical: 4, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: pt.ink.divider },
    oCell: { color: pt.ink.text, fontSize: 12, flex: 1 },
    oNum: { color: pt.ink.textDim, fontSize: 12, width: 56, textAlign: 'right', fontVariant: ['tabular-nums'] },
    pager: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', marginTop: 10 },
    pageLab: { color: pt.ink.textDim, fontSize: 12, marginRight: 6, marginBottom: 6 },
    off: { opacity: 0.35 },
    alertRow: { flexDirection: 'row', alignItems: 'center', marginTop: 8 },
  });
}
