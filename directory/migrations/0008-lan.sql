-- ★★★ SAME NETWORK → GO DIRECT (V12; Stuart, 2026-10-30). A listener on the same network as a server is handed
--     the server's LAN address so they skip the tunnel — see lanColumnsFor in src/index.js for the whole rule.
--
--   lan_json   the server's LAN address(es), "192.168.86.13:48001", as it sent them on its last ping (cleaned:
--              private IPv4 with a port, at most four). '' when it sent none — an older server, or one that stopped.
--   lan_net    ★★★ A HASH of the network the ping came from (the server's public IPv4, or its IPv6 /64) — NEVER the
--              address itself. Enough to answer "is this visitor on the same network?" by equality, nothing more.
--   lan_nonce  a nonce, kept across pings, that the client sends to the LAN address as ?dirNonce=…
--   lan_proof  HMAC-SHA256(key, lan_nonce) — what the real server answers it with (dirProof). A box at the same
--              address that cannot answer it is not this server, and the client uses the tunnel instead.
--
-- ★ Apply BEFORE deploying the Worker that reads them (the Worker copes either way — it skips the LAN half when the
--   columns are missing — but nothing is offered until they exist):
--     npx wrangler d1 execute vibesdr-directory --remote --file migrations/0008-lan.sql
ALTER TABLE servers ADD COLUMN lan_json  TEXT NOT NULL DEFAULT '';
ALTER TABLE servers ADD COLUMN lan_net   TEXT NOT NULL DEFAULT '';
ALTER TABLE servers ADD COLUMN lan_nonce TEXT NOT NULL DEFAULT '';
ALTER TABLE servers ADD COLUMN lan_proof TEXT NOT NULL DEFAULT '';
CREATE INDEX IF NOT EXISTS idx_servers_lan_net ON servers(lan_net);
