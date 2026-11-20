/* ★★★ V12 SATELLITE MODE in the web client (docs/v12/SATELLITE-MODE.md, Stuart 2026-11-20): ONE whole-radio mode for
 *  every satellite — it replaced the SAT picker and the METEOR LRPT decoder key. The words, the order and the half-globe's
 *  geometry are the app's (src/services/satellites.ts) so both clients say the same thing.
 *
 *    header   SATELLITE · AUTO DOPPLER (default ON) · EXIT
 *    the pick (once the server says "picked"):
 *      voice / packet → COMPACT: name · downlink, the corrected frequency, "In range for 9:55" (or the next pass),
 *                       the decoder's line (APRS lines for packet), and a half-globe (2D canvas, no tiles): the bird
 *                       above its sub-point with its beam cone down to the ground, the receiver on the dome
 *      image          → BIG: the same readout with the picture as it arrives (the decoder box's own image canvas,
 *                       #decImgWrap, lent to this window) and the finished picture at pass end
 *      ISS 145.800 (sstv) opens compact and grows when an SSTV picture starts
 *    "Orbits synced 16:08 · 2 h ago" / "Fetching orbit data…" (the list re-asked every 3 s) / "No orbit data — why"
 *    the timetable: name · frequency · "UP · 6:12 left" / "AOS 16:42 (in 0:38)" — up now with the SHORTEST remaining pass
 *      first, then the next passes; what this radio cannot hear GREYED with why (a timetable, so greying is the rule here)
 *
 *  ★★ THE SERVER DOES THE WORK (THE CONTRACT): a row sends {"op":"pick"} — it tunes, locks the Doppler and attaches the
 *     decoder in one step, under the shared-dial contract (spectrum.ts asks before tuning). AUTO DOPPLER sends
 *     {"op":"doppler"}; leaving the mode (EXIT, the key again, another whole-radio mode) sends {"op":"drop"}.
 *  ★ No flashing: COAST dims the readout, nothing blinks. */
import {
  globeScene, satAltKm, satBoxSize, satCorrectedHz, satCountdownText, satCreditText, satHzText, satListEveryMs, satModeRows,
  satStateWord, satSyncedText, satTxCreditText, satWhenText, txText, txUse,
  type SatList, type SatPicked, type SatReadout, type SatTrack, type SatTx,
} from '../../../src/services/satellites';

export interface SatPanelDeps {
  list: () => void;
  /** {"op":"pick"} (spectrum.ts satPick — false = the ask-before-tuning gate held it). */
  pick: (norad: number, tx: number, doppler: boolean) => boolean;
  doppler: (on: boolean) => void;
  drop: () => void;
  /** The receiver's position (the server's location), for the half-globe — null = no globe. */
  rxPos: () => { lat: number; lon: number } | null;
  /** The server attached `decoder` for this pick: main.ts follows it on its decoder socket (to receive its frames). '' = none. */
  follow: (decoder: SatPicked['decoder'], tx: SatTx) => void;
  /** The mode was left: main.ts stops the followed decoder and takes its image canvas back. */
  left: () => void;
  /** Lend the decoder box's image canvas to `slot` (null = take it back). */
  image: (slot: HTMLElement | null) => void;
}

const CSS = `
#satPanel{position:fixed;z-index:60;right:12px;top:64px;width:min(440px,calc(100vw - 32px));max-height:calc(100vh - 96px);
  overflow:auto;background:var(--pill-bg);color:var(--btn-text);border:1px solid var(--btn-border);border-radius:10px;
  padding:10px 12px;font:13px/1.35 system-ui,sans-serif;box-shadow:0 6px 24px rgba(0,0,0,.45);box-sizing:border-box}
#satPanel.big{width:min(680px,calc(100vw - 32px))}
#satPanel[hidden]{display:none}
#satPanel .sat-h{display:flex;align-items:center;gap:8px;margin-bottom:6px}
#satPanel .sat-h b{flex:1;letter-spacing:.08em}
#satPanel button{background:var(--btn-bg);color:var(--btn-text);border:1px solid var(--btn-border);border-radius:6px;padding:4px 9px;
  font:inherit;cursor:pointer;min-height:30px}
#satPanel button.on{background:var(--btn-active-bg);border-color:var(--btn-active-bdr);color:var(--btn-active-tx)}
#satPanel button:disabled{opacity:.45;cursor:default}
#satPanel .sat-dim{color:var(--text-dim)}
#satPanel .sat-why{color:var(--text-dim);margin:4px 0}
#satPanel .sat-box{border:1px solid var(--btn-border);border-radius:8px;padding:8px;margin:6px 0}
#satPanel .sat-box[hidden]{display:none}
#satPanel .sat-ro{display:grid;grid-template-columns:minmax(0,1fr) 168px;gap:8px;align-items:center}
#satPanel .sat-ro.coast .sat-big{opacity:.55}
#satPanel .sat-name{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#satPanel .sat-big{font-size:18px;font-variant-numeric:tabular-nums;margin:2px 0}
#satPanel .sat-cd{font-variant-numeric:tabular-nums}
#satPanel canvas.sat-globe{width:168px;height:96px;display:block}
#satPanel .sat-img{margin-top:8px}
#satPanel .sat-img[hidden]{display:none}
#satPanel .sat-img #decImage{max-height:min(52vh,440px);object-fit:contain}
#satPanel .sat-pk{margin-top:6px;max-height:9.5em;overflow:auto;font:12px/1.35 ui-monospace,Menlo,monospace}
#satPanel .sat-pk[hidden]{display:none}
#satPanel .sat-sync{margin:4px 0}
#satPanel details.sat-list > summary{cursor:pointer;padding:4px 0;letter-spacing:.06em;list-style:none}
#satPanel details.sat-list > summary::-webkit-details-marker{display:none}
#satPanel details.sat-list > summary::before{content:'▸ ';color:var(--text-dim)}
#satPanel details.sat-list[open] > summary::before{content:'▾ '}
#satPanel .sat-row{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:2px 10px;width:100%;text-align:left;border-radius:6px;
  margin:3px 0;padding:6px 8px;min-height:40px}
#satPanel .sat-row .sat-n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#satPanel .sat-row .sat-w{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
#satPanel .sat-row .sat-f{grid-column:1/-1;color:var(--text-dim);font-size:12px}
#satPanel .sat-row.grey{opacity:.5;cursor:default}
#satPanel .sat-row.up .sat-w{color:var(--amber)}
@media (max-width:560px){
  #satPanel,#satPanel.big{left:8px;right:8px;width:auto;top:auto;bottom:8px;max-height:62vh}
  #satPanel .sat-ro{grid-template-columns:minmax(0,1fr) 128px}
  #satPanel canvas.sat-globe{width:128px;height:74px}
}
`;

const esc = (s: string) => s.replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' } as Record<string, string>)[c]);

export class SatPanel {
  private el: HTMLDivElement;
  private list: SatList | null = null;
  private ro: SatReadout | null = null;
  private cap: { ok: boolean; why: string } | null = null;
  private timer = 0;
  private listAt = 0;
  /** ★ in the mode (the key lit) — the window IS the mode: it closes only by leaving it (EXIT, the key, another mode). */
  private on = false;
  private autoDoppler = true;
  private picked: SatPicked | null = null;
  private pending: { norad: number; tx: number } | null = null;
  private refusal = '';
  private picture = false;
  private pictureDone = false;
  private decLine = '';
  private packets: string[] = [];
  private lent = false;
  // the skeleton's slots — built once, filled every second (the lent image canvas must never be re-created)
  private sHead!: HTMLElement; private sWhy!: HTMLElement; private sBox!: HTMLElement; private sRo!: HTMLElement;
  private sName!: HTMLElement; private sHz!: HTMLElement; private sCd!: HTMLElement; private sState!: HTMLElement;
  private sGlobe!: HTMLCanvasElement; private sImg!: HTMLElement; private sPk!: HTMLElement; private sSync!: HTMLElement;
  private sList!: HTMLDetailsElement; private sRows!: HTMLElement; private sCredit!: HTMLElement;

  constructor(private deps: SatPanelDeps) {
    if (!document.getElementById('satPanelCss')) {
      const st = document.createElement('style'); st.id = 'satPanelCss'; st.textContent = CSS; document.head.appendChild(st);
    }
    this.el = document.createElement('div');
    this.el.id = 'satPanel'; this.el.hidden = true;
    this.el.setAttribute('role', 'dialog'); this.el.setAttribute('aria-label', 'Satellite');
    this.el.innerHTML = `<div class="sat-h"><b>SATELLITE</b>`
      + `<button data-act="doppler" class="on" aria-pressed="true" title="AUTO DOPPLER: keep the satellite centred for the whole pass">AUTO DOPPLER</button>`
      + `<button data-act="exit" title="Leave SATELLITE (the Doppler lock and the decoder stop)">EXIT</button></div>`
      + `<div class="sat-why" data-s="why" hidden></div>`
      + `<div class="sat-box" data-s="box" hidden><div class="sat-ro" data-s="ro"><div>`
      +   `<div class="sat-name" data-s="name"></div><div class="sat-big" data-s="hz"></div>`
      +   `<div class="sat-cd" data-s="cd"></div><div class="sat-dim" data-s="state"></div></div>`
      +   `<canvas class="sat-globe" data-s="globe" width="336" height="192" aria-label="Where the satellite is: above the globe, its beam to the ground"></canvas></div>`
      + `<div class="sat-img" data-s="img" hidden></div><div class="sat-pk" data-s="pk" hidden></div></div>`
      + `<div class="sat-sync sat-dim" data-s="sync"></div>`
      + `<details class="sat-list" data-s="list" open><summary>SATELLITES</summary><div data-s="rows"></div></details>`
      + `<div class="sat-dim sat-credit" data-s="credit"></div>`;
    const q = <T extends HTMLElement>(n: string) => this.el.querySelector(`[data-s="${n}"]`) as T;
    this.sHead = this.el.querySelector('.sat-h') as HTMLElement;
    this.sWhy = q('why'); this.sBox = q('box'); this.sRo = q('ro'); this.sName = q('name'); this.sHz = q('hz'); this.sCd = q('cd');
    this.sState = q('state'); this.sGlobe = q<HTMLCanvasElement>('globe'); this.sImg = q('img'); this.sPk = q('pk'); this.sSync = q('sync');
    this.sList = q<HTMLDetailsElement>('list'); this.sRows = q('rows'); this.sCredit = q('credit');
    document.body.appendChild(this.el);
    this.el.addEventListener('click', (e) => this.onClick(e));
  }

  /** In the SATELLITE mode (its key lit). */
  get active(): boolean { return this.on; }
  get open(): boolean { return !this.el.hidden; }
  /** The pick's downlink, once the server confirmed it. */
  pickedTx(): SatTx | undefined { return this.picked ? this.list?.sats.find(s => s.norad === this.picked!.norad)?.tx[this.picked.tx] : undefined; }
  pickedDecoder(): SatPicked['decoder'] | '' { return this.picked?.decoder ?? ''; }
  /** hwinfo's `doppler` / `dopplerWhy` (satCapFrom). null = no key at all. */
  setCap(c: { ok: boolean; why: string } | null) { this.cap = c; if (!c) this.exit(); else if (this.open) this.render(); }
  /** Into the mode: the window opens, the list is asked for. */
  show() {
    if (!this.cap) return;
    this.on = true;
    this.el.hidden = false;
    this.deps.list(); this.listAt = Date.now();
    if (!this.timer) this.timer = window.setInterval(() => this.tick(), 1000);
    this.render();
  }
  /** ★ Out of the mode — the drop is always sent (a lock must never outlive the mode), the decoder stops, the canvas goes home. */
  exit() {
    const was = this.on;
    this.on = false;
    this.el.hidden = true;
    if (this.timer) { clearInterval(this.timer); this.timer = 0; }
    if (was) this.deps.drop();
    this.clearPick();
    if (was) this.deps.left();
  }
  toggle() { this.on ? this.exit() : this.show(); }
  private clearPick() {
    this.picked = null; this.pending = null; this.picture = false; this.pictureDone = false; this.decLine = ''; this.packets = [];
    this.lend(false);
  }
  onList(l: SatList) {
    const wasFetching = this.list?.source?.fetching;
    this.list = l;
    if (wasFetching && !l.source?.fetching) this.listAt = 0;
    if (this.open) this.render();
  }
  /** performance.now() of the last readout — the waterfall marker moves on at its rate between readouts. */
  readoutAt = 0;
  onReadout(r: SatReadout) {
    this.ro = r.state === 'OFF' && !r.why ? null : r; this.readoutAt = performance.now();
    if (r.state === 'OFF' && r.why && this.pending) { this.refusal = r.why; this.pending = null; }   // ★ a refused pick says why
    if (this.open) this.render();
  }
  /** {"state":"picked"} — the server tuned, locked (if asked) and attached the decoder. */
  onPicked(p: SatPicked) {
    if (!this.on) return;
    const changed = !this.picked || this.picked.norad !== p.norad || this.picked.tx !== p.tx;
    if (changed) { this.picture = false; this.pictureDone = false; this.packets = []; this.decLine = ''; }
    this.picked = p; this.pending = null; this.refusal = '';
    const t = this.pickedTx();
    if (t) this.deps.follow(p.decoder, t);
    this.sList.open = false;   // ★ the list folds away under the box once a bird is picked (one tap opens it again)
    this.render();
  }
  /** The decoder's own words (LRPT's carrier / frames / lines, SSTV's mode, APRS listening). */
  status(text: string) { this.decLine = text; if (this.open) this.paintPick(); }
  /** A picture started (LRPT's preview, an ISS SSTV frame): the box grows to show it. */
  pictureStart() { if (!this.picked) return; this.picture = true; this.pictureDone = false; if (this.open) this.render(); }
  pictureEnd() { if (!this.picked) return; this.pictureDone = true; if (this.open) this.paintPick(); }
  /** An APRS line heard on a packet downlink. */
  packet(line: string) {
    if (!this.picked) return;
    this.packets.unshift(line); if (this.packets.length > 30) this.packets.length = 30;
    if (this.open) this.paintPick();
  }
  onTrack(_t: SatTrack) { /* ★ the old sky plot's track — SATELLITE draws the half-globe instead */ }
  readout(): SatReadout | null { return this.ro && this.ro.state !== 'OFF' ? this.ro : null; }

  private tick() {
    if (!this.open) return;
    if (Date.now() - this.listAt > satListEveryMs(this.list)) { this.deps.list(); this.listAt = Date.now(); }
    this.render();
  }

  private onClick(e: Event) {
    const b = (e.target as HTMLElement).closest('button') as HTMLButtonElement | null;
    if (!b || b.disabled) return;
    const act = b.dataset.act;
    if (act === 'exit') { this.exit(); return; }
    if (act === 'doppler') {
      this.autoDoppler = !this.autoDoppler;
      if (this.picked) this.deps.doppler(this.autoDoppler);
      this.render(); return;
    }
    if (act === 'pick') {
      if (b.getAttribute('aria-disabled') === 'true') { this.refusal = b.dataset.why || ''; this.render(); return; }   // ★ greyed: says why, starts nothing
      const norad = Number(b.dataset.norad), i = Number(b.dataset.tx);
      if (!this.list?.sats.find(s => s.norad === norad)?.tx[i]) return;
      if (this.picked && this.picked.norad === norad && this.picked.tx === i) { this.sList.open = false; this.render(); return; }
      if (this.deps.pick(norad, i, this.autoDoppler)) { this.pending = { norad, tx: i }; this.refusal = ''; }
      this.render();
    }
  }

  private lend(on: boolean) {
    if (on === this.lent) return;
    this.lent = on;
    this.deps.image(on ? this.sImg : null);
  }

  private render() {
    const now = Date.now() / 1000;
    const cw = this.cap && !this.cap.ok ? this.cap.why : '';
    const dop = this.sHead.querySelector('[data-act="doppler"]') as HTMLButtonElement;
    dop.classList.toggle('on', this.autoDoppler); dop.setAttribute('aria-pressed', String(this.autoDoppler));
    dop.disabled = !!cw;
    const why = cw || this.refusal || (this.ro?.state === 'OFF' ? this.ro.why ?? '' : '');
    this.sWhy.hidden = !why; this.sWhy.textContent = why;
    this.paintPick();
    // ★ spec item 10: when the orbits were synced (or that they are being fetched, or why there are none)
    const sync = satSyncedText(this.list, now);
    this.sSync.textContent = sync; this.sSync.hidden = !sync;
    // the timetable
    const rows = satModeRows(this.list, now);
    let h = '';
    if (!this.list) h = `<div class="sat-dim">Asking the server…</div>`;
    else if (!rows.length) h = `<div class="sat-dim">${esc(this.list.why || (this.list.source?.fetching ? 'The satellites appear once the orbit data has arrived' : 'No satellites are listed on this server'))}</div>`;
    for (const r of rows.slice(0, 40)) {
      const sel = (this.picked && this.picked.norad === r.sat.norad && this.picked.tx === r.i) || (this.pending && this.pending.norad === r.sat.norad && this.pending.tx === r.i);
      const name = r.sat.tx.length > 1 ? `${r.sat.name} · ${r.tx.label}` : r.sat.name;
      const inactive = r.tx.status === 'inactive' ? ' · INACTIVE' : '';
      h += `<button class="sat-row${r.grey ? ' grey' : ''}${r.up ? ' up' : ''}${sel ? ' on' : ''}" data-act="pick" data-norad="${r.sat.norad}" data-tx="${r.i}"`
        + (r.grey ? ` aria-disabled="true" data-why="${esc(r.why)}" title="${esc(r.why)}"` : ` title="${esc((r.tx.note || r.tx.label) + ' — tune there' + (this.autoDoppler ? ' and correct the Doppler' : ''))}"`)
        + `><span class="sat-n">${esc(name)}</span><span class="sat-w">${esc(satWhenText(r, now))}</span>`
        + `<span class="sat-f">${esc(txText(r.tx))}${inactive}${r.grey ? ' — ' + esc(r.why) : ''}</span></button>`;
    }
    if (this.list?.time === 'system') h += `<div class="sat-dim">TIME: SYSTEM — this server's clock has not been checked against a time server</div>`;
    this.sRows.innerHTML = h;
    const summary = this.sList.querySelector('summary')!;
    summary.textContent = this.picked ? 'ALL SATELLITES' : 'SATELLITES';
    const credit = satCreditText(this.list, now), txc = satTxCreditText(this.list);
    this.sCredit.textContent = credit ? credit + (txc ? ' · ' + txc : '') : '';
  }

  /** The pick's box: COMPACT (voice / packet) or BIG (image; ISS SSTV once a picture starts). */
  private paintPick() {
    const now = Date.now() / 1000;
    const t = this.pickedTx();
    const sat = this.picked ? this.list?.sats.find(s => s.norad === this.picked!.norad) : undefined;
    const pend = this.pending ? this.list?.sats.find(s => s.norad === this.pending!.norad) : undefined;
    if (!t || !sat) {
      this.el.classList.remove('big');
      this.lend(false);
      this.sBox.hidden = !pend;
      if (pend) {
        const pt = pend.tx[this.pending!.tx];
        this.sName.textContent = `${pend.name}${pt ? ' · ' + pt.label : ''}`;
        this.sHz.textContent = pt ? satHzText(satCorrectedHz(pt, null)) : '';
        this.sCd.textContent = satCountdownText(pend, now);
        this.sState.textContent = 'Tuning…';
        this.sImg.hidden = true; this.sPk.hidden = true;
        this.drawGlobe(pend);
      }
      return;
    }
    const big = satBoxSize(t, this.picture) === 'big';
    this.el.classList.toggle('big', big);
    this.sBox.hidden = false;
    const r = this.readout();
    const mine = r && r.norad === sat.norad ? r : null;
    this.sRo.classList.toggle('coast', mine?.state === 'COAST');
    this.sName.textContent = `${sat.name} · ${t.label}`;
    this.sHz.textContent = satHzText(satCorrectedHz(t, mine));
    this.sCd.textContent = satCountdownText(sat, now);
    const use = txUse(t);
    const lockWord = !this.autoDoppler ? 'DOPPLER OFF' : mine ? satStateWord(mine) : '';
    const decWord = this.decLine || (use === 'image' ? 'waiting for the picture' : use === 'packet' ? 'listening for packets' : t.sstv ? 'listening — an SSTV picture opens here' : '');
    this.sState.textContent = [lockWord, decWord].filter(Boolean).join(' · ');
    // big: the decoder box's image canvas, lent to this window (the live preview, the finished picture at pass end)
    this.lend(big);
    this.sImg.hidden = !big;
    if (big && this.pictureDone) this.sState.textContent = [lockWord, 'picture complete — saved to the gallery'].filter(Boolean).join(' · ');
    // packet: the APRS lines
    this.sPk.hidden = use !== 'packet';
    if (use === 'packet') this.sPk.innerHTML = this.packets.length ? this.packets.slice(0, 12).map(l => `<div>${esc(l)}</div>`).join('') : `<div class="sat-dim">no packets yet</div>`;
    this.drawGlobe(sat);
  }

  /** ★ The half-globe: a 2D canvas (no WebGL, no tiles) — the dome, a 30° graticule, the receiver, the bird and its beam. */
  private drawGlobe(sat: { lat?: number; lon?: number; rangeKm?: number; el?: number }) {
    const cv = this.sGlobe, ctx = cv.getContext('2d');
    if (!ctx) return;
    const rx = this.deps.rxPos();
    cv.style.display = rx ? '' : 'none';
    if (!rx) return;
    const W = cv.width, H = cv.height;
    const g = globeScene(W, H, rx, sat.lat != null && sat.lon != null ? { lat: sat.lat, lon: sat.lon, altKm: satAltKm(sat.rangeKm, sat.el) } : null);
    const css = getComputedStyle(this.el);
    const ink = css.color || '#ccc';
    const amber = css.getPropertyValue('--amber').trim() || '#ffb000';
    ctx.clearRect(0, 0, W, H);
    ctx.save();
    ctx.beginPath(); ctx.rect(0, 0, W, g.cy); ctx.clip();
    ctx.beginPath(); ctx.arc(g.cx, g.cy, g.r, Math.PI, 2 * Math.PI);
    ctx.globalAlpha = 0.12; ctx.fillStyle = ink; ctx.fill();
    ctx.globalAlpha = 0.55; ctx.strokeStyle = ink; ctx.lineWidth = 2; ctx.stroke();
    if (g.grid) { ctx.globalAlpha = 0.22; ctx.lineWidth = 1; ctx.stroke(new Path2D(g.grid)); }
    if (g.foot) { ctx.globalAlpha = 0.28; ctx.fillStyle = amber; ctx.fill(new Path2D(g.foot)); }
    if (g.beam) { ctx.globalAlpha = 0.35; ctx.fillStyle = amber; ctx.fill(new Path2D(g.beam)); }
    ctx.globalAlpha = 1;
    if (g.rx) { ctx.fillStyle = ink; ctx.beginPath(); ctx.arc(g.rx.x, g.rx.y, 4, 0, 2 * Math.PI); ctx.fill(); }
    if (g.sat) {
      const { x, y } = g.sat;
      ctx.fillStyle = amber; ctx.strokeStyle = amber; ctx.lineWidth = 2;
      ctx.fillRect(x - 4, y - 4, 8, 8);
      ctx.beginPath(); ctx.moveTo(x - 14, y); ctx.lineTo(x - 6, y); ctx.moveTo(x + 6, y); ctx.lineTo(x + 14, y); ctx.stroke();
      ctx.fillRect(x - 20, y - 4, 6, 8); ctx.fillRect(x + 14, y - 4, 6, 8);
    }
    ctx.restore();
  }
}
