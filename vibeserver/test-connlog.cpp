// ★★★ A SECOND CLOSE FOR ONE SESSION MUST NOT INVENT A 0-SECOND "REFUSAL" ROW.
//
// A listener holds several sockets (spectrum, audio, decoder). The log OPENS on the spectrum one
// alone — "logging both would double-count every ordinary browser" — but CLOSED on any of them,
// and an unmatched close fell through to the branch that records a connection refused before it
// was logged. So every ordinary visit was followed by a phantom 0-second entry: connect, bounce
// instantly. That is the exact pattern an owner examines for abuse, and it was our own audio
// socket. Stuart, 2026-08-11, on a Ukrainian address: "is this a bot or spam as it is a very
// distinct connection pattern 0 seconds then 2:50".
//
// ★★ The tests below pin BOTH sides, because the easy fix breaks the other one: a refusal that
//    was never opened must still be recorded, and two session-less refusals from one address are
//    two events (that is what a scan looks like), not one.
#include "vibe_admin.h"
#include <cstdio>
#include <cstdlib>   // mkdtemp
#include <string>
static int fails = 0;
static void ok(bool c, const char* w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w); if (!c) fails++;
}
static int rowsFor(const std::string& j, const std::string& sess) {
  int n = 0; size_t p = 0;
  while ((p = j.find("\"session\":\"" + sess + "\"", p)) != std::string::npos) { n++; p++; }
  return n;
}
int main() {
  std::printf("connection log — one connection, one row\n");
  {
    vibeadmin::ConnLog log;
    log.open("1.2.3.4", "sess-A", "Mozilla/5.0", "GB");
    log.close("1.2.3.4", "sess-A", "closed");     // the spectrum socket
    log.close("1.2.3.4", "sess-A", "closed");     // ★ the AUDIO socket, moments later
    log.close("1.2.3.4", "sess-A", "closed");     // ★ and a decoder socket
    const std::string j = log.json();
    ok(rowsFor(j, "sess-A") == 1, "★★★ three closes for one session leave ONE row, not three");
  }
  {
    // A genuine refusal — never opened — must still be recorded, or the log stops showing
    // the very event an owner is looking for.
    vibeadmin::ConnLog log;
    log.close("9.9.9.9", "sess-B", "banned");
    ok(rowsFor(log.json(), "sess-B") == 1, "a refusal that was never opened is still recorded");
  }
  {
    // Two refusals from one address with NO session id are two events (that is a scan).
    vibeadmin::ConnLog log;
    log.close("8.8.8.8", "", "banned");
    log.close("8.8.8.8", "", "banned");
    const std::string j = log.json();
    int n = 0; size_t p = 0;
    while ((p = j.find("\"ip\":\"8.8.8.8\"", p)) != std::string::npos) { n++; p++; }
    ok(n == 2, "★ two session-less refusals from one address stay two rows");
  }
  {
    // Distinct sessions from one address are distinct connections.
    vibeadmin::ConnLog log;
    log.open("5.5.5.5", "s1", "UA", "GB"); log.close("5.5.5.5", "s1", "closed");
    log.open("5.5.5.5", "s2", "UA", "GB"); log.close("5.5.5.5", "s2", "closed");
    ok(rowsFor(log.json(), "s1") == 1 && rowsFor(log.json(), "s2") == 1,
       "two real visits from one address stay two rows");
  }
  {
    /* ★★★ HISTORY MUST SURVIVE. saveIfDue() used to call rewriteLocked(), which writes the
     *  in-memory tail back out "discarding whatever the file held beyond it" — so the file was cut
     *  to 2000 rows every time it passed 4000. MEASURED on the Pi 500: the busiest radio takes 465
     *  connections a day, so that is about FOUR DAYS, not the months Stuart believed he had.
     *  ★★ This test writes enough rows to force a rotation and then asserts the OLD rows are still
     *     on disk, in an archive, rather than gone. It is the assertion the old behaviour fails. */
    char dir[] = "/tmp/vibe-connlog-XXXXXX";
    if (!mkdtemp(dir)) { ok(false, "could not make a temp dir"); }
    else {
      const std::string path = std::string(dir) + "/connections.jsonl";
      {
        vibeadmin::ConnLog log;
        log.setPath(path);
        // ★ ~120 bytes a row here, so 30k did NOT reach the 4 MB threshold — the first run of this test
        //   proved rotation had not fired at all. 60k clears it with room to spare.
        for (int i = 0; i < 60000; ++i) {
          char sess[32]; std::snprintf(sess, sizeof sess, "s%d", i);
          log.open("9.9.9.9", sess, "UA", "GB");
          log.close("9.9.9.9", sess, "closed");
          if ((i % 500) == 0) log.saveIfDue();
        }
        log.saveIfDue();
      }
      // An archive must exist, and it must hold rows the current file no longer does.
      const std::string arch = path + ".1";
      FILE* a = fopen(arch.c_str(), "r");
      ok(a != nullptr, "★★★ the rotated archive exists — history is not discarded");
      long archRows = 0;
      if (a) { char l[1024]; while (fgets(l, sizeof l, a)) ++archRows; fclose(a); }
      ok(archRows > 2000, "the archive holds far more than the 2000-row live view");
      long curRows = 0;
      if (FILE* c = fopen(path.c_str(), "r")) { char l[1024]; while (fgets(l, sizeof l, c)) ++curRows; fclose(c); }
      std::printf("      archive %ld rows, current %ld rows\n", archRows, curRows);
      ok(archRows + curRows > 20000, "★★★ the great majority of the history is still on disk");
      // ★ And a fresh log must still find rows after a rotation — a restart at the wrong moment
      //   must not show an empty page, which reads exactly like "the log was wiped".
      vibeadmin::ConnLog reloaded;
      reloaded.setPath(path);
      ok(reloaded.json().find("9.9.9.9") != std::string::npos,
         "★★ a reload straight after a rotation still shows connections");
    }
  }
  {
    /* ★★★ THE VISIT VERDICT — "static and left" vs "tuned about and found things".
     *  Stuart, 2026-09-21: "if a user has come for a couple of mins and heard nothing but static
     *  and then left that I really want to know about". Duration and bytes cannot tell those apart. */
    vibeadmin::ConnLog log;
    log.open("1.2.3.4", "quiet", "UA", "GB");
    log.close("1.2.3.4", "quiet", "closed", 4000000, 0, /*stops*/6, /*heard*/0, /*best*/2.1f, 0);
    const std::string j1 = log.json();
    ok(j1.find("\"stops\":6") != std::string::npos && j1.find("\"heard\":0") != std::string::npos,
       "★★★ six stops, nothing heard — the visit that came for static and left");

    log.open("1.2.3.5", "busy", "UA", "GB");
    log.close("1.2.3.5", "busy", "closed", 9000000, 0, 4, 3, 28.6f, 0);
    ok(log.json().find("\"heard\":3") != std::string::npos,
       "★★ four stops, three with signal — somebody who found things to listen to");

    // ★ PARKED: one stop all visit, so the frequency IS worth recording. "dont need exact
    //   frequencies unless they literally only stayed on Heart FM for the session".
    log.open("1.2.3.6", "parked", "UA", "GB");
    log.close("1.2.3.6", "parked", "closed", 90000000, 0, 1, 1, 34.2f, 96600000.0);
    ok(log.json().find("\"parkedHz\":96600000") != std::string::npos,
       "★★ a parked visit records the one frequency they sat on");

    // ★★★ AND THE DISTINCTION THAT MATTERS: "not measured" must not read as "found nothing".
    log.open("1.2.3.7", "unwatched", "UA", "GB");
    log.close("1.2.3.7", "unwatched", "closed");        // no verdict passed at all
    const std::string j4 = log.json();
    // ★ Isolate THAT row — the others in this log legitimately carry a verdict.
    const size_t at     = j4.find("unwatched");
    const size_t rowBeg = (at == std::string::npos) ? std::string::npos : j4.rfind('{', at);
    const size_t rowEnd = (at == std::string::npos) ? std::string::npos : j4.find('}', at);
    const std::string row = (rowBeg == std::string::npos || rowEnd == std::string::npos)
                          ? std::string() : j4.substr(rowBeg, rowEnd - rowBeg);
    ok(!row.empty() && row.find("\"stops\":") == std::string::npos,
       "★★★ a close with no verdict records NO stops field — absent, not a zero");
  }
  {
    /* ★★★ HEARD WAS "—" ON ALMOST EVERY ROW (Stuart, 2026-09-29). Two of the ways the verdict
     *  and the audio figure were lost, pinned here; the third (no tally on a direct radio) lives
     *  in the shim and is proven end to end against a running server. */
    auto rowOf = [](const std::string& j, const std::string& sess) {
      const size_t at = j.find("\"session\":\"" + sess + "\"");
      if (at == std::string::npos) return std::string();
      const size_t a = j.rfind('{', at), b = j.find('}', at);
      return j.substr(a, b - a);
    };
    vibeadmin::ConnLog log;
    // A limit ends the visit FIRST, by address and with no figures; the spectrum socket's own
    // close — the one holding the verdict — arrives after the row is already shut.
    log.open("7.7.7.7", "limited", "UA", "GB");
    log.close("7.7.7.7", "", "timeout");
    log.close("7.7.7.7", "limited", "closed", 5000000, 0, 3, 2, 21.0f, 0, 2400000);
    const std::string r1 = rowOf(log.json(), "limited");
    ok(r1.find("\"reason\":\"timeout\"") != std::string::npos
       && r1.find("\"heard\":2") != std::string::npos && r1.find("\"audio\":2400000") != std::string::npos
       && r1.find("\"bytes\":5000000") != std::string::npos,
       "★★★ a verdict arriving after a timeout closed the row is KEPT (reason stays 'timeout')");

    // The audio socket closes AFTER the spectrum socket ended the row.
    log.open("7.7.7.8", "tabshut", "UA", "GB");
    log.close("7.7.7.8", "tabshut", "closed", 100000, 0, 1, 1, 30.0f, 96600000.0, 12000);
    ok(log.noteAudio("tabshut", 3400000), "noteAudio on a CLOSED row says so (the bank can go)");
    ok(rowOf(log.json(), "tabshut").find("\"audio\":3400000") != std::string::npos,
       "★★★ audio delivered after the spectrum close still reaches the row");

    // Connected, took the spectrum, never opened audio.
    log.open("7.7.7.9", "silent", "UA", "GB");
    log.close("7.7.7.9", "silent", "closed", 80000, 0, 0, 0, 0.0f, 0, 0);
    ok(rowOf(log.json(), "silent").find("\"audio\":0") != std::string::npos,
       "★★ a visit that was sent no audio records audio:0 — known, and nothing");

    // Persisted and reloaded: the audio field survives the file.
    char tmpl[] = "/tmp/connlog-audio-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir) {
      const std::string path = std::string(dir) + "/c.jsonl";
      { vibeadmin::ConnLog a; a.setPath(path);
        a.open("6.6.6.6", "disk", "UA", "GB");
        a.close("6.6.6.6", "disk", "closed", 1, 0, 2, 1, 9.0f, 0, 777);
        a.saveIfDue(); }
      vibeadmin::ConnLog b; b.setPath(path);
      const std::string r = rowOf(b.json(), "disk");
      ok(r.find("\"audio\":777") != std::string::npos && r.find("\"heard\":1") != std::string::npos,
         "★ audio and verdict survive a save and reload");
    }
  }
  {
    /* ★★★ CLOUDFLARE'S WORKER ADDRESS IS NOT A VISITOR (Stuart, 2026-09-29). 2a06:98c0::/29 is the
     *  Workers egress — our directory proxying somebody's page — and must be labelled, never counted
     *  as a person or a country. WARP (2a09:bac0::/29, 104.28/16) is people and must NOT match. */
    using vibeadmin::cloudflareWorkerAddr;
    ok(cloudflareWorkerAddr("2a06:98c0:3600::103"), "★★★ the Worker egress address matches");
    ok(cloudflareWorkerAddr("2A06:98C7:ffff::1"), "the top of the /29, in capitals, matches");
    ok(!cloudflareWorkerAddr("2a06:98c8::1"), "just past the /29 does not");
    ok(!cloudflareWorkerAddr("2a09:bac0::1") && !cloudflareWorkerAddr("104.28.1.2"),
       "★★ WARP users (a real person) are NOT labelled Cloudflare");
    ok(!cloudflareWorkerAddr("81.159.1.2") && !cloudflareWorkerAddr("garbage"), "ordinary and junk input do not");
    vibeadmin::ConnLog log;
    log.open("2a06:98c0:3600::103", "cfrow", "Mozilla", "US");
    log.close("2a06:98c0:3600::103", "cfrow", "closed");
    log.open("81.159.1.2", "person", "Mozilla", "GB");
    log.close("81.159.1.2", "person", "closed");
    const std::string j = log.json();
    ok(j.find("\"cfw\":true") != std::string::npos && j.find("\"cfw\":true") == j.rfind("\"cfw\":true"),
       "★ the Cloudflare row, and only it, is flagged cfw");
    ok(log.uniqueSince(3600) == 1, "★★ the day's visitor count leaves it out");
    ok(log.topCountriesJson(3600).find("\"US\"") == std::string::npos,
       "★★ and so does the country chart — no US flag for Cloudflare");
  }
  std::printf(fails ? "\n\033[31m%d failed\033[0m\n" : "\n\033[32mpassed\033[0m\n", fails);
  return fails ? 1 : 0;
}
