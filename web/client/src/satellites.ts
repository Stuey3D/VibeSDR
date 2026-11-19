/* ★★ V12 SATELLITES in the web client (docs/v12/SATELLITE-ENGINE.md): the picker (next passes, where each bird is now),
 *  the live readout of a Doppler lock, a sky track, and the DOPPLER button. The words and shapes are the app's
 *  (src/services/satellites.ts) so both clients say the same thing.
 *  ★ No dead controls: the panel's entry exists only where the server sends `doppler` (the radio reaches a listed
 *    downlink); greyed with `dopplerWhy` when not ready; a downlink this radio cannot hear is not listed at all.
 *  ★ No flashing anywhere: COAST dims the readout, nothing blinks.
 *  ★ The main digits keep the NOMINAL frequency (the server snaps the dial there); the white text beside them is
 *    satChanText() — main.ts puts it in the frequency card's `chan` slot. */
import {
  ageText, clockIn, pickerRows, satStateWord, satStatusLine, skySvg, txListenMode, txText, txTuneHz,
  type SatList, type SatReadout, type SatTrack, type SatTx,
} from '../../../src/services/satellites';

export interface SatPanelDeps {
  list: () => void;
  track: (norad: number, aos: number, los: number) => void;
  /** The lock (spectrum.ts satLock — it asks before tuning on a shared dial, as a tune does). */
  lock: (on: boolean, norad?: number) => void;
  /** A person's tune to a downlink (main.ts tuneTo: the mode, the band step, the passband). */
  tune: (hz: number, mode: string) => void;
}

const CSS = `
#satPanel{position:fixed;z-index:60;right:12px;top:64px;width:min(420px,calc(100vw - 32px));max-height:calc(100vh - 96px);
  overflow:auto;background:var(--pill-bg);color:var(--btn-text);border:1px solid var(--btn-border);border-radius:10px;
  padding:10px 12px;font:13px/1.35 system-ui,sans-serif;box-shadow:0 6px 24px rgba(0,0,0,.45)}
#satPanel[hidden]{display:none}
#satPanel .sat-h{display:flex;align-items:center;gap:8px;margin-bottom:6px}
#satPanel .sat-h b{flex:1;letter-spacing:.08em}
#satPanel button{background:var(--btn-bg);color:var(--btn-text);border:1px solid var(--btn-border);border-radius:6px;padding:4px 9px;
  font:inherit;cursor:pointer;min-height:30px}
#satPanel button.on{background:var(--btn-active-bg);border-color:var(--btn-active-bdr);color:var(--btn-active-tx)}
#satPanel button:disabled{opacity:.45;cursor:default}
#satPanel .sat-dim{color:var(--text-dim)}
#satPanel .sat-ro{border:1px solid var(--btn-border);border-radius:8px;padding:8px;margin:6px 0;display:grid;grid-template-columns:1fr 112px;gap:8px}
#satPanel .sat-ro.coast .sat-big{opacity:.55}
#satPanel .sat-big{font-size:17px;font-variant-numeric:tabular-nums}
#satPanel .sat-sky{width:112px;height:112px;color:var(--amber)}
#satPanel .sat-row{border-top:1px solid var(--bar-border);padding:7px 0;display:grid;gap:4px}
#satPanel .sat-row .sat-n{display:flex;justify-content:space-between;gap:8px}
#satPanel .sat-tx{display:flex;flex-wrap:wrap;gap:6px}
#satPanel .sat-why{color:var(--text-dim);margin:4px 0}
@media (max-width:560px){#satPanel{left:8px;right:8px;width:auto;top:auto;bottom:8px;max-height:55vh}}
`;

export class SatPanel {
  private el: HTMLDivElement;
  private list: SatList | null = null;
  private ro: SatReadout | null = null;
  private track: SatTrack | null = null;
  private cap: { ok: boolean; why: string } | null = null;
  private timer = 0;
  private listAt = 0;

  constructor(private deps: SatPanelDeps) {
    if (!document.getElementById('satPanelCss')) {
      const st = document.createElement('style'); st.id = 'satPanelCss'; st.textContent = CSS; document.head.appendChild(st);
    }
    this.el = document.createElement('div');
    this.el.id = 'satPanel'; this.el.hidden = true;
    this.el.setAttribute('role', 'dialog'); this.el.setAttribute('aria-label', 'Satellites');
    document.body.appendChild(this.el);
    this.el.addEventListener('click', (e) => this.onClick(e));
  }

  get open(): boolean { return !this.el.hidden; }
  /** hwinfo's `doppler` / `dopplerWhy` (satCapFrom). null = no entry at all. */
  setCap(c: { ok: boolean; why: string } | null) { this.cap = c; if (!c) this.close(); else if (this.open) this.render(); }
  show() {
    if (!this.cap) return;
    this.el.hidden = false;
    this.deps.list(); this.listAt = Date.now();
    if (!this.timer) this.timer = window.setInterval(() => this.tick(), 1000);
    this.render();
  }
  close() { this.el.hidden = true; if (this.timer) { clearInterval(this.timer); this.timer = 0; } }
  toggle() { this.open ? this.close() : this.show(); }
  onList(l: SatList) { this.list = l; this.askTrack(); if (this.open) this.render(); }
  /** performance.now() of the last readout — the waterfall marker moves on at its rate between readouts. */
  readoutAt = 0;
  onReadout(r: SatReadout) {
    this.ro = r.state === 'OFF' && !r.why ? null : r; this.readoutAt = performance.now();
    this.askTrack(); if (this.open) this.render();
  }
  onTrack(t: SatTrack) { this.track = t; if (this.open) this.render(); }
  readout(): SatReadout | null { return this.ro && this.ro.state !== 'OFF' ? this.ro : null; }

  private tick() {
    if (!this.open) return;
    if (Date.now() - this.listAt > 30000) { this.deps.list(); this.listAt = Date.now(); }
    this.render();
  }
  /** The sky track: the locked bird's pass, else the first bird in the list's next pass. */
  private askTrack() {
    const now = Date.now() / 1000;
    let norad = this.ro && this.ro.state !== 'OFF' ? this.ro.norad : undefined;
    let p = norad != null ? this.list?.sats.find(s => s.norad === norad)?.passes.find(x => x.los > now) : undefined;
    if (!p) { const r = pickerRows(this.list, now)[0]; if (r?.next) { norad = r.sat.norad; p = r.next; } }
    if (norad == null || !p) return;
    if (this.track && this.track.norad === norad && Math.abs(this.track.aos - p.aos) < 30) return;
    this.deps.track(norad, p.aos, p.los);
    this.track = { norad, aos: p.aos, los: p.los, pts: [] };   // ★ asked: not again until the pass changes
  }

  private onClick(e: Event) {
    const b = (e.target as HTMLElement).closest('button') as HTMLButtonElement | null;
    if (!b || b.disabled) return;
    const act = b.dataset.act;
    if (act === 'close') { this.close(); return; }
    if (act === 'doppler') { this.deps.lock(!this.readout()); return; }
    if (act === 'track') {
      const norad = Number(b.dataset.norad), i = Number(b.dataset.tx);
      const sat = this.list?.sats.find(s => s.norad === norad);
      const t: SatTx | undefined = sat?.tx[i];
      if (!sat || !t) return;
      this.deps.tune(txTuneHz(t), txListenMode(t));
      // ★ after the tune has landed, so the lock is made at the new dial (the server matches within 12 kHz)
      window.setTimeout(() => this.deps.lock(true, norad), 400);
    }
  }

  private render() {
    const now = Date.now() / 1000;
    const esc = (s: string) => s.replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' } as Record<string, string>)[c]);
    const r = this.readout();
    let h = `<div class="sat-h"><b>SATELLITES</b>`;
    const why = this.cap && !this.cap.ok ? this.cap.why : '';
    h += `<button data-act="doppler" class="${r ? 'on' : ''}" ${why ? 'disabled' : ''} title="${esc(why || 'Doppler correction: keep the satellite centred for the whole pass')}">DOPPLER</button>`;
    h += `<button data-act="close" aria-label="Close">✕</button></div>`;
    if (why) h += `<div class="sat-why">${esc(why)}</div>`;
    if (this.ro?.state === 'OFF' && this.ro.why) h += `<div class="sat-why">${esc(this.ro.why)}</div>`;
    if (r) {
      const off = r.offset ?? 0;
      const sel = this.list?.sats.find(s => s.norad === r.norad);
      h += `<div class="sat-ro ${r.state === 'COAST' ? 'coast' : ''}"><div>`;
      h += `<div class="sat-big">${esc(r.name || '')} · ${off >= 0 ? '▲' : '▼'} ${Math.abs(off / 1000).toFixed(2)} kHz</div>`;
      h += `<div>${esc(satStateWord(r))}${r.tx ? ' · ' + esc(r.tx) : ''}</div>`;
      h += `<div class="sat-dim">${esc(satStatusLine(r, now))}</div>`;
      if (r.az != null && r.el != null && r.state !== 'ARMED') h += `<div class="sat-dim">AZ ${Math.round(r.az)}° · EL ${Math.round(r.el)}° · ${(r.rate ?? 0).toFixed(0)} Hz/s</div>`;
      if (sel) { const a = ageText(sel); if (a) h += `<div class="sat-dim">${esc(a)}</div>`; }
      h += `</div><div class="sat-sky">${skySvg(this.track && this.track.norad === r.norad ? this.track : null, r)}</div></div>`;
    }
    const rows = pickerRows(this.list, now);
    if (!this.list) h += `<div class="sat-dim">Asking the server…</div>`;
    else if (!rows.length) h += `<div class="sat-dim">${esc(this.list.why || 'No listed satellite can be heard on this receiver')}</div>`;
    for (const row of rows.slice(0, 20)) {
      const s = row.sat;
      const when = row.up ? `UP · EL ${Math.round(s.el ?? 0)}°`
        : row.next ? `AOS ${new Date(row.next.aos * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })} (in ${clockIn(row.next.aos, now)}) · max ${Math.round(row.next.maxEl)}°`
        : 'no pass in the next day';
      h += `<div class="sat-row"><div class="sat-n"><span>${esc(s.name)}</span><span class="sat-dim">${esc(when)}</span></div>`;
      if (s.note) h += `<div class="sat-dim">${esc(s.note)}</div>`;
      const age = ageText(s);
      if (age) h += `<div class="sat-dim">${esc(age)}</div>`;
      h += `<div class="sat-tx">`;
      s.tx.forEach((t, i) => {
        if (!t.reach) return;
        const dis = !!why || s.ageState === 'expired' || s.ageState === 'none';
        h += `<button data-act="track" data-norad="${s.norad}" data-tx="${i}" ${dis ? 'disabled' : ''} title="${esc((t.note || t.label) + ' — tune there and correct the Doppler')}">${esc(t.label)} · ${esc(txText(t))}</button>`;
      });
      h += `</div></div>`;
    }
    if (this.list?.time === 'system') h += `<div class="sat-dim">TIME: SYSTEM — this server's clock has not been checked against a time server</div>`;
    this.el.innerHTML = h;
  }
}
