// QuickTunnelUrlTest.kt — VibeQuickTunnelUrl.kt (the file the APK compiles) against the lines that matter.
// Run by scripts/test-tunnel-url-android.sh. Same lines as vibeserver/test-tunnel-url.cpp — the two parsers must agree.
// ★★★ The first literal is Kiko's (BTV B9, Android 6.0.1, Lite 11.0.13): the error line the old regex read as an address.
import com.vibesdr.app.VibeQuickTunnelUrl

var bad = 0
fun check(cond: Boolean, what: String) { if (cond) println("  ok   $what") else { bad++; println("  FAIL $what") } }
fun url(l: String) = VibeQuickTunnelUrl.fromLogLine(l)

fun main() {
    println("tunnel address (Android) — which cloudflared line announces it")
    val kikoErr = "failed to request quick Tunnel: Post \"https://api.trycloudflare.com/tunnel\": context deadline exceeded " +
        "(Client.Timeout exceeded while awaiting headers)"
    check(url(kikoErr) == null, "★★★ Kiko's error line yields NO address (it used to yield api.trycloudflare.com)")
    check(url("2026-10-26T22:04:00Z ERR $kikoErr") == null, "★★★ … nor with cloudflared's timestamp and ERR level")
    check(url("2026-10-26T22:04:00Z INF |  https://api.trycloudflare.com  |") == null, "★★ api.trycloudflare.com refused even in a box")
    check(url("INF |  https://www.trycloudflare.com  |") == null, "★ www is refused too")
    val box = "2026-10-26T22:04:05Z INF |  https://printed-attacks-postposted-hong.trycloudflare.com                                 |\n"
    check(url(box) == "https://printed-attacks-postposted-hong.trycloudflare.com", "★★★ the real box row yields the address")
    check(url("INF |  https://resume-test.trycloudflare.com  |") == "https://resume-test.trycloudflare.com", "★ the test scripts' fake line parses")
    check(url("INF |  https://example.trycloudflare.com  |") == "https://example.trycloudflare.com", "★★ a one-word name — no hyphen rule")
    check(url("Your quick Tunnel has been created! Visit it at: https://printed-attacks-postposted-hong.trycloudflare.com") ==
        "https://printed-attacks-postposted-hong.trycloudflare.com", "★ the one-line \"Visit it at: …\" form is accepted")
    check(url("INF |  https://Printed-Attacks-Hong.TryCloudflare.com  |") == "https://printed-attacks-hong.trycloudflare.com", "★ case is folded")
    check(url("INF |  https://error-tomato-fail-fox.trycloudflare.com  |") == "https://error-tomato-fail-fox.trycloudflare.com",
        "★★ a name containing 'error'/'fail' is still a name")
    check(url("INF |  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable):  |") == null,
        "★ the heading row has no address")
    check(url("INF +--------------------------------------------------------------------+") == null, "★ a border has none")
    check(url("INF Requesting new quick Tunnel on trycloudflare.com...") == null, "★ the 'Requesting' line has none")
    check(url("INF Thank you for trying Cloudflare Tunnel. See https://developers.cloudflare.com/x") == null, "★ another URL is not ours")
    check(url("WRN |  https://printed-attacks-hong.trycloudflare.com  |") == null, "★★ a WARNING line never yields one")
    check(url("INF url=https://printed-attacks-hong.trycloudflare.com/ws") == null, "★★ a URL with a path is a mention")
    check(url("INF connection to https://a.b.trycloudflare.com  |") == null, "★ a dotted label is not an assigned name")
    check(url("INF |  https://-bad-.trycloudflare.com  |") == null, "★ a label cannot start or end with '-'")
    check(url("") == null, "★ an empty line is nothing")
    println(if (bad == 0) "\nall tunnel-url checks passed (Android)" else "\n$bad FAILURE(S)")
    kotlin.system.exitProcess(if (bad == 0) 0 else 1)
}
