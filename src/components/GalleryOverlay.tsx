/**
 * GalleryOverlay — a VibeServer's GALLERY in the app (V12, 2026-10-22): what the server's decoders heard, kept on the
 * server (vibe_gallery.h) — background decoding while nobody listened (Stuart, for NickB's NAVTEX the night Storm
 * Austen came in) and listeners' own decoders. The same list as the web client's GALLERY key: newest first, filters by
 * decoder, a NAVTEX message / RTTY run / FT8 summary opens as text, a WEFAX chart or SSTV picture as an image, and
 * OPEN hands the item to the phone (its own browser saves or shares it).
 * ★ Text from the air is shown as text, never parsed or linked. Read-only: nothing here changes the server.
 * ★ Offered only on a VibeServer (SDRScreen passes `galleryAvail` from client.isVibe): a GALLERY key on a receiver that
 *   has none would be a dead control (AGENTS.md).
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { ActivityIndicator, FlatList, Image, Linking, Modal, Platform, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { usePopupTheme, scrollLane, type PopupTokens } from './PopupShell';
import { withReadAuth } from '../services/vibeAuth';

export interface GalleryItem {
  f: string; k: string; o: string; t: number; hz: number; m: string; d: string; p: string; r: string;
  title: string; partial?: boolean; bytes: number;
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

export default function GalleryOverlay({ visible, base, readAuth = '', onClose }: {
  visible: boolean; base: string; readAuth?: string; onClose: () => void;
}) {
  const pt = usePopupTheme();
  const s = useMemo(() => makeStyles(pt), [pt]);
  const b = base.replace(/\/+$/, '');
  const [kind, setKind] = useState('');
  const [items, setItems] = useState<GalleryItem[] | null>(null);
  const [note, setNote] = useState('');
  const [open, setOpen] = useState<GalleryItem | null>(null);
  const [text, setText] = useState('');

  const load = useCallback(async () => {
    setItems(null); setNote('');
    try {
      const r = await fetch(withReadAuth(`${b}/vibeserver/gallery.json?limit=300${kind ? `&kind=${kind}` : ''}`, readAuth), { cache: 'no-store' } as any);
      const j = await r.json();
      if (!r.ok || j?.error) { setItems([]); setNote(String(j?.error || 'This server has no gallery.')); return; }
      setItems(Array.isArray(j.items) ? j.items : []);
      setNote(j.enabled === false ? 'This server keeps no gallery.'
        : `${j.count ?? 0} item${j.count === 1 ? '' : 's'} on this server · ${size(j.usedBytes || 0)}. Times are UTC.`);
    } catch { setItems([]); setNote('This server has no gallery (it may need updating).'); }
  }, [b, kind, readAuth]);
  useEffect(() => { if (visible && !open) void load(); }, [visible, open, load]);

  const fileUrl = (it: GalleryItem) => withReadAuth(`${b}/vibeserver/gallery/file/${encodeURIComponent(it.f)}`, readAuth);
  const isImage = (it: GalleryItem) => /\.png$/i.test(it.f);
  const show = async (it: GalleryItem) => {
    setOpen(it); setText('');
    if (isImage(it)) return;
    try {
      const r = await fetch(fileUrl(it), { cache: 'no-store' } as any);
      setText(r.ok ? await r.text() : 'This item is no longer on the server (the gallery is full and keeps the newest).');
    } catch { setText('Could not reach the server.'); }
  };
  const close = () => { setOpen(null); onClose(); };

  return (
    <Modal visible={visible} animationType="fade" transparent onRequestClose={() => (open ? setOpen(null) : close())}>
      <View style={s.scrim}>
        <SafeAreaView style={s.panel} edges={['top', 'bottom']}>
          <View style={s.head}>
            <Text style={s.title}>GALLERY</Text>
            <TouchableOpacity onPress={open ? () => setOpen(null) : close} style={s.key} accessibilityRole="button">
              <Text style={s.keyText}>{open ? '‹ BACK' : 'CLOSE'}</Text>
            </TouchableOpacity>
          </View>
          {open ? (
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
                <ScrollView style={s.textBox} contentContainerStyle={scrollLane}><Text selectable style={s.mono}>{text || 'Loading…'}</Text></ScrollView>
              )}
              <TouchableOpacity onPress={() => { void Linking.openURL(fileUrl(open) + (fileUrl(open).includes('?') ? '&' : '?') + 'download=1'); }}
                                style={[s.key, { alignSelf: 'flex-start', marginTop: 8 }]} accessibilityRole="button">
                <Text style={s.keyText}>OPEN / SAVE</Text>
              </TouchableOpacity>
            </View>
          ) : (
            <View style={{ flex: 1 }}>
              <View style={s.filters}>
                {KINDS.map((k) => (
                  <TouchableOpacity key={k.id || 'all'} onPress={() => setKind(k.id)}
                                    style={[s.key, kind === k.id && { borderColor: pt.ink.selected, backgroundColor: pt.ink.selectedBg }]}>
                    <Text style={[s.keyText, kind === k.id && { color: pt.ink.selected }]}>{k.label}</Text>
                  </TouchableOpacity>
                ))}
              </View>
              {items == null ? <ActivityIndicator style={{ marginTop: 24 }} /> : (
                <FlatList data={items} keyExtractor={(it) => it.f} contentContainerStyle={scrollLane}
                  ListEmptyComponent={<Text style={s.dim}>{kind ? 'Nothing from this decoder yet.' : 'Nothing decoded yet — background decoding fills this while nobody listens.'}</Text>}
                  renderItem={({ item: it }) => (
                    <TouchableOpacity onPress={() => { void show(it); }} style={s.row} accessibilityRole="button">
                      <Text style={s.freq}>{galleryFreq(it.hz)}</Text>
                      <View style={{ flex: 1 }}>
                        <Text style={s.name}>{it.title || it.k.toUpperCase()}
                          <Text style={s.tag}>{`  ${it.o === 'background' ? 'BACKGROUND' : 'LISTENER'}${it.partial ? ' · PARTIAL' : ''}`}</Text>
                        </Text>
                        <Text style={s.dim}>{`${galleryWhen(it.t)} · ${(it.m || '').toUpperCase()} · ${it.d}${it.p ? ` · ${it.p}` : ''}${it.r ? ` · ${it.r}` : ''} · ${size(it.bytes)}`}</Text>
                      </View>
                    </TouchableOpacity>
                  )} />
              )}
              {!!note && <Text style={s.dim}>{note}</Text>}
            </View>
          )}
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
    textBox: { flex: 1, borderWidth: 1, borderColor: pt.ink.border, borderRadius: 8, padding: 10, backgroundColor: 'rgba(0,0,0,0.25)' },
    mono: { color: pt.ink.text, fontFamily: Platform.select({ ios: 'Menlo', default: 'monospace' }), fontSize: 13, lineHeight: 19 },
    image: { width: '100%', aspectRatio: 1.2, backgroundColor: '#fff' },
  });
}
