# UberSDR HFDL add-on fixture

`aircraft-2026-11-27.json`: one response of `GET http://<ubersdr>:8080/addon/hfdl/aircraft` from Stuart's UberSDR
(RX888), captured 2026-11-27 08:55 UTC, unedited (public aircraft data). 261 rows: 253 with an ICAO address (8 keyed by
flight or registration only), 8 at lat/lon 0,0 with `last_seen` 0 (no position). `last_seen` is Unix seconds.

Used by `vibeserver/test-feed-test.cpp`: the poller's parser, and the fake UberSDR the feed test and auto-detect probe.
The test shifts its clock to the capture's newest row (1795769725) so the ages are as they were when captured.
