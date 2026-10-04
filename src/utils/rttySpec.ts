/**
 * rttySpec — the full manual RTTY specification (Stuart, 2026-10-04: "add controls for the entire RTTY specification.
 * Keep the presets and the auto detect as they are but add the full stack of controls"). One table for the app's two
 * menus and the web client, and ONE rule for what goes to the server (`rttyFraming`).
 *
 * ★ Rows appear only where they mean something (AGENTS.md: no dead controls): data bits and parity are ASCII's; unshift
 *   on space is ITA2's; 1.5 stop bits is the 5-bit Baudot case only; SITOR-B (CCIR476) has its own fixed 4-of-7 framing.
 *   There is no AFC row — the decoder has none (AUTO re-centres on the tones instead).
 */
export const RTTY_SHIFTS = [85, 170, 200, 425, 450, 850] as const;
/** Two rows of baud keys, so nine fit a phone. */
export const RTTY_BAUD_ROWS = [[45.45, 50, 56.88, 75, 100], [110, 150, 200, 300]] as const;
export const RTTY_PARITIES = [['N', 'NONE'], ['E', 'EVEN'], ['O', 'ODD'], ['M', 'MARK'], ['S', 'SPACE']] as const;
export type RttyParity = 'N' | 'E' | 'O' | 'M' | 'S';

export interface RttySpecFields {
  encoding: string;            // 'ITA2' | 'ASCII' | 'CCIR476'
  stop?: 1 | 1.5 | 2;
  dataBits?: 7 | 8;            // ASCII
  parity?: RttyParity;         // ASCII
}

/** Stop-bit keys for this encoding (1.5 is Baudot only; none for SITOR-B). */
export function rttyStops(encoding: string): Array<1 | 1.5 | 2> {
  return encoding === 'CCIR476' ? [] : encoding === 'ASCII' ? [1, 2] : [1, 1.5, 2];
}

/** The framing the server parses (<data><parity><stop>, or SITOR-B's 4/7). Defaults: ITA2 5N1.5, ASCII 7N1. */
export function rttyFraming(s: RttySpecFields): string {
  if (s.encoding === 'CCIR476') return '4/7';
  if (s.encoding === 'ASCII') {
    const stop = s.stop === 2 ? 2 : 1;
    return `${s.dataBits === 8 ? 8 : 7}${s.parity ?? 'N'}${stop}`;
  }
  return `5N${s.stop === 1 ? '1' : s.stop === 2 ? '2' : '1.5'}`;
}
