-- vibeserver.vibesdr.net — the public VibeServer directory.
--
-- ★★★ D1, NEVER KV. A 15-minute ping is 96 writes/day per server and KV's free tier is 1,000
--     writes/day TOTAL — about ten servers and the budget is gone. It would look perfectly healthy
--     in testing with one server and fall over the moment it was used. D1 gives 100,000 row
--     writes/day. And NOT Durable Objects: they require the paid plan, which is the whole £5/month
--     this design exists to avoid.

CREATE TABLE IF NOT EXISTS servers (
  id           TEXT PRIMARY KEY,
  -- ★★★ THE KEY IS STORED HASHED, NEVER IN THE CLEAR. It is the identity — anyone holding it can
  --     take over or delist the listing — so a leak of this table must not hand out control of
  --     every listed receiver. We compare hashes; we can never print the key back.
  key_hash     TEXT NOT NULL,

  name         TEXT NOT NULL,          -- the operator's "Public server name"
  url          TEXT NOT NULL,          -- tunnel hostname or the operator's own address
  kind         TEXT NOT NULL,          -- 'tunnel' | 'direct'

  -- ★★★ LOCATION IS THE OPERATOR'S MAIDENHEAD SQUARE, NEVER DERIVED FROM THE ADDRESS.
  --     Geolocating a tunnelled server would put CLOUDFLARE'S EDGE on the map instead of the
  --     operator — "Frankfurt" for a receiver in Manchester. A grid square is also the privacy
  --     answer: a square is a square, not a house.
  grid         TEXT NOT NULL,
  lat          REAL NOT NULL,
  lon          REAL NOT NULL,
  -- ★ Country is for the collapsed country list only, and comes from the REGISTERING request, not
  --   from the tunnel: the server POSTs to us directly from home, so this is the operator's own
  --   country. Two letters, never an address.
  country      TEXT NOT NULL DEFAULT '',

  -- What the server last told us about itself. Whole-JSON so the shape can grow without a
  -- migration; the page reads it defensively.
  status_json  TEXT NOT NULL DEFAULT '{}',

  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,
  -- ★★★ EXPIRY, NOT DETECTION. The list query is `WHERE expires_at > now()`, evaluated at READ
  --     time, so a phone that goes flat, gets killed by Android or leaves wifi VANISHES ON ITS
  --     OWN: no probe, no cron, no write. A dead server stops RENEWING rather than needing to be
  --     DETECTED. This is what makes a slow ping interval safe and temporary shares work at all.
  expires_at   INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_servers_expires ON servers(expires_at);
CREATE INDEX IF NOT EXISTS idx_servers_country ON servers(country);

-- ★ Registration rate limiting. The free tier is generous, not infinite, and a public register
--   endpoint with no brake is an invitation.
CREATE TABLE IF NOT EXISTS reg_log (
  ip           TEXT NOT NULL,
  at           INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_reg_log_at ON reg_log(at);
