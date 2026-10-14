/**
 * ★★★ A VIBESERVER SQUELCH THRESHOLD, AND WHETHER IT IS ON — ONE COPY, BOTH CLIENTS (app + web).
 *
 * The wire used to say both things with one number: `{"type":"squelch","db":X}`, **db <= -100 = off**.
 * That sentinel sits INSIDE the range of real channel levels. Stuart, 2026-10-14:
 *  - iPhone, Airspy HF+, 127.875 MHz AM: the quiet channel reads about -110 dBFS (needle ~S3, spectrum
 *    floor -131). Auto squelch put its threshold at quiet + 12 ≈ -100 and drew the red line there
 *    correctly — but every threshold at or below -100 went out as OFF, so the gate never closed.
 *    "It works on higher signal and noise levels" = it works whenever the threshold is above -100.
 *  - Mac, SDRplay RSP1A on MW (signal S4 ≈ -100): any hand-set threshold in the left third of the bar
 *    (-130..-100) was OFF, and the handle snapped to the far left.
 *  - The web client hid it differently: it clamped every threshold to -99, so on the same Airspy auto
 *    squelch sat SHUT, and its "hang = held open" sent -99 — a real threshold above the noise.
 * Same class as the OWRX S-meter fault the day before (a floor at the bottom of the scale), different
 * place: here it was the on/off sentinel.
 *
 * ★★ THE FIX SAYS "ON" IN WORDS. The message carries `on` (the server reads it — vibe_squelch_msg.h),
 *    and "off" inside this code is VS_SQL_OFF, a value no channel can read. A server that does not
 *    advertise `sqlOn` in hwinfo is older and still applies the legacy rule, so there a threshold at or
 *    below -100 IS off — vsSqlEffective() says so, and the control shows OFF rather than drawing a gate
 *    the server is not applying (the old behaviour, unchanged, against an old server).
 */

/** "Off", in this code. Far below any real channel level, so it can never collide with a threshold. */
export const VS_SQL_OFF = -999;
/** The wire's legacy sentinel: without `on`, a server treats db <= this as off. Also what goes out
 *  as `db` for OFF, so an older server reads an off message exactly as it always did. */
export const VS_SQL_LEGACY_OFF_DB = -100;

/** Is this threshold (dBFS) a gate that is ON? */
export function vsSqlOn(db: number): boolean {
  return Number.isFinite(db) && db > VS_SQL_OFF;
}

/** The threshold as the server will actually apply it. `serverSpeaksOn` = hwinfo said `sqlOn`.
 *  ★ Against an older server a threshold at or below -100 is OFF there, so it is OFF here too — the
 *    control must never show a gate the receiver is not applying. */
export function vsSqlEffective(db: number, serverSpeaksOn: boolean): number {
  if (!vsSqlOn(db)) return VS_SQL_OFF;
  if (!serverSpeaksOn && db <= VS_SQL_LEGACY_OFF_DB) return VS_SQL_OFF;
  return db;
}

/** The control message. `db` is sent as the legacy sentinel when off, so an old server agrees. */
export function vsSquelchMsg(db: number): { type: 'squelch'; db: number; on: boolean } {
  const on = vsSqlOn(db);
  return { type: 'squelch', db: on ? db : VS_SQL_LEGACY_OFF_DB, on };
}

/** A remembered threshold. `savedOn` is written alongside it from this change on; a value saved
 *  before it has none, and then -100 or below meant OFF (that was the only way to store off). */
export function vsSqlRestore(saved: unknown, savedOn: unknown): number {
  if (typeof saved !== 'number' || !Number.isFinite(saved)) return VS_SQL_OFF;
  if (typeof savedOn === 'boolean') return savedOn && vsSqlOn(saved) ? saved : VS_SQL_OFF;
  return saved > VS_SQL_LEGACY_OFF_DB ? saved : VS_SQL_OFF;
}
