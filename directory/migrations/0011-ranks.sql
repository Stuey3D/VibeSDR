-- ★★ V12 beta90 RECEIVER RANKINGS (src/ranks.js, docs/v12/PSK-REPORTER.md) — the hourly round's state and its lookup.
--    k = 'state' (each source's holds and last error in words), 'index' (when each source last answered, the bucket
--    names), 'b:<first character of the callsign>' (that bucket's {callsign → places}). ~40 rows, ~40 writes an hour.
-- ★ Apply BEFORE deploying the Worker that runs the cron:
--     npx wrangler d1 execute vibesdr-directory --remote --file migrations/0011-ranks.sql
CREATE TABLE IF NOT EXISTS rank_mirror (
  k TEXT PRIMARY KEY,
  v TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
