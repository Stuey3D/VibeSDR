# SSTV slant correction can half-apply, tearing the picture

**For:** M9PSY / UberSDR — the SSTV decoder VibeSDR's is derived from.
**Found:** 2026-07-31, on a Scottie S2 received from WESSEX.
**Status here:** fixed and shipping in VibeSDR 10.0.1. Patch attached.

---

## Symptom

An image decodes cleanly and is perfectly readable, but arrives **wrapped
horizontally** — the right-hand edge of the picture appears as a strip down the
left, the same offset on every line.

Slant correction then runs. It **lines the top of the image up correctly and the
bottom does not follow**: the picture ends up sliced, with a visible tear part-way
down and the lower portion out of alignment.

The corrected image is *worse than the wrapped one it replaced.*

## Cause

`SstvVideo::redrawFromLuminance()` rebuilds the entire image from `storedLum` using
a pixel grid computed at the corrected sample rate. For any sample time beyond the
end of the stored data it fell back to the last stored value:

```cpp
if (p.time >= 0 && p.time < (int)storedLum.size()) lum = storedLum[p.time];
else if (p.time >= (int)storedLum.size())          lum = storedLum.back();   // ← here
else continue;
```

The trap is that **`storedLum.size()` is not how much of it is real.** It is
allocated with headroom:

```cpp
maxLen = (int)(m->lineTime * m->numLines * sr * 1.3) + 15000;
storedLum.assign(maxLen, 0);
```

but only *written* while the demodulation loop is running, and that loop breaks
early when the PCM buffer runs dry:

```cpp
if (pcm.available() < 1024) break;
```

So everything between the last written sample and `maxLen` is a zero that was never
a sample. When the slant correction shifts the grid such that later rows map past
the captured range, the top of the image is rebuilt from genuine samples and the
bottom from one repeated value — hence "the top lined up, the bottom didn't".

Because the fallback is silent, the decoder cannot tell the difference between a
correction that worked and one that ran off the end.

## Fix

Three small changes (patch attached):

1. **Track what was actually written** — `storedLumWritten`, set as each sample is
   stored, rather than trusting the allocated size.
2. **Report completeness** — `redrawFromLuminance(rate, skip, bool* okOut)` sets
   `*okOut = false` if any pixel would need a sample that was never captured, and
   skips that pixel rather than inventing one.
3. **Only replace the picture if the correction is complete.** Otherwise keep the
   uncorrected image and say so in the status line.

## The design point behind (3)

**The uncorrected picture is the better one when the correction cannot be finished.**

A constant horizontal wrap is readable — the eye slides straight past it, and an
operator can still read the callsign, locator and any text in the image. A tear is
not readable, and it looks like a decoder fault rather than a propagation artefact.

So a partial correction is strictly worse than none, and "leave what the user had"
is the right failure mode.

## Notes

- The two status strings that made this diagnosable were `"Decoding <mode>..."` and
  `"Correcting slant..."` — being able to see which phase produced the bad image is
  what identified it. Worth keeping.
- No change to the sync search or the rate estimate; the correction maths is fine.
  This only stops it being applied when the data to apply it to is not there.
- Patch is against VibeSDR's copy, which has diverged cosmetically. The three
  changes are small enough to port by hand if the surrounding code differs.

Thanks for the decoder — it works well, and this was the only place it could
mislead a user rather than simply fail.
