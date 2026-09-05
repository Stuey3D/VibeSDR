/**
 * fmp4.ts — the smallest fragmented-MP4 muxer that will carry DAB+ access units.
 *
 * ★★★ WHY THIS EXISTS. Safari has AAC everywhere in its media stack — Apple co-owns the patents
 *     — but its WebCodecs AudioDecoder does not implement it, and we probed eight combinations of
 *     codec string and configuration to be sure (see audio.ts). MediaSource, however, takes
 *     `audio/mp4; codecs="mp4a.40.x"` quite happily. So the same access units the server already
 *     sends are wrapped as fMP4 here and handed to the platform decoder through MSE.
 *
 * ★★ WE STILL DECODE NOTHING. This writes boxes around bytes; the browser decodes them. That is
 *    the licence position the whole DAB+ design was chosen for and it is unchanged.
 *
 * ★ The timescale is the AAC CORE rate and every access unit is 1024 samples in it. Under SBR the
 *   decoder doubles the OUTPUT rate itself; the mp4 timeline stays in core units. Writing the
 *   output rate here would be the 2x chipmunk, which DAB has already produced twice by other
 *   routes — once in MP2 (mono read as stereo) and once in DAB+ (an assumed output rate).
 */

function u32(v: number): Uint8Array {
  return new Uint8Array([(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]);
}
function u16(v: number): Uint8Array { return new Uint8Array([(v >>> 8) & 255, v & 255]); }
function str4(s: string): Uint8Array {
  return new Uint8Array([s.charCodeAt(0), s.charCodeAt(1), s.charCodeAt(2), s.charCodeAt(3)]);
}
function cat(parts: Uint8Array[]): Uint8Array {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
/** A box is its own length, its type, then its payload. */
function box(type: string, ...payload: Uint8Array[]): Uint8Array {
  const body = cat(payload);
  return cat([u32(body.length + 8), str4(type), body]);
}
/** A full box carries a version and 24 bits of flags before the payload. */
function fbox(type: string, version: number, flags: number, ...payload: Uint8Array[]): Uint8Array {
  return box(type, new Uint8Array([version, (flags >> 16) & 255, (flags >> 8) & 255, flags & 255]),
             ...payload);
}

const ZERO = (n: number) => new Uint8Array(n);
/** The unity matrix every mp4 carries, whether or not anything is displayed. */
const MATRIX = cat([u32(0x00010000), u32(0), u32(0),
                    u32(0), u32(0x00010000), u32(0),
                    u32(0), u32(0), u32(0x40000000)]);

/** ES_Descriptor for AAC, carrying the AudioSpecificConfig verbatim. */
function esds(asc: Uint8Array): Uint8Array {
  const dsi  = cat([new Uint8Array([0x05, asc.length]), asc]);
  const dcd  = cat([new Uint8Array([0x04, 13 + dsi.length,
                                    0x40,        // MPEG-4 audio
                                    0x15]),      // audio stream
                    ZERO(3),                     // buffer size
                    u32(0), u32(0),              // max / average bitrate: unknown
                    dsi]);
  const sl   = new Uint8Array([0x06, 0x01, 0x02]);
  const es   = cat([new Uint8Array([0x03, 3 + dcd.length + sl.length]),
                    u16(1), new Uint8Array([0]), dcd, sl]);
  return fbox('esds', 0, 0, es);
}

/**
 * The init segment: ftyp + moov. `asc` is the AudioSpecificConfig, `rateHz` the AAC core rate.
 */
export function initSegment(rateHz: number, channels: number, asc: Uint8Array): Uint8Array {
  const ftyp = box('ftyp', str4('isom'), u32(0x200),
                   str4('isom'), str4('iso2'), str4('mp41'), str4('iso5'));

  const mvhd = fbox('mvhd', 0, 0, u32(0), u32(0), u32(1000), u32(0),
                    u32(0x00010000), u16(0x0100), ZERO(10), MATRIX, ZERO(24), u32(2));

  const tkhd = fbox('tkhd', 0, 7, u32(0), u32(0), u32(1), u32(0), u32(0),
                    ZERO(8), u16(0), u16(0), u16(0x0100), u16(0), MATRIX, u32(0), u32(0));

  const mdhd = fbox('mdhd', 0, 0, u32(0), u32(0), u32(rateHz), u32(0), u16(0x55C4), u16(0));
  const hdlr = fbox('hdlr', 0, 0, u32(0), str4('soun'), ZERO(12), new Uint8Array([0]));

  const mp4a = box('mp4a', ZERO(6), u16(1), u16(0), u16(0), u32(0),
                   u16(channels), u16(16), u16(0), u16(0), u32(rateHz << 16),
                   esds(asc));
  const stbl = box('stbl',
                   fbox('stsd', 0, 0, u32(1), mp4a),
                   fbox('stts', 0, 0, u32(0)),
                   fbox('stsc', 0, 0, u32(0)),
                   fbox('stsz', 0, 0, u32(0), u32(0)),
                   fbox('stco', 0, 0, u32(0)));
  const dinf = box('dinf', fbox('dref', 0, 0, u32(1), fbox('url ', 0, 1)));
  const minf = box('minf', fbox('smhd', 0, 0, u16(0), u16(0)), dinf, stbl);
  const mdia = box('mdia', mdhd, hdlr, minf);
  const trak = box('trak', tkhd, mdia);
  const mvex = box('mvex', fbox('trex', 0, 0, u32(1), u32(1), u32(0), u32(0), u32(0)));
  return cat([ftyp, box('moov', mvhd, trak, mvex)]);
}

/**
 * One access unit as a media segment: moof + mdat.
 *
 * ★ trun's data_offset is measured from the START OF THE MOOF, so it is the moof's own length
 *   plus the 8-byte mdat header. Getting it wrong yields a SourceBuffer that accepts the append
 *   and plays silence, which is the failure mode hardest to tell from "no AAC support".
 */
export function mediaSegment(seq: number, decodeTime: number, duration: number,
                             au: Uint8Array): Uint8Array {
  const mfhd = fbox('mfhd', 0, 0, u32(seq));
  const tfhd = fbox('tfhd', 0, 0x020000, u32(1));                 // default-base-is-moof
  const tfdt = fbox('tfdt', 1, 0, u32(Math.floor(decodeTime / 4294967296)),
                                  u32(decodeTime >>> 0));
  // flags: data-offset | sample-duration | sample-size
  const trunNoOffset = (off: number) =>
    fbox('trun', 0, 0x000301, u32(1), u32(off), u32(duration), u32(au.length));
  // The offset depends on the moof's length, which depends on the trun — so size it once with a
  // placeholder, then write it again with the real value. Both are the same length by construction.
  const probe = box('moof', mfhd, box('traf', tfhd, tfdt, trunNoOffset(0)));
  const moof  = box('moof', mfhd, box('traf', tfhd, tfdt, trunNoOffset(probe.length + 8)));
  return cat([moof, box('mdat', au)]);
}
