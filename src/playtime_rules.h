#pragma once

#include <cstdint>

// Freeze play time's hold on plain values - no game memory here (cheats.cpp
// applies it each tick of a game; tests/test_playtime.cpp checks it).
//
// What the game shows and records is the finished chapters' laps plus this
// chapter's lap: the clock's record time less the chapter's start (game.h). The
// clock itself always runs (the countdowns and the HUD go by it), so the hold
// keeps this chapter's lap instead: each look puts the chapter's start that far
// behind the clock. A start the hold did not write - a new chapter, a load - is
// the game's, and its lap is what is held from then on.
//
// One kind of load does not take the play time back: Continue after a death,
// and a load list's load of the save Continue would load (the pause menu's, the
// game over screen's) - SaveLoadMenuGuiManager.<IsLastDataLoadContinueData> for
// both. It loads that save's chapter start, the one the hold wrote when the save
// was made, but the game keeps the run's clock (game.h, clock_kept), so the lap
// it shows takes in everything since that save: the play since the checkpoint
// and the game over screen, whose clock runs. Across such a load the play time
// held carries over: the first look keeps it (`carried`), and the start written
// last before the load is still the hold's own should the game put it back later
// (`before`: a save that starts a chapter takes over the start the chapter had,
// GameStatsManager.<ChapterStartTimeBackup>, as play begins).
namespace re4cc::cheats::rules {

// Never less than this: the game shows 999:59:59 for a lap that is not above 0,
// and the clock's parts are written at different moments of a frame.
constexpr uint64_t kMinLap = 1000000ull;  // a second, in the clock's microseconds

// The clock as one look sees it (game::Clock).
struct PlayLook {
  uint64_t now = 0;       // the record time
  uint64_t start = 0;     // this chapter's start
  uint64_t laps = 0;      // the finished chapters' laps
  uintptr_t element = 0;  // the start's element
};

struct PlayHold {
  bool have = false;
  uint64_t lap = 0;         // this chapter's lap, held
  uint64_t shown = 0;       // the play time held: the laps and the lap
  uintptr_t element = 0;    // the start's element written last
  uint64_t written = 0;     // and what was written
  bool carried = false;     // a load kept the run's clock: the next look keeps the play time held
  bool has_before = false;  // the start written last before that load, still the hold's own
  uint64_t before = 0;
};

// A game starting: the first look in play after the title, a load or a game
// over. `kept`: that load kept the run's clock (game::clock_kept). Any other
// start brings a clock and a lap of its own, taken afresh.
inline void play_started(PlayHold& h, bool kept) {
  if (!h.have || !kept) {
    h = PlayHold{};
    return;
  }
  h.carried = true;
  h.has_before = true;
  h.before = h.written;
}

// What a look made of the start it found: the hold's own, the game's (its lap
// held from now on), or one a load put there while the play time held carries over.
enum class PlayTook { kOwn, kGame, kCarried };

// One look: the start that holds the lap (*start; the look's own when it already
// does). False when there is none to hold: no lap under way (the game's own
// 999:59:59) or a clock too short for it.
inline bool play_look(PlayHold& h, const PlayLook& c, uint64_t* start, PlayTook* took) {
  if (c.start > c.now) return false;
  const bool own = h.have && ((c.element == h.element && c.start == h.written) || (h.has_before && c.start == h.before));
  if (h.carried) {
    // This chapter's lap is what the laps the load brought leave of the play
    // time held - the start never below the clock's own.
    h.carried = false;
    uint64_t lap = h.shown > c.laps ? h.shown - c.laps : 0;
    if (lap + kMinLap > c.now) lap = c.now > kMinLap ? c.now - kMinLap : 0;
    h.lap = lap;
    *took = own ? PlayTook::kOwn : PlayTook::kCarried;
  } else if (!own) {
    h.have = true;
    h.lap = c.now - c.start;
    h.has_before = false;
    *took = PlayTook::kGame;
  } else {
    *took = PlayTook::kOwn;
  }
  const uint64_t lap = h.lap < kMinLap ? kMinLap : h.lap;
  if (c.now < lap) return false;
  *start = c.now - lap;
  return true;
}

// The start went in (or was there already): the hold's own from now on.
inline void play_wrote(PlayHold& h, const PlayLook& c, uint64_t start) {
  h.element = c.element;
  h.written = start;
  h.shown = c.laps + (c.now - start);
}

}  // namespace re4cc::cheats::rules
