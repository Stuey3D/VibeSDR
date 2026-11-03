/**
 * ★★★ THE OWNER'S NAMES FOR A RADIO'S OWN ANTENNA PORTS (V12, 2026-11-03) — what a listener READS beside a port.
 *
 * The server publishes `radio.antennaNames` in hwinfo — an object keyed by the port names the same radio lists in
 * `antennas` ("A", "Hi-Z", "Tuner 2 50Ω", "RX2 (B)"), cleaned, bounded and escaped by vibe_port_names.h. A client draws
 * the label from here and still SENDS the port name ({type:"antenna"}, rsp_control `antenna`): a name is only ever read.
 * Nothing about any radio is known here (feedback: the app has no hardcoded settings) — no names, no ports, the bare port.
 *
 * ★ The server's twin is portnames::display in android/app/src/main/cpp/vibe_port_names.h; the same cases are pinned in
 *   scripts/test_antenna_port_names.ts and vibeserver/test-port-names.cpp. The web client imports this file.
 */

export const PORT_NAME_MAX = 40;   // bytes, as the server bounds it — the setup inputs use the same figure

export type PortNames = Record<string, string>;

/** `radio.antennaNames` as the client received it: only string values, each trimmed. Anything else = no names. */
export function portNamesFrom(raw: unknown): PortNames {
  const out: PortNames = {};
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return out;
  for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
    if (typeof v !== 'string') continue;
    const t = v.replace(/[\u0000-\u001f\u007f-\u009f]/g, ' ').replace(/\s+/g, ' ').trim();
    if (t) out[k] = t;
  }
  return out;
}

/** "Discone (RX2)", "Discone · RX2 (A)" (no nested brackets), or the bare port when it has no name. */
export function portLabel(port: string, names?: PortNames | null): string {
  const n = names?.[port];
  if (!n) return port;
  return port.endsWith(')') ? `${n} · ${port}` : `${n} (${port})`;
}

/** What an owner typed, cut to the server's bound on a whole character (UTF-8 bytes, as vibe_port_names.h counts). */
export function clampPortName(s: string, max = PORT_NAME_MAX): string {
  let out = '';
  let bytes = 0;
  for (const ch of s.replace(/[\u0000-\u001f\u007f-\u009f]/g, ' ')) {
    const b = new TextEncoder().encode(ch).length;
    if (bytes + b > max) break;
    out += ch; bytes += b;
  }
  return out;
}
