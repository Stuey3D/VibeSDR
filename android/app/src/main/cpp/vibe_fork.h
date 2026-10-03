/**
 * ★★★ FORKING VIBESERVER? NAME YOUR FORK HERE. (Stuart, 2026-10-03: forks are welcome — think
 *     OpenWebRX and OpenWebRX+ — and the directory labels them, as it labels our own builds.)
 *
 *  Fill in VIBE_FORK_NAME (and ideally a version and a link) and your server tells the VibeSDR
 *  directory and the web client's About section that it is YOUR build, shown as
 *  "<name> <version> — community fork of VibeServer Lite 11.0.0". Or pass them at build time:
 *      -DVIBE_FORK_NAME="\"Lite+\"" -DVIBE_FORK_VERSION="\"1.2\"" -DVIBE_FORK_URL="\"https://…\""
 *
 *  ★ Please do not call a fork "VibeServer", "VibeServer Lite" or "VibeServer inside VibeSDR" — the
 *    directory ignores those as fork names, because a listener reads them as the official builds.
 *  ★ Keep `proto` / `minProto` (VS_PROTO in local_sdr_shim.cpp) honest: VibeSDR clients decide what
 *    they can talk to by them. A fork that breaks the directory or the apps may be delisted.
 *  ★ The directory accepts, in the name (2–40 characters): letters in any script with their marks,
 *    digits, spaces and . + - _ ( ). In the version (≤24): letters, digits and . ~ + -. The link must
 *    be http(s), ≤200. Anything else is dropped, not shown.
 *  See FORKING.md at the top of the repository.
 *
 *  Empty on official builds — empty means "not a fork".
 */
#pragma once

#ifndef VIBE_FORK_NAME
#define VIBE_FORK_NAME ""
#endif
#ifndef VIBE_FORK_VERSION
#define VIBE_FORK_VERSION ""
#endif
#ifndef VIBE_FORK_URL
#define VIBE_FORK_URL ""
#endif
