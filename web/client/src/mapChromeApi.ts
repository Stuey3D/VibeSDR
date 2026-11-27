/**
 * mapChromeApi.ts — ★★★★ THE STABLE SURFACE of the map card system (mapChrome.ts; Stuart's design, signed off 11-27).
 *
 * Any web map mounts the same chrome — chips, cards, the LIST drawer (and its live waterfall), the short-landscape top
 * row inside the safe rectangle, RATE, HELP, STATS, the shared-tuner pill, the bottom-right group — through this small
 * API, and never reaches into the DOM or the CSS. The air & sea and ISM maps still drive `TrackView` directly (they came
 * first); the amateur / spots map and the directory's community map are ported onto THIS.
 *
 *   const m = mountMapChrome({ title: 'SPOTS', exitLabel: 'CLOSE MAP', listTitle: 'SPOTS', onExit, icon, label, card });
 *   m.chips({ layers: true, stats: true, rate: true });           // which chips (and so cards) this map offers
 *   m.layers([{ id: 'ft8', label: 'FT8', words: 'FT8 spots', n: 120, colour: '#5BE36B' }], { ft8: true }, (id, on) => …);
 *   m.list({ items, rowsHtml, status: '120 spots', count: 120 });  // the LIST drawer's source (items drawn on the map)
 *   m.rate([{ name: 'FT8', rate: '42/min', live: true }]);         // the RATE chip's source
 *   m.show(rx);   …   m.destroy();
 *
 * ★ KEEP IT STABLE: add, never rename or remove — a second map is built on it. The ids are mapCards.ts CARD_IDS.
 */
import { TrackView, type TrackViewOpts, type LatLon, type LayerGlyph } from './mapChrome';
import type { CardId } from '../../../src/utils/mapCards';

export type { CardId, LatLon, LayerGlyph };
/** What the map draws: its title and exit words, and how an item looks (icon, label, card) — TrackViewOpts. */
export type MapChromeOpts = TrackViewOpts;
export interface MapLayer { id: string; label: string; words: string; group?: string; live?: boolean; n?: number; colour?: string; glyph?: LayerGlyph }
export interface MapListSource { items: any[]; rowsHtml: string; status: string; count: number; rx?: LatLon | null; safety?: string }
export interface MapRateRow { name: string; rate: string; live: boolean; words?: string; mine?: boolean }
export interface MapStatsSection { mode: string; rows: Array<{ key: string; label: string; value: string; name: string; lat?: number; lon?: number }> }

export interface MapChrome {
  /** The implementation, for what the API does not cover yet (ask to add it here rather than lean on this). */
  readonly view: TrackView;
  show(rx: LatLon | null): void;
  hide(): void;
  destroy(): void;
  /** Register the chips this map offers (each opens its card); LIST, MAP and HELP are always there. */
  chips(on: Partial<Record<CardId, boolean>>): void;
  /** Open / shut a card as if its chip were pressed. */
  card(id: CardId, open: boolean): void;
  /** LAYERS: the map's layers with their counts; `onToggle` when the listener shows / hides one. */
  layers(layers: MapLayer[], shown: Record<string, boolean>, onToggle: (id: string, on: boolean) => void): void;
  /** The LIST drawer: its rows and the items the map draws (one source). Tabs and sort are optional. */
  list(src: MapListSource): void;
  listTabs(tabs: Array<{ id: string; label: string; n: number }>, current: string, onTab: (id: string) => void): void;
  listSort(sort: string | null, onSort: (s: string) => void, what?: string): void;
  /** RATE: one row per live source (the main one first). */
  rate(rows: MapRateRow[]): void;
  /** STATS: sections of ranked rows; `onPick` with a row's key (show it on the map). */
  stats(sections: MapStatsSection[], onPick?: (key: string) => void): void;
  /** SERVERS: the other servers to open the same map on. */
  servers(rows: Array<{ id: string; label: string; sub: string }>, onPick: (id: string) => void): void;
  /** CHAT chip state. */
  chat(state: { show: boolean; disabled?: boolean; unread?: number; why?: string }): void;
  /** The HELP card's credit line (a data licence). */
  credit(html: string): void;
  /** Events: a LIST row / a map item chosen, a card opened or shut. */
  onRow(f: ((key: string) => void) | null): void;
  onSelect(f: ((key: string) => void) | null): void;
  onCard(f: ((id: CardId, open: boolean) => void) | null): void;
}

export function mountMapChrome(o: MapChromeOpts): MapChrome {
  let toggleCb: ((id: string, on: boolean) => void) | null = null;
  const v = new TrackView({ ...o, onToggle: (id, on) => { toggleCb?.(id, on); o.onToggle?.(id, on); } });
  return {
    view: v,
    show: (rx) => v.show(rx),
    hide: () => v.hide(),
    destroy: () => v.destroy(),
    chips: (on) => { for (const [id, yes] of Object.entries(on) as Array<[CardId, boolean]>) v.cardsAvail(id, !!yes); v.cardsLayout(); },
    card: (id, open) => v.cardOpen(id, open, true),
    layers: (layers, shown, onToggle) => {
      toggleCb = onToggle;
      v.setToggles(layers.map(({ glyph: _g, ...l }) => l), shown);
      v.setLayerSummary(layers.filter((l) => shown[l.id] !== false && (l.n ?? 0) > 0)
        .map((l) => ({ glyph: l.glyph ?? 'dot', n: l.n ?? 0, colour: l.colour ?? '#5BE36B', words: `${l.n} ${l.words}` })));
    },
    list: (s) => v.update(s.items, s.rowsHtml, s.status, s.count, s.rx ?? null, s.safety ?? ''),
    listTabs: (t, cur, f) => v.setTabs(t, cur, f),
    listSort: (s, f, what) => v.setSort(s, f, what),
    rate: (rows) => v.setRateRows(rows),
    stats: (s, f) => v.setStats(s, f),
    servers: (rows, f) => v.setServers(rows, f),
    chat: (s) => v.setChat(s),
    credit: (h) => v.setCredit(h),
    onRow: (f) => { v.onRow = f; },
    onSelect: (f) => { v.onSel = f; },
    onCard: (f) => { v.onCard = f; },
  };
}
