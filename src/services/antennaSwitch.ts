/**
 * antennaSwitch — the owner's external antenna switch, as Lite's / the app's server screen keeps it, and what it sends the
 * server (2026-10-09, docs/v12/ANTENNA-SWITCH-ROTATOR.md §3a, Stuart's design).
 *
 * ★ The SERVER side reuses the RSP aerial path: `antennaSwitch` (this JSON), `antennaMap` (the per-band preset — antenna
 *   NAMES, "0-30MHz Sky Loop, 30MHz+ VHF Vertical") and `antennaLocked` (admin only). See LocalSdrShim::setAntennaSwitch.
 * ★ Pure: no React Native, so scripts/test_antenna_switch_cfg.ts can drive it.
 */

/** One relay a discovered switch offers (the shim's antSwitchSearch JSON). */
export type SwitchRelay = { label: string; cmd: string; state: string; on: string; off: string };
/** One switch found by "Search for switch…". */
export type FoundSwitch = { broker: string; id: string; name: string; kind: string; relays: SwitchRelay[] };

/** One antenna the owner has connected: their name for it, which relay it is, and the bands it is chosen for. */
export type SwitchAntenna = { name: string; relay: number; bands: string };

export type AntennaSwitchCfg = {
  enabled: boolean;
  /** The broker, "host:port" — as the search reported it, or typed. */
  broker: string;
  user: string;
  pass: string;
  /** What the owner chose from the search, kept so the screen can say what it is talking to. */
  deviceName: string;
  relays: SwitchRelay[];
  antennas: SwitchAntenna[];
  /** "Lock antenna controls behind the admin password". */
  locked: boolean;
};

export const EMPTY_SWITCH: AntennaSwitchCfg = {
  enabled: false, broker: '', user: '', pass: '', deviceName: '', relays: [], antennas: [], locked: false,
};

/** Read what was stored (AsyncStorage), tolerating anything — a bad blob is "no switch", never a crash. */
export function parseSwitchCfg(raw: string | null | undefined): AntennaSwitchCfg {
  if (!raw) return { ...EMPTY_SWITCH };
  try {
    const j = JSON.parse(raw);
    const str = (v: unknown, max = 200) => (typeof v === 'string' ? v.slice(0, max) : '');
    const relays: SwitchRelay[] = Array.isArray(j.relays) ? j.relays.slice(0, 16).map((r: any) => ({
      label: str(r?.label, 60), cmd: str(r?.cmd), state: str(r?.state), on: str(r?.on, 20) || 'ON', off: str(r?.off, 20) || 'OFF',
    })).filter((r: SwitchRelay) => r.cmd) : [];
    const antennas: SwitchAntenna[] = Array.isArray(j.antennas) ? j.antennas.slice(0, 16).map((a: any) => ({
      name: str(a?.name, 40), relay: Number.isInteger(a?.relay) ? a.relay : 0, bands: str(a?.bands, 120),
    })) : [];
    return { enabled: j.enabled === true, broker: str(j.broker, 260), user: str(j.user, 100), pass: str(j.pass, 100),
             deviceName: str(j.deviceName, 80), relays, antennas, locked: j.locked === true };
  } catch { return { ...EMPTY_SWITCH }; }
}

/** Split "host:port" (port defaults to 1883). */
export function splitBroker(b: string): { host: string; port: number } {
  const t = b.trim();
  const i = t.lastIndexOf(':');
  if (i > 0 && /^\d+$/.test(t.slice(i + 1))) return { host: t.slice(0, i), port: Number(t.slice(i + 1)) || 1883 };
  return { host: t, port: 1883 };
}

/** What the server is sent (the same three fields the Linux setup page saves). A switch that is not complete — off, no
 *  broker, fewer than two named antennas — sends an empty switch, so the server draws no selector rather than a broken one. */
export function serverFields(c: AntennaSwitchCfg): { antennaSwitch: string; antennaMap: string; antennaLocked: boolean } {
  const named = c.antennas.filter((a) => a.name.trim() && c.relays[a.relay]);
  if (!c.enabled || !c.broker.trim() || named.length < 2) return { antennaSwitch: '', antennaMap: '', antennaLocked: false };
  const { host, port } = splitBroker(c.broker);
  const antennas = named.map((a) => {
    const r = c.relays[a.relay];
    return { name: a.name.trim(), cmd: r.cmd, state: r.state, on: r.on, off: r.off };
  });
  // ★ The per-band preset: "<bands> <name>" per antenna that has bands — the RSP list's own grammar (vibe_bands.h
  //   parseAntennaList), so one parser serves both. Bands may be several, comma-separated: each becomes its own entry.
  const map = named.flatMap((a) => a.bands.split(',').map((b) => b.trim()).filter(Boolean).map((b) => `${b} ${a.name.trim()}`))
                   .join(', ');
  return {
    antennaSwitch: JSON.stringify({ enabled: true, host, port, user: c.user, pass: c.pass, antennas }),
    antennaMap: map,
    antennaLocked: c.locked,
  };
}

/** Grow or shrink the antenna list to `n` (2..relays), keeping what the owner already typed. */
export function setAntennaCount(c: AntennaSwitchCfg, n: number): AntennaSwitchCfg {
  const max = Math.max(2, c.relays.length);
  const want = Math.max(2, Math.min(max, Math.round(n)));
  const antennas = c.antennas.slice(0, want);
  while (antennas.length < want) {
    const i = antennas.length;
    antennas.push({ name: c.relays[i]?.label && !/^Relay \d+$/.test(c.relays[i].label) ? c.relays[i].label : `Antenna ${i + 1}`,
                    relay: Math.min(i, Math.max(0, c.relays.length - 1)), bands: '' });
  }
  return { ...c, antennas };
}
