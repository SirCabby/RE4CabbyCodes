#include "dispatch.h"

#include <cstdio>

#include "challenges.h"
#include "cheats.h"
#include "crash.h"
#include "exshop.h"
#include "game.h"
#include "inventory.h"
#include "log.h"
#include "overlay.h"
#include "re.h"
#include "savefiles.h"

namespace re4cc::dispatch {
namespace {

volatile HWND g_window = nullptr;
Snapshot g_snap;           // the tick's (under g_tick_cs)
Snapshot g_published;      // what the panel reads (under g_snap_cs)
DWORD g_published_time = 0;
CRITICAL_SECTION g_tick_cs;
CRITICAL_SECTION g_snap_cs;
volatile LONG g_updated = 0;  // UpdateBehavior ticked since the last EndRendering
thread_local bool t_in_tick = false;
DWORD g_last_second = 0;
unsigned long g_last_thread = 0;
int g_thread_logs = 0;
DWORD g_pointer_due = 0;  // when the next "into play" pointer line is due (0: none)
int g_pointer_lines = 0;
int g_list_logs = 0;  // the save and load lists coming up and closing, logged so far (at most 200)

// One line of what the pointer is (overlay.h), at the pause menu's open and
// close and a moment into play: what a report of a mouse that stopped working
// in play needs first.
void log_pointer(const char* when) {
  if (g_pointer_lines >= 200) return;
  ++g_pointer_lines;
  char ptr[512];
  overlay::pointer_report(ptr, sizeof(ptr));
  if (!ptr[0]) std::snprintf(ptr, sizeof(ptr), "the game's window is not known (the overlay is off or not up yet)");
  logf("pointer, %s: %s", when, ptr);
}

void tick() {
  const DWORD now = GetTickCount();
  const unsigned long me = GetCurrentThreadId();
  if (!g_snap.ticks) logf("dispatch: the game's frame ticks the mod (thread %lu)", me);
  if (me != g_last_thread) {
    if (g_last_thread && g_thread_logs < 5) {
      ++g_thread_logs;
      logf("dispatch: the tick moved from thread %lu to %lu%s", g_last_thread, me,
           g_thread_logs == 5 ? " (no more of these)" : "");
    }
    g_last_thread = me;
  }
  if (now - g_last_second >= 1000) {
    g_last_second = now;
    reassert_crash_filter();
  }

  Snapshot s;
  s.ticking = true;
  s.thread = me;
  s.ticks = g_snap.ticks + 1;
  s.game_ready = re::ready() && game::ready();
  if (s.game_ready) {
    // The one read every frame: where the game is. Everything else happens on
    // the game's events (events.h), in the holds the cheats keep while they are
    // on, or while the panel is up.
    s.run = game::run();
    const game::Run& was = g_snap.run;
    const int was_list = g_snap.save_list;
    if (s.run.in_game != was.in_game || s.run.mode != was.mode) {
      logf("game: %s%s%s (state %s)", s.run.in_game ? "playing " : "not playing",
           s.run.in_game ? game::mode_name(s.run.mode) : "", "", game::state_name(s.run.state));
      g_pointer_due = s.run.in_game ? (now + 2000) | 1 : 0;
    } else if (s.run.state != was.state && g_snap.ticks) {
      static int s_state_logs = 0;
      if (s_state_logs++ < 400) logf("game: state %s", game::state_name(s.run.state));
    }
    if (s.run.paused != was.paused) {
      logf("pause menu %s", s.run.paused ? "opened" : "closed");
      log_pointer(s.run.paused ? "the pause menu opened" : "the pause menu closed");
      g_pointer_due = s.run.paused ? 0 : (now + 2000) | 1;
    }
    if (s.run.case_scene != was.case_scene) logf("the attache case's screen %s", s.run.case_scene ? "is up" : "closed");
    if (g_pointer_due && s.run.in_game && !s.run.paused && now - g_pointer_due < 0x80000000u) {
      g_pointer_due = 0;
      log_pointer("2 s into play");
    }
    // The panel's requests, the cheats' holds, and the panel's figures while it is up.
    cheats::tick(s.run);
    inventory::tick(s.run);
    // The save files: their requests and the change under way while a save list is up.
    savefiles::tick();
    s.save_list = savefiles::screen_shown();
    if (s.save_list != was_list && g_list_logs < 200) {
      ++g_list_logs;
      logf("save files: %s %s%s", savefiles::screen_name(s.save_list >= 0 ? s.save_list : was_list),
           s.save_list >= 0 ? "is up" : "closed", g_list_logs == 200 ? " (no more of these)" : "");
    }
    // The challenges: their requests, the save, the window's figures while their screen is up.
    challenges::tick(s.run);
    s.challenges = s.run.challenges;
    // The Extra Content Shop: the same, while the shop is up.
    exshop::tick(s.run);
    s.shop = s.run.shop;
  }
  s.show_panel = s.run.paused || s.save_list >= 0 || s.challenges || s.shop;
  g_snap = s;
  EnterCriticalSection(&g_snap_cs);
  g_published = s;
  g_published_time = now;
  LeaveCriticalSection(&g_snap_cs);
}

}  // namespace

void init() {
  InitializeCriticalSection(&g_tick_cs);
  InitializeCriticalSection(&g_snap_cs);
}

void frame(int source) {
  // One tick a frame: UpdateBehavior's, or EndRendering's when no behaviour
  // update came since the last one.
  if (source == kEndRendering && InterlockedExchange(&g_updated, 0)) return;
  // A tick still running elsewhere keeps this one out rather than queueing it:
  // the entries are jobs, and a job must not wait on the mod.
  if (!TryEnterCriticalSection(&g_tick_cs)) return;
  if (t_in_tick) {  // re-entered from inside the tick (a call into the game that came back to the frame)
    LeaveCriticalSection(&g_tick_cs);
    return;
  }
  if (source == kUpdateBehavior) InterlockedExchange(&g_updated, 1);
  t_in_tick = true;
  tick();
  t_in_tick = false;
  LeaveCriticalSection(&g_tick_cs);
}

void set_game_window(HWND hwnd) {
  if (!hwnd || hwnd == g_window) return;
  g_window = hwnd;
  char cls[64] = "?";
  GetClassNameA(hwnd, cls, sizeof(cls));
  logf("dispatch: the game window is %p (class '%s', thread %lu)", static_cast<void*>(hwnd), cls,
       GetWindowThreadProcessId(hwnd, nullptr));
}

HWND game_window() { return g_window; }

Snapshot snapshot() {
  EnterCriticalSection(&g_snap_cs);
  Snapshot s = g_published;
  const DWORD at = g_published_time;
  LeaveCriticalSection(&g_snap_cs);
  s.age_ms = s.ticks ? GetTickCount() - at : 0;
  return s;
}

bool in_tick() { return t_in_tick; }

}  // namespace re4cc::dispatch
