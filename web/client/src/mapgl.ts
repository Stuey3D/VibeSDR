/**
 * mapgl.ts — the GPU map (MapLibre + our own packs) for the server's own pages: the admin
 * "visitors by country" map and the digital spots map.
 *
 * ★★★ SENT FROM THE SERVER, RENDERED ON THE CLIENT. Every file comes from THIS server at /mapgl/ —
 *     the renderer, the style, the fonts, the icons and the tile packs (streamed by HTTP Range, only
 *     the tiles in view). Nothing comes from a CDN, so a LAN-only server still draws its maps
 *     (Stuart, 2026-09-28). The same renderer and style the app and the directory use.
 *
 * ★★ WHERE /mapgl/ IS: the ROOT of the page's origin, exactly like /mapdata/v1/. A radio page lives
 *    under /r/<id>/, but the front door serves /mapgl/ at the root (and a radio process strips its own
 *    prefix, so /r/<id>/mapgl/ would reach the same files — the root is simply one URL, cached once for
 *    every radio on the machine). ABSOLUTE, with the origin, because the spots map is an about:blank
 *    popup whose own location says nothing about where the server is.
 *
 * ★ Anything missing — no WebGL 2, an older server with no /mapgl/, a script that fails — returns
 *   null here and the caller keeps today's Leaflet map. Every failure is logged, never swallowed.
 */

export interface MapGLKit {
  /** e.g. "http://pi.local:8073/mapgl/" — absolute, trailing slash. */
  base: string;
  /** web/mapkit/vibemap-style.json as served by this server, parsed. */
  style: any;
  /** HEAD /mapgl/vibemap-detail.pmtiles answered 200: the owner installed High Detail Maps. */
  detail: boolean;
}

/** Absolute URL of this server's /mapgl/ directory, or '' where the page has no http(s) origin. */
export function mapglBase(): string {
  if (typeof location === 'undefined' || !/^https?:$/.test(location.protocol)) return '';
  return location.origin + '/mapgl/';
}

/** ★ Asked BEFORE loading ~800 KB of MapLibre: a browser without WebGL 2 keeps Leaflet. */
export function hasWebGL2(): boolean {
  try { return !!document.createElement('canvas').getContext('webgl2'); } catch { return false; }
}

let basePending: Promise<{ base: string; style: any } | null> | null = null;

/** Can this page draw the GPU map, and with what?
 *  ★ The style is fetched ONCE per page; whether the DETAIL pack is installed is asked on EVERY call.
 *    It was cached with the rest, so a pack downloaded from the admin page while the page was open was
 *    not used by the spots map opened afterwards (Stuart, 2026-09-28, Pi 500: "High detail maps
 *    downloaded but the spots map doesnt seem to be using them"). One HEAD per map opening is nothing.
 *  A failure is retried on the next call (a server mid-restart must not cost the map for the visit). */
export async function probeMapGL(): Promise<MapGLKit | null> {
  if (!basePending) {
    basePending = (async () => {
      const base = mapglBase();
      if (!base) return null;
      if (!hasWebGL2()) { console.info('GPU map: no WebGL 2 in this browser — using the Leaflet map'); return null; }
      try {
        const r = await fetch(base + 'vibemap-style.json', { cache: 'no-cache' });
        if (!r.ok) {
          // ★ An older server (no /mapgl/ yet) says 404 here. Leaflet it is — said, not hidden.
          console.error(`GPU map: ${base}vibemap-style.json answered HTTP ${r.status} — using the Leaflet map`);
          return null;
        }
        return { base, style: await r.json() };
      } catch (e) {
        console.error('GPU map: the style could not be loaded — using the Leaflet map', e);
        return null;
      }
    })();
    basePending.then((k) => { if (!k) basePending = null; });
  }
  const got = await basePending;
  if (!got) return null;
  let detail = false;
  try {
    const h = await fetch(got.base + 'vibemap-detail.pmtiles', { method: 'HEAD', cache: 'no-store' });
    detail = h.status === 200;
    // ★ 404 is the normal "not installed" answer; anything else is worth a line in the console.
    if (!h.ok && h.status !== 404) console.error(`GPU map: HEAD vibemap-detail.pmtiles answered HTTP ${h.status}`);
  } catch (e) {
    console.error('GPU map: could not ask whether the detail pack is installed', e);
  }
  return { base: got.base, style: got.style, detail };
}


/** The loader vibemapgl.js asks for fonts and icons with. A miss is logged and answered null (the
 *  renderer then skips that glyph range or icon — a missing label, never a dead map). */
export function mapglLoad(path: string, kind: 'arraybuffer' | 'text'): Promise<ArrayBuffer | string | null> {
  return fetch(path).then((r): Promise<ArrayBuffer | string> | null => {
    if (!r.ok) { console.error(`GPU map: ${path} answered HTTP ${r.status}`); return null; }
    return kind === 'text' ? r.text() : r.arrayBuffer();
  }).catch((e) => { console.error(`GPU map: ${path} could not be fetched`, e); return null; });
}

let scriptsPending: Promise<boolean> | null = null;

/** Load MapLibre, pmtiles, vibemapgl.js and the Leaflet-shaped adapter into THIS page, in order. */
export function loadMapGLScripts(base: string): Promise<boolean> {
  const w = window as any;
  if (w.VibeMapGL && w.VibeMapGLCompat && w.maplibregl && w.pmtiles) return Promise.resolve(true);
  if (scriptsPending) return scriptsPending;
  scriptsPending = new Promise<boolean>((resolve) => {
    const css = document.createElement('link');
    css.rel = 'stylesheet';
    css.href = base + 'vendor/maplibre-gl.css';
    document.head.appendChild(css);
    const files = ['maplibre-gl.js', 'pmtiles.js', 'vibemapgl.js', 'vibemapgl-compat.js'];
    let left = files.length, failed = false;
    for (const f of files) {
      const s = document.createElement('script');
      s.src = base + 'vendor/' + f;
      s.async = false;                    // ★ execute in THIS order: each one needs the one before it
      s.onload = () => { if (--left === 0 && !failed) resolve(true); };
      s.onerror = () => {
        console.error(`GPU map: ${s.src} failed to load — using the Leaflet map`);
        failed = true; resolve(false);
      };
      document.head.appendChild(s);
    }
  });
  scriptsPending.then((ok) => { if (!ok) scriptsPending = null; });
  return scriptsPending;
}
