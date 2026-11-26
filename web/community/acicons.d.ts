// The ACICONS block of web/mapkit/adsblayer.js, as scripts/build-community-kit.mjs serves it (the 'vibe:acicons' import).
declare module 'vibe:acicons' {
  /** The glyph class an aircraft draws: the ADS-B emitter category, else the aircraft data, else the type's family, else 'jet'. */
  export function iconClassOf(a: unknown): string;
  /** One glyph as SVG: any CSS colour, `size` px square, rotated `rot` degrees (not the balloon / parachute / obstacle). */
  export function acIconSvg(kind: string, fill: string, size: number, rot?: number): string;
  export const AC_CLASS_NAME: Record<string, string>;
  export const ICONS: Record<string, unknown[]>;
}
