// Offline test of Freeze play time's hold (src/playtime_rules.h): what each tick
// of a game makes of the clock it finds, through the game's own sequences.
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_playtime.cpp -o tests/build/test_playtime.exe
//   wine tests/build/test_playtime.exe
//
// The play time is the finished chapters' laps plus the clock's record time less
// this chapter's start; the hold keeps the lap by moving the start. A Continue
// after a death loads the checkpoint's save - its start the one the hold wrote
// then - while the game keeps the run's clock, so the lap it shows takes in the
// play since the checkpoint and the game over screen (2026-10-25: the reported
// leak; the hold took that lap afresh). Across such a load the play time held
// stays - also when the save starts a chapter and the game puts back the start
// the chapter had as play begins - while any other load brings its own time.
#include <cstdio>

#include "../src/playtime_rules.h"

namespace {

using namespace re4cc::cheats::rules;

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

constexpr uint64_t S = 1000000ull;  // a second

// The game, as far as the hold sees it: the clock's record time, this chapter's
// start (and the element it is in), the finished chapters' laps.
struct Game {
  uint64_t now = 0, start = 0, laps = 0;
  uintptr_t element = 1;
  PlayLook look() const { return {now, start, laps, element}; }
  uint64_t shown() const { return laps + (now > start ? now - start : 0); }  // what the game shows
};

// A save: what a load brings back of it.
struct Save {
  uint64_t now = 0, start = 0, laps = 0;
};
Save save_of(const Game& g) { return {g.now, g.start, g.laps}; }

// A load of `s`: the save's start and laps into a new element; the clock the
// save's - or the run's, kept.
void load(Game& g, const Save& s, bool kept) {
  if (!kept) g.now = s.now;
  g.start = s.start;
  g.laps = s.laps;
  ++g.element;
}

// One tick of the freeze (cheats.cpp's freeze_play_time): a look, and the start written.
bool g_looked = false;
PlayTook tick(PlayHold& h, Game& g) {
  uint64_t start = 0;
  PlayTook took = PlayTook::kOwn;
  g_looked = play_look(h, g.look(), &start, &took);
  if (!g_looked) return took;
  play_wrote(h, g.look(), start);
  g.start = start;
  return took;
}

// Play: `seconds` of the clock, a tick at each.
void play(PlayHold& h, Game& g, int seconds) {
  for (int i = 0; i < seconds; ++i) {
    g.now += S;
    tick(h, g);
  }
}

}  // namespace

int main() {
  // --- the hold in play ---
  // Chapter 3: 3000 s of finished laps, the chapter 600 s along - 1:00:00.
  Game g;
  g.laps = 3000 * S;
  g.start = 5000 * S;
  g.now = 5600 * S;
  PlayHold h;
  check(tick(h, g) == PlayTook::kGame && h.lap == 600 * S && g.start == 5000 * S && h.shown == 3600 * S,
        "the freeze switched on: the game's lap is held (1:00:00)");
  play(h, g, 60);
  check(g.shown() == 3600 * S && g.start == 5060 * S && h.shown == 3600 * S, "a minute of play: the start moves with the clock");

  // --- a Continue after a death ---
  // A checkpoint's save, two minutes more, a death and 20 s on the game over
  // screen (its clock runs; no tick: not in play).
  const Save checkpoint = save_of(g);
  play(h, g, 120);
  g.now += 20 * S;
  check(g.shown() == 3620 * S, "the game over screen: its 20 s run on (not in play)");
  {
    // As the hold was: every game start took the lap afresh.
    Game old = g;
    PlayHold oh = h;
    load(old, checkpoint, true);
    oh = PlayHold{};
    tick(oh, old);
    check(old.shown() == 3740 * S, "before the fix: the Continue's lap took in the 2 min since the checkpoint and the 20 s");
  }
  load(g, checkpoint, true);
  check(g.shown() == 3740 * S, "the Continue: the checkpoint's start, the run's clock - the game would show 1:02:20");
  play_started(h, true);
  check(tick(h, g) == PlayTook::kCarried && g.shown() == 3600 * S && h.shown == 3600 * S,
        "... the play time held stays 1:00:00");
  play(h, g, 30);
  check(g.shown() == 3600 * S, "... and is held on in play");

  // A second death, Continue from the same checkpoint.
  g.now += 15 * S;
  load(g, checkpoint, true);
  play_started(h, true);
  check(tick(h, g) == PlayTook::kCarried && g.shown() == 3600 * S, "a second Continue: still 1:00:00");

  // --- a save that starts a chapter (the take-over of the chapter's start) ---
  {
    Game t = g;
    PlayHold th = h;
    const uint64_t before = t.start;  // the start written last before the load
    t.now += 25 * S;
    // The chapter clear's save: a new element with no start yet, the run's clock.
    load(t, Save{0, 0, t.laps}, true);
    play_started(th, true);
    check(tick(th, t) == PlayTook::kCarried && t.shown() == 3600 * S, "a chapter's save: no start yet - the time held stays");
    t.now += S;
    t.start = before;  // as play begins, the game puts back the start the chapter had
    check(tick(th, t) == PlayTook::kOwn && t.shown() == 3600 * S, "... and when the game puts the chapter's start back");
    play(th, t, 10);
    check(t.shown() == 3600 * S, "... held on in play");
  }
  {
    // The game puts it back before the first look.
    Game t = g;
    PlayHold th = h;
    const uint64_t before = t.start;
    t.now += 25 * S;
    load(t, Save{0, before, t.laps}, true);
    play_started(th, true);
    check(tick(th, t) == PlayTook::kOwn && t.shown() == 3600 * S, "a chapter's save, its start back before the first look");
  }

  // --- other loads bring their own time ---
  {
    // Another save, loaded from a list (the clock its own): its time, 1:01:40,
    // whatever the hold had.
    Game t = g;
    PlayHold th = h;
    const Save other{7000 * S, 6400 * S, 3100 * S};
    load(t, other, false);
    play_started(th, false);
    check(tick(th, t) == PlayTook::kGame && t.shown() == 3700 * S && th.shown == 3700 * S, "a load of another save: its time");
    play(th, t, 5);
    check(t.shown() == 3700 * S, "... held from there");
  }
  {
    // A new game: a new clock.
    Game t;
    t.now = 40 * S;
    t.start = 10 * S;
    t.element = 99;
    PlayHold th = h;
    play_started(th, false);
    check(tick(th, t) == PlayTook::kGame && t.shown() == 30 * S, "a new game: its own time");
  }
  {
    // Freeze play time off across the death: nothing held, the game's own time.
    Game t = g;
    PlayHold th;
    t.now += 40 * S;
    load(t, checkpoint, true);
    play_started(th, true);
    check(tick(th, t) == PlayTook::kGame && th.lap == t.now - checkpoint.start, "the switch off: the game's own lap is taken");
  }

  // --- a new chapter in play ---
  {
    Game t = g;
    PlayHold th = h;
    play(th, t, 5);
    t.laps += t.now - t.start;  // the chapter's record, its lap the held one
    t.start = t.now;            // the next chapter starts now
    ++t.element;
    check(tick(th, t) == PlayTook::kGame && t.laps == 3600 * S && t.shown() == 3600 * S + kMinLap,
          "a new chapter: its record holds the lap, the new lap starts (never below a second)");
  }

  // --- the editor, then a Continue ---
  {
    Game t = g;
    PlayHold th = h;
    const Save cp = save_of(t);
    // The editor sets 2:00:00: this chapter's lap 4200 s.
    th.lap = 4200 * S;
    t.start = t.now - th.lap;
    play_wrote(th, t.look(), t.start);
    play(th, t, 30);
    t.now += 10 * S;
    load(t, cp, true);
    play_started(th, true);
    tick(th, t);
    check(t.shown() == 7200 * S, "the time set by the editor stays across a Continue to an earlier save");
  }
  {
    // A Continue whose save holds fewer finished laps: this chapter's lap takes the rest.
    Game t = g;
    PlayHold th = h;
    t.now += 30 * S;
    load(t, Save{0, t.start - 50 * S, t.laps - 100 * S}, true);
    play_started(th, true);
    check(tick(th, t) == PlayTook::kCarried && t.shown() == 3600 * S && th.lap == 700 * S,
          "a save with fewer laps: the time held stays, this chapter's lap the rest");
  }

  // --- what is not held ---
  {
    Game t = g;
    PlayHold th = h;
    t.now += 30 * S;
    load(t, checkpoint, true);
    play_started(th, true);
    const uint64_t at = t.start;
    t.start = t.now + S;  // no lap under way: the start above the clock
    tick(th, t);
    check(!g_looked && t.start == t.now + S && th.carried, "no lap under way: nothing written, the carry waits");
    t.start = at;
    check(tick(th, t) == PlayTook::kCarried && t.shown() == 3600 * S, "... and is taken at the next look");
  }
  {
    // A load that changed nothing the hold wrote (the copies differing from an earlier one).
    Game t = g;
    PlayHold th = h;
    play_started(th, true);
    check(tick(th, t) == PlayTook::kOwn && t.shown() == 3600 * S, "a start still the hold's own: kept");
  }

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
  return g_fail ? 1 : 0;
}
