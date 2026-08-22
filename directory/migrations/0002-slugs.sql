-- The shareable address: <slug>.vibeserver.vibesdr.net
--
-- ★★★ THE SLUG IS DERIVED ONCE, AT REGISTRATION, AND NEVER RECOMPUTED. vibeserver_config.h:78
--     already says the .local label is "DERIVED from it, once". For a SHARED PUBLIC address the
--     rule is stronger: if the operator renames their server, every link their friends already
--     hold must keep working.
ALTER TABLE servers ADD COLUMN slug TEXT;

-- ★★★ CASE-INSENSITIVE AND UNIQUE. mdnsLabel() already lowercases, so this is belt and braces
--     against a caller that did its own derivation — two servers must never hold one address.
CREATE UNIQUE INDEX IF NOT EXISTS idx_servers_slug ON servers(slug) WHERE slug IS NOT NULL;

-- ★★ A NAME IS HELD FOR AS LONG AS THE ROW EXISTS, WHICH INCLUDES AN EXPIRED LISTING. Expiry only
--    HIDES a server (the list query filters on expires_at), so a receiver that is merely switched
--    off overnight keeps its address and its shared links keep resolving when it returns.
--    ★★★ ONLY AN EXPLICIT DELIST FREES A NAME — because freeing one silently would point links
--        somebody already shared at a STRANGER'S receiver.
