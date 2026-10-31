# Decoders, wave 2 (V12) — status log

Branch `v12-decoders-wave2` (worktree `~/VibeSDR-decoders2`), off `v12-experimental`. One section per decoder, in queue
order, appended as each lands, so a follow-up agent can resume exactly where the last one stopped.

Queue (Stuart's order): Q65 → FST4W → FST4 → MSK144 → CTCSS/DCS → JJY/BPC/TDF/CHU → ACARS → VDL2 → HFDL → RS41 →
PAGING (POCSAG + FLEX) → MFSK family + Hell → SELCAL → EAS/SAME. Never: HD Radio, AMBE voice, anything encrypted, TV on
servers.

## Standing specs recorded before the work reached them

### PAGING (queue item 7) — spec as refined by Stuart, 2026-10-31

- **ONE paging demodulator with an AUTO mode**, built like our other auto decoders (`decoders/rtty_auto.*` is the
  pattern: candidates run side by side, scored, the winner's text shown, what it chose said in the text, keeps watching).
- It detects and decodes on its own, picking encoding and rate from the signal:
  - **POCSAG** 512 / 1200 / 2400 baud (2-FSK). Tell: the preamble (alternating 1010…, ≥ 576 bits) and the frame sync
    codeword **0x7CD215D8**.
  - **FLEX** 1600 / 3200 / 6400 bps, 2-level and 4-level FSK. Tell: bit sync, the A-word and the frame sync.
  - **A channel that carries both** must work (a shared paging channel switching between POCSAG and FLEX).
- **Show the detected mode/rate on the readout** (e.g. "POCSAG 1200", "FLEX 3200/4").
- **Manual override** to a specific mode/rate when auto cannot lock.
- **0 false decodes on noise** (long noise runs, measured, as every other decoder).
- Unchanged from the earlier decision: **ONE "PAGE" button**; **Android local hardware only** (the phone's own
  plugged-in radio) — never on any server, not even to LAN clients; **nothing to the gallery**.

