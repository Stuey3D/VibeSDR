// test-cors.cpp — who may read this receiver cross-origin (vibe_cors.h, beta93 2026-11-24).
//
// ★★★ WHY THIS EXISTS. The web page served at <slug>.vibeserver.vibesdr.net now reads the receiver DIRECTLY instead of
//     through the directory's Worker (Stuart: "the gallery etc should all be handled via the free tunnel not the main
//     workers"). That makes every read cross-origin, so the rule for who may read the answer is now load-bearing:
//     the slug origins get themselves echoed, a foreign origin never reads anything that carried a credential, and
//     the page's policy names the tunnel host only when the directory proxied it. The end-to-end half — a stand-in
//     Worker counting what still reaches it — is scripts/test-web-direct-data.mjs.
#include "vibe_cors.h"
#include <cstdio>
#include <string>

static int fails = 0, passes = 0;
static void ok(bool c, const char* what) { printf("  %s  %s\n", c ? "ok  " : "FAIL", what); c ? passes++ : fails++; }

int main() {
    using namespace vibecors;
    printf("allowed origins\n");
    ok(allowedOrigin("https://stuey3d-xcover-4s.vibeserver.vibesdr.net") == "https://stuey3d-xcover-4s.vibeserver.vibesdr.net",
       "★★ a server's slug address is echoed exactly");
    ok(allowedOrigin("https://vibeserver.vibesdr.net") == "https://vibeserver.vibesdr.net", "the directory itself");
    ok(allowedOrigin("https://Pi500.VibeServer.VibeSDR.net") == "https://Pi500.VibeServer.VibeSDR.net", "case-insensitive, echoed as sent");
    ok(allowedOrigin("https://pi500.vibeserver.vibesdr.net:8443") == "https://pi500.vibeserver.vibesdr.net:8443", "a port on our name");
    ok(allowedOrigin("http://pi500.vibeserver.vibesdr.net").empty(), "★ plain http is not ours");
    ok(allowedOrigin("https://evil.example").empty(), "a stranger");
    ok(allowedOrigin("https://evilvibeserver.vibesdr.net").empty(), "★★ a name that merely ENDS in the zone");
    ok(allowedOrigin("https://a.b.vibeserver.vibesdr.net").empty(), "★ two labels deep");
    ok(allowedOrigin("https://pi500.vibeserver.vibesdr.net.evil.example").empty(), "★★ the zone as a prefix of a stranger's");
    ok(allowedOrigin("https://pi500.vibeserver.vibesdr.net/x").empty(), "a path");
    ok(allowedOrigin("https://pi500.vibeserver.vibesdr.net:").empty(), "an empty port");
    ok(allowedOrigin("https://pi500.vibeserver.vibesdr.net:80a").empty(), "a port that is not digits");
    ok(allowedOrigin("https://-x.vibeserver.vibesdr.net").empty(), "a label starting with '-'");
    ok(allowedOrigin("https://.vibeserver.vibesdr.net").empty(), "an empty label");
    ok(allowedOrigin("null").empty(), "the opaque origin");
    ok(allowedOrigin("").empty(), "no origin");

    printf("credentialed requests\n");
    ok(credentialed("GET /vibeserver/gallery.json?kind=dsc&vs_nonce=ab&vs_auth=cd HTTP/1.1"), "★★ the PIN proof");
    ok(credentialed("GET /vibeserver/dxlog.json?vs_admin_ticket=xyz HTTP/1.1"), "★★ the admin ticket");
    ok(credentialed("GET /r/abc/vibeserver/conditions?x=1&vs_admin_nonce=1&vs_admin_auth=2 HTTP/1.1"), "an admin proof, on a radio path");
    ok(credentialed("GET /ws/iq?tok=abcdefgh HTTP/1.1"), "the raw IQ token");
    ok(credentialed("GET /vibeserver/admin/status HTTP/1.1"), "★ an admin route, even bare");
    ok(credentialed("POST /r/x/vibeserver/admin/autoppm HTTP/1.1"), "an admin route behind the front door");
    ok(credentialed("GET /setup HTTP/1.1"), "the setup page");
    ok(!credentialed("GET /vibeserver.json HTTP/1.1"), "★ the public status read");
    ok(!credentialed("GET /vibeserver/gallery.json?kind=ism&limit=1000 HTTP/1.1"), "an open server's gallery");
    ok(!credentialed("GET /vibeserver/gallery.json?xvs_nonce=1 HTTP/1.1"), "★ a parameter that merely CONTAINS the name");

    printf("the head a request gets\n");
    std::string line;
    ok(headFor("https://pi500.vibeserver.vibesdr.net", "GET /vibeserver/dxlog.json?vs_nonce=1&vs_auth=2 HTTP/1.1", line)
       && line == "Access-Control-Allow-Origin: https://pi500.vibeserver.vibesdr.net\r\nVary: Origin\r\n",
       "★★★ a slug page reading with its PIN: its own origin, never *");
    ok(headFor("https://evil.example", "GET /vibeserver/dxlog.json?vs_nonce=1&vs_auth=2 HTTP/1.1", line) && line.empty(),
       "★★★ a stranger with a credential in the URL: every Allow-Origin line stripped");
    ok(!headFor("https://evil.example", "GET /vibeserver.json HTTP/1.1", line), "a stranger on the public read: unchanged (its *)");
    ok(!headFor("", "GET /vibeserver/dxlog.json?vs_nonce=1 HTTP/1.1", line), "no Origin (same-origin GET, the apps): unchanged");

    printf("preflight\n");
    const std::string pf = preflight("https://pi500.vibeserver.vibesdr.net");
    ok(pf.rfind("HTTP/1.1 204", 0) == 0, "204");
    ok(pf.find("Access-Control-Allow-Origin: https://pi500.vibeserver.vibesdr.net\r\n") != std::string::npos, "echoes the origin");
    ok(pf.find("Range") != std::string::npos, "allows Range (the GPU map's PMTiles reads)");
    ok(pf.find("*") == std::string::npos, "★ no wildcard anywhere");

    printf("the page's policy names the direct host\n");
    ok(cspDirect("directory", "jolly-cat.trycloudflare.com", "x") == "https://jolly-cat.trycloudflare.com", "★★ the Worker's header");
    ok(cspDirect("directory", "", "jolly-cat.trycloudflare.com") == "https://jolly-cat.trycloudflare.com", "an older Worker: the tunnel's Host");
    ok(cspDirect("directory", "", "radio.example.org:8443") == "https://radio.example.org:8443", "an owner's own name, with a port");
    ok(cspDirect("", "jolly-cat.trycloudflare.com", "x").empty(), "★★ not proxied by the directory: nothing added");
    ok(cspDirect("directory", "evil.example; script-src *", "").empty(), "★★★ anything but a bare name: nothing added");
    ok(cspDirect("directory", "localhost", "").empty(), "a bare word");
    ok(cspDirect("directory", "", "pi500.vibeserver.vibesdr.net").empty(), "the slug itself ('self' already)");
    ok(cspDirect("directory", "a.b:99999x", "").empty(), "a bad port");

    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
