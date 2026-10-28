#pragma once

#include <windows.h>

#include "game.h"

// The game tick. RE Engine's window thread only blocks in GetMessageW, so the tick
// runs inside the game's frame instead: via.Application's UpdateBehavior entry
// - the phase that updates every behaviour - calls it first (app.cpp).
// EndRendering stands in for a frame whose behaviours did not update, so the
// panel still learns where the game is. The engine runs those entries as jobs
// on its thread pool, so the tick is not tied to one thread: it is serialised
// here, and never runs twice at once. Everything that touches game memory runs
// from it. The panel (drawn on the render thread) only flips atomics and posts
// requests; the tick applies them.
namespace re4cc::dispatch {

enum Source { kUpdateBehavior = 0, kEndRendering = 1 };

void init();                      // from DllMain: the locks
void frame(int source);           // from the application entries (app.cpp)
void set_game_window(HWND hwnd);  // the swap chain's window (overlay), or found by the mod thread
HWND game_window();

struct Snapshot {
  bool ticking = false;     // the game's frame calls the mod
  bool game_ready = false;  // the type database and the game's classes are known
  game::Run run;            // where the game is (game.h)
  int save_list = -1;       // savefiles::Screen the panel is beside - a load or save list (-1: none)
  bool challenges = false;  // the game's challenges screen is up (the challenges window)
  bool shop = false;        // the game's Extra Content Shop is up (the shop window)
  bool show_panel = false;  // the visibility rule, evaluated in the tick
  unsigned long thread = 0; // the thread the last tick ran on
  unsigned long long ticks = 0;
  unsigned long age_ms = 0; // since the last tick (filled in by snapshot())
};
Snapshot snapshot();
bool in_tick();  // the calling thread is inside the tick

}  // namespace re4cc::dispatch
