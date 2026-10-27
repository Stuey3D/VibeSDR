package com.vibesdr.app

/**
 * Which cloudflared output line is a Quick Tunnel's public address — and which is not (2026-10-27).
 *
 * ★★★ AN ERROR LINE WAS READ AS THE ADDRESS (Kiko, BTV B9 TV box, Android 6.0.1, Lite 11.0.13). When cloudflared
 *     cannot create the tunnel it logs
 *         failed to request quick Tunnel: Post "https://api.trycloudflare.com/tunnel": context deadline exceeded …
 *     and the old regex — any `https://<label>.trycloudflare.com` anywhere in any line — matched Cloudflare's own
 *     provisioning API in it. The self-check could never reach us through that, so the tunnel was recycled every 15 s
 *     for ever and the screen said it "stopped before it was given an address".
 *
 * ★★ WHAT cloudflared ACTUALLY PRINTS (quick_tunnel.go → cliutil.LogTable, tag 2026.10.4) is a box:
 *         INF |  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable):  |
 *         INF |  https://printed-attacks-postposted-hong.trycloudflare.com                                 |
 *     so an address is accepted ONLY as the whole of a box row, or after "Visit it at" on the same line — never a URL
 *     that merely appears in a message, and never from an error or warning line.
 * ★★ THE C++ TWIN IS vibeserver/tunnel_url.h (Linux/Mac servers). The two must agree; both are tested against the
 *    same lines (vibeserver/test-tunnel-url.cpp, scripts/test-tunnel-url-android.sh).
 * ★ No android.* imports — scripts/test-tunnel-url-android.sh compiles THIS file on a desktop JVM.
 */
object VibeQuickTunnelUrl {

    private const val SCHEME = "https://"
    private const val ZONE = ".trycloudflare.com"

    /** ★ Labels under trycloudflare.com that are Cloudflare's services, never an assigned tunnel. `api` is the one
     *  that bit (it is in every provisioning failure line); `www` is the site itself. */
    private val RESERVED = setOf("api", "www")

    /** ★ The shape the edge assigns: 1–63 of [a-z0-9-], not starting or ending with '-'. NO hyphen-count rule —
     *  cloudflared's own test uses "example.trycloudflare.com", and the naming is Cloudflare's to change. */
    private val LABEL = Regex("""[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?""")

    private val LEVEL = Regex("""(?<![A-Za-z0-9])(?:ERR|WRN|FTL)(?![A-Za-z0-9])""")

    private fun hostChar(c: Char) = c in 'a'..'z' || c in 'A'..'Z' || c in '0'..'9' || c == '-' || c == '.'

    /** ★★ Checked on the line WITH THE CANDIDATE CUT OUT: the edge picks random words, and a good tunnel called
     *  error-tomato-fail-fox must not be refused for its own name. */
    private fun looksLikeFailure(rest: String): Boolean {
        val low = rest.lowercase()
        return "fail" in low || "error" in low || LEVEL.containsMatchIn(rest)
    }

    /** The public address in one line of cloudflared output, or null when the line does not announce one. */
    fun fromLogLine(lineIn: String): String? {
        val line = lineIn.trimEnd('\n', '\r')
        var at = line.indexOf(SCHEME)
        while (at >= 0) {
            var end = at + SCHEME.length
            while (end < line.length && hostChar(line[end])) end++
            val host = line.substring(at + SCHEME.length, end).lowercase()
            val next = line.indexOf(SCHEME, at + 1)
            if (host.length > ZONE.length && host.endsWith(ZONE)) {
                val label = host.substring(0, host.length - ZONE.length)
                val standalone = (end == line.length || line[end] == ' ' || line[end] == '\t' || line[end] == '|') &&
                    (at == 0 || line[at - 1] == ' ' || line[at - 1] == '\t' || line[at - 1] == '|' || line[at - 1] == ':')
                if ('.' !in label && LABEL.matches(label) && label !in RESERVED && standalone) {
                    val before = line.substring(0, at)
                    val after = line.substring(end)
                    if (looksLikeFailure("$before $after")) return null
                    val boxed = before.trimEnd().endsWith('|') && after.trimStart().startsWith('|')
                    val visit = "visit it at" in before.lowercase()
                    if (boxed || visit) return SCHEME + host
                }
            }
            at = next
        }
        return null
    }
}
