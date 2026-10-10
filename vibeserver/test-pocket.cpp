// ★★★ THE POCKET VIBESERVER'S DAEMON SIDE — host rules, captive answers, the first-run claim, the
// Wi-Fi request (2026-10-10).
//
// Every rule here is one a phone or a stranger meets before the owner has ever signed in, so each
// is tested from the side that can go wrong:
//   · a probe from iOS must be REDIRECTED until "stay connected", then answered byte-for-byte;
//   · a request to OUR address must never be redirected (that would loop the setup page);
//   · the first password can be claimed ONCE, from a private address, never through the tunnel;
//   · a Wi-Fi request is refused with a sentence, not silently trimmed — and the passwords in it
//     survive the trip intact (quotes, backslashes, unicode) without ever leaking into a reply.
// ★ Runs against temp files: never /etc, never /run, never NetworkManager.

#include "pocket.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}
static std::string slurp(const std::string& p) {
    std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str();
}
static void put(const std::string& p, const std::string& c) { std::ofstream f(p); f << c; }

int main() {
    using namespace vibepocket;
    std::printf("pocket — captive portal, first-run claim, Wi-Fi requests\n");

    char tmpl[] = "/tmp/vs-pocket-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    paths().marker   = dir + "/enabled";
    paths().state    = dir + "/state.json";
    paths().inbox    = dir + "/pocket-wifi.request";
    paths().kick     = dir + "/pocket-kick.request";
    paths().portFile = dir + "/pocket-port";

    // ── host rules ──
    ok(isOurHost("10.42.0.1:48000", "vibepocket"), "an IP literal with a port is ours");
    ok(isOurHost("", "vibepocket"), "no Host header is ours (never redirect what we cannot classify)");
    ok(isOurHost("coastal-sdr.local:48000", "vibepocket"), "a .local name is ours");
    ok(isOurHost("VIBEPOCKET", "vibepocket"), "our hostname, any case, is ours");
    ok(isOurHost("[fe80::1]:48000", "vibepocket"), "an IPv6 literal is ours");
    ok(!isOurHost("captive.apple.com", "vibepocket"), "captive.apple.com is foreign");
    ok(!isOurHost("connectivitycheck.gstatic.com", "vibepocket"), "Android's check host is foreign");
    ok(!isOurHost("www.msftconnecttest.com.", "vibepocket"), "a trailing dot does not hide a foreign host");

    ok(probeFor("/hotspot-detect.html") == Probe::Apple, "iOS probe recognised");
    ok(probeFor("/generate_204?x=1") == Probe::Android, "Android probe recognised, query ignored");
    ok(probeFor("/connecttest.txt") == Probe::MsConnect, "Windows probe recognised");
    ok(probeFor("/index.html") == Probe::None, "an ordinary page is not a probe");

    ok(isPrivatePeer("10.42.0.17"), "a hotspot client is private");
    ok(isPrivatePeer("192.168.1.20") && isPrivatePeer("172.20.0.5"), "home LAN ranges are private");
    ok(isPrivatePeer("::ffff:10.42.0.9"), "v4-mapped hotspot client is private");
    ok(!isPrivatePeer("127.0.0.1") && !isPrivatePeer("::1"), "loopback is NOT private — the tunnel arrives as loopback");
    ok(!isPrivatePeer("81.2.69.160") && !isPrivatePeer("172.32.0.1"), "public addresses are not private");
    ok(!isPrivatePeer("10.0.0.1x") && !isPrivatePeer("999.1.1.1"), "malformed addresses are not private");

    // ── validation ──
    ok(validPsk("12345678") && !validPsk("1234567"), "WPA2: eight characters is the minimum");
    ok(validPsk(std::string(64, 'a')) && !validPsk(std::string(64, 'g')), "64 characters must be hex");
    ok(!validPsk("abcdefgh\x01"), "a control character is refused");
    ok(validSsid("Stuart's iPhone") && !validSsid("") && !validSsid(std::string(33, 'x')), "SSID 1-32 bytes");

    Hooks h;
    bool adminSet = false, configured = false;
    std::string claimedPass, claimedPin;
    h.adminSet   = [&]{ return adminSet; };
    h.configured = [&]{ return configured; };
    h.port       = []{ return 48000; };
    h.hostname   = []{ return std::string("vibepocket"); };
    h.serverName = []{ return std::string("Pocket <b>SDR</b>"); };
    h.claim = [&](const std::string& p, const std::string& pin, std::string&) {
        claimedPass = p; claimedPin = pin; adminSet = true; return true;
    };

    // ── not a pocket box: nothing happens ──
    {
        Req rq; rq.method = "GET"; rq.path = "/hotspot-detect.html"; rq.host = "captive.apple.com"; rq.peer = "10.42.0.5";
        setApActive(true);
        Reply rp;
        ok(!handle(rq, rp, h), "no marker: a probe is NOT intercepted (desktop installs untouched)");
        rq.path = "/vibeserver/pocket/hello";
        Reply rp2;
        ok(handle(rq, rp2, h) && rp2.body.find("\"pocket\":false") != std::string::npos, "no marker: hello says pocket:false");
        rq.method = "POST"; rq.path = "/vibeserver/pocket/claim"; rq.body = "{\"pass\":\"secret1\"}";
        Reply rp3;
        ok(handle(rq, rp3, h) && rp3.code == 404 && !adminSet, "no marker: claim is refused");
    }
    put(paths().marker, "1\n");
    ok(enabled(), "the marker switches pocket mode on");
    put(paths().state, "{\"mode\":\"setup-ap\",\"ap\":{\"set\":false,\"ssid\":\"\"},\"saved\":[]}\n");

    // ── captive ──
    {
        Req rq; rq.method = "GET"; rq.path = "/hotspot-detect.html"; rq.host = "captive.apple.com"; rq.peer = "10.42.0.5";
        Reply rp;
        ok(handle(rq, rp, h) && rp.code == 302 && rp.location == "http://10.42.0.1:48000/",
           "unconfigured: iOS probe → 302 to the SETUP PAGE itself");
        rq.host = "10.42.0.1:48000"; rq.path = "/";
        Reply own;
        ok(!handle(rq, own, h), "our own address is never redirected (no loop)");
        rq.host = "captive.apple.com"; rq.path = "/hotspot-detect.html"; rq.peer = "127.0.0.1";
        Reply lo;
        ok(!handle(rq, lo, h), "loopback is never captive");

        configured = true;
        rq.peer = "10.42.0.5";
        Reply rp2;
        ok(handle(rq, rp2, h) && rp2.code == 302 && rp2.location == "http://10.42.0.1:48000/vibeserver/pocket/welcome",
           "configured: iOS probe → the small welcome page");

        Req acc; acc.method = "POST"; acc.path = "/vibeserver/pocket/accept"; acc.peer = "10.42.0.5";
        Reply ra;
        ok(handle(acc, ra, h) && ra.code == 200, "stay connected is accepted while the hotspot is up");
        Reply rp3;
        ok(handle(rq, rp3, h) && rp3.code == 200 &&
           rp3.body == "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>",
           "after stay connected: iOS gets its exact Success page (sheet shows Done)");
        Req and2 = rq; and2.host = "connectivitycheck.gstatic.com"; and2.path = "/generate_204";
        Reply r204;
        ok(handle(and2, r204, h) && r204.code == 204 && r204.body.empty(), "Android gets its 204");
        Req other = rq; other.peer = "10.42.0.6";
        Reply ro;
        ok(handle(other, ro, h) && ro.code == 302, "a DIFFERENT phone is still redirected");
        setApActive(false); setApActive(true);
        Reply rp4;
        ok(handle(rq, rp4, h) && rp4.code == 302, "a new hotspot session forgets every stay-connected");
        setApActive(false);
        Reply off;
        ok(!handle(rq, off, h), "hotspot down: nothing captive at all");
        setApActive(true);

        Req w; w.method = "GET"; w.path = "/vibeserver/pocket/welcome"; w.peer = "10.42.0.5";
        Reply rw;
        ok(handle(w, rw, h) && rw.body.find("Pocket &lt;b&gt;SDR&lt;/b&gt;") != std::string::npos,
           "the welcome page escapes the server's name");
        configured = false;
    }

    // ── the first-run claim ──
    {
        Req rq; rq.method = "GET"; rq.path = "/vibeserver/pocket/hello"; rq.peer = "10.42.0.5";
        Reply rp;
        ok(handle(rq, rp, h) && rp.body.find("\"claimable\":true") != std::string::npos, "hello: claimable from the hotspot");
        Req tun = rq; tun.viaTunnel = true;
        Reply rt;
        ok(handle(tun, rt, h) && rt.body.find("\"claimable\":false") != std::string::npos, "hello: NOT claimable through the tunnel");

        Req c; c.method = "POST"; c.path = "/vibeserver/pocket/claim"; c.peer = "127.0.0.1"; c.body = "{\"pass\":\"secret1\"}";
        Reply r1;
        ok(handle(c, r1, h) && r1.code == 403 && !adminSet, "claim from loopback (= the tunnel) refused");
        c.peer = "10.42.0.5"; c.body = "{\"pass\":\"short\"}";
        Reply r2;
        ok(handle(c, r2, h) && r2.code == 400 && !adminSet, "a 5-character password is refused (TUI rule: 6)");
        c.body = "{\"pass\":\"secret1\",\"pin\":\"12a4\"}";
        Reply r3;
        ok(handle(c, r3, h) && r3.code == 400 && !adminSet, "a PIN with a letter is refused");
        c.body = "{\"pass\":\"se\\\"cr\\u00e9t\",\"pin\":\"1234\"}";
        Reply r4;
        ok(handle(c, r4, h) && r4.code == 200 && claimedPass == "se\"cr\xc3\xa9t" && claimedPin == "1234",
           "claim saves the password exactly as typed (quote, unicode) and the PIN");
        c.body = "{\"pass\":\"another1\"}";
        Reply r5;
        ok(handle(c, r5, h) && r5.code == 409 && claimedPass == "se\"cr\xc3\xa9t", "a SECOND claim is refused — once only");
    }

    // ── Wi-Fi settings ──
    {
        Req rq; rq.method = "POST"; rq.path = "/vibeserver/pocket/wifi"; rq.peer = "10.42.0.5";
        rq.body = "{\"networks\":[{\"ssid\":\"Home\",\"psk\":\"pa\\\"ss\\\\word\"}],\"ap\":{\"ssid\":\"Pocket\",\"psk\":\"hotspot1\"}}";
        Reply r0;
        ok(handle(rq, r0, h) && r0.code == 401, "Wi-Fi settings need the admin proof");
        rq.adminOk = true;

        auto post = [&](const std::string& body) { Req x = rq; x.body = body; Reply r; handle(x, r, h); return r; };
        Reply bad = post("{\"networks\":[],\"ap\":{\"ssid\":\"Pocket\",\"psk\":\"short\"}}");
        ok(bad.code == 400 && bad.body.find("8 to 63") != std::string::npos, "hotspot password under 8 refused, with a sentence");
        Reply vs = post("{\"networks\":[],\"ap\":{\"ssid\":\"vibeserver\",\"psk\":\"hotspot1\"}}");
        ok(vs.code == 400, "the hotspot may not be called VibeServer (the open setup name)");
        Reply four = post("{\"networks\":[{\"ssid\":\"a\",\"psk\":\"12345678\"},{\"ssid\":\"b\",\"psk\":\"12345678\"},"
                          "{\"ssid\":\"c\",\"psk\":\"12345678\"},{\"ssid\":\"d\",\"psk\":\"12345678\"}],"
                          "\"ap\":{\"ssid\":\"P\",\"psk\":\"12345678\"}}");
        ok(four.code == 400, "a fourth network is refused");
        Reply dup = post("{\"networks\":[{\"ssid\":\"a\",\"psk\":\"12345678\"},{\"ssid\":\"a\",\"psk\":\"12345678\"}],"
                         "\"ap\":{\"ssid\":\"P\",\"psk\":\"12345678\"}}");
        ok(dup.code == 400, "the same network twice is refused");
        Reply keepNew = post("{\"networks\":[{\"ssid\":\"Home\",\"keep\":true}],\"ap\":{\"ssid\":\"P\",\"psk\":\"12345678\"}}");
        ok(keepNew.code == 400, "keep is refused for a network with no saved password");
        Reply apKeep = post("{\"networks\":[],\"ap\":{\"ssid\":\"P\",\"keep\":true}}");
        ok(apKeep.code == 400, "keeping a hotspot password that was never set is refused");
        Reply ctry = post("{\"networks\":[],\"ap\":{\"ssid\":\"P\",\"psk\":\"12345678\"},\"country\":\"gb\"}");
        ok(ctry.code == 400, "country must be two capitals");
        Reply junk = post("{\"networks\":[{\"ssid\":\"a\"]}");
        ok(junk.code == 400, "malformed JSON is refused");

        Reply good = post("{\"networks\":[{\"ssid\":\"Home \\\"Wi-Fi\\\"\",\"psk\":\"pa\\\"ss\\\\word\"},"
                          "{\"ssid\":\"Stuart's iPhone\",\"psk\":\"phonepass\",\"hidden\":true}],"
                          "\"ap\":{\"ssid\":\"Pocket\",\"psk\":\"hotspot1\"},\"country\":\"GB\",\"switch\":true}");
        ok(good.code == 200 && good.body.find("\"switching\":true") != std::string::npos, "a good request is accepted");
        ok(good.body.find("pa") == std::string::npos && good.body.find("hotspot1") == std::string::npos,
           "the reply carries no password");
        struct stat st{};
        ok(::stat(paths().inbox.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the inbox file is 0600");
        const std::string inbox = slurp(paths().inbox);
        ok(inbox.find("\"ssid\":\"Home \\\"Wi-Fi\\\"\"") != std::string::npos, "SSID with quotes survives, escaped");
        ok(inbox.find("\"psk\":\"pa\\\"ss\\\\word\"") != std::string::npos, "password with quote + backslash survives, escaped");
        ok(inbox.find("Home") < inbox.find("Stuart"), "ORDER IS KEPT: network 1 before network 2");
        ok(inbox.find("\"hidden\":true") != std::string::npos, "the hidden flag (phone hotspot) is carried");

        // Keep works once the state reports the network as saved.
        put(paths().state, "{\"mode\":\"setup-ap\",\"ap\":{\"set\":true,\"ssid\":\"Pocket\"},"
                           "\"saved\":[{\"ssid\":\"Home\"},{\"ssid\":\"Stuart's iPhone\"}]}\n");
        Reply reorder = post("{\"networks\":[{\"ssid\":\"Stuart's iPhone\",\"keep\":true},{\"ssid\":\"Home\",\"keep\":true}],"
                             "\"ap\":{\"ssid\":\"Pocket\",\"keep\":true}}");
        ok(reorder.code == 200, "reordering without retyping passwords is accepted");
        const std::string inbox2 = slurp(paths().inbox);
        ok(inbox2.find("Stuart") < inbox2.find("Home"), "the new order is what is written");

        Req g; g.method = "GET"; g.path = "/vibeserver/pocket/wifi"; g.adminOk = true; g.peer = "10.42.0.5";
        Reply rg;
        ok(handle(g, rg, h) && rg.body.find("\"saved\"") != std::string::npos, "GET wifi returns the state");

        Req k; k.method = "POST"; k.path = "/vibeserver/pocket/scan"; k.adminOk = true; k.body = "{\"force\":true}";
        Reply rk;
        ok(handle(k, rk, h) && rk.code == 200 && slurp(paths().kick) == "scan-force\n", "forced scan writes the kick word");
    }

    // ── PORTABLE CONNECTION: Network | Own Wi-Fi (Stuart, 2026-10-10) ──
    {
        ok(linkAdvice(-76, 65, 2) == "weak", "advice: signal -76 dBm is weak");
        ok(linkAdvice(-60, 6.5, 0) == "weak", "advice: a 6.5 Mbit/s link is weak");
        ok(linkAdvice(-60, 65, 12) == "weak", "advice: 12 % retries is weak");
        ok(linkAdvice(-60, 65, 2).empty(), "advice: -60 dBm, 65 Mbit/s, 2 % retries is fine");
        ok(linkAdvice(-1, -1, -1).empty(), "advice: figures not reported never count as weak");

        put(paths().state, "{\"mode\":\"client\",\"ssid\":\"Home\",\"hold\":false,\"ap\":{\"set\":true,\"ssid\":\"Pocket\"},"
                           "\"saved\":[{\"ssid\":\"Home\"}],\"link\":{\"signal\":-78,\"tx\":13.0,\"rx\":72.2,\"retry\":9}}\n");
        Req g; g.method = "GET"; g.path = "/vibeserver/pocket/connection"; g.peer = "192.168.86.61";
        Reply rg;
        ok(handle(g, rg, h) && rg.body.find("\"local\":true") != std::string::npos
           && rg.body.find("\"signal\":-78") != std::string::npos && rg.body.find("\"tx\":13") != std::string::npos
           && rg.body.find("\"advice\":\"weak\"") != std::string::npos && rg.body.find("\"ssid\":\"Home\"") != std::string::npos,
           "GET connection on the local network: the link's figures and the advice (weak at -78 dBm)");
        Req gt = g; gt.viaTunnel = true;
        Reply rgt;
        ok(handle(gt, rgt, h) && rgt.body == "{\"pocket\":true,\"local\":false}", "…through the tunnel: local:false and nothing else");
        Req gp = g; gp.peer = "81.2.69.160";
        Reply rgp;
        ok(handle(gp, rgp, h) && rgp.body.find("\"local\":false") != std::string::npos, "…from a public address: local:false");

        Req p; p.method = "POST"; p.path = "/vibeserver/pocket/connection"; p.peer = "192.168.86.61"; p.body = "{\"to\":\"own\"}";
        Reply rp;
        ok(handle(p, rp, h) && rp.code == 200 && slurp(paths().kick) == "own\n", "Own Wi-Fi from the local network writes the kick word");
        Req pt = p; pt.viaTunnel = true;
        Reply rpt;
        ok(handle(pt, rpt, h) && rpt.code == 403, "…and is REFUSED through the tunnel");
        Req pp = p; pp.peer = "81.2.69.160";
        Reply rpp;
        ok(handle(pp, rpp, h) && rpp.code == 403, "…and from a public address");
        Req pn = p; pn.body = "{\"to\":\"network\"}";
        Reply rpn;
        ok(handle(pn, rpn, h) && rpn.code == 200 && slurp(paths().kick) == "network\n", "Network writes its kick word");
        Req px = p; px.body = "{\"to\":\"reboot\"}";
        Reply rpx;
        ok(handle(px, rpx, h) && rpx.code == 400, "anything else is refused");
        put(paths().state, "{\"mode\":\"client\",\"ap\":{\"set\":false,\"ssid\":\"\"},\"saved\":[{\"ssid\":\"Home\"}]}\n");
        Reply rno;
        ok(handle(p, rno, h) && rno.code == 409, "Own Wi-Fi with no hotspot of its own set up: refused (409)");
    }

    std::string cleanup = "rm -rf '" + dir + "'";
    (void)!std::system(cleanup.c_str());
    if (failures) { std::printf("\n\033[31m%d failed\033[0m\n", failures); return 1; }
    std::printf("\n\033[32mall passed\033[0m\n");
    return 0;
}
