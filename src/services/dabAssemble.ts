/**
 * dabAssemble — puts the DAB report back together from the pieces a `dab=2` server sends (2026-10-10).
 *
 * ★★★ WHY (Stuart: "make the connection as efficient as possible … light on CPU on client and server and light on
 *     data"). Measured on the XCover, one silent listener in DAB: the `dab` report was 74 % of everything sent —
 *     32 KB/s, 16 KB twice a second, three times the waterfall. 12 KB of each was the station list, which changes only
 *     while the multiplex is being read, and 1.8 KB the constellation + impulse response, drawn only while the Signal
 *     pane is open. A client that opens its spectrum socket with `dab=2` now receives:
 *       `dab_list` — every service's fixed fields + dataSvcs + licensed + blocks, ONLY when that changes (`rev`);
 *       `dab_dls`  — every service's radio text with its age, ONLY when a text changes (`rev`);
 *       `dab`      — the live report (`v: 2`, naming the listRev/dlsRev it belongs to), with `ir`/`iq` only while
 *                    the client has said `dab_scopes` on.
 *     The server side is vibe_dab_service.h ReportParts + local_sdr_shim.cpp sendDab.
 *
 * ★★ ONE ASSEMBLER, BOTH CLIENTS (app VibeServerWsClient, web spectrum.ts) — and it hands back EXACTLY the legacy
 *    report's shape, so no screen code changes and an older server's full report passes straight through.
 * ★ Ages: the server's `dlsAge` is "seconds old when sent"; the assembler adds the time since it arrived, so an
 *   unchanged text ages correctly between sends with no clock shared between the two machines. The TUNED service's
 *   text is age 0 by definition (its label is live), as in the legacy report.
 */

/** A message off the spectrum socket — only `type` is assumed. */
type Msg = { type?: string; [k: string]: unknown };

export class DabAssembler {
  private list: Msg | null = null;
  private dls: { msg: Msg; at: number } | null = null;
  private lastResyncAt = -1e12;

  /** Forget everything — on dab_off, a new connection, or leaving DAB. */
  reset(): void { this.list = null; this.dls = null; }

  /**
   * Feed a parsed message. `report` is the full legacy-shaped `dab` report to hand on (null for the pieces, which are
   * only stored); `resync` is true when the client should send `{type:"dab_resync"}` — its list or radio text does
   * not match the revisions the live report names (asked at most every 5 s).
   */
  ingest(m: Msg, nowMs: number): { report: Msg | null; resync: boolean } {
    if (m.type === 'dab_list') { this.list = m; return { report: null, resync: false }; }
    if (m.type === 'dab_dls') { this.dls = { msg: m, at: nowMs }; return { report: null, resync: false }; }
    if (m.type === 'dab_off') { this.reset(); return { report: null, resync: false }; }
    if (m.type !== 'dab') return { report: null, resync: false };
    // ★ An older server, or the truncation guard's short object: already whole.
    if (m.v !== 2 || Array.isArray(m.services)) return { report: m, resync: false };

    const list = this.list && this.list.channel === m.channel ? this.list : null;
    const dls = this.dls && this.dls.msg.channel === m.channel ? this.dls : null;
    const texts = (dls?.msg.dls ?? {}) as Record<string, Record<string, unknown>>;
    const elapsed = dls ? Math.max(0, (nowMs - dls.at) / 1000) : 0;
    const services = (Array.isArray(list?.services) ? list!.services as Msg[] : []).map((s) => {
      const d = texts[String(s.sid)];
      if (!d) return s;
      const out: Msg = { ...s, ...d };
      if (typeof d.dlsAge === 'number') out.dlsAge = s.sid === m.sid && d.dlsAge === 0 ? 0 : Math.round(d.dlsAge + elapsed);
      return out;
    });
    const report: Msg = { ...m, services };
    for (const k of ['dataSvcs', 'licensed', 'blocks'] as const) if (list && list[k] !== undefined) report[k] = list[k];

    let resync = false;
    if ((list?.rev !== m.listRev || dls?.msg.rev !== m.dlsRev) && nowMs - this.lastResyncAt >= 5000) {
      this.lastResyncAt = nowMs;
      resync = true;
    }
    return { report, resync };
  }
}
