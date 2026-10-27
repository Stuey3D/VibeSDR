// test-tunnel-url.cpp — cloudflared's FAILURE line is never a tunnel address (tunnel_url.h, 2026-10-27).
//
// ★★★ WHY THIS EXISTS. Kiko's TV box (Android 6.0.1, Lite 11.0.13) logged "quick tunnel up: https://api.trycloudflare.com"
//     — read out of cloudflared's own error line — then recycled the tunnel every 15 s for ever. The first literal below
//     is that line exactly as he reported it; the success lines are cloudflared 2026.10.4's real box (quick_tunnel.go).
//     The Kotlin twin (VibeQuickTunnelUrl.kt) is held to the same lines by scripts/test-tunnel-url-android.sh.
#include "tunnel_url.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const char* what) { printf("  %s  %s\n", c ? "ok  " : "FAIL", what); if (!c) fails++; }

int main() {
    using vstunnel::quickTunnelUrl;
    printf("tunnel address — which cloudflared line announces it\n");
    const std::string kikoErr =
        "failed to request quick Tunnel: Post \"https://api.trycloudflare.com/tunnel\": context deadline exceeded "
        "(Client.Timeout exceeded while awaiting headers)";
    ok(quickTunnelUrl(kikoErr).empty(), "★★★ Kiko's error line yields NO address (it used to yield api.trycloudflare.com)");
    ok(quickTunnelUrl("2026-10-26T22:04:00Z ERR " + kikoErr).empty(), "★★★ … nor with cloudflared's timestamp and ERR level");
    ok(quickTunnelUrl("2026-10-26T22:04:00Z INF |  https://api.trycloudflare.com  |").empty(),
       "★★ api.trycloudflare.com is Cloudflare's API — refused even in a box");
    ok(quickTunnelUrl("INF |  https://www.trycloudflare.com  |").empty(), "★ www is refused too");

    const std::string box = "2026-10-26T22:04:05Z INF |  https://printed-attacks-postposted-hong.trycloudflare.com"
                            "                                 |\n";
    ok(quickTunnelUrl(box) == "https://printed-attacks-postposted-hong.trycloudflare.com",
       "★★★ the real box row yields the address (trailing padding, bar and newline dropped)");
    ok(quickTunnelUrl("INF |  https://resume-test.trycloudflare.com  |") == "https://resume-test.trycloudflare.com",
       "★ the shape scripts/test-tunnel-*.mjs fake cloudflared with still parses");
    ok(quickTunnelUrl("INF |  https://example.trycloudflare.com  |") == "https://example.trycloudflare.com",
       "★★ a one-word name (cloudflared's own test uses one) — no hyphen rule");
    ok(quickTunnelUrl("Your quick Tunnel has been created! Visit it at: https://printed-attacks-postposted-hong.trycloudflare.com")
           == "https://printed-attacks-postposted-hong.trycloudflare.com",
       "★ the one-line \"Visit it at: …\" form (as Kiko quoted it) is accepted");
    ok(quickTunnelUrl("INF |  https://Printed-Attacks-Hong.TryCloudflare.com  |") == "https://printed-attacks-hong.trycloudflare.com",
       "★ case is folded");
    ok(quickTunnelUrl("INF |  https://error-tomato-fail-fox.trycloudflare.com  |") == "https://error-tomato-fail-fox.trycloudflare.com",
       "★★ a name that happens to contain 'error'/'fail' is still a name — the failure check skips the URL itself");

    ok(quickTunnelUrl("INF |  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable):  |").empty(),
       "★ the heading row has no address");
    ok(quickTunnelUrl("INF +--------------------------------------------------------------------+").empty(), "★ a border has none");
    ok(quickTunnelUrl("INF Requesting new quick Tunnel on trycloudflare.com...").empty(), "★ the 'Requesting' line has none");
    ok(quickTunnelUrl("INF Thank you for trying Cloudflare Tunnel. See https://developers.cloudflare.com/x").empty(),
       "★ another https:// URL is not ours");
    ok(quickTunnelUrl("WRN |  https://printed-attacks-hong.trycloudflare.com  |").empty(), "★★ a WARNING line never yields one");
    ok(quickTunnelUrl("INF url=https://printed-attacks-hong.trycloudflare.com/ws").empty(), "★★ a URL with a path is a mention, not the address");
    ok(quickTunnelUrl("INF connection to https://a.b.trycloudflare.com  |").empty(), "★ a dotted label is not an assigned name");
    ok(quickTunnelUrl("INF |  https://-bad-.trycloudflare.com  |").empty(), "★ a label cannot start or end with '-'");
    ok(quickTunnelUrl("").empty(), "★ an empty line is nothing");

    printf("\n%s\n", fails ? "FAILURES" : "all tunnel-url checks passed");
    return fails ? 1 : 0;
}
