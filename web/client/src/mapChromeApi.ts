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
import { TrackView, esc, type TrackViewOpts, type LatLon, type LayerGlyph } from './mapChrome';
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

/* ══ ★★★ b97-maps-port ADDITIONS (the amateur / spots map and the directory's community map). Additive only — nothing above
 *  renamed or removed. Noted for the merge with b96-mapcards: HOST MAP (hostMap / MapHost) reaches into TrackView's
 *  private members by name (gen, mapHost, acMap, af, fitBtn, watchAttrib, attribAuto, scaleDock, zoomStateSync,
 *  cardsObstacle, mapSay, note, fitPauseForCards) — if one of those is renamed in mapChrome.ts, rename it here too
 *  (test-web-digimap-cards / test-community-render catch it: no scale, no AUTO). ══ */

/** ★ A map the HOST draws (its own MapLibre / Leaflet layers, its own popups): the chrome sits around it and keeps its
 *  scale chip, the credits' auto-fold and — given `points` — the shared AUTO / FIT (mapAutoFit.ts) on it. */
export interface MapHost {
  /** Leaflet-like for the scale chip: getCenter() → {lat,lng}, getZoom() in LEAFLET zoom (MapLibre's + 1), on('a b', f). */
  map: any;
  /** The camera the auto fit drives: a Leaflet map, a MapLibre map or a VibeMapGL vm (mapAutoFit.ts attach). Default `map`. */
  fitMap?: any;
  /** The VibeMapGL vm when it is the GPU map (for host layers that ask — whenMap). */
  gl?: any;
  /** Every point AUTO / FIT should frame ([lat, lon]) — given, the shared auto fit runs on this map. */
  points?: () => Array<[number, number]>;
  receiver?: () => { lat: number; lon: number } | null;
  maxZoom?: number;
  /** What one point is, for the AUTO key's words ("station shown"). */
  what?: string;
  /** The map goes with the view (hide / destroy). */
  destroy?: () => void;
}
export interface MapChromeHostOpts extends MapChromeOpts {
  /** ★ The host makes its map in `holder` (absolutely placed, under every chip) — TrackView makes none. */
  hostMap?: (holder: HTMLElement) => MapHost | null | Promise<MapHost | null>;
}
/** A LIST row in the house look (main.ts trkRowHtml's shape, without the aircraft tags): a colour dot, the name, a small
 *  `pill` (distance | band …), the subtitle; the age on the right. `key` is what onRow hands back. */
export function mapRowHtml(x: { key: string; colour: string; name: string; second?: string; pill?: string[]; age?: string; dim?: boolean; extra?: string }): string {
  const pill = (x.pill || []).filter(Boolean);
  return `<div class="trkRow kvRow${x.dim ? ' nopos' : ''}" role="button" tabindex="0" data-nofocus="1" data-key="${esc(x.key)}">`
    + `<span class="id"><b><i class="sw" style="background:${/^#[0-9a-fA-F]{3,8}$|^rgba?\([\d.,\s]+\)$/.test(x.colour) ? x.colour : '#9fb0c2'}"></i><span class="nmT">${esc(x.name)}</span>`
    + (pill.length ? `<span class="kv">${pill.map((t) => `<span>${esc(t)}</span>`).join('<i></i>')}</span>` : '') + `</b><small>${esc(x.second || '')}</small></span>`
    + `<span class="rg"></span><span class="ag"><span class="agL">${esc(x.age || '')}</span><span class="agS">${esc(String(x.age || '').replace(/^(\d+)\s*([a-z]).*$/i, '$1$2'))}</span></span>${x.extra || ''}</div>`;
}
export interface MapChromeMore extends MapChrome {
  /** ★ The host's own section in MAP CONTROLS (shown once asked for): its filters (band, mode, NAVTEX type …). */
  controls(): HTMLElement;
  /** ★ The time filter in MAP CONTROLS (TIME): ranges, the current one; onRange from the opts or `f`. */
  ranges(ranges: Array<{ id: string; label: string; words: string }>, current: string, f?: (id: string) => void, label?: string): void;
  /** ★ The KEY fold in MAP CONTROLS ('' = none). */
  legend(html: string): void;
  /** ★ A card's title (and its chip's caption) — SERVERS as the community map's RECEIVERS. */
  retitle(id: CardId, title: string): void;
  /** ★ The shared auto fit on a host map: frame now / let it look again / hold it (a station chosen). */
  fitNow(): void;
  fitPoke(): void;
  fitPause(): void;
  /** ★ A card's body for the host's own rows (the community map's RECEIVERS, with LISTEN and ONLY THIS); `avail` offers
   *  its chip. The host owns what is in it from then on. */
  cardBody(id: CardId, avail?: boolean): HTMLElement;
  /** ★ The view's root (cards, rows) — for a host that fills a row after drawing it (a NAVTEX message's text). */
  readonly root: HTMLElement;
}

export function mountMapChrome(o: MapChromeHostOpts): MapChromeMore {
  let toggleCb: ((id: string, on: boolean) => void) | null = null;
  let rangeCb: ((id: string) => void) | null = null;
  const v = new TrackView({ ...o, onToggle: (id, on) => { toggleCb?.(id, on); o.onToggle?.(id, on); }, onRange: (id) => { rangeCb?.(id); o.onRange?.(id); } });
  const t = v as any;
  let host: MapHost | null = null;
  if (o.hostMap) {
    const make = o.hostMap;
    // ★ TrackView.show() → boot(): the host's map instead of TrackView's own (and no track layer)
    t.boot = async (my: number) => {
      const holder = document.createElement('div');
      holder.style.cssText = 'position:absolute;inset:0;z-index:0';
      t.mapHost.insertBefore(holder, t.mapHost.firstChild);
      let hm: MapHost | null = null;
      try { hm = await make(holder); } catch (e) { console.error('[mapChrome] the host map', e); }
      if (my !== t.gen) { try { hm?.destroy?.(); } catch { /* tidy */ } return; }
      if (!hm) { t.note('The map could not be loaded here — the list still updates.'); return; }
      host = hm;
      t.acMap = hm.map;
      const w = window as any;
      if (hm.points && w.VibeAutoFit) {
        try {
          t.af = w.VibeAutoFit.attach(hm.fitMap ?? hm.map, {
            points: hm.points, receiver: hm.receiver ?? (() => null), button: t.fitBtn, maxZoom: hm.maxZoom,
            what: hm.what || o.listTitle.toLowerCase().replace(/s$/, ''),
            onState: () => t.zoomStateSync(), obstacle: () => t.cardsObstacle(),
          });
        } catch (e) { console.error('[mapChrome] auto fit', e); t.af = null; }
      }
      t.fitBtn.hidden = !t.af;
      (t.root.querySelector('.fitOnce') as HTMLElement).hidden = !t.af;
      try { t.watchAttrib(); t.attribAuto(); t.scaleDock(); } catch (e) { console.error('[mapChrome] host map chrome', e); }
      t.zoomStateSync();
      t.mapSay({ gl: hm.gl ?? null, lm: hm.gl ? null : hm.map });
    };
    const hide0 = v.hide.bind(v);
    v.hide = () => { hide0(); const h = host; host = null; try { h?.destroy?.(); } catch (e) { console.error('[mapChrome] host map destroy', e); } };
  }
  return {
    get root() { return v.root; },
    cardBody: (id, avail = true) => { v.cardsAvail(id, avail); v.cardsLayout(); return v.root.querySelector(`.trkCard2[data-card="${id}"] .cb`) as HTMLElement; },
    controls: () => { const s = v.slot('ctl'); (v.root.querySelector('.ctlSect') as HTMLElement).hidden = false; return s; },
    ranges: (r, cur, f, label) => { if (f) rangeCb = f; v.setRanges(r, cur, label); },
    legend: (h) => v.setLegend(h),
    retitle: (id, title) => {
      const ct = v.root.querySelector(`.trkCard2[data-card="${id}"] .ct`) as HTMLElement | null;
      if (ct) ct.textContent = title;
      const sec = v.root.querySelector(`.trkCard2[data-card="${id}"]`) as HTMLElement | null;
      sec?.setAttribute('aria-label', title);
      const cap = v.root.querySelector(`[data-cbtn="${id}"] .cap`) as HTMLElement | null;
      if (cap) cap.textContent = title;
      const b = v.root.querySelector(`[data-cbtn="${id}"]`) as HTMLElement | null;
      b?.setAttribute('aria-label', `${title} — open the ${title.toLowerCase()} card`);
    },
    fitNow: () => { try { t.af?.fitNow(); } catch (e) { console.error('[mapChrome] fit', e); } },
    fitPoke: () => { try { t.af?.poke(); } catch (e) { console.error('[mapChrome] fit', e); } },
    fitPause: () => { try { t.af?.pause(); } catch (e) { console.error('[mapChrome] fit', e); } },
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
