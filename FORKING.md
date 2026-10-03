# Forking VibeServer

VibeServer and VibeServer Lite are GPLv3, so you are welcome to fork them. OpenWebRX+ is the
model here: an independent build under its own name, which still talks to the same clients.

## Name your fork

Set your fork's identity in [`android/app/src/main/cpp/vibe_fork.h`](android/app/src/main/cpp/vibe_fork.h),
or pass it at build time:

```
-DVIBE_FORK_NAME="\"Lite+\"" -DVIBE_FORK_VERSION="\"1.2\"" -DVIBE_FORK_URL="\"https://example.org/liteplus\""
```

Your server then reports itself under that name, and two places show it:

- **The VibeSDR directory** lists it as *"Lite+ 1.2 — community fork of VibeServer Lite 11.0.0"*,
  with an "about this fork" link if you give a URL.
- **The web client's About section** says *"This receiver runs Lite+ 1.2, a community fork of
  VibeServer Lite 11.0.0."*

The part after "fork of" is the build you started from (`flavour` and `version`). Leave those as
they are.

## Please

- **Use your own name.** Don't call your fork "VibeServer", "VibeServer Lite", "VibeServer inside
  VibeSDR" or "VibeSDR". Listeners read those as the official builds, so the directory ignores
  them as fork names.
- **Keep `proto` / `minProto` honest** (`VS_PROTO` / `VS_MIN_PROTO` in `local_sdr_shim.cpp`). The
  VibeSDR apps and web client use them to decide what they can talk to. If you change the wire
  protocol, change the numbers.
- **Accepted characters:**
  - Name: 2–40 characters. Letters in any script with their marks, digits, spaces, and `. + - _ ( )`.
  - Version: up to 24 characters. Letters, digits, and `. ~ + -`.
  - Link: `http(s)`, up to 200 characters.

  Anything else is dropped rather than shown.

## The directory

A fork is listed and labelled like any other server. A server that breaks the directory or the
VibeSDR apps, or that presents itself as an official build, may be delisted.
