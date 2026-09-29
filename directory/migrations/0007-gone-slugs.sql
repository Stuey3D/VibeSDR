-- ★★★ AN ADDRESS THAT HAS GONE STILL GETS VISITORS. <slug>.vibeserver.vibesdr.net is shared in
--     Discord posts and sits in browser history; when the server behind it is delisted, the row is
--     DELETED (delist() — the owner's one-press privacy switch) and the next visitor got a bare
--     "No VibeServer is listed" with nowhere to go. Stuart, 2026-09-29: "anybody opening a dead link
--     from history gets a sorry this server is no longer available on this address, here are some
--     other servers that you may be interested in … closest to location of the old dead server and
--     which covers the same ranges".
--
-- ★★ SO WHEN A ROW IS DELETED, A SMALL NOTE OF WHERE IT WAS AND WHAT IT COVERED IS KEPT — enough to
--    rank other receivers for the visitor, and nothing else:
--      · the slug (it is in the visitor's own address bar already);
--      · the grid square's centre, COARSENED TO THE 4-CHARACTER SQUARE (~70 x 110 km) even when the
--        listing published a 6-character one — nearest-server ranking does not need more;
--      · the country and the band keys the receiver covered (FM, DAB, HF… — see bandsFor()).
--    ★★★ NOT THE NAME, NOT THE URL, NOT THE KEY, NOT THE STATUS BLOB. A delist is the owner saying
--        "take me off"; the note keeps only what is needed to point the visitor somewhere else.
--
-- ★ An EXPIRED server is not deleted — its row keeps its slug through the address hold and after —
--   so it needs no note here; serveBySlug reads the row itself. This table only covers the rows
--   that no longer exist.
-- ★ Pruned after 180 days on the write path (delist/register), the same "no cron" rule reg_log
--   follows. Re-registering the slug deletes the note: the address belongs to somebody again.
CREATE TABLE IF NOT EXISTS gone_slugs (
  slug      TEXT PRIMARY KEY,
  lat       REAL NOT NULL,
  lon       REAL NOT NULL,
  country   TEXT NOT NULL DEFAULT '',
  bands     TEXT NOT NULL DEFAULT '[]',   -- JSON array of band keys, e.g. ["fm","dab","hf"]
  gone_at   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_gone_slugs_at ON gone_slugs(gone_at);
