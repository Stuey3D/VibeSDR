// Amateur-radio callsign prefix → ISO alpha-2, the final fallback for directory servers named
// only by their callsign (CS8ACT, EA8DJF, FR4KM) with no country word and no usable GPS.
// Distinct from callsignCountry.ts, which yields display NAMES for FT8 spots; grouping needs
// ISO codes. ITU prefix blocks; longest-prefix wins so overseas territories (FR→Réunion,
// VP9→Bermuda, EA8→Spain/Canary) beat their parent (F→France, G→UK). Not exhaustive — covers
// the countries that host SDRs.

const PREFIX: Record<string, string> = {
  // Territories that differ from a parent prefix — must out-length the parent to win
  VP9: 'BM', KP4: 'PR', KP3: 'PR', KH6: 'US', KL: 'US', CU: 'PT', CT3: 'PT', TK: 'FR',
  FR: 'RE', FG: 'GP', FM: 'MQ', FY: 'GF', FO: 'PF', FK: 'NC', FH: 'YT', FP: 'PM', FW: 'WF',
  // Europe
  CT: 'PT', CS: 'PT', CR: 'PT', CQ: 'PT',
  EA: 'ES', EB: 'ES', EC: 'ES', ED: 'ES', EE: 'ES', EF: 'ES', EG: 'ES', EH: 'ES',
  F: 'FR', TM: 'FR',
  G: 'GB', M: 'GB', GM: 'GB', MM: 'GB', GW: 'GB', MW: 'GB', GI: 'GB', MI: 'GB',
  GD: 'GB', GU: 'GB', GJ: 'GB', '2': 'GB',
  D: 'DE', I: 'IT', ON: 'BE', OO: 'BE', OT: 'BE',
  PA: 'NL', PB: 'NL', PD: 'NL', PE: 'NL', PH: 'NL', PI: 'NL', LX: 'LU',
  HB: 'CH', OE: 'AT',
  LA: 'NO', LB: 'NO', LG: 'NO', LN: 'NO',
  SM: 'SE', SA: 'SE', SK: 'SE', SL: 'SE',
  OZ: 'DK', OU: 'DK', OV: 'DK', OH: 'FI', OF: 'FI', OG: 'FI', OI: 'FI',
  SP: 'PL', SQ: 'PL', SO: 'PL', SN: 'PL', HF: 'PL',
  OK: 'CZ', OL: 'CZ', OM: 'SK', HA: 'HU', HG: 'HU',
  YO: 'RO', YP: 'RO', YR: 'RO', LZ: 'BG',
  S5: 'SI', '9A': 'HR', E7: 'BA', YT: 'RS', YU: 'RS', '4O': 'ME', Z3: 'MK', ZA: 'AL',
  SV: 'GR', SW: 'GR', SY: 'GR', SZ: 'GR', '5B': 'CY', '9H': 'MT',
  EI: 'IE', EJ: 'IE', TF: 'IS', LY: 'LT', YL: 'LV', ES: 'EE',
  UR: 'UA', US: 'UA', UT: 'UA', UU: 'UA', UX: 'UA', UY: 'UA', EM: 'UA', EO: 'UA',
  EU: 'BY', EV: 'BY', EW: 'BY', ER: 'MD',
  R: 'RU', U: 'RU', RA: 'RU', RK: 'RU', RN: 'RU', RU: 'RU', RV: 'RU', RW: 'RU', RX: 'RU', RZ: 'RU', UA: 'RU',
  // North America
  K: 'US', W: 'US', N: 'US', A: 'US', AL: 'US', NL: 'US', WL: 'US',
  VE: 'CA', VA: 'CA', VO: 'CA', VY: 'CA', CY: 'CA',
  XE: 'MX', XF: 'MX', '4A': 'MX',
  // Caribbean / Central / South America
  CO: 'CU', CM: 'CU', HI: 'DO', HH: 'HT', '8P': 'BB', '9Y': 'TT', PJ: 'CW', P4: 'AW',
  PY: 'BR', PP: 'BR', PT: 'BR', PU: 'BR', PR: 'BR', ZZ: 'BR', ZV: 'BR',
  LU: 'AR', LW: 'AR', AY: 'AR', CE: 'CL', CA: 'CL', XQ: 'CL',
  CX: 'UY', HK: 'CO', HJ: 'CO', YV: 'VE', YY: 'VE', OA: 'PE', HC: 'EC', CP: 'BO', ZP: 'PY',
  // Asia
  JA: 'JP', JE: 'JP', JF: 'JP', JG: 'JP', JH: 'JP', JI: 'JP', JJ: 'JP', JK: 'JP',
  JL: 'JP', JM: 'JP', JN: 'JP', JO: 'JP', JP: 'JP', JQ: 'JP', JR: 'JP', JS: 'JP', '7K': 'JP',
  HL: 'KR', DS: 'KR', BY: 'CN', BA: 'CN', BD: 'CN', BG: 'CN', BH: 'CN', BI: 'CN',
  BV: 'TW', BU: 'TW', BX: 'TW', BM: 'TW', VR: 'HK',
  HS: 'TH', E2: 'TH', '9M': 'MY', '9V': 'SG', '9W': 'MY', YB: 'ID', YC: 'ID', YD: 'ID',
  DU: 'PH', DV: 'PH', DW: 'PH', '4F': 'PH', XV: 'VN', '3W': 'VN', VU: 'IN', AT: 'IN', '4S': 'LK',
  AP: 'PK', EP: 'IR', '4X': 'IL', '4Z': 'IL', JY: 'JO', HZ: 'SA',
  A4: 'OM', A6: 'AE', A7: 'QA', A9: 'BH', '9K': 'KW', TA: 'TR', TB: 'TR', TC: 'TR', YM: 'TR',
  UN: 'KZ', '4L': 'GE', '4J': 'AZ', '4K': 'AZ', EK: 'AM',
  // Africa
  ZS: 'ZA', ZR: 'ZA', ZT: 'ZA', ZU: 'ZA', CN: 'MA', '3V': 'TN', '7X': 'DZ', SU: 'EG',
  '5N': 'NG', '5Z': 'KE', '5H': 'TZ', '5R': 'MG', '3B8': 'MU', TR: 'GA', '9G': 'GH', '6W': 'SN',
  // Oceania
  VK: 'AU', AX: 'AU', ZL: 'NZ', ZM: 'NZ', '3D2': 'FJ', '5W': 'WS', FT: 'FR',
  P2: 'PG', YJ: 'VU', H4: 'SB', KH2: 'GU',
};

const KEYS = Object.keys(PREFIX).sort((a, b) => b.length - a.length);

// First callsign-shaped token: prefix (letters/digit) + a digit + letter suffix (EA8DJF, VP9NI).
const CALL = /\b([A-Z0-9]{1,3}[0-9][A-Z]{1,4})\b/;

/** ISO alpha-2 from an amateur callsign found in the text, or '' if none matches. */
export function isoForCallsign(text?: string | null): string {
  if (!text) return '';
  const m = text.toUpperCase().match(CALL);
  if (!m) return '';
  const call = m[1];
  for (const k of KEYS) {
    if (call.startsWith(k)) return PREFIX[k];
  }
  return '';
}
