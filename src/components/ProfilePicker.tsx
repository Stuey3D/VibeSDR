/**
 * OWRX profile picker — grouped by SDR, with the in-use etiquette warning.
 *
 * MOVED OUT OF THE MAIN MENU (2026-07-12). On OWRX a profile IS a frequency choice —
 * it's how you pick the band you want to be in — so it belongs where you go to change
 * frequency, not buried in settings. It also stops the menu sheet sitting over the
 * decoder panel while you switch to a decoding profile, which made ADS-B look like it
 * wasn't producing anything.
 *
 * The etiquette rules are unchanged, and they matter: an OWRX profile switch changes
 * the band/sample-rate for EVERYONE on that SDR. So an SDR in use is badged, the
 * server's own current profile is marked, and the live user count is shown — the point
 * being to turn "am I about to interrupt someone?" into something you can see rather
 * than guess.
 */
import { useEffect, useMemo, useState } from 'react';
import { View, Text, TouchableOpacity, ScrollView, StyleSheet } from 'react-native';
import { useRef } from 'react';
import { useListNav, useKeyboardMode, NAV_FOCUS } from './PanelNav';
import { NativeEventEmitter, NativeModules } from 'react-native';
import { useTheme, type ThemeTokens } from '../contexts/ThemeContext';

export interface ProfilePickerProps {
  /** True only while the tune card is on screen — see the note by the P shortcut. */
  active?: boolean;
  profiles: { id: string; name: string }[];
  activeProfileId?: string;
  /** From /status.json: which SDR each profile belongs to, and whether it's in use. */
  sdrUsage?: Record<string, { name: string; inUse: boolean; activeProfileId?: string }>;
  clientCount?: number;
  onSelectProfile?: (id: string) => void;
  /** Close the surface we're hosted in, once a profile is picked. */
  onPicked?: () => void;
}

export default function ProfilePicker({
  active = false,
  profiles, activeProfileId, sdrUsage = {}, clientCount = 0, onSelectProfile, onPicked,
}: ProfilePickerProps) {
  const { theme } = useTheme();
  const s = useMemo(() => styles(theme), [theme]);
  const [open, setOpen] = useState(false);

  // Profiles are LEFT in server order on purpose — they're SDR-type ordered, and
  // re-sorting risks the user tapping the wrong one and disturbing an SDR in use.
  const sdrGroups = useMemo(() => {
    const order: string[] = [];
    const byId = new Map<string, { id: string; name: string }[]>();
    for (const p of profiles) {
      const sid = p.id.includes('|') ? p.id.split('|')[0] : p.id;
      if (!byId.has(sid)) { byId.set(sid, []); order.push(sid); }
      byId.get(sid)!.push(p);
    }
    // The wire name is "{sdrName} {profileName}" — strip the prefix so the list reads
    // cleanly under its header. Fall back to the longest common prefix when
    // /status.json hasn't named the SDR.
    const lcp = (a: string[]) => {
      if (!a.length) return '';
      let pre = a[0];
      for (let i = 1; i < a.length; i++) {
        let k = 0;
        while (k < pre.length && k < a[i].length && pre[k] === a[i][k]) k++;
        pre = pre.slice(0, k);
      }
      return pre;
    };
    return order.map((sid) => {
      const items = byId.get(sid)!;
      const info = sdrUsage[sid];
      const sdrName = info?.name || lcp(items.map((i) => i.name)).replace(/\s+\S*$/, '').trim() || sid;
      const strip = (n: string) => (sdrName && n.startsWith(sdrName + ' ') ? n.slice(sdrName.length + 1) : n);
      return {
        sid, sdrName,
        inUse: !!info?.inUse,
        activeProfileId: info?.activeProfileId,
        items: items.map((i) => ({ id: i.id, label: strip(i.name) })),
      };
    });
  }, [profiles, sdrUsage]);

  // ★ ABOVE the `if (!profiles.length) return null` guard, deliberately: hooks after an
  // early return run on some renders and not others, which is the "Rendered more hooks
  // than during the previous render" crash. `open` is false when there are no profiles,
  // so the nav is inert anyway — it just has to be CALLED every time.
  // ★ Keyboard / D-pad navigation of the dropdown. The list is GROUPED, so navigation runs
  // over a flattened array of the selectable profiles only — the SDR header rows are labels,
  // not choices, and stopping on one would be a dead step.
  //
  // Because PanelNav's owner stack is last-in-wins, opening this takes the arrows away from
  // whatever is behind and hands them back on close, with no precedence rule to maintain.
  const flatProfiles = useMemo(
    () => sdrGroups.flatMap((g) => g.items.map((it) => it.id)),
    [sdrGroups],
  );
  // ★ The list SCROLLS — a server with many profiles overflows it — so focus has to drag the
  // view with it, exactly as the bookmarks pane needed. Without a reveal the caret walks off
  // the bottom of the dropdown and the list stays where it was.
  const listScroll = useRef<ScrollView | null>(null);
  const itemViews  = useRef<any[]>([]);

  const navFocus = useListNav(open, flatProfiles.length, (i) => {
    const id = flatProfiles[i];
    if (id != null) { onSelectProfile?.(id); setOpen(false); onPicked?.(); }
  },
    (i) => {
      const v = itemViews.current[i];
      v?.measureLayout?.(
        (listScroll.current as any)?.getInnerViewNode?.(),
        (_x: number, y: number) => listScroll.current?.scrollTo({ y: Math.max(0, y - 60), animated: true }),
        () => {},
      );
    },
    undefined,
    // Backspace leaves WITHOUT switching — a profile change retunes a shared SDR for
    // everyone on it, so backing out has to be as easy as opening.
    () => setOpen(false),
  );

  // ★ [P] OPENS IT. Stuart's call, and the right one: arrowing all the way down the tune
  // card to reach a dropdown is a lot of presses for something you want in one. Once open it
  // behaves like every other dropdown — up/down, Enter to choose, Backspace to leave.
  //
  // ★★ Gated on `active`, which the card passes as "visible AND on the tune tab". A modal's
  // children can stay MOUNTED while hidden, so listening on mount alone would have this
  // grabbing P from the rest of the app — the same trap that killed the keyboard app-wide
  // earlier today.
  const kbSeen = useKeyboardMode();
  useEffect(() => {
    if (!active) return;
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', (e: { key: string }) => {
      if (e?.key === 'P') setOpen(o => !o);
    });
    return () => sub.remove();
  }, [active]);

  if (!profiles.length) return null;

  return (
    <View style={s.wrap}>
      <Text style={s.label}>PROFILE</Text>

      {sdrGroups.some((g) => g.inUse) && (
        <View style={s.etiquette}>
          <Text style={s.etiquetteText}>
            <Text style={s.etiquetteLead}>Etiquette: </Text>
            if an SDR shows IN USE, check chat before changing its profile — you may
            interrupt another listener.
            {clientCount > 0 ? `  ${clientCount} user${clientCount === 1 ? '' : 's'} online.` : ''}
          </Text>
        </View>
      )}

      <View style={s.drop}>
        <TouchableOpacity style={[s.dropHead, open && { borderColor: NAV_FOCUS }]}
                          onPress={() => setOpen((o) => !o)} activeOpacity={0.7}>
          {kbSeen && (
            <View style={s.keyCap}><Text style={s.keyCapText}>P</Text></View>
          )}
          <Text style={s.dropHeadText} numberOfLines={1}>
            {profiles.find((p) => p.id === activeProfileId)?.name ?? 'Select profile'}
          </Text>
          <Text style={s.chevron}>{open ? '▴' : '▾'}</Text>
        </TouchableOpacity>

        {open && (
          <ScrollView ref={listScroll} style={s.list} nestedScrollEnabled keyboardShouldPersistTaps="handled">
            {sdrGroups.flatMap((g) => {
              const isCurrentSdr = g.items.some((it) => it.id === activeProfileId);
              return [
                <View key={'h:' + g.sid} style={s.headRow}>
                  <Text style={s.headText} numberOfLines={1}>{g.sdrName}</Text>
                  {isCurrentSdr && <Text style={[s.badge, s.badgeCurrent]}>CURRENT</Text>}
                  {g.inUse && !isCurrentSdr && <Text style={[s.badge, s.badgeInUse]}>IN USE</Text>}
                </View>,
                ...g.items.map((it) => {
                  const active = it.id === activeProfileId;          // our pick (green)
                  const serverActive = it.id === g.activeProfileId;  // tuned on the server (amber)
                  return (
                    <TouchableOpacity
                      key={it.id}
                      ref={(r: any) => { itemViews.current[flatProfiles.indexOf(it.id)] = r; }}
                      style={[s.item, serverActive && !active && s.itemInUse,
                              flatProfiles[navFocus] === it.id
                                && s.itemFocused]}
                      onPress={() => { onSelectProfile?.(it.id); setOpen(false); onPicked?.(); }}
                      activeOpacity={0.7}
                    >
                      <Text
                        style={[s.itemText,
                                serverActive && !active && s.itemTextInUse,
                                active && s.itemTextActive]}
                        numberOfLines={1}
                      >
                        {active ? '✓ ' : serverActive ? '● ' : ''}{it.label}
                        {serverActive && !active ? '  (in use)' : ''}
                      </Text>
                    </TouchableOpacity>
                  );
                }),
              ];
            })}
          </ScrollView>
        )}
      </View>
    </View>
  );
}

const styles = (t: ThemeTokens) => StyleSheet.create({
  wrap:          { gap: 6 },
  label:         { color: t.sectionColor, fontFamily: t.font, fontSize: 11,
                   fontWeight: 'bold', letterSpacing: 2 },
  etiquette:     { backgroundColor: t.barBg, borderRadius: 8, padding: 8,
                   borderWidth: 1, borderColor: t.barBorder },
  etiquetteText: { color: t.btnText, fontFamily: t.font, fontSize: 11, lineHeight: 15 },
  etiquetteLead: { color: t.snrColor, fontWeight: 'bold' },
  drop:          { borderWidth: 1, borderColor: t.barBorder, borderRadius: 8,
                   overflow: 'hidden' },
  // A real view, not styled text: iOS drops borders on nested Text runs.
  keyCap: { borderWidth: 1, borderColor: '#7CFF9B', borderRadius: 3, paddingHorizontal: 3, marginRight: 6 },
  keyCapText: { color: '#7CFF9B', fontSize: 11, fontWeight: '700' as const },
  dropHead:      { flexDirection: 'row', alignItems: 'center', gap: 8,
                   paddingHorizontal: 10, paddingVertical: 10, backgroundColor: t.barBg },
  dropHeadText:  { flex: 1, color: t.freqColor, fontFamily: t.font, fontSize: 13 },
  chevron:       { color: t.sectionColor, fontFamily: t.font, fontSize: 12 },
  list:          { maxHeight: 220, borderTopWidth: 1, borderTopColor: t.barBorder },
  headRow:       { flexDirection: 'row', alignItems: 'center', gap: 6,
                   paddingHorizontal: 10, paddingVertical: 6, backgroundColor: t.barBorder },
  headText:      { flex: 1, color: t.sectionColor, fontFamily: t.font, fontSize: 10,
                   fontWeight: 'bold', letterSpacing: 1 },
  badge:         { fontFamily: t.font, fontSize: 9, fontWeight: 'bold',
                   paddingHorizontal: 5, paddingVertical: 1, borderRadius: 4, overflow: 'hidden' },
  badgeCurrent:  { color: '#0b0b0b', backgroundColor: '#3ddc84' },
  badgeInUse:    { color: '#0b0b0b', backgroundColor: '#f5a524' },
  item:          { paddingHorizontal: 14, paddingVertical: 9 },
  itemInUse:     { backgroundColor: 'rgba(245,165,36,0.10)' },
  // Focus is a background tint, not a border: these rows are a dense list and a border
  // would shift every row as focus moved down it.
  itemFocused: { backgroundColor: 'rgba(124,255,155,0.18)' },
  itemText:      { color: t.btnText, fontFamily: t.font, fontSize: 13 },
  itemTextInUse: { color: '#f5a524' },
  itemTextActive:{ color: '#3ddc84', fontWeight: 'bold' },
});
