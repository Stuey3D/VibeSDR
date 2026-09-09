-- ★ A receiver behind a multi-radio front door lives at /r/<serial>/, and the pairing pointed at
--   the front door alone: the bridge asked the door for /ws/iq and got 503. The page path is part
--   of WHICH receiver, so it is registered with the code.
ALTER TABLE iq_codes ADD COLUMN path TEXT NOT NULL DEFAULT '';
