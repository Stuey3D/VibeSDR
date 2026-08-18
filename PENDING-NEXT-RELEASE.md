# Must ship in the NEXT released version

Written 2026-08-18, while 10.3.1 (build 140) was in review. These are changes that
exist in the tree but are NOT in the build users will get, so they are invisible
until something deliberately carries them forward.

## 1. The in-app release note for 10.3.1 is WRONG in the shipped build
`src/components/AboutOverlay.tsx` — the V10.3.1 entry in build 140 blames the
local-address silence on the session-registration bug. That bug was real and is
fixed, but it is **not** what was silencing UberSDR over a LAN address: the audio
connection could not reach a plain `ws://` on a private address at all, and the
cure was the transport fallback (`d2dd42d9`).

The corrected note is already in the tree. It must reach a RELEASED build, or the
app keeps explaining its own most visible fault with the wrong reason.

★ This is the AGENTS.md rule about copy that tells a user where something is,
  applied to copy that tells them WHY: verify the whole sentence, not the part
  you came to fix. The first note was written before the cause was known and read
  as settled fact.

## 2. The in-app notice endpoint
See the `app_notice_endpoint` memory. Agreed 2026-08-18: `/api/notice` on the
Worker that already serves the website, version-gated and fail-silent, so a known
fault can be announced to users instead of one GitHub issue being the whole
channel.

## 3. Jr
1.3.1 carries the `.waiting` connect deadline, the IPv4 escalation and the
`stop()` serialisation. Awaiting Stuart's test before submission.
