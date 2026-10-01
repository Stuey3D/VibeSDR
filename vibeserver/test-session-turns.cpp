// ★★★ WHOSE TURN IT IS, AND BORROWED TIME AFTER IT (vibe_session_turns.h) — on a synthetic clock.
//
// The fault (B10, Kiko's server, 2026-10-01): a listener whose 15-minute turn ended was told "try
// again in about 2 minutes", came back after the cooldown to a radio nobody was using — and was ended
// on the spot, because a listener back inside the turn break resumes the SAME turn and theirs had no
// time left. Every attempt re-armed the cooldown and refreshed the turn's `seen`, so the fresh turn
// never came while they kept trying. This pins the rule that replaced it: a spent turn is BORROWED
// time, kept until somebody else wants the radio, on a hard server as on a soft one.
// End to end through a real vibeserver: scripts/test-session-turn.mjs (slow — VIBESERVER_SLOW=1).
#include "vibe_session_turns.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
  std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
using namespace vibeturn;

int main() {
  std::printf("session turns — the turn book and borrowed time\n");
  const double LIMIT = 15 * 60, COOLDOWN = 120;

  {
    Book b; const std::string k = "browser-a";
    double t = 1000;
    const double s0 = b.startFor(k, t);
    ok(s0 == t && !b.spent(k, t), "a new listener starts a new, unspent turn");
    // Listens for the whole limit, touched every tick.
    for (double x = t; x <= t + LIMIT; x += 1) b.touch(k, x);
    t += LIMIT;
    ok(LIMIT - (t - s0) <= 0, "…and reaches the limit");
    b.markSpent(k, t);                                 // the server ends the turn
    ok(b.spent(k, t), "the ended turn is SPENT");

    // ★ A reload a few seconds later is still the same turn — the limit is not dodged by reloading.
    const double back = t + 5;
    const double s1 = b.startFor(k, back);
    ok(LIMIT - (back - s1) <= 0, "a reload inside the break does NOT buy a fresh clock (no time left)");
    ok(b.spent(k, back), "…and is on borrowed time");

    // ★★ After the cooldown, nobody waiting: the borrowed listener is KEPT, on a HARD server too.
    const double after = t + COOLDOWN + 1;
    b.startFor(k, after);
    const bool kept = keptUntilWanted(/*soft*/ false, b.spent(k, after));
    ok(kept, "★★ HARD server: back after the cooldown, a spent turn is kept until somebody wants the radio");
    ok(overLimit(kept, /*contended*/ false, 0, after) == OverAct::Keep,
       "★★ …so with nobody waiting the listener STAYS — the B10 fault ended them here");
    // Somebody arrives: notice, then the end.
    ok(overLimit(kept, true, 0, after) == OverAct::StartNotice, "somebody waiting: the handover notice starts");
    ok(overLimit(kept, true, after + 15, after + 5) == OverAct::Wait, "…and runs");
    ok(overLimit(kept, true, after + 15, after + 16) == OverAct::End, "…and ends the borrowed session when it runs out");
    ok(overLimit(kept, false, after + 15, after + 16) == OverAct::Withdraw,
       "…unless the waiter gave up first: the notice is withdrawn, nobody is evicted for nobody");
  }
  {
    // ★★ A HARD limit still ends a FIRST turn on the dot — borrowed time is not a loophole in it.
    Book b; const std::string k = "addr-b";
    b.startFor(k, 0);
    const bool kept = keptUntilWanted(false, b.spent(k, LIMIT));
    ok(!kept && overLimit(kept, false, 0, LIMIT) == OverAct::End,
       "★★ HARD server, first turn at its limit: ended now, even with nobody waiting");
    ok(keptUntilWanted(true, false), "SOFT server, first turn: kept until somebody wants the radio (unchanged)");
  }
  {
    // ★ The turn break: away longer than kTurnBreakS and a FULL turn comes back, unspent.
    Book b; const std::string k = "addr-c";
    b.startFor(k, 0);
    b.markSpent(k, LIMIT);
    const double later = LIMIT + kTurnBreakS + 1;
    const double s = b.startFor(k, later);
    ok(s == later && !b.spent(k, later), "away longer than the turn break: a FULL fresh turn, not borrowed");
    ok(kTurnBreakS == 900, "the break the server reports as `fresh` is 15 minutes, whatever the limit");
    // ★ Time away is not time used: back inside the break with time left, the clock resumes.
    Book c; c.startFor("x", 0); c.touch("x", 300);       // 5 minutes listened
    const double s2 = c.startFor("x", 300 + 600);        // 10 minutes away
    ok(LIMIT - ((300 + 600) - s2) == LIMIT - 300, "back inside the break: the turn resumes with the time away not charged");
  }
  {
    // ★ Pruning keeps the book bounded, and does not drop the key being asked about.
    Book b;
    for (int i = 0; i < 1000; i++) b.startFor("v" + std::to_string(i), i);
    b.startFor("late", 1000 + kTurnBreakS + 10);
    ok(b.turns.size() == 1, "visitors gone longer than the break are pruned (" + std::to_string(b.turns.size()) + " left)");
  }

  std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
