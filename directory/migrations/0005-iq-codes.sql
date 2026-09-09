-- ★★★ RAW IQ OUT PAIRING CODES. A listener on a tunnelled receiver who switches raw IQ on is
--     given a six-character code; the VibeIQ bridge on their PC types it in and needs to know
--     WHICH receiver (the slug — the tunnel hostname itself rotates) and the session token the
--     server issued. Registered by the CLIENT, which is on the slug host and holds the token;
--     see src/services/iqPairing.ts for why not the server.
--  ★ Short-lived: refreshed every ten minutes while on, gone twenty minutes after the last
--    refresh, deleted when the listener turns it off. Read at lookup with expires_at > now().
--  ★ A bogus row buys an attacker nothing — the token is checked by the real server on /ws/iq.
CREATE TABLE IF NOT EXISTS iq_codes (
  code        TEXT PRIMARY KEY,
  slug        TEXT NOT NULL,
  token       TEXT NOT NULL,
  ip          TEXT NOT NULL DEFAULT '',
  expires_at  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS iq_codes_expires ON iq_codes (expires_at);
