// ★★★ "ASK BEFORE TUNING" (vibe_tune_etiquette.h) — the rank order, the lock and both timers, on a synthetic clock.
//
// Stuart's spec (2026-10-21): on a shared dial with company, a newcomer asks in chat before tuning. A YES unlocks at
// once, no answer in 15 s unlocks, a NO holds for at most 5 minutes. Admin outranks everybody, then the
// longest-connected, but a listener past their time limit drops to the bottom; a reply counts only from above the
// asker, and the highest-ranked reply wins. The shim wiring is in local_sdr_shim.cpp (askRoomLocked / askGate).
#include "vibe_tune_etiquette.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
using namespace vibeask;

static Person P(const char* s, double since, bool admin = false, bool over = false, int h = 0) {
  Person p; p.session = s; p.since = since; p.admin = admin; p.overLimit = over; p.handle = h; return p;
}

int main() {
  std::printf("tune etiquette — ask before tuning\n");

  {
    Room r; r.inForce = true; r.people = { P("b", 200), P("a", 100), P("c", 300) };
    auto o = rankOrder(r.people);
    ok(o.size() == 3 && o[0] == "a" && o[1] == "b" && o[2] == "c", "longest-connected first");
    r.people.push_back(P("adm", 900, true));
    o = rankOrder(r.people);
    ok(o[0] == "adm", "the admin outranks everybody, however recently they arrived");
    r.people[1].overLimit = true;   // "a", the longest here, is past their time limit
    o = rankOrder(r.people);
    ok(o.back() == "a", "…and a listener past their time limit drops to the bottom");
    ok(o[1] == "b", "…so the next-longest moves up");
  }

  {
    Book b; Room r; r.inForce = false; r.people = { P("a", 100), P("b", 200) };
    ok(!b.locked(r, "b"), "switch off (or alone, or not a shared dial): nobody is locked");
    r.inForce = true;
    ok(!b.locked(r, "a"), "the longest-connected listener is never locked — nobody above them is here");
    ok(b.locked(r, "b"), "the newcomer is locked");
    ok(b.locked(r, "zzz"), "an unknown session is locked too (no id, no dial)");
    Room ra = r; ra.people.push_back(P("adm", 50, true));
    ok(!b.locked(ra, "adm"), "an admin is never locked");
    ok(b.locked(ra, "a"), "with the admin present, the longest-connected listener asks the admin");
    Room one; one.inForce = true; one.people = { P("b", 200) };
    ok(!b.locked(one, "b"), "the listener left alone at the top is free");
  }

  {   // ★ YES unlocks at once.
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200) };
    double t = 1000;
    ok(b.onSaid(r, "b", "ask_tune", t), "'Can I tune?' from a locked listener opens an ask");
    View v = b.view(r, "b", t + 1);
    ok(v.locked && v.state == "open" && v.secs == 14, "…which they are told, with the seconds left");
    ok(!b.onSaid(r, "b", "ask_tune", t + 2), "asking again does not restart the window");
    ok(b.onSaid(r, "a", "go_ahead", t + 3), "a YES from above…");
    ok(!b.locked(r, "b"), "…unlocks at once");
    ok(!b.view(r, "b", t + 3).locked, "…and the view says so");
  }

  {   // ★ No answer in 15 s unlocks.
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200) };
    double t = 1000;
    b.onSaid(r, "b", "anyone_using", t);
    ok(!b.tick(r, t + 14) && b.locked(r, "b"), "14 s: still waiting");
    ok(b.tick(r, t + 15) && !b.locked(r, "b"), "15 s and nobody answered: unlocked");
  }

  {   // ★ NO holds, then lapses — without asking again.
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200) };
    double t = 1000;
    b.onSaid(r, "b", "ask_tune", t);
    ok(b.onSaid(r, "a", "listening_few_min", t + 2), "'I'm listening to this one — a few more minutes, please?' is a NO");
    ok(b.view(r, "b", t + 3).state == "no", "…said at once, not after the 15 s window runs out");
    b.tick(r, t + 15);
    ok(b.locked(r, "b"), "after the window, the NO stands");
    View v = b.view(r, "b", t + 60);
    ok(v.state == "no" && v.secs == (int)kNoHoldS + 2 - 60, "…and the listener sees how long it lasts");
    ok(!b.onSaid(r, "b", "ask_tune", t + 70), "asking again during a NO does nothing (no second no to collect)");
    ok(!b.onSaid(r, "a", "please_hold", t + 80) && b.view(r, "b", t + 80).secs == (int)kNoHoldS + 2 - 80,
       "…and a second NO buys no extra time");
    b.tick(r, t + 2 + kNoHoldS - 1);
    ok(b.locked(r, "b"), "just before the limit: still locked");
    ok(b.tick(r, t + 2 + kNoHoldS) && !b.locked(r, "b"), "at the limit the NO lapses and the asker may tune — without asking again");
  }

  {   // ★ Only from above counts.
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200), P("c", 300) };
    double t = 1000;
    b.onSaid(r, "b", "ask_tune", t);
    ok(!b.onSaid(r, "c", "please_hold", t + 1), "a NO from BELOW the asker does not count");
    b.tick(r, t + 15);
    ok(!b.locked(r, "b"), "…so nobody answered, and the asker is let in at 15 s");
  }

  {   // ★ Conflicts: the highest-ranked wins.
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200), P("c", 300) };
    double t = 1000;
    b.onSaid(r, "c", "ask_tune", t);
    b.onSaid(r, "b", "go_ahead", t + 1);
    ok(!b.locked(r, "c"), "a YES from User b unlocks…");
    ok(b.onSaid(r, "a", "yes_hold", t + 3), "…but a NO from the longest-connected inside the window…");
    ok(b.locked(r, "c"), "…overrules it");
    ok(!b.onSaid(r, "b", "go_ahead", t + 5), "a lower-ranked YES cannot overrule the higher NO back");
    b.tick(r, t + 15);
    ok(b.locked(r, "c"), "the NO stands after the window");
    // The admin's answer always wins — even after the window.
    Room ra = r; ra.people.push_back(P("adm", 900, true));
    ok(b.onSaid(ra, "adm", "go_ahead", t + 30) && !b.locked(ra, "c"), "the ADMIN's YES overrules a standing NO");
  }

  {   // ★ The admin's NO beats everybody's YES.
    Book b; Room r; r.inForce = true; r.people = { P("adm", 500, true), P("a", 100), P("b", 200) };
    double t = 1000;
    b.onSaid(r, "b", "ask_tune", t);
    b.onSaid(r, "adm", "mid_decode", t + 1);
    ok(!b.onSaid(r, "a", "go_ahead", t + 2), "a listener's YES cannot overrule the admin's NO");
    b.tick(r, t + 15);
    ok(b.locked(r, "b"), "the admin's NO stands");
  }

  {   // ★ A NO lapses when its sayer goes, or drops below the asker (time limit).
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200) };
    double t = 1000;
    b.onSaid(r, "b", "ask_tune", t);
    b.onSaid(r, "a", "please_hold", t + 1);
    b.tick(r, t + 15);
    Room gone = r; gone.people = { P("b", 200), P("c", 300) };
    ok(b.tick(gone, t + 20) && !b.locked(gone, "b"), "the NO's sayer left: the asker may tune");
    Book b2; double u = 5000;
    b2.onSaid(r, "b", "ask_tune", u);
    b2.onSaid(r, "a", "please_hold", u + 1);
    b2.tick(r, u + 15);
    Room over = r; over.people[0].overLimit = true;   // "a" ran out of time
    ok(!b2.locked(over, "b"), "the NO's sayer ran out of time: they are below the asker now, so the asker is top");
    ok(b2.tick(over, u + 20), "…and the NO is cleared");
  }

  {   // ★ A grant survives a reconnect; a long absence forgets it.
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200) };
    double t = 1000;
    b.onSaid(r, "b", "ask_tune", t);
    b.onSaid(r, "a", "all_yours", t + 1);
    Room away = r; away.people = { P("a", 100) };
    b.tick(away, t + 30);
    ok(!b.locked(r, "b"), "back within the hold: still granted");
    b.tick(away, t + 30 + kGoneForgetS + 1);
    ok(b.locked(r, "b"), "away longer than the hold: a newcomer again");
  }

  {   // ★ The vocabulary.
    ok(isAsk("ask_tune") && isAsk("anyone_using") && !isAsk("thanks"), "the asks are 'Can I tune?' and 'Anyone using this?'");
    ok(replyMeaning("go_ahead") == Reply::Yes && replyMeaning("yes_go_ahead") == Reply::Yes
       && replyMeaning("all_yours") == Reply::Yes && replyMeaning("decode_done") == Reply::Yes, "the YES answers");
    ok(replyMeaning("please_hold") == Reply::No && replyMeaning("yes_hold") == Reply::No
       && replyMeaning("mid_decode") == Reply::No && replyMeaning("decoding_10min") == Reply::No
       && replyMeaning("listening_few_min") == Reply::No, "the NO answers");
    ok(replyMeaning("thanks") == Reply::None && replyMeaning("hello") == Reply::None, "everything else means nothing");
    Book b; Room r; r.inForce = true; r.people = { P("a", 100), P("b", 200) };
    ok(!b.onSaid(r, "a", "go_ahead", 1000), "a YES with nothing asked changes nothing");
    ok(b.locked(r, "b"), "…the newcomer still has to ask");
    ok(!b.onSaid(r, "a", "ask_tune", 1000), "an ask from somebody already free opens nothing");
  }

  std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
