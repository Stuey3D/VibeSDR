/**
 * GalleryOverlay — a VibeServer's GALLERY in the app (V12, 2026-10-22): what the server's decoders heard, kept on the
 * server (vibe_gallery.h) — background decoding while nobody listened (Stuart, for NickB's NAVTEX the night Storm
 * Austen came in) and listeners' own decoders. The same list as the web client's GALLERY key: newest first, filters by
 * decoder, a NAVTEX message / RTTY run / FT8 summary opens as text, a WEFAX chart or SSTV picture as an image, and
 * OPEN hands the item to the phone (its own browser saves or shares it).
 * ★ Text from the air is shown as text, never parsed or linked. Read-only: nothing here changes the server.
 * ★★ V12 beta 4 — PAGES (utils/galleryPager.ts, the web client's rule): 25 a page, newest first, the pager at the
 *   bottom ("‹ Newer · 1 … 6 7 8 … 14 · Older ›", "Page 7 of 14"); the filter kept across pages, page 1 on a filter
 *   change; BACK from an item returns to the same page and scroll — the list stays MOUNTED (hidden) under the item, so
 *   it is neither reloaded nor scrolled back to the top. An older server sends no "total": its newest 300, no pager.
 * ★ Offered only on a VibeServer (SDRScreen passes `galleryAvail` from client.isVibe): a GALLERY key on a receiver that
 *   has none would be a dead control (AGENTS.md).
 * ★★ TRANSLATE (2026-10-23) beside ‹ BACK on a text item: the message through the phone's own translator — the one
 *   TRANSCRIBE's captions use (Apple Translation / ML Kit), into TRANSLATE TO (the phone's language when that is off).
 *   Line by line, so the layout survives; ZCZC/NNNN, figures, positions, fade markers and garbage are left as received
 *   (utils/galleryTranslate.ts). Shown in place, marked MACHINE TRANSLATION, ORIGINAL toggles back. Drawn only when the
 *   phone can translate, the language is known and is not already the target.
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { ActivityIndicator, FlatList, Image, Linking, Modal, Platform, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { usePopupTheme, scrollLane, type PopupTokens } from './PopupShell';
import { withReadAuth } from '../services/vibeAuth';
import { galleryClampPage, galleryPageCount, galleryPageLabel, galleryPageQuery, galleryPagerSlots } from '../utils/galleryPager';
import { galleryCanTranslate, galleryGuessLanguage, galleryHintLanguage, galleryTranslatableText, galleryTranslate,
         galleryTranslateLines, type GalleryTrLine } from '../utils/galleryTranslate';
import { detectTextLanguage, langName, textTranslateInfo, translateText } from '../services/transcriber';

export interface GalleryItem {
  f: string; k: string; o: string; t: number; hz: number; m: string; d: string; p: string; r: string;
  title: string; partial?: boolean; wl?: boolean; bytes: number;
}
const KINDS: { id: string; label: string }[] = [
  { id: '', label: 'ALL' }, { id: 'navtex', label: 'NAVTEX' }, { id: 'rtty', label: 'RTTY' },
  { id: 'wefax', label: 'WEFAX' }, { id: 'sstv', label: 'SSTV' }, { id: 'ft8', label: 'FT8' },
];
const p2 = (n: number) => String(n).padStart(2, '0');
export function galleryWhen(t: number): string {
  const d = new Date(t * 1000);
  return `${d.getUTCFullYear()}-${p2(d.getUTCMonth() + 1)}-${p2(d.getUTCDate())} ${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())} UTC`;
}
export function galleryFreq(hz: number): string { return hz < 30e6 ? `${(hz / 1e3).toFixed(1)} kHz` : `${(hz / 1e6).toFixed(4)} MHz`; }
function size(b: number): string { return b >= 1_048_576 ? `${(b / 1_048_576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`; }

export default function GalleryOverlay({ visible, base, readAuth = '', translateTo = '', onClose }: {
  /** TRANSCRIBE's TRANSLATE TO ('' = off → the phone's own language). */
  visible: boolean; base: string; readAuth?: string; translateTo?: string; onClose: () => void;
}) {
  const pt = usePopupTheme();
  const s = useMemo(() => makeStyles(pt), [pt]);
  const b = base.replace(/\/+$/, '');
  const [kind, setKind] = useState('');
  const [page, setPage] = useState(1);
  const [pages, setPages] = useState(1);
  const [items, setItems] = useState<GalleryItem[] | null>(null);
  const listRef = React.useRef<FlatList<GalleryItem>>(null);
  const [note, setNote] = useState('');
  const [open, setOpen] = useState<GalleryItem | null>(null);
  const [text, setText] = useState('');
  const [textOk, setTextOk] = useState(false);
  // ★ TRANSLATE: the plan (what is sent, from what, into what), the result, which is shown, and what to say.
  const [trPlan, setTrPlan] = useState<{ lines: GalleryTrLine[]; source: string; target: string } | null>(null);
  const [trText, setTrText] = useState<string | null>(null);
  const [showTr, setShowTr] = useState(false);
  const [trBusy, setTrBusy] = useState(false);
  const [trNote, setTrNote] = useState('');

  const load = useCallback(async () => {
    setItems(null); setNote('');
    try {
      let r = await fetch(withReadAuth(`${b}/vibeserver/gallery.json?${galleryPageQuery(page, kind)}`, readAuth), { cache: 'no-store' } as any);
      let j = await r.json();
      const paged = typeof j?.total === 'number';
      if (r.ok && !j?.error && !paged) {   // ★ an older server: its newest 300, no pages
        r = await fetch(withReadAuth(`${b}/vibeserver/gallery.json?limit=300${kind ? `&kind=${kind}` : ''}`, readAuth), { cache: 'no-store' } as any);
        j = await r.json();
      }
      if (!r.ok || j?.error) { setItems([]); setPages(1); setNote(String(j?.error || 'This server has no gallery.')); return; }
      const n = paged ? galleryPageCount(j.total) : 1;
      setPages(n);
      if (paged && page > n && j.total > 0) { setPage(n); return; }   // ★ the gallery shrank under us
      setItems(Array.isArray(j.items) ? j.items : []);
      listRef.current?.scrollToOffset({ offset: 0, animated: false });
      setNote(j.enabled === false ? 'This server keeps no gallery.'
        : `${j.count ?? 0} item${j.count === 1 ? '' : 's'} on this server · ${size(j.usedBytes || 0)}. Times are UTC.`);
    } catch { setItems([]); setNote('This server has no gallery (it may need updating).'); }
  }, [b, kind, page, readAuth]);
  // ★ Loaded on opening, a filter and a page — NOT on BACK from an item (the list under it is still there).
  useEffect(() => { if (visible) void load(); }, [visible, load]);
  useEffect(() => { if (!visible) { setOpen(null); setPage(1); } }, [visible]);

  const fileUrl = (it: GalleryItem) => withReadAuth(`${b}/vibeserver/gallery/file/${encodeURIComponent(it.f)}`, readAuth);
  const isImage = (it: GalleryItem) => /\.png$/i.test(it.f);
  const show = async (it: GalleryItem) => {
    setOpen(it); setText(''); setTextOk(false);
    if (isImage(it)) return;
    try {
      const r = await fetch(fileUrl(it), { cache: 'no-store' } as any);
      setText(r.ok ? await r.text() : 'This item is no longer on the server (the gallery is full and keeps the newest).');
      setTextOk(r.ok);
    } catch { setText('Could not reach the server.'); }
  };
  // ★ Whether to draw TRANSLATE for the item now open — worked out once its text is here.
  useEffect(() => {
    setTrPlan(null); setTrText(null); setShowTr(false); setTrBusy(false); setTrNote('');
    if (!open || !textOk || !text || isImage(open)) return;
    let dead = false;
    void (async () => {
      const info = await textTranslateInfo();
      if (dead || !info.available) return;
      const target = translateTo || info.deviceLanguage || '';
      const lines = galleryTranslateLines(text, open.k);
      const body = galleryTranslatableText(lines);
      if (!body || !target) return;
      const hint = galleryHintLanguage(open.title);
      const guess = galleryGuessLanguage(body, hint);
      // ★ iOS: NaturalLanguage reads it, with the station's language and the stopword guess as its priors.
      const source = (info.detect ? await detectTextLanguage(body, [hint, guess].filter(Boolean)) : '') || guess;
      if (!dead && galleryCanTranslate(lines, source, target)) setTrPlan({ lines, source, target });
    })();
    return () => { dead = true; };
  }, [open, text, textOk, translateTo]);
  const onTranslate = async () => {
    if (!trPlan || trBusy) return;
    if (trText != null) { setShowTr(v => !v); return; }
    setTrBusy(true); setTrNote('');
    const r = await galleryTranslate(trPlan.lines, trPlan.target, (t) => translateText(t, trPlan.source, trPlan.target));
    setTrBusy(false);
    if (!r.done) { setTrNote(r.reason || 'The translation did not work on this phone.'); return; }
    setTrText(r.text); setShowTr(true);
    setTrNote(r.failed ? `${r.failed} line${r.failed === 1 ? '' : 's'} left as received${r.reason ? ` (${r.reason})` : ''}` : '');
  };
  const close = () => { setOpen(null); onClose(); };

  return (
    <Modal visible={visible} animationType="fade" transparent onRequestClose={() => (open ? setOpen(null) : close())}>
      <View style={s.scrim}>
        <SafeAreaView style={s.panel} edges={['top', 'bottom']}>
          <View style={s.head}>
            <Text style={s.title}>GALLERY</Text>
            <View style={{ flexDirection: 'row' }}>
              {open && trPlan && (
                <TouchableOpacity onPress={() => { void onTranslate(); }} style={[s.key, showTr && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}
                                  accessibilityRole="button" accessibilityState={{ busy: trBusy, selected: showTr }}
                                  accessibilityLabel={showTr ? 'Show the original' : `Translate from ${langName(trPlan.source)}`}>
                  <Text style={[s.keyText, showTr && { color: pt.ink.selected }]}>{trBusy ? 'TRANSLATING…' : showTr ? 'ORIGINAL' : 'TRANSLATE'}</Text>
                </TouchableOpacity>
              )}
              <TouchableOpacity onPress={open ? () => setOpen(null) : close} style={s.key} accessibilityRole="button">
                <Text style={s.keyText}>{open ? '‹ BACK' : 'CLOSE'}</Text>
              </TouchableOpacity>
            </View>
          </View>
          {open && (
            <View style={{ flex: 1 }}>
              <Text style={s.meta}>
                {`${open.title} · ${galleryFreq(open.hz)} ${(open.m || '').toUpperCase()} · ${galleryWhen(open.t)} · `
                 + (open.o === 'background' ? `background decoding${open.p ? ` (${open.p})` : ''}` : 'a listener\'s decoder')
                 + (open.partial ? ' · PARTIAL' : '')}
              </Text>
              {isImage(open) ? (
                <ScrollView style={{ flex: 1 }} contentContainerStyle={[scrollLane, { alignItems: 'center' }]} maximumZoomScale={4}>
                  <Image source={{ uri: fileUrl(open) }} style={s.image} resizeMode="contain" />
                </ScrollView>
              ) : (
                <>
                  {(showTr || !!trNote) && (
                    <Text style={s.trNote}>
                      {(showTr && trPlan ? `MACHINE TRANSLATION · ${langName(trPlan.source)} → ${langName(trPlan.target)} · on this device` : '')
                       + (showTr && trNote ? ' · ' : '') + trNote}
                    </Text>
                  )}
                  <ScrollView style={s.textBox} contentContainerStyle={scrollLane}>
                    <Text selectable style={s.mono}>{(showTr && trText != null ? trText : text) || 'Loading…'}</Text>
                  </ScrollView>
                </>
              )}
              <TouchableOpacity onPress={() => { void Linking.openURL(fileUrl(open) + (fileUrl(open).includes('?') ? '&' : '?') + 'download=1'); }}
                                style={[s.key, { alignSelf: 'flex-start', marginTop: 8 }]} accessibilityRole="button">
                <Text style={s.keyText}>OPEN / SAVE</Text>
              </TouchableOpacity>
            </View>
          )}
          {/* ★ The list stays mounted under an open item (display: none), so BACK lands on the same page and scroll. */}
            <View style={open ? { display: 'none' } : { flex: 1 }}>
              <View style={s.filters}>
                {KINDS.map((k) => (
                  <TouchableOpacity key={k.id || 'all'} onPress={() => { setKind(k.id); setPage(1); }}
                                    style={[s.key, kind === k.id && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                    <Text style={[s.keyText, kind === k.id && { color: pt.ink.selected }]}>{k.label}</Text>
                  </TouchableOpacity>
                ))}
              </View>
              {items == null ? <ActivityIndicator style={{ marginTop: 24 }} /> : (
                <FlatList ref={listRef} data={items} keyExtractor={(it) => it.f} contentContainerStyle={scrollLane}
                  ListFooterComponent={pages > 1 ? (
                    <View style={s.pager} accessibilityRole="toolbar">
                      <TouchableOpacity onPress={() => setPage(galleryClampPage(page - 1, pages))} disabled={page <= 1}
                                        style={[s.key, page <= 1 && s.off]} accessibilityRole="button" accessibilityLabel="Newer page">
                        <Text style={s.keyText}>{'\u2039 Newer'}</Text>
                      </TouchableOpacity>
                      {galleryPagerSlots(page, pages).map((sl, i) => sl === 'gap'
                        ? <Text key={`g${i}`} style={s.gap}>{'\u2026'}</Text>
                        : (
                          <TouchableOpacity key={sl} onPress={() => setPage(sl)} accessibilityRole="button" accessibilityLabel={`Page ${sl}`}
                                            accessibilityState={{ selected: sl === page }}
                                            style={[s.key, sl === page && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                            <Text style={[s.keyText, sl === page && { color: pt.ink.selected }]}>{String(sl)}</Text>
                          </TouchableOpacity>
                        ))}
                      <TouchableOpacity onPress={() => setPage(galleryClampPage(page + 1, pages))} disabled={page >= pages}
                                        style={[s.key, page >= pages && s.off]} accessibilityRole="button" accessibilityLabel="Older page">
                        <Text style={s.keyText}>{'Older \u203a'}</Text>
                      </TouchableOpacity>
                      <Text style={s.pageLab}>{galleryPageLabel(page, pages)}</Text>
                    </View>
                  ) : null}
                  ListEmptyComponent={<Text style={s.dim}>{kind ? 'Nothing from this decoder yet.' : 'Nothing decoded yet — background decoding fills this while nobody listens.'}</Text>}
                  renderItem={({ item: it }) => (
                    <TouchableOpacity onPress={() => { void show(it); }} style={s.row} accessibilityRole="button">
                      <Text style={s.freq}>{galleryFreq(it.hz)}</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{it.title || it.k.toUpperCase()}
                          <Text style={s.tag}>{`  ${it.o === 'background' ? (it.wl ? 'BACKGROUND · LISTENER ON' : 'BACKGROUND') : 'LISTENER'}${it.partial ? ' · PARTIAL' : ''}`}</Text>
                        </Text>
                        <Text style={s.dim}>{`${galleryWhen(it.t)} · ${(it.m || '').toUpperCase()} · ${it.d}${it.p ? ` · ${it.p}` : ''}${it.r ? ` · ${it.r}` : ''} · ${size(it.bytes)}`}</Text>
                      </View>
                    </TouchableOpacity>
                  )} />
              )}
              {!!note && <Text style={s.dim}>{note}</Text>}
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
    key: { borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 6, marginRight: 6, marginBottom: 6 },
    keyText: { color: pt.ink.key, fontSize: 13, letterSpacing: 1 },
    filters: { flexDirection: 'row', flexWrap: 'wrap', marginBottom: 6 },
    row: { flexDirection: 'row', paddingVertical: 8, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: pt.ink.divider },
    freq: { color: pt.gold.readout, width: 92, fontVariant: ['tabular-nums'] },
    name: { color: pt.ink.text, fontSize: 14 },
    tag: { color: pt.ink.textFaint, fontSize: 10, letterSpacing: 1 },
    dim: { color: pt.ink.textDim, fontSize: 12, marginTop: 4 },
    meta: { color: pt.ink.textDim, fontSize: 12, marginBottom: 8 },
    trNote: { color: pt.ink.selected, fontSize: 11, letterSpacing: 1, marginBottom: 6 },
    textBox: { flex: 1, borderWidth: 1, borderColor: pt.ink.border, borderRadius: 8, padding: 10, backgroundColor: 'rgba(0,0,0,0.25)' },
    mono: { color: pt.ink.text, fontFamily: Platform.select({ ios: 'Menlo', default: 'monospace' }), fontSize: 13, lineHeight: 19 },
    image: { width: '100%', aspectRatio: 1.2, backgroundColor: '#fff' },
    pager: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', marginTop: 10 },
    gap: { color: pt.ink.textDim, marginRight: 6, marginBottom: 6 },
    pageLab: { color: pt.ink.textDim, fontSize: 12, marginLeft: 'auto', marginBottom: 6 },
    off: { opacity: 0.35 },
  });
}
