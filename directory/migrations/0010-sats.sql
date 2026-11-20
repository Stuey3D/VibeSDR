-- ★★ V12 SATELLITE ENGINE — the sats.json mirror's state and copies (src/sats.js, docs/v12/SATELLITE-ENGINE.md §6.2).
--    k = 'state' (holds, the last error in words, the dead/moved URL lists), 'omm:<group>' (each CelesTrak group's last
--    GOOD element sets for the curated birds), 'satnogs' (the curated birds' SatNOGS transmitters), 'sats' (the served
--    body + its ETag). A handful of rows, a few writes every 6 hours.
-- ★ Apply BEFORE deploying the Worker that runs the cron:
--     npx wrangler d1 execute vibesdr-directory --remote --file migrations/0010-sats.sql
CREATE TABLE IF NOT EXISTS sat_mirror (
  k TEXT PRIMARY KEY,
  v TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
