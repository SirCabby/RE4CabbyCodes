#include "overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "app.h"
#include "call.h"
#include "challenges.h"
#include "cheats.h"
#include "config.h"
#include "dispatch.h"
#include "events.h"
#include "exshop.h"
#include "game.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "input.h"
#include "inventory.h"
#include "itembox.h"
#include "items.h"
#include "keyboard.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "savefiles.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace re4cc::overlay {
namespace {

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
volatile LONG g_visible = 0;
bool g_draw_cursor = false;  // the panel draws its own cursor (Wine, or Disable = cursor: the pointer section)
// The toggle key (under the context lock): beside one of the windows' screens -
// the pause menu, a save or load list, the challenges screen, the Extra Content
// Shop - it hides and shows that window; in play it opens the panel over the
// game; out of a game (the title, the main menu) there is no panel. Where the
// mod cannot see the game, it opens the panel to say why. Both are forgotten
// whenever any of that changes, so each window is back the next time its screen
// opens.
bool g_user_hidden = false;
bool g_forced = false;
int g_was_place = -1;  // Place at the last frame
int g_was_saves = -1;  // the save list the panel was beside at the last frame (savefiles::Screen, -1 none)
bool g_was_challenges = false;  // the challenges screen was up at the last frame
bool g_was_shop = false;        // the Extra Content Shop was up at the last frame
volatile LONG g_legacy_keys = 0;  // the window gets WM_KEYDOWN: without the panel's own keyboard the toggle comes from there
volatile LONG g_legacy_chars = 0; // the window gets WM_CHAR (the log: whether typing could come from messages at all)
DWORD g_polled_toggle_at = 0;     // the render thread took a press of the toggle key (no WM_KEYDOWN seen yet)
UINT g_wake_msg = 0;              // posted to the window when the panel comes or goes: the pointer is the window thread's

// The panel is drawn half as big again as ImGui's own sizes: ImGui's 13-pixel
// font is small at the resolutions the game runs at. Every size below that is
// written in pixels is at ImGui's own scale and goes through scaled().
constexpr int kScalePercent = 150;
constexpr float kScale = kScalePercent / 100.0f;
float scaled(float px) { return px * kScale; }

// Kept in the ImGui layout file ([RE4CabbyCodes][Panel]) beside the panel's
// position and size: which sections are folded away, and the scale that size
// was saved at (a size saved at another scale is brought to this one once).
bool g_cheats_open = true;
bool g_mercs_open = true;
bool g_time_open = true;
bool g_results_open = false;
bool g_case_open = true;
bool g_add_open = true;
bool g_box_open = true;
bool g_key_open = false;
bool g_key_add_open = false;
bool g_treasures_open = false;
bool g_treasures_add_open = false;
bool g_unique_open = false;
bool g_unique_add_open = false;
int g_layout_percent = kScalePercent;
bool g_layout_settled = false;

// The panel's four windows, each with its own place and size in the layout
// file: the pause panel, the save files beside the load and save lists, the
// challenges beside the challenges screen, and the shop's items beside the Extra
// Content Shop. Each opens expanded whenever its screen comes up. The version is
// only in their titles (after ### comes the name ImGui keeps them by), so a new
// version keeps the layout.
const char* const kPanelWindow = "Resident Evil 4 - Cabby Codes  v" RE4CC_VERSION "###RE4CC panel";
const char* const kFilesWindow = "Resident Evil 4 - Cabby Codes: save files  v" RE4CC_VERSION "###RE4CC save files";
const char* const kChallengesWindow = "Resident Evil 4 - Cabby Codes: challenges  v" RE4CC_VERSION "###RE4CC challenges";
const char* const kShopWindow = "Resident Evil 4 - Cabby Codes: Extra Content Shop  v" RE4CC_VERSION "###RE4CC shop";

void cursor_update(bool now);
void forget_dialogs();

// The pause menu, a save list, the challenges screen or the Extra Content Shop is
// up, as of a recent tick (a tick that stopped coming is not a pause menu).
bool showable(const dispatch::Snapshot& s) {
  return (s.ticking && s.age_ms < 2000 && s.show_panel) || config::get().always_show;
}

// Where the toggle key is pressed: beside a window's screen, in play, out of a
// game, or where the mod cannot tell (no tick, or the game not understood).
enum Place : int { kAtScreen = 0, kInPlay, kOutOfGame, kUnknown };
Place place(const dispatch::Snapshot& s) {
  if (showable(s)) return kAtScreen;
  if (!s.ticking || s.age_ms >= 2000 || !s.game_ready) return kUnknown;
  return s.run.in_game ? kInPlay : kOutOfGame;
}

void toggle(Place where, const char* how) {
  if (where == kAtScreen) {
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key%s", g_user_hidden ? "hidden" : "shown", how);
  } else if (where == kOutOfGame) {
    logf("the toggle key out of a game: no panel there - the pause menu, the save lists, the challenges screen and "
         "the Extra Content Shop have theirs%s", how);
  } else {
    g_forced = !g_forced;
    logf("panel %s by the toggle key outside the pause menu%s", g_forced ? "opened" : "closed", how);
  }
}

// The context lock (see overlay.h). Created in install_early(), from DllMain,
// before either path that takes it exists; never destroyed, because a frame
// can be in progress while the process tears down.
CRITICAL_SECTION g_imgui_cs;
bool g_imgui_cs_ready = false;

// What the panel is taking this frame, published for the input guard.
volatile LONG g_capture_mouse = 0;
volatile LONG g_capture_keyboard = 0;

void watch_message(UINT msg, WPARAM wp);

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_wake_msg && msg == g_wake_msg) {
    cursor_update(true);
    return 0;
  }
  cursor_update(false);
  watch_message(msg, wp);
  // F-keys above F9 arrive as WM_SYSKEYDOWN; accept both, ignore auto-repeat.
  if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
    const bool first = !InterlockedExchange(&g_legacy_keys, 1);
    if (first) logf("overlay: the game's window gets key messages (WM_KEYDOWN)");
    if (!(lp & (1 << 30)) && static_cast<int>(wp) == config::get().toggle_key) {
      // With the panel's own keyboard the toggle comes from there (wants_draw);
      // the key is kept from the game either way.
      if (!keyboard::active()) {
        ImGuiLock guard;  // wants_draw reads and clears these
        // The render thread polls the key until the window is seen to get key
        // messages; a press it already took is not taken twice.
        if (!first || GetTickCount() - g_polled_toggle_at > 500) toggle(place(dispatch::snapshot()), "");
      }
      return 0;
    }
  }
  if (msg == WM_CHAR && !InterlockedExchange(&g_legacy_chars, 1))
    logf("overlay: the game's window gets character messages (WM_CHAR)");
  // The game reads its mouse as Raw Input: a packet for the device the panel is
  // using goes no further. ImGui reads none of it, so these - as many as the
  // mouse's polling rate - never wait for the lock a frame of the panel holds:
  // what the panel takes is as its last frame published it.
  if (msg == WM_INPUT) {
    if (g_context_ready && g_visible && !config::get().disable_input) {
      const int type = input::raw_input_type(lp);
      if ((type == RIM_TYPEMOUSE && g_capture_mouse) || (type == RIM_TYPEKEYBOARD && g_capture_keyboard))
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
  }
  // ImGui sees every other message, visible or not: a button release that
  // arrives after the panel is hidden would otherwise never be delivered. Its
  // keys come from one place only: the panel's own keyboard (keyboard.h) when it
  // has one, these messages when it has not.
  if (g_context_ready) {
    bool swallow = false;
    {
      ImGuiLock guard;
      const bool key_message = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_UNICHAR;
      if (!(key_message && keyboard::active()) && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
      if (g_visible && !config::get().disable_input) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) swallow = true;
        if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) swallow = true;
      }
    }
    if (g_visible && msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && !g_draw_cursor) {
      // The game hides its cursor in the window; the panel needs an arrow.
      SetCursor(LoadCursorA(nullptr, reinterpret_cast<LPCSTR>(IDC_ARROW)));
      return TRUE;
    }
    if (swallow) return 0;
  }
  // The game's own window procedure runs outside the lock.
  return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

// --- the pointer -------------------------------------------------------------------------------
// The game hides its cursor in play and can hold the pointer in its window
// (ClipCursor, called every frame with the window's client rectangle). The
// panel needs a cursor. On Windows the game's is shown while the panel is up -
// ShowCursor(TRUE) until the display count is at 0, each call counted and taken
// back when the panel goes. Under Wine the panel draws its own (ImGui's) and the
// game's stays hidden: under gamescope a cursor shown ends the mouse look
// (Xwayland locks the pointer only while the X cursor is hidden, and gamescope's
// relative mouse needs that lock). The lock stays the game's: the panel is
// drawn inside the game's window, so the lock never keeps the pointer from it.
//
// One thing is done to the game's calls: a rectangle the pointer is already held
// to is not asked for again - the call returns TRUE, as Windows would, with
// nothing to change. Under Proton such a call is not nothing: each ClipCursor
// reaches the foreground window's thread as a message, and Wine keeps its
// fullscreen clipping (the pointer grab gamescope's mouse look needs) only if
// the thread was clipping when the message came - at a hundred calls a second,
// two of them merging lets the pointer go for good. The calls that change the
// clip - the first, a window moved or resized, a lock put back after a focus
// change - and every release (NULL) pass as before.
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
ClipCursorFn g_real_clip = nullptr;
uintptr_t g_clip_slot = 0;
CRITICAL_SECTION g_cursor_cs;
int g_cursor_shows = 0;

// What the log is told about the pointer: the game's lock calls (and how many of
// them asked for the clip already in place), raw mouse input reaching its window
// and its focus, counted between reports (pointer_report, from the tick at the
// pause menu's open and close).
std::atomic<long> g_clip_rect_calls{0}, g_clip_kept_calls{0}, g_clip_free_calls{0}, g_raw_mouse{0};
std::atomic<int> g_clip_last{-1};  // the game's last lock call: 1 a rectangle, 0 none (-1: no call yet)
RECT g_clip_last_rect{};           // under g_cursor_cs
std::atomic<int> g_focus_logs{0};

BOOL WINAPI hk_clip_cursor(const RECT* r) {
  RECT now{};
  const bool kept = r && GetClipCursor(&now) && EqualRect(&now, r);  // held there already: not asked again
  (r ? g_clip_rect_calls : g_clip_free_calls).fetch_add(1, std::memory_order_relaxed);
  if (kept) g_clip_kept_calls.fetch_add(1, std::memory_order_relaxed);
  const int kind = r ? 1 : 0;
  bool changed = false;
  RECT rect{};
  EnterCriticalSection(&g_cursor_cs);
  if (r) {
    changed = !EqualRect(r, &g_clip_last_rect);
    g_clip_last_rect = *r;
    rect = *r;
  }
  LeaveCriticalSection(&g_cursor_cs);
  changed = g_clip_last.exchange(kind, std::memory_order_relaxed) != kind || changed;
  static std::atomic<int> s_logs{0};
  if (changed && s_logs.fetch_add(1, std::memory_order_relaxed) < 40) {
    if (r) logf("pointer: the game locks the pointer to (%ld,%ld)-(%ld,%ld)", rect.left, rect.top, rect.right, rect.bottom);
    else logf("pointer: the game lets the pointer go");
  }
  return kept ? TRUE : g_real_clip(r);
}

// Messages the log is told about (the window's thread, every message): focus and
// activation, and a count of raw input.
void watch_message(UINT msg, WPARAM wp) {
  switch (msg) {
    case WM_INPUT:
      g_raw_mouse.fetch_add(1, std::memory_order_relaxed);
      return;
    case WM_ACTIVATEAPP:
    case WM_ACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
      if (g_focus_logs.fetch_add(1, std::memory_order_relaxed) >= 60) return;
      const char* what = msg == WM_ACTIVATEAPP ? (wp ? "the game is the active app" : "the game is no longer the active app")
                         : msg == WM_ACTIVATE  ? (LOWORD(wp) == WA_INACTIVE ? "the game's window is deactivated"
                                                                            : "the game's window is activated")
                         : msg == WM_SETFOCUS  ? "the game's window takes the keyboard focus"
                                               : "the game's window loses the keyboard focus";
      logf("focus: %s", what);
      return;
    }
    default:
      return;
  }
}

void show_cursor(bool show) {
  if (show) {
    int count = ShowCursor(TRUE) - 1;  // read the display count without changing it
    ShowCursor(FALSE);
    const bool first = g_cursor_shows == 0;
    while (count < 0 && g_cursor_shows < 64) {
      count = ShowCursor(TRUE);
      ++g_cursor_shows;
    }
    if (first && g_cursor_shows) logf("pointer: the cursor is shown while the panel is up (%d ShowCursor call(s))", g_cursor_shows);
  } else if (g_cursor_shows) {
    for (; g_cursor_shows > 0; --g_cursor_shows) ShowCursor(FALSE);
  }
}

// --- panel helpers ----------------------------------------------------------------------------------
const ImVec4 kOrange(1.0f, 0.6f, 0.3f, 1.0f);
const ImVec4 kBlue(0.45f, 0.75f, 1.0f, 1.0f);

// Keep the current window inside the display: moved back whenever any part of
// it has ended up outside (a saved position from a bigger window, a panel that
// grew).
void keep_in_reach(const ImVec2& display) {
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  ImVec2 want = pos;
  if (want.x + size.x > display.x) want.x = display.x - size.x;
  if (want.y + size.y > display.y) want.y = display.y - size.y;
  if (want.x < 0) want.x = 0;
  if (want.y < 0) want.y = 0;
  if (want.x != pos.x || want.y != pos.y) ImGui::SetWindowPos(want);
}

// The help for the item just drawn, on hover (a disabled one too).
void item_help(const char* help) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(380.0f));
    ImGui::TextUnformatted(help);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void help_marker(const char* help) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  item_help(help);
}

void cheat_row(cheats::Kind k, const char* label, const char* help) {
  const bool available = cheats::available(k);
  bool v = cheats::enabled(k);
  if (!available) ImGui::BeginDisabled();
  if (ImGui::Checkbox(label, &v)) {
    cheats::set_enabled(k, v);
    logf("%s %s (panel)", cheats::name(k), v ? "ON" : "OFF");
  }
  if (!available) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(380.0f));
    ImGui::TextUnformatted(help);
    if (!available) {
      ImGui::Separator();
      ImGui::TextUnformatted(cheats::why_not(k));
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!available) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", cheats::why_not(k));
  }
}

void hms(long long seconds, char* out, size_t n) {
  if (seconds < 0) {
    std::snprintf(out, n, "?");
    return;
  }
  std::snprintf(out, n, "%lld:%02lld:%02lld", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

// An amount with thousands separators.
void grouped(long long v, char* out, size_t n) {
  if (v < 0) std::snprintf(out, n, "?");
  else if (v >= 1000000) std::snprintf(out, n, "%lld,%03lld,%03lld", v / 1000000, (v / 1000) % 1000, v % 1000);
  else if (v >= 1000) std::snprintf(out, n, "%lld,%03lld", v / 1000, v % 1000);
  else std::snprintf(out, n, "%lld", v);
}

// An item as the panel names it: its name, or its number for one the table does
// not name.
const char* item_label(int32_t id, char* buf, size_t n) {
  const char* name = items::name(id);
  if (name[0]) return name;
  std::snprintf(buf, n, "item %d", id);
  return buf;
}

// Whether an item answers a search: every word typed is part of its name (case
// aside), or - a number - its item number. Nothing typed: every item.
bool item_matches(int32_t id, const char* name, const char* query) {
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* q = query ? query : ""; *q;) {
    while (*q == ' ') ++q;
    const char* word = q;
    while (*q && *q != ' ') ++q;
    const size_t n = static_cast<size_t>(q - word);
    if (!n) break;
    bool hit = false;
    for (const char* p = name; *p && !hit; ++p) {
      size_t i = 0;
      while (i < n && p[i] && lower(p[i]) == lower(word[i])) ++i;
      hit = i == n;
    }
    if (!hit) {
      long long num = 0;
      size_t i = 0;
      while (i < n && word[i] >= '0' && word[i] <= '9' && num < 10000000000LL) num = num * 10 + (word[i++] - '0');
      hit = i == n && num == id;
    }
    if (!hit) return false;
  }
  return true;
}

// The game an item belongs to (its catalog), for the Add lists: only what the
// game being played defines is offered.
bool in_this_game(const items::Def& d, game::Mode m) {
  switch (m) {
    case game::Mode::kMain: return (d.games & items::kMain) != 0;
    case game::Mode::kSeparateWays: return (d.games & items::kSeparateWays) != 0;
    case game::Mode::kMercenaries: return (d.games & items::kMercenaries) != 0;
    default: return false;
  }
}

// --- the cheats --------------------------------------------------------------------------
// Edits the panel is typing: kept until Set, then let go so the field follows the game again.
struct Edit {
  int v = 0;
  bool typing = false;
};

void int_editor(const char* id, Edit& e, int live, int lo, int hi, float width, bool* set) {
  if (!e.typing) e.v = live;
  ImGui::SetNextItemWidth(scaled(width));
  if (ImGui::InputInt(id, &e.v, 0, 0)) e.typing = true;
  if (e.v < lo) e.v = lo;
  if (e.v > hi) e.v = hi;
  *set = false;
  ImGui::SameLine();
  char b[48];
  std::snprintf(b, sizeof(b), "Set%s", id);
  if (ImGui::SmallButton(b)) {
    *set = true;
    e.typing = false;
  }
}

Edit g_armor, g_pesetas, g_spinels, g_tickets, g_saves, g_counters[game::kCounters], g_totals[game::kCounters];
int g_pt_h = 0, g_pt_m = 0, g_pt_s = 0;
bool g_pt_typing = false;
int g_cd_m = 0, g_cd_s = 0;
bool g_cd_typing = false;

void forget_edits() {
  g_armor.typing = g_pesetas.typing = g_spinels.typing = g_tickets.typing = g_saves.typing = false;
  for (Edit& e : g_counters) e.typing = false;
  for (Edit& e : g_totals) e.typing = false;
  g_pt_typing = g_cd_typing = false;
}

bool saves_game(game::Mode m) { return m == game::Mode::kMain || m == game::Mode::kSeparateWays; }

void draw_cheats(const cheats::Status& st) {
  const game::Run& run = st.run;
  const bool main_game = run.mode == game::Mode::kMain;
  ImGui::SeparatorText("Player");
  cheat_row(cheats::kGodMode, "God mode",
            "You take no damage: the game's own no-damage flag on your health, which its damage code checks before it "
            "takes any health off and before it decides a hit kills - so no hit, grab or fire takes health, and none "
            "kills. Your health is held at its maximum too, so what you had lost when you switch it on comes back. You "
            "still flinch and get knocked about, and grabs still play out. Luis, when he fights beside you, gets the "
            "same: no hit takes his health, and it is held at its maximum. In Separate Ways, the time Ada has to save "
            "him from the flames (the gauge that drains as if it were his health) is held full, so it never runs out. "
            "The water scooter of the escape at the end has a health of its own, which crashes take: it is held full "
            "and crashes take none of it (nor shake the view), so it never sinks - and it ends the ride undamaged, "
            "which the game rewards with the Smooth Escape challenge. Any other countdown on screen is Freeze countdown "
            "timer's. Deaths the game scripts without a hit - a fall, a missed QTE, a trap's own death scene - are "
            "left to it.");
  if (st.health_ok && st.health.max > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d / %d)", st.health.hp, st.health.max);
  }
  if (st.luis_ok && st.luis.max > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(Luis %d / %d)", st.luis.hp, st.luis.max);
  }
  if (st.luis_time_ok) {
    ImGui::SameLine();
    if (st.luis_time_held) {
      ImGui::TextDisabled("(time to save Luis held)");
    } else {
      const int left = static_cast<int>(st.luis_time_left + 0.5f);
      ImGui::TextDisabled("(%d:%02d to save Luis)", left / 60, left % 60);
    }
  }
  if (st.jet_ski_ok && st.jet_ski.max > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(water scooter %d / %d)", st.jet_ski.hp, st.jet_ski.max);
  }
  if (main_game) {
    cheat_row(cheats::kAshleyGod, "Ashley takes no damage",
              "The same for Ashley: no hit takes any of her health, and it is held at its maximum. A hit that does "
              "damage no longer puts her down either (she may still flinch). Being carried off is not damage - the "
              "game ends that its own way.");
    ImGui::SameLine();
    if (st.ashley_ok && st.ashley.max > 0) ImGui::TextDisabled("(%d / %d)", st.ashley.hp, st.ashley.max);
    else ImGui::TextDisabled("(not with you)");
  }
  cheat_row(cheats::kOneHitKills, "One hit kills",
            "Any hit that does damage takes all of an enemy's health, so the game's own code kills it - deaths, drops "
            "and parasites as usual. An enemy the game keeps alive for now (a boss between the parts of its fight, a "
            "scripted moment) keeps its last point, and the game decides. Bosses whose fights are scripted around "
            "their health may end sooner.");
  if (st.enemies > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d enemies about)", st.enemies);
  }
  cheat_row(cheats::kInfiniteAmmo, "Infinite ammo",
            "Firing uses no ammo: the gun in your hands keeps its loaded rounds, the bolt thrower and the crossbows "
            "their bolts and arrows, and a thrown grenade or egg stays in the case. Reloads and pickups work as "
            "usual.");
  cheat_row(cheats::kNoDurability, "No durability loss",
            "A knife never wears down: slashes, parries, stealth kills and breaking free of a grab take none of its "
            "durability.");
  cheat_row(cheats::kNoArmorWear, "No armor durability loss",
            "Your body armor never wears down: the hits it takes for you, and fire and frost, take none of its "
            "durability, so it never breaks and goes on protecting you as the game's armor does. Armor that is "
            "already broken stays broken until it is mended - by the merchant, or by giving it durability below.");
  if (st.armor_ok) {
    ImGui::SameLine();
    if (!st.armor.item) ImGui::TextDisabled("(you have no body armor)");
    else if (st.armor.broken) ImGui::TextColored(kOrange, "(broken)");
    else ImGui::TextDisabled("(%d / %d)", st.armor.durability, st.armor.max);
  }
  if (run.in_game && st.armor_ok && st.armor.item && st.armor.max > 0) {
    ImGui::BeginDisabled(!run.paused || run.case_scene);
    ImGui::Text("Armor durability:");
    ImGui::SameLine();
    bool set = false;
    int_editor("##armor", g_armor, st.armor.durability, 0, st.armor.max, 80.0f, &set);
    if (set) cheats::request_armor(g_armor.v);
    ImGui::EndDisabled();
    char help[160];
    std::snprintf(help, sizeof(help),
                  "Your body armor's durability, up to %d. Armor given some is mended if it was broken; 0 leaves it "
                  "broken.",
                  st.armor.max);
    item_help(help);
  }
  cheat_row(cheats::kSteadyAim, "Steady aim",
            "Your aim holds still. Firing no longer kicks or jolts the view, holding your aim (or looking down a "
            "scope) no longer sways it or your arms, and the reticle is always fully focused - the shots go where it "
            "points, the random spread of each shot taken out (a shotgun keeps its pattern of pellets, centred on the "
            "aim). "
            "While you aim, nothing else shakes the view either. Switching it off puts the spread back.");
  cheat_row(cheats::kAutoQte, "Automatic QTEs",
            "Every timed prompt is taken for you, as if you pressed the button at the right moment: the big dodge and "
            "knife prompts of enemies and bosses (El Gigante, Krauser's knife fight, Verdugo, Mendez, Saddler and the "
            "rest), the boulder, the laser corridors, Ada's hookshot prompt and the lever you pull with Ashley. A grab "
            "you can break free from is broken at once - by its button mashing, or with the knife where only the "
            "knife frees you (when you have one) - and the swim back to the boat fills up by itself. Ordinary parries "
            "(the small knife icon on an enemy's swing) are left to you. The prompts still flash up.");
  if (main_game) {
    cheat_row(cheats::kAutoGallery, "Automatic shooting gallery",
              "At the merchant's shooting gallery, the game plays itself: once a game starts, every target that "
              "scores is hit in the head as soon as it shows and every medal is taken - never a sailor. No shot is "
              "fired, so every bonus for few shots or no misses is met, and each game ends with the top rating and "
              "its rewards (the medals' token too). Shots you fire yourself still count.");
    if (st.gallery && st.gallery_score >= 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%d points)", st.gallery_score);
    }
  }
  if (run.mode != game::Mode::kMercenaries) {
    cheat_row(cheats::kCostumes, "Change costumes in game",
              "The pause menu's EXTRAS changes costumes too, not only accessories: Leon's and Ashley's, and Ada's in "
              "Separate Ways. It is the game's own costume list - the one the main menu offers - under the rule the game "
              "keeps for accessories: not during combat or a cutscene (and not while you are grabbed, on a ladder or in "
              "the boat). The new costume loads at once, cutscenes use it, and it stays for later loads and chapters. "
              "EXTRAS lists only what you have unlocked.");
  }
  if (saves_game(run.mode)) {
    ImGui::SeparatorText("Difficulty");
    int cur = st.difficulty;
    const char* names[game::kDifficulties];
    for (int i = 0; i < game::kDifficulties; ++i) names[i] = game::difficulty_name(i);
    ImGui::BeginDisabled(cur < 0 || !run.paused || !call::difficulty_ready());
    ImGui::SetNextItemWidth(scaled(160.0f));
    if (ImGui::Combo("##difficulty", &cur, names, game::kDifficulties) && cur != st.difficulty) {
      cheats::request_difficulty(cur);
      logf("difficulty: %s asked for (panel)", game::difficulty_name(cur));
    }
    ImGui::EndDisabled();
    item_help(
        "Switch the game you are playing between Assisted, Standard, Hardcore and Professional on the spot, with the "
        "game's own switch - the one a game over's offer to lower the difficulty makes. What the game reads as it "
        "goes follows at once: the damage you take and deal, the merchant's prices, Assisted's aim assist. What it "
        "set up when the area loaded stays until the next one. Your next save records it.");
    if (st.rank_ok) {
      ImGui::SameLine();
      ImGui::TextDisabled("(game rank %d%s)", st.rank.rank, st.rank.fixed ? ", held by the game" : "");
      item_help("The dynamic difficulty the game keeps on top of the difficulty you chose: it rises as you play well "
                "and falls as you take damage and die.");
    }
  }
  if (st.last_action[0]) {
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", st.last_action);
    ImGui::PopStyleColor();
  }
}

// The Mercenaries' own: shown only there.
void draw_mercs(const cheats::Status& st) {
  const game::Mercs& m = st.merc;
  char score[32];
  grouped(st.merc_ok ? m.score_value : -1, score, sizeof(score));
  if (st.merc_ok) ImGui::TextDisabled("%s points, %d kills, combo %d (best %d)", score, m.kills, m.combo_count, m.max_combo);
  cheat_row(cheats::kEndlessCombo, "Endless combo",
            "A combo never runs out of time: every kill counts as the next one in it however long since the last, so "
            "its bonus keeps climbing. The game puts what a combo has earned onto your score only once the combo is "
            "over - and loses it if you die first; with this on, it goes on as it is earned instead.");
  cheat_row(cheats::kMercMode, "Mercenary Mode always on",
            "Mercenary Mode's gauge is always full, so you can start it whenever you like, and once started it never "
            "runs out.");
  if (st.merc_ok && m.rush_busy) {
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "(in Mercenary Mode)");
  }
  cheat_row(cheats::kAutoKill, "Kill enemies as they appear",
            "Every enemy dies the moment it is in play, wherever it is (those already about when you switch it on "
            "too), to a shot of yours from afar: the game's own death, and the kill scores as yours do - its points, "
            "the combo, the kill count and the time a kill earns. The big ones (the chainsaw men, the Garradors, "
            "J.J. and the like) as well; a Ganado carrying a parasite dies before the parasite can come out, and one "
            "already out dies as soon as it is. Every part of the stage keeps sending enemies, wherever you stand. A "
            "stage holds 150 enemies in all, coming in waves as the kills mount, and it ends the moment the last one is "
            "down - with this on, a few minutes in. It waits for the clock to start and stops when time is up or you are "
            "down. It lasts one match: it switches itself off when the match ends, so switch it on again in the next. "
            "With Endless combo on too, every kill of the game adds to one combo.");
  if (st.kill_waiting) {
    ImGui::SameLine();
    ImGui::TextDisabled("(waiting for the clock)");
  } else if (st.kill_about >= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d enemies about)", st.kill_about);
  }
  cheat_row(cheats::kFreezeCountdown, "Freeze countdown timer",
            "The clock stops where it is, so the time never runs out. Time bonuses still add to it.");
  if (st.merc_ok && m.timer_busy) {
    const int left = static_cast<int>(m.remaining);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d:%02d left)", left / 60, left % 60);
  }
}

void draw_time_money(const cheats::Status& st) {
  const game::Run& run = st.run;
  cheat_row(cheats::kNoSaveCount, "Save without counting",
            "Saving does not add to the save count - the number the game's save lists show for each save, and the "
            "clear's rank counts. The save itself is made as usual.");
  if (run.in_game && st.saves >= 0) {
    ImGui::BeginDisabled(!run.paused);
    ImGui::Text("Saves: %d", st.saves);
    ImGui::SameLine();
    bool set = false;
    int_editor("##saves", g_saves, st.saves, 0, game::kSavesMax, 90.0f, &set);
    if (set) cheats::request_saves(g_saves.v);
    ImGui::EndDisabled();
    item_help("The saves made in this game so far. Your next save records it (plus one, unless Save without counting "
              "is on); loading a save brings back the count that save was made with.");
  }
  {
    char t[32];
    hms(st.play_seconds, t, sizeof(t));
    cheat_row(cheats::kFreezePlaytime, "Freeze play time",
              "The play time stops where it is - the time the pause menu, your saves and the results show and the "
              "clear's rank goes by - and so does the RESULTS screen's total play time; both run on from there the "
              "moment you switch this off. The game's own clock keeps running underneath (its countdowns go by it). "
              "Loading a save brings back that save's own time - but Continue after a death keeps the time where it "
              "stopped, and so does loading your newest save while you play (the game treats that as a Continue).");
    if (run.in_game) {
      ImGui::BeginDisabled(!st.clock_ok || !run.paused);
      ImGui::Text("Play time: %s%s", t, st.play_held ? " (frozen)" : "");
      if (!g_pt_typing && st.play_seconds >= 0) {
        g_pt_h = static_cast<int>(st.play_seconds / 3600);
        g_pt_m = static_cast<int>((st.play_seconds / 60) % 60);
        g_pt_s = static_cast<int>(st.play_seconds % 60);
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(scaled(50.0f));
      if (ImGui::InputInt("h##pt", &g_pt_h, 0, 0)) g_pt_typing = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(scaled(40.0f));
      if (ImGui::InputInt("m##pt", &g_pt_m, 0, 0)) g_pt_typing = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(scaled(40.0f));
      if (ImGui::InputInt("s##pt", &g_pt_s, 0, 0)) g_pt_typing = true;
      ImGui::SameLine();
      if (ImGui::SmallButton("Set##pt")) {
        g_pt_h = std::clamp(g_pt_h, 0, 999);
        g_pt_m = std::clamp(g_pt_m, 0, 59);
        g_pt_s = std::clamp(g_pt_s, 0, 59);
        cheats::request_playtime(g_pt_h * 3600 + g_pt_m * 60 + g_pt_s);
        g_pt_typing = false;
      }
      ImGui::EndDisabled();
      item_help("The play time the game shows and records, up to its own limit of 999:59:59. It counts on from what "
                "you set (or stays there, frozen). A time lower than the chapters you have finished took is taken off "
                "their own times, the latest first. The RESULTS screen's total play time is set to it too. If the "
                "pause menu's own clock still shows the old time, close the menu and open it again.");
      if (st.results_seconds >= 0) {
        char r[32];
        hms(st.results_seconds, r, sizeof(r));
        ImGui::TextDisabled("RESULTS screen's total play time: %s%s", r, st.results_held ? " (frozen)" : "");
        help_marker("The RESULTS screen (the pause menu's Chapter Results, the main menu's RESULTS) shows a total of "
                    "its own: every moment the game has run - all your saves together, the menus, cutscenes and "
                    "pauses too - kept by the game itself, not in a save. So it grows faster than the play time. "
                    "Setting the play time sets it to the same; Freeze play time holds it.");
      }
    }
  }
  {
    const game::Countdown& cd = st.countdown;
    cheat_row(cheats::kFreezeCountdown, "Freeze countdown timer",
              "A countdown on screen stops where it is, so the time never runs out, and runs on again the moment you "
              "switch this off.");
    if (st.countdown_ok && cd.running) {
      const int left = static_cast<int>(cd.seconds);
      ImGui::SameLine();
      if (st.countdown_held) ImGui::TextColored(kOrange, "(%d:%02d left, frozen)", left / 60, left % 60);
      else ImGui::TextDisabled("(%d:%02d left)", left / 60, left % 60);
      if (!g_cd_typing) {
        g_cd_m = left / 60;
        g_cd_s = left % 60;
      }
      ImGui::BeginDisabled(!run.paused);
      ImGui::Text("Countdown:");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(scaled(50.0f));
      if (ImGui::InputInt("m##cd", &g_cd_m, 0, 0)) g_cd_typing = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(scaled(40.0f));
      if (ImGui::InputInt("s##cd", &g_cd_s, 0, 0)) g_cd_typing = true;
      ImGui::SameLine();
      if (ImGui::SmallButton("Set##cd")) {
        g_cd_m = std::clamp(g_cd_m, 0, 99);
        g_cd_s = std::clamp(g_cd_s, 0, 59);
        cheats::request_countdown(g_cd_m * 60 + g_cd_s);
        g_cd_typing = false;
      }
      ImGui::EndDisabled();
      item_help("What is left of the countdown running now. It counts down from what you set once you leave the "
                "pause.");
    }
  }
  cheat_row(cheats::kInfiniteMoney, "Infinite money",
            "The merchant takes no pesetas: buying from him, tuning up a weapon and repairing a knife leave your money "
            "as it was, and selling to him still adds to it. He still only sells what your pesetas cover - set them "
            "below for something dearer.");
  if (run.in_game && st.pesetas >= 0) {
    char p[32];
    grouped(st.pesetas, p, sizeof(p));
    ImGui::BeginDisabled(!run.paused);
    ImGui::Text("Pesetas: %s", p);
    ImGui::SameLine();
    bool set = false;
    int_editor("##ptas", g_pesetas, st.pesetas, 0, game::kPesetasMax, 130.0f, &set);
    if (set) cheats::request_pesetas(g_pesetas.v);
    ImGui::EndDisabled();
    item_help("The money the merchant takes, up to 999,999,999.");
  }
  cheat_row(cheats::kInfiniteSpinels, "Infinite spinels",
            "The merchant takes no spinels: trading for his special stock leaves your spinels as they were, and the "
            "spinels you find still add to them. He still only trades what your spinels cover - set them below for "
            "something dearer.");
  if (run.in_game && st.spinels >= 0) {
    ImGui::BeginDisabled(!run.paused);
    ImGui::Text("Spinels: %d", st.spinels);
    ImGui::SameLine();
    bool set = false;
    int_editor("##spinels", g_spinels, st.spinels, 0, game::kSpinelsMax, 90.0f, &set);
    if (set) cheats::request_spinels(g_spinels.v);
    ImGui::EndDisabled();
    item_help("The spinels the merchant trades for his special stock, up to 9,999.");
  }
  cheat_row(cheats::kInfiniteTickets, "Infinite upgrade tickets",
            "An exclusive upgrade paid with an Exclusive Upgrade Ticket keeps the ticket: the merchant makes the "
            "upgrade and your tickets stay as they were, so one ticket does every weapon's. You need one to start "
            "with - trade spinels for it, or set them below. A ticket bought as DLC is still used up.");
  if (run.in_game && st.tickets >= 0) {
    const items::Def* d = items::find(game::upgrade_ticket_id());
    const int most = d && d->stack > 0 ? d->stack : 1;
    ImGui::BeginDisabled(!run.paused);
    ImGui::Text("Tickets: %d", st.tickets);
    ImGui::SameLine();
    bool set = false;
    int_editor("##tickets", g_tickets, st.tickets, 0, most, 90.0f, &set);
    if (set) cheats::request_tickets(g_tickets.v);
    ImGui::EndDisabled();
    char help[200];
    std::snprintf(help, sizeof(help),
                  "The Exclusive Upgrade Tickets in your treasures, up to %d - the merchant takes one for an exclusive "
                  "upgrade paid with a ticket. 0 takes them all out.",
                  most);
    item_help(help);
  }
}

bool is_results_switch(cheats::Kind k) {
  return k == cheats::kNoMissCount || k == cheats::kNoDeathCount || k == cheats::kNoDamageCount;
}
bool is_merc_switch(cheats::Kind k) {
  return k == cheats::kEndlessCombo || k == cheats::kMercMode || k == cheats::kAutoKill;
}
bool is_time_switch(cheats::Kind k) {
  return k == cheats::kNoSaveCount || k == cheats::kFreezePlaytime || k == cheats::kFreezeCountdown ||
         k == cheats::kInfiniteMoney || k == cheats::kInfiniteSpinels || k == cheats::kInfiniteTickets;
}

void draw_results(const cheats::Status& st) {
  cheat_row(cheats::kNoMissCount, "Misses don't count",
            "A shot that misses is not counted: only a shot that hits adds to the shots fired, so the misses stay as "
            "they are and the hit ratio climbs toward 100%. Hits are counted as usual.");
  cheat_row(cheats::kNoDeathCount, "Deaths don't count",
            "Dying and continuing adds nothing to the deaths - this chapter's or the whole game's.");
  cheat_row(cheats::kNoDamageCount, "Damage doesn't count",
            "Getting hurt adds nothing to the times damaged - this chapter's or the whole game's. The hit itself "
            "still hurts: God mode is what keeps your health up.");
  if (!st.stats_ok) return;
  const game::Stats& s = st.stats;
  static const char* const kNames[game::kCounters] = {"Deaths", "Times damaged", "Enemies killed", "Hits", "Misses"};
  ImGui::BeginDisabled(!st.run.paused);
  if (ImGui::BeginTable("results", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("this chapter");
    ImGui::TableSetupColumn("the whole game");
    ImGui::TableHeadersRow();
    for (int i = 0; i < game::kCounters; ++i) {
      if (s.chapter[i] < 0) continue;  // not found (the times damaged: see the log)
      const auto c = static_cast<game::Counter>(i);
      const int max = game::counter_max(c);
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(kNames[i]);
      bool set = false;
      ImGui::TableNextColumn();
      int_editor("##c", g_counters[i], s.chapter[i], 0, max, 90.0f, &set);
      if (set) cheats::request_counter(c, g_counters[i].v);
      ImGui::TableNextColumn();
      int_editor("##t", g_totals[i], s.total[i], 0, max * (s.finished + 1), 90.0f, &set);
      if (set) cheats::request_total(c, g_totals[i].v);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();
  const int hits = static_cast<int>(game::Counter::kHits), misses = static_cast<int>(game::Counter::kMisses);
  const long long shots_c = static_cast<long long>(s.chapter[hits]) + s.chapter[misses];
  const long long shots_t = static_cast<long long>(s.total[hits]) + s.total[misses];
  ImGui::TextDisabled("Shots fired %lld, hit ratio %lld%% this chapter; %lld and %lld%% in all", shots_c,
                      shots_c ? s.chapter[hits] * 100LL / shots_c : 100, shots_t, shots_t ? s.total[hits] * 100LL / shots_t : 100);
  help_marker("What the results screen at the end of each chapter shows: this chapter's deaths, times damaged, enemies "
              "killed and hit ratio (hits of the shots fired) - and, added up with the chapters you have finished, the "
              "whole game's. Set either one: a whole game's count changes this chapter's, and one lower than the "
              "finished chapters hold takes the latest of them down. Your saves keep them.");
}

// --- the attache case, the item box and the lists off the case ------------------------------------------
// An item of the lists, as the panel keeps what is typed into its row and whether
// it is ticked: its list, the item and its Guid - so both stay with the item as
// the rows around it come and go.
struct ItemKey {
  int kind = -1;
  int32_t id = 0;
  uint8_t guid[16] = {};
};
ItemKey key_of(const inventory::Row& r) {
  ItemKey k;
  k.kind = r.kind;
  k.id = r.v.id;
  std::memcpy(k.guid, r.v.guid, 16);
  return k;
}
bool same_key(const ItemKey& a, const ItemKey& b) {
  return a.kind == b.kind && a.id == b.id && game::guid_equal(a.guid, b.guid);
}
// An entry of the item box's own part, whatever its count: everything of one item
// is one entry, a weapon one of its own with its state.
bool same_entry(const itembox::Entry& a, const itembox::Entry& b) {
  return a.id == b.id && a.weapon == b.weapon && (!a.weapon || itembox::same(a, b));
}

// A row being edited: what is typed into it lives here until Apply (or Apply
// all) or Undo, and is let go when the panel goes, so a reopened panel shows the
// lists as they are.
struct RowEdit {
  ItemKey key;
  int num = 0;
  int ammo = 0;
  int durability = 0;
  int levels[16] = {};
};
std::vector<RowEdit> g_edits;
// A count typed for an entry of the item box's own part (by entry), and for a
// stack of the game's storage - the First Aid Spray (by its item).
struct BoxEdit {
  itembox::Entry entry;
  int num = 0;
};
std::vector<BoxEdit> g_box_edits;
struct StoredEdit {
  ItemKey key;
  int num = 0;
};
std::vector<StoredEdit> g_stored_edits;

// The rows ticked for a change of many at once - Store, Take, Throw away - by
// item (the game's storage's too), and by entry for the box's own part. Let go
// when the panel goes.
std::vector<ItemKey> g_ticks;
std::vector<itembox::Entry> g_box_ticks;

RowEdit* edit_of(const inventory::Row& r) {
  const ItemKey k = key_of(r);
  for (RowEdit& e : g_edits)
    if (same_key(e.key, k)) return &e;
  return nullptr;
}
void drop_edit(const inventory::Row& r) {
  const ItemKey k = key_of(r);
  std::erase_if(g_edits, [&](const RowEdit& e) { return same_key(e.key, k); });
}
BoxEdit* box_edit_of(const itembox::Entry& e) {
  for (BoxEdit& b : g_box_edits)
    if (same_entry(b.entry, e)) return &b;
  return nullptr;
}
void drop_box_edit(const itembox::Entry& e) {
  std::erase_if(g_box_edits, [&](const BoxEdit& b) { return same_entry(b.entry, e); });
}
StoredEdit* stored_edit_of(const inventory::Row& r) {
  const ItemKey k = key_of(r);
  for (StoredEdit& s : g_stored_edits)
    if (same_key(s.key, k)) return &s;
  return nullptr;
}
void drop_stored_edit(const inventory::Row& r) {
  const ItemKey k = key_of(r);
  std::erase_if(g_stored_edits, [&](const StoredEdit& s) { return same_key(s.key, k); });
}

bool ticked(const inventory::Row& r) {
  const ItemKey k = key_of(r);
  for (const ItemKey& t : g_ticks)
    if (same_key(t, k)) return true;
  return false;
}
void set_tick(const inventory::Row& r, bool on) {
  const ItemKey k = key_of(r);
  std::erase_if(g_ticks, [&](const ItemKey& t) { return same_key(t, k); });
  if (on) g_ticks.push_back(k);
}
bool box_ticked(const itembox::Entry& e) {
  for (const itembox::Entry& t : g_box_ticks)
    if (same_entry(t, e)) return true;
  return false;
}
void set_box_tick(const itembox::Entry& e, bool on) {
  std::erase_if(g_box_ticks, [&](const itembox::Entry& t) { return same_entry(t, e); });
  if (on) g_box_ticks.push_back(e);
}

// What is ticked or typed for an item that is not in the lists any more is let
// go: for the lists as a fresh view of them has them, and for the box's own part.
void forget_gone(const inventory::View& v) {
  auto there = [&](const ItemKey& k) {
    for (int i = 0; i < v.nrows; ++i)
      if (same_key(key_of(v.row[i]), k)) return true;
    return false;
  };
  std::erase_if(g_ticks, [&](const ItemKey& k) { return !there(k); });
  std::erase_if(g_edits, [&](const RowEdit& e) { return !there(e.key); });
  std::erase_if(g_stored_edits, [&](const StoredEdit& s) { return !there(s.key); });
}
void forget_gone_box(const itembox::View& box) {
  auto there = [&](const itembox::Entry& e) {
    for (const itembox::Entry& x : box.entry)
      if (same_entry(x, e)) return true;
    return false;
  };
  std::erase_if(g_box_ticks, [&](const itembox::Entry& e) { return !there(e); });
  std::erase_if(g_box_edits, [&](const BoxEdit& b) { return !there(b.entry); });
}

// The confirmations.
enum class Ask { kNone, kApply, kRemove, kBoxCount, kDiscardBox, kCaseSize, kStoredCount, kDiscardStored, kImport, kBatch };
Ask g_ask = Ask::kNone;
inventory::Row g_ask_row;
RowEdit g_ask_edit;
int g_ask_box = -1;
itembox::Entry g_ask_entry;
int g_ask_num = 0;    // the count a box entry's Apply asks about, the size a case's asks about
// A change of many items, asked about once: the question, a line for each item,
// what it says under them, its button, and what the button does.
struct BatchAsk {
  char question[96] = {};
  std::vector<std::string> lines;
  char note[240] = {};
  char button[40] = {};
  std::function<void()> go;
};
BatchAsk g_batch;

// Where the last changes were asked for from (a list's line, an Add list): how
// far the game has got with them shows there, and once they are done what went
// wrong, if anything did. The view's generation they were asked at: an older
// view does not know of them yet.
const void* g_change_place = nullptr;
unsigned g_change_gen = 0;
unsigned g_view_gen = 0;      // the generation of the view the panel draws
bool g_note_placed = false;   // the note was drawn at its place this frame
const int g_list_place[inventory::kKinds] = {};  // (their addresses: the places of the lists' lines)

void changes_from(const void* place) {
  g_change_place = place;
  g_change_gen = inventory::view_generation();
}

void place_note(const inventory::View& v, const void* place) {
  if (!place || place != g_change_place) return;
  if (g_view_gen <= g_change_gen || v.waiting > 0) {
    if (v.waiting > 0) ImGui::TextDisabled("Working on it - %d to go...", v.waiting);
    else ImGui::TextDisabled("Working on it...");
  } else if (v.note_error && v.note[0]) {
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", v.note);
    ImGui::PopStyleColor();
    g_note_placed = true;
  }
}

void forget_item_dialogs() {
  g_edits.clear();
  g_box_edits.clear();
  g_stored_edits.clear();
  g_ticks.clear();
  g_box_ticks.clear();
  g_ask = Ask::kNone;
  g_batch = BatchAsk{};
  g_change_place = nullptr;
}

// The upgrades the panel shows: a weapon's levels with a name (game.h level_name).
bool shown_level(const game::Levels::Entry& e) { return game::level_name(e.list, e.category) != nullptr; }
const char* level_short(const game::Levels::Entry& e) {
  if (e.list == 2) return "Ex";
  if (e.list == 0) return e.category == 0 ? "P" : "C";
  return e.category == 7 ? "R" : e.category == 8 ? "F" : "D";
}

// A weapon's state in a line: "12 rounds, P3 C2 R1 F1", "durability 800, P2 D1".
void weapon_state(const inventory::Row& r, char* out, size_t n) {
  size_t k = static_cast<size_t>(r.gun ? std::snprintf(out, n, "%d round%s", r.v.ammo, r.v.ammo == 1 ? "" : "s")
                                       : std::snprintf(out, n, "durability %d", r.v.durability));
  bool first = true;
  for (int i = 0; i < r.levels.n && k < n; ++i) {
    const game::Levels::Entry& e = r.levels.e[i];
    if (!shown_level(e) || (e.list == 2 && e.level <= 0)) continue;
    const char* sep = first ? ", " : " ";
    first = false;
    if (e.list == 2) k += static_cast<size_t>(std::snprintf(out + k, n - k, "%sexclusive", sep));
    else k += static_cast<size_t>(std::snprintf(out + k, n - k, "%s%s%d", sep, level_short(e), e.level + 1));
  }
}

// An item in a line, for the confirmations: "Handgun Ammo x30", "Red9 (12 rounds,
// P3)", "Body Armor".
std::string item_line(const inventory::Row& r) {
  char lb[32], state[200], line[300];
  const char* name = item_label(r.v.id, lb, sizeof(lb));
  const int n = r.kind == inventory::kStorage ? r.v.stored : r.v.count;
  if (r.weapon && (r.kind == inventory::kCase || r.kind == inventory::kStorage)) {
    weapon_state(r, state, sizeof(state));
    std::snprintf(line, sizeof(line), "%s (%s)", name, state);
  } else if (n > 1) {
    std::snprintf(line, sizeof(line), "%s x%d", name, n);
  } else {
    std::snprintf(line, sizeof(line), "%s", name);
  }
  return line;
}
std::string entry_line(const itembox::Entry& e) {
  char lb[32], state[160], line[260];
  const char* name = item_label(e.id, lb, sizeof(lb));
  if (e.weapon) {
    itembox::describe(e, state, sizeof(state));
    std::snprintf(line, sizeof(line), "%s (%s)", name, state);
  } else {
    std::snprintf(line, sizeof(line), "%s x%d", name, e.num);
  }
  return line;
}

// A row's fields as the game has them: what an edit starts from.
RowEdit live_edit(const inventory::Row& r) {
  RowEdit e;
  e.key = key_of(r);
  e.num = r.v.count;
  e.ammo = r.v.ammo;
  e.durability = r.v.durability;
  for (int i = 0; i < r.levels.n && i < 16; ++i) e.levels[i] = r.levels.e[i].level;
  return e;
}
// A weapon in the case is edited for its state, anything else for its count.
bool edits_weapon(const inventory::Row& r) { return r.weapon && r.kind == inventory::kCase; }
// Whether an edit changes its row: what its Apply would set.
bool differs(const inventory::Row& r, const RowEdit& e) {
  if (edits_weapon(r)) {
    if (r.gun && e.ammo != r.v.ammo) return true;
    if (r.knife && e.durability != r.v.durability) return true;
    for (int i = 0; i < r.levels.n && i < 16; ++i)
      if (shown_level(r.levels.e[i]) && e.levels[i] != r.levels.e[i].level) return true;
    return false;
  }
  return r.stack > 1 && e.num != r.v.count;
}
// An edit as the game is asked for it, and in a line: "Handgun Ammo: 30 -> 50",
// "Red9: 12 rounds, P3 -> 18 rounds, P5".
void request_edit(const inventory::Row& r, const RowEdit& e) {
  if (edits_weapon(r)) inventory::request_weapon(r, r.gun ? e.ammo : -1, r.knife ? e.durability : -1, e.levels);
  else inventory::request_count(r, e.num);
}
std::string change_line(const inventory::Row& r, const RowEdit& e) {
  char lb[32], was[200], now[200], line[460];
  const char* name = item_label(r.v.id, lb, sizeof(lb));
  if (edits_weapon(r)) {
    inventory::Row shown = r;
    shown.v.ammo = e.ammo;
    shown.v.durability = e.durability;
    for (int i = 0; i < r.levels.n && i < 16; ++i) shown.levels.e[i].level = e.levels[i];
    weapon_state(r, was, sizeof(was));
    weapon_state(shown, now, sizeof(now));
    std::snprintf(line, sizeof(line), "%s: %s -> %s", name, was, now);
  } else {
    std::snprintf(line, sizeof(line), "%s: %d -> %d", name, r.v.count, e.num);
  }
  return line;
}

// A tick box for many rows at once: ticked when all of them are, half ticked
// when some are. A click ticks them all, or unticks them when all were: true,
// with what they are to be in *all.
bool tick_all(const char* label, int on, int of, bool* all) {
  bool v = of > 0 && on == of;
  const bool some = on > 0 && on < of;
  ImGui::BeginDisabled(of == 0);
  if (some) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
  const bool clicked = ImGui::Checkbox(label, &v);
  if (some) ImGui::PopItemFlag();
  ImGui::EndDisabled();
  *all = v;
  return clicked;
}

// One item of a list, with its tick and its editor. Only a piece of the case
// (`piece`) can go in the item box: what lives off the grid takes no room in the
// case. `box` is the view when the game has an item box, or null in a game with
// none (inventory::has_box): no Store there.
void item_row(int index, const inventory::Row& r, bool editable, bool piece, const inventory::View* box) {
  ImGui::PushID(index);
  RowEdit* edit = edit_of(r);
  RowEdit e = edit ? *edit : live_edit(r);  // what the fields show
  bool typed = false;
  char lb[32];
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!editable || r.v.force_get);
  bool on = ticked(r);
  if (ImGui::Checkbox("##tick", &on)) set_tick(r, on);
  ImGui::EndDisabled();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(item_label(r.v.id, lb, sizeof(lb)));
  if (r.v.equipped) {
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "E");
    item_help("Equipped. Storing or throwing it away unequips it.");
  }
  if (r.v.force_get) {
    ImGui::SameLine();
    ImGui::TextDisabled("(the game's)");
    item_help("A weapon the game handed you for a scene and takes back itself: it is left alone.");
  }
  if (r.knife && (r.v.condition & game::kConditionBroken)) {
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "broken");
  }

  // The count, or a weapon's rounds or durability and its upgrades.
  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!editable || r.v.force_get);
  if (r.weapon && piece) {
    if (r.gun) {
      ImGui::SetNextItemWidth(scaled(60.0f));
      if (ImGui::InputInt("##ammo", &e.ammo, 0, 0)) typed = true;
      e.ammo = std::clamp(e.ammo, 0, 9999);
      item_help("Rounds loaded. The game keeps no more than the magazine holds at its capacity level.");
    } else {
      ImGui::SetNextItemWidth(scaled(70.0f));
      if (ImGui::InputInt("##dur", &e.durability, 0, 0)) typed = true;
      const int most = std::max(r.durability_max, r.v.durability);
      e.durability = std::clamp(e.durability, 0, std::max(most, 1));
      char help[160];
      std::snprintf(help, sizeof(help),
                    "Durability, up to %d (what it was made with%s). A broken knife given some is mended.", most,
                    most > r.durability_max ? ", or what it has now" : "");
      item_help(help);
    }
    for (int i = 0; i < r.levels.n && i < 16; ++i) {
      const game::Levels::Entry& lv = r.levels.e[i];
      if (!shown_level(lv)) continue;
      const int max = r.max_level[i];
      ImGui::SameLine();
      ImGui::PushID(i);
      if (lv.list == 2) {
        bool ex = e.levels[i] > 0;
        ImGui::BeginDisabled(max < 1);
        if (ImGui::Checkbox("Ex", &ex)) {
          e.levels[i] = ex ? 1 : 0;
          typed = true;
        }
        ImGui::EndDisabled();
        item_help("The merchant's exclusive upgrade for this weapon.");
      } else {
        int shown = e.levels[i] + 1;
        ImGui::BeginDisabled(max < 0);
        ImGui::SetNextItemWidth(scaled(34.0f));
        if (ImGui::InputInt(level_short(lv), &shown, 0, 0)) typed = true;
        ImGui::EndDisabled();
        if (max >= 0) e.levels[i] = std::clamp(shown, 1, max + 1) - 1;  // (one whose most is not known stays)
        char help[96];
        if (max >= 0) std::snprintf(help, sizeof(help), "%s level, 1 to %d", game::level_name(lv.list, lv.category), max + 1);
        else std::snprintf(help, sizeof(help), "%s level (its most was not found)", game::level_name(lv.list, lv.category));
        item_help(help);
      }
      ImGui::PopID();
    }
  } else if (r.stack > 1) {
    ImGui::SetNextItemWidth(scaled(80.0f));
    if (ImGui::InputInt("##num", &e.num, 0, 0)) typed = true;
    e.num = std::clamp(e.num, 1, r.stack);
    char help[64];
    std::snprintf(help, sizeof(help), "Up to %d in one %s", r.stack, piece ? "piece" : "entry");
    item_help(help);
  } else {
    ImGui::TextDisabled("-");
  }
  ImGui::EndDisabled();
  if (typed && !differs(r, e)) {  // typed back to what the game has: no change
    drop_edit(r);
    edit = nullptr;
  } else if (typed && edit) {
    *edit = e;
  } else if (typed) {
    g_edits.push_back(e);
    edit = &g_edits.back();
  }

  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!editable || r.v.force_get);
  if (edit && differs(r, *edit)) {
    if (ImGui::SmallButton("Apply")) {
      g_ask = Ask::kApply;
      g_ask_row = r;
      g_ask_edit = *edit;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo")) drop_edit(r);
    ImGui::SameLine();
  }
  if (piece && box) {
    ImGui::BeginDisabled(!box->storage && !box->box_ok);
    if (ImGui::SmallButton("Store")) inventory::request_store(r);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
      ImGui::PushTextWrapPos(scaled(380.0f));
      ImGui::TextUnformatted("Put all of it in the item box (below): the game's storage when it takes it, else the "
                             "box the mod keeps beside it in your save.");
      if (!box->storage) ImGui::TextDisabled("The game's storage: %s.", box->storage_why);
      if (!box->box_ok) ImGui::TextDisabled("The mod's box: %s.", box->box_why);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    ImGui::SameLine();
  }
  if (ImGui::SmallButton("x")) {
    g_ask = Ask::kRemove;
    g_ask_row = r;
  }
  item_help(piece ? "Throw it away."
                  : r.kind == inventory::kKeyItems ? "Remove it. A key item the story still needs is gone until you add it back."
                                                   : "Remove it.");
  ImGui::EndDisabled();
  ImGui::PopID();
}

// --- many at once: the rows ticked, the changes typed ---
// The ticked rows among `rows` (the view's), as the game is asked about them:
// those it is not keeping for itself.
std::vector<inventory::Row> ticked_rows(const inventory::View& v, const std::vector<int>& rows) {
  std::vector<inventory::Row> out;
  for (const int i : rows)
    if (!v.row[i].v.force_get && ticked(v.row[i])) out.push_back(v.row[i]);
  return out;
}

void store_ticked(const inventory::View& v, const std::vector<int>& rows) {
  changes_from(&g_list_place[inventory::kCase]);
  inventory::begin_batch();
  for (const inventory::Row& r : ticked_rows(v, rows)) {
    inventory::request_store(r);
    set_tick(r, false);
  }
  inventory::end_batch("stored");
}

void ask_remove_ticked(const inventory::View& v, const std::vector<int>& rows, int kind) {
  const std::vector<inventory::Row> sel = ticked_rows(v, rows);
  if (sel.empty()) return;
  const bool piece = kind == inventory::kCase;
  const int n = static_cast<int>(sel.size());
  g_batch = BatchAsk{};
  std::snprintf(g_batch.question, sizeof(g_batch.question), "%s %s?", piece ? "Throw away" : "Remove",
                n == 1 ? "this item" : "these items");
  for (const inventory::Row& r : sel) g_batch.lines.push_back(item_line(r));
  std::snprintf(g_batch.note, sizeof(g_batch.note), "%s",
                kind == inventory::kKeyItems ? "They are gone for good once the game is saved - a key item the story "
                                               "still needs until you add it back."
                                             : "They are gone for good once the game is saved.");
  if (n == 1) std::snprintf(g_batch.button, sizeof(g_batch.button), "%s", piece ? "Throw away" : "Remove");
  else std::snprintf(g_batch.button, sizeof(g_batch.button), "%s %d", piece ? "Throw away" : "Remove", n);
  g_batch.go = [sel, piece, kind] {
    changes_from(&g_list_place[kind]);
    inventory::begin_batch();
    for (const inventory::Row& r : sel) {
      inventory::request_remove(r);
      set_tick(r, false);
    }
    inventory::end_batch(piece ? "thrown away" : "removed");
  };
  g_ask = Ask::kBatch;
}

void ask_apply_all(const inventory::View& v, const std::vector<int>& rows) {
  struct Change {
    inventory::Row row;
    RowEdit edit;
  };
  std::vector<Change> changes;
  for (const int i : rows)
    if (const RowEdit* e = edit_of(v.row[i]); e && differs(v.row[i], *e)) changes.push_back(Change{v.row[i], *e});
  if (changes.empty()) return;
  g_batch = BatchAsk{};
  const int n = static_cast<int>(changes.size());
  std::snprintf(g_batch.question, sizeof(g_batch.question), "Make %s?", n == 1 ? "this change" : "these changes");
  bool upgrades = false;
  for (const Change& c : changes) {
    g_batch.lines.push_back(change_line(c.row, c.edit));
    upgrades = upgrades || (edits_weapon(c.row) && c.row.levels.n);
  }
  std::snprintf(g_batch.note, sizeof(g_batch.note), "%s",
                upgrades ? "Each is set as its own Apply would set it; upgrades go through the merchant's own setters."
                         : "Each is set as its own Apply would set it.");
  if (n == 1) std::snprintf(g_batch.button, sizeof(g_batch.button), "Apply");
  else std::snprintf(g_batch.button, sizeof(g_batch.button), "Apply %d", n);
  g_batch.go = [changes, kind = changes.front().row.kind] {
    changes_from(&g_list_place[kind]);
    inventory::begin_batch();
    for (const Change& c : changes) {
      request_edit(c.row, c.edit);
      drop_edit(c.row);
    }
    inventory::end_batch("changed");
  };
  g_ask = Ask::kBatch;
}

// The line over a list of the case or off it: a tick box for all of its rows,
// what can be done to the ticked ones, and the changes typed into its rows.
void list_bar(const inventory::View& v, int kind, const std::vector<int>& rows) {
  int of = 0, on = 0, changes = 0;
  for (const int i : rows) {
    const inventory::Row& r = v.row[i];
    if (const RowEdit* e = edit_of(r); e && differs(r, *e)) ++changes;
    if (r.v.force_get) continue;
    ++of;
    on += ticked(r);
  }
  const bool piece = kind == inventory::kCase;
  ImGui::PushID(kind);
  ImGui::BeginDisabled(!v.editable);
  bool all = false;
  if (tick_all("All", on, of, &all))
    for (const int i : rows)
      if (!v.row[i].v.force_get) set_tick(v.row[i], all);
  item_help("Tick every item in the list - or, all ticked, untick them. Ticked items can be stored or thrown away "
            "together, asked once for all of them.");
  if (on) {
    ImGui::SameLine();
    ImGui::TextDisabled("\xC2\xB7");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d ticked:", on);
    if (piece && v.has_box) {
      ImGui::SameLine();
      ImGui::BeginDisabled(!v.storage && !v.box_ok);
      if (ImGui::SmallButton("Store")) store_ticked(v, rows);
      ImGui::EndDisabled();
      item_help("Put every ticked item in the item box (below), as Store beside each one would.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(piece ? "Throw away" : "Remove")) ask_remove_ticked(v, rows, kind);
    item_help(piece ? "Throw every ticked item away - asked once for all of them."
                    : "Remove every ticked item - asked once for all of them.");
  }
  if (changes) {
    ImGui::SameLine();
    ImGui::TextDisabled("\xC2\xB7");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d changed:", changes);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply all")) ask_apply_all(v, rows);
    item_help("Set every row you have changed (each with an Apply beside it) - asked once for all of them.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo all"))
      for (const int i : rows) drop_edit(v.row[i]);
  }
  ImGui::EndDisabled();
  ImGui::PopID();
  place_note(v, &g_list_place[kind]);
}

void draw_case(const inventory::View& v) {
  help_marker(
      "The attache case of the character you are playing, straight from the game. Change a count, a gun's loaded "
      "rounds, a knife's durability or a weapon's upgrades (P power, C capacity, R reload speed, F rate of fire, D a "
      "knife's durability upgrade, Ex the exclusive upgrade) and press Apply - or change several and press Apply all, "
      "above the list. Store puts the item in this game's item box (the Mercenaries has none); x throws it away. Tick "
      "items (All ticks every one) to store or throw them away together. New ones come in from Add to the case, "
      "below. The case can be made bigger here too - never smaller, so nothing it holds is lost.");
  if (!v.known) {
    ImGui::TextDisabled("(%s)", v.why[0] ? v.why : "no game is being played");
    return;
  }
  if (!v.editable && v.why[0]) ImGui::TextDisabled("(%s)", v.why);
  // The case's size: grows through the game's own change of case.
  ImGui::AlignTextToFramePadding();
  ImGui::Text("Size %s - %d item%s", game::case_size_name(v.size.size), v.count[inventory::kCase],
              v.count[inventory::kCase] == 1 ? "" : "s");
  if (v.size.size >= 0 && v.size.size < 4) {
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.editable);
    ImGui::SetNextItemWidth(scaled(140.0f));
    if (ImGui::BeginCombo("##grow", "Make it bigger...")) {
      for (int s = v.size.size + 1; s < 5; ++s)
        if (ImGui::Selectable(game::case_size_name(s))) {
          g_ask = Ask::kCaseSize;
          g_ask_num = s;
        }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
  }
  if (!v.count[inventory::kCase]) return;
  std::vector<int> rows;
  for (int i = 0; i < v.nrows; ++i)
    if (v.row[i].kind == inventory::kCase) rows.push_back(i);
  list_bar(v, inventory::kCase, rows);
  if (ImGui::BeginTable("case", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("##tick");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count / rounds, upgrades");
    ImGui::TableSetupColumn("");
    for (const int i : rows) item_row(i, v.row[i], v.editable, true, v.has_box ? &v : nullptr);
    ImGui::EndTable();
  }
}

// An item picker: a search box, tabs for the banks (or not), the items under
// their groups' headings, each with a tick box (and how many, once ticked), and
// Add for every one ticked, asked once. One for each place an item goes, each
// offering only its own: the attache case's, the key items', the treasures' and
// the other items'.
struct Picker {
  const char* id;
  char query[48] = {};
  int tab = -1;            // the bank shown, -1 every one (with tabs)
  int want_tab = -2;       // a tab to bring forward next frame (-2 none)
  int listed_tab = -1;     // the tab the list was last drawn for
  int scroll_to = -1;      // an item to scroll the list to (Enter ticked it), -1 none
  int num[items::kCount] = {};  // how many of each item (kItems' order) are to be added: 0 not ticked
};
Picker g_add{"add"}, g_key_add{"key add"}, g_treasure_add{"treasure add"}, g_unique_add{"unique add"};
using Accept = bool (*)(const items::Def& d, const inventory::View& v);

int held_count(const inventory::View& v, int32_t id) {
  int n = 0;
  for (int i = 0; i < v.nrows; ++i)
    if (v.row[i].v.id == id) n += v.row[i].v.count;
  return n;
}

// The most of an item one Add gives: what one piece (or entry) of it holds, a
// weapon one.
int most_of(const items::Def& d) { return items::keeps_state(d.id) ? 1 : std::max(1, d.stack); }

// Add for the items ticked, in the list's order: asked once for all of them.
void ask_add(Picker& p, const inventory::View& v, Accept accept) {
  struct Pick {
    int32_t id;
    int num;
  };
  std::vector<Pick> picks;
  g_batch = BatchAsk{};
  int kind = inventory::kCase;
  for (const int16_t i : items::kByRank) {
    const items::Def& d = items::kItems[i];
    if (p.num[i] <= 0 || !accept(d, v)) continue;
    const int n = std::clamp(p.num[i], 1, most_of(d));
    picks.push_back(Pick{d.id, n});
    kind = inventory::kind_of_item(d.id);
    char line[96];
    if (n > 1) std::snprintf(line, sizeof(line), "%s x%d", d.name, n);
    else std::snprintf(line, sizeof(line), "%s", d.name);
    g_batch.lines.push_back(line);
  }
  if (picks.empty()) return;
  const int n = static_cast<int>(picks.size());
  std::snprintf(g_batch.question, sizeof(g_batch.question), "Add %s?", n == 1 ? "this item" : "these items");
  if (kind == inventory::kCase)
    std::snprintf(g_batch.note, sizeof(g_batch.note),
                  "Each goes into the attache case where the game finds it room, as a pickup would - what finds no "
                  "room is not added.");
  else
    std::snprintf(g_batch.note, sizeof(g_batch.note), "Each goes with the %s, as a pickup would.",
                  inventory::kind_name(kind));
  if (n == 1) std::snprintf(g_batch.button, sizeof(g_batch.button), "Add");
  else std::snprintf(g_batch.button, sizeof(g_batch.button), "Add %d", n);
  g_batch.go = [picks, &p] {
    changes_from(&p);
    inventory::begin_batch();
    for (const Pick& k : picks) inventory::request_add(k.id, k.num);
    inventory::end_batch("added");
    std::fill(std::begin(p.num), std::end(p.num), 0);
  };
  g_ask = Ask::kBatch;
}

void draw_picker(Picker& p, const inventory::View& v, bool tabs, Accept accept, float rows, const char* help) {
  const ImGuiStyle& style = ImGui::GetStyle();
  char id[64];
  const float clear_w = p.query[0] ? ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x
                                   : 0.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clear_w - ImGui::CalcTextSize("(?)").x -
                          style.ItemSpacing.x);
  std::snprintf(id, sizeof(id), "##%s search", p.id);
  const bool enter = ImGui::InputTextWithHint(id, "search: part of a name, or an item number", p.query, sizeof(p.query),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  bool edited = ImGui::IsItemEdited();
  if (p.query[0]) {
    ImGui::SameLine();
    std::snprintf(id, sizeof(id), "Clear##%s", p.id);
    if (ImGui::SmallButton(id)) {
      p.query[0] = '\0';
      edited = true;
    }
  }
  help_marker(help);

  // A tick the list no longer offers (the item was added, or the game changed) is let go.
  for (int i = 0; i < items::kCount; ++i)
    if (p.num[i] > 0 && !accept(items::kItems[i], v)) p.num[i] = 0;
  // How many items match under each tab, which tabs have any at all, and
  // whether there is more than one heading's worth.
  int matched[items::kBanks] = {}, offers[items::kBanks] = {}, total = 0, one_group = -1;
  bool headings = false;
  for (const int16_t i : items::kByRank) {
    const items::Def& d = items::kItems[i];
    if (!accept(d, v)) continue;
    ++offers[d.bank];
    if (one_group < 0) one_group = d.group;
    else if (d.group != one_group) headings = true;
    if (item_matches(d.id, d.name, p.query)) {
      ++matched[d.bank];
      ++total;
    }
  }
  if (!tabs) p.tab = -1;
  std::snprintf(id, sizeof(id), "%s banks", p.id);
  if (tabs && ImGui::BeginTabBar(id)) {
    for (int k = -1; k < items::kBanks; ++k) {
      if (k >= 0 && !offers[k]) continue;
      char tab[48];
      const char* name = k < 0 ? "All" : items::bank_name(k);
      if (p.query[0]) std::snprintf(tab, sizeof(tab), "%s %d###%s%d", name, k < 0 ? total : matched[k], p.id, k + 1);
      else std::snprintf(tab, sizeof(tab), "%s###%s%d", name, p.id, k + 1);
      if (ImGui::BeginTabItem(tab, nullptr, k == p.want_tab ? ImGuiTabItemFlags_SetSelected : 0)) {
        p.tab = k;
        ImGui::EndTabItem();
      }
    }
    p.want_tab = -2;
    ImGui::EndTabBar();
  }

  // The list, under its headings; it keeps its height while it is narrowed. A
  // ticked item that holds more than one has its count beside it.
  const int tab = p.tab;
  int first = -1;
  std::vector<int16_t> listed;
  std::snprintf(id, sizeof(id), "##%s list", p.id);
  if (ImGui::BeginChild(id, ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * rows + style.WindowPadding.y * 2.0f),
                        ImGuiChildFlags_Borders)) {
    if (edited || tab != p.listed_tab) ImGui::SetScrollY(0.0f);  // a new search or tab starts at the top
    p.listed_tab = tab;
    int group = -1;
    const float num_w = scaled(70.0f);
    const float num_x = ImGui::GetCursorPosX() + scaled(260.0f);  // past the longest names
    // The tick boxes and counts a line of text high, as the list's rows were.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
    for (const int16_t i : items::kByRank) {
      const items::Def& d = items::kItems[i];
      if (!accept(d, v) || (tab >= 0 && d.bank != tab) || !item_matches(d.id, d.name, p.query)) continue;
      if (first < 0) first = i;
      listed.push_back(i);
      if (headings && d.group != group) {
        group = d.group;
        ImGui::SeparatorText(items::kGroups[group].name);
      }
      ImGui::PushID(i);
      bool on = p.num[i] > 0;
      if (ImGui::Checkbox(d.name, &on)) p.num[i] = on ? 1 : 0;
      const int max = most_of(d);
      if (on && max > 1) {
        const float after = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + style.ItemSpacing.x;
        ImGui::SameLine(std::max(num_x, after));
        ImGui::SetNextItemWidth(num_w);
        ImGui::InputInt("##num", &p.num[i], 0, 0);
        p.num[i] = std::clamp(p.num[i], 1, max);
        char h[64];
        std::snprintf(h, sizeof(h), "How many: up to %d, what one %s of it holds.", max,
                      inventory::kind_of_item(d.id) == inventory::kCase ? "piece" : "entry");
        item_help(h);
      }
      if (i == p.scroll_to) {
        ImGui::SetScrollHereY(0.5f);
        p.scroll_to = -1;
      }
      ImGui::PopID();
    }
    ImGui::PopStyleVar();
    p.scroll_to = -1;  // (the item is not in this list)
    if (first < 0 && total > 0) {
      ImGui::TextDisabled("Nothing under %s matches \"%s\".", items::bank_name(tab), p.query);
      char all[32];
      std::snprintf(all, sizeof(all), "Show all %d", total);
      if (ImGui::SmallButton(all)) p.want_tab = -1;
    } else if (first < 0 && p.query[0]) {
      ImGui::TextDisabled("Nothing matches \"%s\".", p.query);
    } else if (first < 0) {
      ImGui::TextDisabled("Nothing to add - you have them all.");
    }
  }
  ImGui::EndChild();
  if (enter && first >= 0) {  // Enter ticks the first match
    if (p.num[first] <= 0) p.num[first] = 1;
    p.scroll_to = first;
  }

  // Under the list: ticks for every item listed, how many are ticked, and Add.
  int on = 0, short_of_most = 0, listed_on = 0;
  for (int i = 0; i < items::kCount; ++i)
    if (p.num[i] > 0) {
      ++on;
      short_of_most += p.num[i] < most_of(items::kItems[i]);
    }
  for (const int16_t i : listed) listed_on += p.num[i] > 0;
  ImGui::BeginDisabled(listed.empty() || listed_on == static_cast<int>(listed.size()));
  std::snprintf(id, sizeof(id), "Tick all %d listed###%s tick all", static_cast<int>(listed.size()), p.id);
  if (ImGui::SmallButton(id))
    for (const int16_t i : listed)
      if (p.num[i] <= 0) p.num[i] = 1;
  ImGui::EndDisabled();
  item_help("Tick every item the list shows: the tab's, and only what the search keeps when you type one.");
  ImGui::SameLine();
  if (!on) {
    ImGui::TextDisabled("Tick the items to add.");
    place_note(v, &p);
    return;
  }
  std::snprintf(id, sizeof(id), "Untick all###%s untick", p.id);
  if (ImGui::SmallButton(id)) std::fill(std::begin(p.num), std::end(p.num), 0);
  if (short_of_most) {
    ImGui::SameLine();
    std::snprintf(id, sizeof(id), "Most of each###%s most", p.id);
    if (ImGui::SmallButton(id))
      for (int i = 0; i < items::kCount; ++i)
        if (p.num[i] > 0) p.num[i] = most_of(items::kItems[i]);
    item_help("Each ticked item's count set to the most one piece (or entry) of it holds.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("\xC2\xB7");
  ImGui::SameLine();
  ImGui::Text("%d ticked", on);
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    int shown = 0;
    for (const int16_t i : items::kByRank) {
      if (p.num[i] <= 0) continue;
      if (++shown > 20) {
        ImGui::TextDisabled("and %d more", on - 20);
        break;
      }
      if (p.num[i] > 1) ImGui::Text("%s x%d", items::kItems[i].name, p.num[i]);
      else ImGui::TextUnformatted(items::kItems[i].name);
    }
    ImGui::EndTooltip();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!v.editable);
  if (on == 1) std::snprintf(id, sizeof(id), "Add###%s add", p.id);
  else std::snprintf(id, sizeof(id), "Add %d###%s add", on, p.id);
  if (ImGui::SmallButton(id)) ask_add(p, v, accept);
  ImGui::EndDisabled();
  item_help("Add every ticked item - asked once for all of them.");
  place_note(v, &p);
}

game::Mode g_mode = game::Mode::kNone;  // the game the Add lists are for (this frame)

bool accept_case(const items::Def& d, const inventory::View&) {
  return d.offered && in_this_game(d, g_mode) && inventory::kind_of_item(d.id) == inventory::kCase && d.w;
}
bool accept_key(const items::Def& d, const inventory::View& v) {
  return d.offered && in_this_game(d, g_mode) && inventory::kind_of_item(d.id) == inventory::kKeyItems &&
         held_count(v, d.id) < std::max(1, d.stack);
}
bool accept_treasure(const items::Def& d, const inventory::View& v) {
  return d.offered && in_this_game(d, g_mode) && inventory::kind_of_item(d.id) == inventory::kTreasures &&
         held_count(v, d.id) < std::max(1, d.stack);
}
bool accept_unique(const items::Def& d, const inventory::View& v) {
  return d.offered && in_this_game(d, g_mode) && inventory::kind_of_item(d.id) == inventory::kUnique &&
         held_count(v, d.id) == 0;
}

void draw_add(const inventory::View& v) {
  draw_picker(g_add, v, true, accept_case, 10.5f,
              "What takes room in the attache case - weapons (every knife that can be upgraded too), sub-weapons "
              "(the other knives and grenades), attachments, ammo, "
              "recovery items and resources - under a tab for its kind and a heading for what it is. Type in the "
              "search box to keep only the items whose name holds every word you type (or an item number); Enter "
              "ticks the first match. Tick the items you want (or Tick all listed), say how many of each, and press "
              "Add - asked once for all of them. Each goes into the case where the game's own placement finds it room "
              "(topping up what you carry of it first, turning it if it has to) - or, with no room, nowhere. A new "
              "weapon arrives loaded, with no upgrades.");
}

// The panel's order of an item (the Add lists'), and its group (-1: one the
// table does not have).
int rank_of(int32_t id) {
  const items::Def* d = items::find(id);
  return d ? static_cast<int>(d->rank) : items::kCount;
}
int group_of(int32_t id) {
  const items::Def* d = items::find(id);
  return d ? static_cast<int>(d->group) : -1;
}

// A heading over a group of a list's rows - `rows` from `from` on while they are
// of `group` - with a tick box for all of them.
void group_row(const inventory::View& v, const std::vector<int>& rows, size_t from, int group) {
  int of = 0, on = 0;
  for (size_t k = from; k < rows.size() && group_of(v.row[rows[k]].v.id) == group; ++k) {
    if (v.row[rows[k]].v.force_get) continue;
    ++of;
    on += ticked(v.row[rows[k]]);
  }
  ImGui::TableNextRow();
  ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
  ImGui::TableNextColumn();
  ImGui::PushID(0x20000 + group);
  ImGui::BeginDisabled(!v.editable);
  bool all = false;
  if (tick_all("##group", on, of, &all))
    for (size_t k = from; k < rows.size() && group_of(v.row[rows[k]].v.id) == group; ++k)
      if (!v.row[rows[k]].v.force_get) set_tick(v.row[rows[k]], all);
  ImGui::EndDisabled();
  ImGui::PopID();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(group >= 0 ? items::kGroups[group].name : "Other");
}

// The view's rows of a list off the case's grid in the Add list's order: group by
// group, each group in the game's own order (the game's list keeps them in the
// order they came).
std::vector<int> offgrid_rows(const inventory::View& v, int kind) {
  std::vector<int> rows;
  for (int i = 0; i < v.nrows; ++i)
    if (v.row[i].kind == kind) rows.push_back(i);
  std::stable_sort(rows.begin(), rows.end(),
                   [&](int a, int b) { return rank_of(v.row[a].v.id) < rank_of(v.row[b].v.id); });
  return rows;
}

// One of the lists off the case's grid: the key items, the treasures, the other
// items (recipes, charms, treasure maps, case skins, armor) - in the Add list's
// order, under its groups' headings.
void draw_offgrid(const inventory::View& v, int kind) {
  if (!v.known) {
    ImGui::TextDisabled("(%s)", v.why[0] ? v.why : "no game is being played");
    return;
  }
  if (!v.editable && v.why[0]) ImGui::TextDisabled("(%s)", v.why);
  if (!v.count[kind]) {
    ImGui::TextDisabled("You carry none.");
    return;
  }
  const std::vector<int> rows = offgrid_rows(v, kind);
  int groups = 0;
  for (size_t k = 0; k < rows.size(); ++k)
    groups += k == 0 || group_of(v.row[rows[k]].v.id) != group_of(v.row[rows[k - 1]].v.id);
  const bool headings = groups > 1;
  list_bar(v, kind, rows);
  const bool scroll = static_cast<int>(rows.size()) + (headings ? groups : 0) > 12;
  char id[32];
  std::snprintf(id, sizeof(id), "list%d", kind);
  if (ImGui::BeginTable(id, 4,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | (scroll ? ImGuiTableFlags_ScrollY : 0),
                        ImVec2(0.0f, scroll ? ImGui::GetFrameHeightWithSpacing() * 12.5f : 0.0f))) {
    ImGui::TableSetupColumn("##tick");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    for (size_t k = 0; k < rows.size(); ++k) {
      const int group = group_of(v.row[rows[k]].v.id);
      if (headings && (k == 0 || group != group_of(v.row[rows[k - 1]].v.id))) group_row(v, rows, k, group);
      item_row(rows[k], v.row[rows[k]], v.editable, false, nullptr);
    }
    ImGui::EndTable();
  }
}

// The item box of the game being played (inventory.h): the game's storage, whose
// entries are the view's kStorage rows, and the mod's own part beside it
// (itembox.h), both kept in the game's save. One list in the panel's order under
// the banks' tabs, each row saying which of the two holds it.
struct BoxRow {
  int stored = -1;  // a row of the view (the game's storage)
  int entry = -1;   // an entry of the mod's part
  int32_t id = 0;
};

// A row of the game's storage: its item as the game keeps it, handed over one at
// a time as a typewriter's Storage does.
void stored_row(const inventory::View& v, const inventory::Row& r) {
  char lb[32];
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!v.editable || !v.storage);
  bool on = ticked(r);
  if (ImGui::Checkbox("##tick", &on)) set_tick(r, on);
  ImGui::EndDisabled();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(item_label(r.v.id, lb, sizeof(lb)));
  ImGui::SameLine();
  ImGui::TextDisabled("storage");
  item_help("In the game's storage: a typewriter's Storage reaches it too.");
  if (r.knife && (r.v.condition & game::kConditionBroken)) {
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "broken");
  }

  ImGui::TableNextColumn();
  const bool stack = r.v.stored_max > 1;
  StoredEdit* edit = stored_edit_of(r);
  if (r.weapon) {
    char state[200];
    weapon_state(r, state, sizeof(state));
    ImGui::TextUnformatted(state);
  } else if (stack) {
    int num = edit ? edit->num : r.v.stored;
    ImGui::BeginDisabled(!v.editable || !v.storage);
    ImGui::SetNextItemWidth(scaled(90.0f));
    if (ImGui::InputInt("##num", &num, 0, 0)) {
      num = std::clamp(num, 1, r.v.stored_max);
      if (num == r.v.stored) {
        drop_stored_edit(r);
        edit = nullptr;
      } else if (edit) {
        edit->num = num;
      } else {
        g_stored_edits.push_back(StoredEdit{key_of(r), num});
        edit = &g_stored_edits.back();
      }
    }
    ImGui::EndDisabled();
    char h[128];
    std::snprintf(h, sizeof(h), "How many the game's storage holds, up to %d. Type a new count and press Apply.",
                  r.v.stored_max);
    item_help(h);
  } else {
    ImGui::TextDisabled("-");
  }

  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!v.editable || !v.storage);
  if (edit && edit->num != r.v.stored) {
    if (ImGui::SmallButton("Apply")) {
      g_ask = Ask::kStoredCount;
      g_ask_row = r;
      g_ask_num = edit->num;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo")) drop_stored_edit(r);
    ImGui::SameLine();
  }
  if (ImGui::SmallButton("Take")) inventory::request_take_stored(r);
  item_help(stack ? "Put one in the attache case where the game finds it room, as a typewriter's Storage hands them "
                    "over."
                  : r.weapon ? "Put it back in the attache case where the game finds it room, with its upgrades and "
                               "loaded rounds."
                             : "Put it back in the attache case where the game finds it room.");
  ImGui::SameLine();
  if (ImGui::SmallButton("x")) {
    g_ask = Ask::kDiscardStored;
    g_ask_row = r;
  }
  item_help("Throw it out of the game's storage.");
  ImGui::EndDisabled();
}

// An entry of the mod's part: everything of one item, or a weapon of its own.
void box_row(const inventory::View& v, const itembox::View& box, int i) {
  char lb[32];
  const itembox::Entry& e = box.entry[i];
  const char* label = item_label(e.id, lb, sizeof(lb));
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!v.editable);
  bool on = box_ticked(e);
  if (ImGui::Checkbox("##tick", &on)) set_box_tick(e, on);
  ImGui::EndDisabled();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  const items::Def* d = items::find(e.id);
  const int max = d && d->stack > 0 ? d->stack : 1;
  const int take = e.weapon ? 1 : e.num < max ? e.num : max;

  ImGui::TableNextColumn();
  BoxEdit* edit = box_edit_of(e);
  if (e.weapon) {
    char state[160];
    itembox::describe(e, state, sizeof(state));
    ImGui::TextUnformatted(state);
  } else {
    int num = edit ? edit->num : e.num;
    ImGui::BeginDisabled(!v.editable);
    ImGui::SetNextItemWidth(scaled(90.0f));
    if (ImGui::InputInt("##num", &num, 0, 0)) {
      num = std::clamp(num, 1, itembox::kMaxCount);
      if (num == e.num) {
        drop_box_edit(e);
        edit = nullptr;
      } else if (edit) {
        edit->num = num;
      } else {
        g_box_edits.push_back(BoxEdit{e, num});
        edit = &g_box_edits.back();
      }
    }
    ImGui::EndDisabled();
    char h[128];
    std::snprintf(h, sizeof(h), "How many the box holds. Type a new count and press Apply. Take hands over %d at a "
                  "time.", max);
    item_help(h);
  }

  ImGui::TableNextColumn();
  ImGui::BeginDisabled(!v.editable);
  if (edit && edit->num != e.num) {
    if (ImGui::SmallButton("Apply")) {
      g_ask = Ask::kBoxCount;
      g_ask_box = i;
      g_ask_entry = e;
      g_ask_num = edit->num;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo")) drop_box_edit(e);
    ImGui::SameLine();
  }
  if (ImGui::SmallButton("Take")) inventory::request_take(i, e);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(scaled(380.0f));
    if (e.weapon)
      ImGui::TextUnformatted("Put it back in the attache case where the game finds it room, with its upgrades and "
                             "loaded rounds.");
    else
      ImGui::Text("Put %d in the attache case: the game tops up the %s you carry first, then finds room for a new "
                  "piece.", take, label);
    if (!e.weapon && e.num > take)
      ImGui::TextDisabled("One piece holds %d, so %d stay in the box - Take again for more.", max, e.num - take);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("x")) {
    g_ask = Ask::kDiscardBox;
    g_ask_box = i;
    g_ask_entry = e;
  }
  item_help("Throw it out of the box.");
  ImGui::EndDisabled();
}

// --- the box's many at once ---
bool box_row_ticked(const inventory::View& v, const itembox::View& box, const BoxRow& b) {
  return b.stored >= 0 ? ticked(v.row[b.stored]) : box_ticked(box.entry[b.entry]);
}

void take_ticked(const inventory::View& v, const itembox::View& box, const std::vector<BoxRow>& rows) {
  changes_from(&g_list_place[inventory::kStorage]);
  inventory::begin_batch();
  for (const BoxRow& b : rows) {
    if (!box_row_ticked(v, box, b)) continue;
    if (b.stored >= 0) {
      inventory::request_take_stored(v.row[b.stored]);
      set_tick(v.row[b.stored], false);
    } else {
      inventory::request_take(b.entry, box.entry[b.entry]);
      set_box_tick(box.entry[b.entry], false);
    }
  }
  inventory::end_batch("taken");
}

void ask_discard_ticked(const inventory::View& v, const itembox::View& box, const std::vector<BoxRow>& rows) {
  std::vector<inventory::Row> stored;
  std::vector<std::pair<int, itembox::Entry>> entries;
  g_batch = BatchAsk{};
  for (const BoxRow& b : rows) {
    if (!box_row_ticked(v, box, b)) continue;
    if (b.stored >= 0) {
      stored.push_back(v.row[b.stored]);
      g_batch.lines.push_back(item_line(v.row[b.stored]) + "  (storage)");
    } else {
      entries.emplace_back(b.entry, box.entry[b.entry]);
      g_batch.lines.push_back(entry_line(box.entry[b.entry]));
    }
  }
  const int n = static_cast<int>(g_batch.lines.size());
  if (!n) return;
  std::snprintf(g_batch.question, sizeof(g_batch.question), "Throw %s out of the item box?",
                n == 1 ? "this" : "these");
  std::snprintf(g_batch.note, sizeof(g_batch.note), "They are gone for good once the game is saved.");
  if (n == 1) std::snprintf(g_batch.button, sizeof(g_batch.button), "Throw away");
  else std::snprintf(g_batch.button, sizeof(g_batch.button), "Throw away %d", n);
  g_batch.go = [stored, entries] {
    changes_from(&g_list_place[inventory::kStorage]);
    inventory::begin_batch();
    for (const inventory::Row& r : stored) {
      inventory::request_discard_stored(r);
      set_tick(r, false);
    }
    for (const auto& [index, e] : entries) {
      inventory::request_discard(index, e);
      set_box_tick(e, false);
    }
    inventory::end_batch("thrown away");
  };
  g_ask = Ask::kBatch;
}

void ask_apply_box(const inventory::View& v, const itembox::View& box, const std::vector<BoxRow>& rows) {
  std::vector<std::pair<inventory::Row, int>> stored;
  std::vector<std::tuple<int, itembox::Entry, int>> entries;
  char lb[32], line[160];
  g_batch = BatchAsk{};
  bool fewer = false;
  for (const BoxRow& b : rows) {
    if (b.stored >= 0) {
      const inventory::Row& r = v.row[b.stored];
      const StoredEdit* s = stored_edit_of(r);
      if (!s || s->num == r.v.stored) continue;
      stored.emplace_back(r, s->num);
      fewer = fewer || s->num < r.v.stored;
      std::snprintf(line, sizeof(line), "%s in the game's storage: %d -> %d", item_label(r.v.id, lb, sizeof(lb)),
                    r.v.stored, s->num);
    } else {
      const itembox::Entry& e = box.entry[b.entry];
      const BoxEdit* be = box_edit_of(e);
      if (!be || be->num == e.num) continue;
      entries.emplace_back(b.entry, e, be->num);
      fewer = fewer || be->num < e.num;
      std::snprintf(line, sizeof(line), "%s in the box: %d -> %d", item_label(e.id, lb, sizeof(lb)), e.num, be->num);
    }
    g_batch.lines.push_back(line);
  }
  const int n = static_cast<int>(g_batch.lines.size());
  if (!n) return;
  std::snprintf(g_batch.question, sizeof(g_batch.question), "Set %s?", n == 1 ? "this count" : "these counts");
  std::snprintf(g_batch.note, sizeof(g_batch.note), "%s",
                fewer ? "What a lower count takes out is gone for good once the game is saved." : "");
  if (n == 1) std::snprintf(g_batch.button, sizeof(g_batch.button), "Apply");
  else std::snprintf(g_batch.button, sizeof(g_batch.button), "Apply %d", n);
  g_batch.go = [stored, entries] {
    changes_from(&g_list_place[inventory::kStorage]);
    inventory::begin_batch();
    for (const auto& [r, num] : stored) {
      inventory::request_stored_count(r, num);
      drop_stored_edit(r);
    }
    for (const auto& [index, e, num] : entries) {
      inventory::request_box_count(index, e, num);
      drop_box_edit(e);
    }
    inventory::end_batch("changed");
  };
  g_ask = Ask::kBatch;
}

// The line over the item box's list: a tick box for the rows shown, what can be
// done to the ticked ones (under any tab), and the counts typed into its rows.
template <typename Shown>
void box_bar(const inventory::View& v, const itembox::View& box, const std::vector<BoxRow>& rows, Shown&& shown) {
  int of = 0, on = 0, on_shown = 0, changes = 0;
  for (const BoxRow& b : rows) {
    const bool t = box_row_ticked(v, box, b);
    on += t;
    if (shown(b)) {
      ++of;
      on_shown += t;
    }
    if (b.stored >= 0) {
      const StoredEdit* s = stored_edit_of(v.row[b.stored]);
      changes += s && s->num != v.row[b.stored].v.stored;
    } else {
      const BoxEdit* e = box_edit_of(box.entry[b.entry]);
      changes += e && e->num != box.entry[b.entry].num;
    }
  }
  ImGui::PushID("box bar");
  ImGui::BeginDisabled(!v.editable);
  bool all = false;
  if (tick_all("All", on_shown, of, &all))
    for (const BoxRow& b : rows) {
      if (!shown(b)) continue;
      if (b.stored >= 0) set_tick(v.row[b.stored], all);
      else set_box_tick(box.entry[b.entry], all);
    }
  item_help("Tick every item the list shows (the tab's, and what the filter keeps) - or, all ticked, untick them. "
            "Ticked items can be taken or thrown away together, from every tab at once.");
  if (on) {
    ImGui::SameLine();
    ImGui::TextDisabled("\xC2\xB7");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (on > on_shown) ImGui::Text("%d ticked (%d under other tabs):", on, on - on_shown);
    else ImGui::Text("%d ticked:", on);
    ImGui::SameLine();
    if (ImGui::SmallButton("Take")) take_ticked(v, box, rows);
    item_help("Put every ticked item back in the attache case, as Take beside each one would: one piece's worth of "
              "each, a weapon with its upgrades and rounds.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Throw away")) ask_discard_ticked(v, box, rows);
    item_help("Throw every ticked item out of the box - asked once for all of them.");
  }
  if (changes) {
    ImGui::SameLine();
    ImGui::TextDisabled("\xC2\xB7");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d changed:", changes);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply all")) ask_apply_box(v, box, rows);
    item_help("Set every count you have changed (each with an Apply beside it) - asked once for all of them.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo all")) {
      g_box_edits.clear();
      g_stored_edits.clear();
    }
  }
  ImGui::EndDisabled();
  ImGui::PopID();
  place_note(v, &g_list_place[inventory::kStorage]);
}

void draw_box(const inventory::View& v, const itembox::View& box) {
  char lb[32];
  ImGui::TextDisabled("Kept in your save: a load brings back the box that save had.");
  help_marker(
      "The item box of this game, with no limit on what it holds. Weapons, attachments and First Aid Sprays go into "
      "the game's own storage, where a typewriter's Storage reaches them too - it keeps one of each, and sprays up to "
      "999. Everything else - ammo, herbs, grenades, resources, a second copy of a weapon - the mod keeps beside "
      "them, in the same save.\n\n"
      "Store, beside an item of the case, puts all of it in; Take puts it back where the game's own placement finds "
      "it room. The game's storage hands things over one at a time, as at a typewriter; in the mod's part everything "
      "of one item is one entry however much of it you store, and Take hands over one piece's worth at a time. A "
      "weapon keeps its upgrades and loaded rounds (a knife its durability). Change a count and press Apply to set "
      "it (or change several and press Apply all); x throws an entry away. Tick entries (All ticks every one shown) "
      "to take or throw them away together.\n\n"
      "Each save has its own box: the game saves it with everything else, a load brings back the box that save had, "
      "and a new game starts with an empty one. As with the attache case, what you store or take is kept by your "
      "next save and lost if you quit without saving. The Mercenaries has no box.");
  // A box beside the mod (from before the box went into the saves, or one a save
  // could not keep): moved in when asked.
  itembox::File file;
  if (v.box_files > 0 && inventory::box_file(v.mode, &file)) {
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%d entr%s of an item box wait beside the mod, in %s.", v.box_files,
                       v.box_files == 1 ? "y" : "ies", itembox::file_name(file));
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(!v.editable || !v.box_ok);
    if (ImGui::SmallButton("Move into this game")) g_ask = Ask::kImport;
    ImGui::EndDisabled();
    item_help("Puts them in this game's item box - the game's storage for what it takes, the rest beside it. Your "
              "next save keeps them, and the file goes then; quit without saving and it stays, offered again.");
  }
  if (!v.storage) ImGui::TextDisabled("(the game's storage: %s)", v.storage_why);
  if (!v.box_ok) ImGui::TextDisabled("(the mod's part: %s)", v.box_why);
  if (box.save_failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("The last save could not keep the mod's part of the box: it went to a file beside the mod "
                       "instead (see the log), and Move into this game brings it back.");
    ImGui::PopStyleColor();
  }
  // Both parts in the panel's order (the Add list's), the storage's first for one item.
  static std::vector<BoxRow> rows;
  rows.clear();
  for (int i = 0; i < v.nrows; ++i)
    if (v.row[i].kind == inventory::kStorage) rows.push_back(BoxRow{i, -1, v.row[i].v.id});
  for (int i = 0; i < static_cast<int>(box.entry.size()); ++i) rows.push_back(BoxRow{-1, i, box.entry[i].id});
  std::stable_sort(rows.begin(), rows.end(), [&](const BoxRow& a, const BoxRow& b) {
    const int ra = rank_of(a.id), rb = rank_of(b.id);
    if (ra != rb) return ra < rb;
    if (a.id != b.id) return a.id < b.id;
    return a.stored >= 0 && b.stored < 0;
  });
  const int n = static_cast<int>(rows.size());
  static int shown_bank = items::kBankWeapons;
  static char filter[48] = {};
  auto bank_of = [](int32_t id) {
    const items::Def* d = items::find(id);
    return d ? static_cast<int>(d->bank) : static_cast<int>(items::kBankOther);
  };
  int held[items::kBanks] = {}, matched[items::kBanks] = {};
  for (const BoxRow& r : rows) {
    ++held[bank_of(r.id)];
    if (item_matches(r.id, item_label(r.id, lb, sizeof(lb)), filter)) ++matched[bank_of(r.id)];
  }
  if (ImGui::BeginTabBar("banks")) {
    for (int k = 0; k < items::kBanks; ++k) {
      if (k >= items::kCaseBanks && !held[k]) continue;
      char tab[48];
      std::snprintf(tab, sizeof(tab), "%s %d###bank%d", items::bank_name(k), held[k], k);
      if (ImGui::BeginTabItem(tab)) {
        shown_bank = k;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  const int bank = shown_bank;
  if (n > 0) {
    ImGui::SetNextItemWidth(scaled(230.0f));
    ImGui::InputTextWithHint("##box filter", "type to narrow the list", filter, sizeof(filter));
    if (filter[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) filter[0] = '\0';
      ImGui::SameLine();
      ImGui::TextDisabled("%d of %d shown", matched[bank], held[bank]);
    }
  }
  auto shown = [&](const BoxRow& r) {
    return bank_of(r.id) == bank && item_matches(r.id, item_label(r.id, lb, sizeof(lb)), filter);
  };
  if (n > 0) box_bar(v, box, rows, shown);
  char table_id[16];  // each bank keeps its own scroll position
  std::snprintf(table_id, sizeof(table_id), "box%d", bank);
  float height = 0.0f, last = 0.0f;
  int listed = 0;
  for (const BoxRow& r : rows) {
    if (!shown(r)) continue;
    // Every row has a tick box: a frame high.
    last = ImGui::GetFrameHeightWithSpacing();
    height += listed++ < 10 ? last : last * 0.5f;
    if (listed > 10) break;
  }
  if (listed <= 10) height += last * 0.5f;
  if (n == 0) {
    ImGui::TextDisabled("The box is empty - Store, beside an item of the attache case, puts it in.");
  } else if (held[bank] == 0) {
    ImGui::TextDisabled("Nothing in the box is filed under %s.", items::bank_name(bank));
  } else if (matched[bank] == 0) {
    ImGui::TextDisabled("nothing under %s matches \"%s\"", items::bank_name(bank), filter);
  } else if (ImGui::BeginTable(table_id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                               ImVec2(0.0f, height))) {
    ImGui::TableSetupColumn("##tick");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    for (const BoxRow& r : rows) {
      if (!shown(r)) continue;
      ImGui::PushID(r.stored >= 0 ? 0x10000 + r.stored : r.entry);
      if (r.stored >= 0) stored_row(v, v.row[r.stored]);
      else box_row(v, box, r.entry);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

// The confirmations, asked outside the tables so a dialog does not belong to a
// row that may be gone by the time it is answered.
void draw_item_dialogs(const inventory::View& v) {
  char lb[32];
  static Ask open = Ask::kNone;
  if (g_ask != Ask::kNone && open == Ask::kNone) {
    open = g_ask;
    ImGui::OpenPopup("item change?");
  }
  if (!ImGui::BeginPopupModal("item change?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (open != Ask::kNone && !ImGui::IsPopupOpen("item change?")) {
      open = g_ask = Ask::kNone;
      g_batch = BatchAsk{};
    }
    return;
  }
  if (g_ask == Ask::kNone) {  // forgotten while it was up (the panel went away): closed, not asked again
    open = Ask::kNone;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  bool close = !v.editable && open != Ask::kBoxCount && open != Ask::kDiscardBox;
  const char* name = item_label(g_ask_row.v.id, lb, sizeof(lb));
  switch (open) {
    case Ask::kApply: {
      const inventory::Row& r = g_ask_row;
      if (edits_weapon(r)) {
        inventory::Row shown = r;
        shown.v.ammo = g_ask_edit.ammo;
        shown.v.durability = g_ask_edit.durability;
        for (int i = 0; i < r.levels.n && i < 16; ++i) shown.levels.e[i].level = g_ask_edit.levels[i];
        char state[200];
        weapon_state(shown, state, sizeof(state));
        ImGui::Text("Set %s to %s?", name, state);
        ImGui::TextDisabled("The upgrades go through the merchant's own setters.");
      } else {
        ImGui::Text("Set %s to %d?", name, g_ask_edit.num);
      }
      if (ImGui::Button("Apply")) {
        request_edit(r, g_ask_edit);
        drop_edit(r);
        close = true;
      }
      break;
    }
    case Ask::kRemove: {
      const bool piece = g_ask_row.kind == inventory::kCase;
      if (g_ask_row.v.count > 1) ImGui::Text("%s %s x%d?", piece ? "Throw away" : "Remove", name, g_ask_row.v.count);
      else ImGui::Text("%s %s?", piece ? "Throw away" : "Remove", name);
      ImGui::TextDisabled("It is gone for good once the game is saved.");
      if (ImGui::Button(piece ? "Throw away" : "Remove")) {
        inventory::request_remove(g_ask_row);
        close = true;
      }
      break;
    }
    case Ask::kBatch: {
      ImGui::TextUnformatted(g_batch.question);
      // A line for each item, scrolled past a dozen.
      const ImGuiStyle& style = ImGui::GetStyle();
      float w = 0.0f;
      for (const std::string& l : g_batch.lines) w = std::max(w, ImGui::CalcTextSize(l.c_str()).x);
      w = std::clamp(w + style.WindowPadding.x * 2.0f + style.ScrollbarSize, scaled(260.0f), scaled(560.0f));
      const int n = static_cast<int>(g_batch.lines.size());
      const float h = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min(n, 12)) + style.WindowPadding.y * 2.0f;
      if (ImGui::BeginChild("##batch", ImVec2(w, h), ImGuiChildFlags_Borders))
        for (const std::string& l : g_batch.lines) ImGui::TextUnformatted(l.c_str());
      ImGui::EndChild();
      if (g_batch.note[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextDisabled("%s", g_batch.note);
        ImGui::PopTextWrapPos();
      }
      if (ImGui::Button(g_batch.button)) {
        if (g_batch.go) g_batch.go();
        close = true;
      }
      break;
    }
    case Ask::kCaseSize: {
      ImGui::Text("Make the attache case %s?", game::case_size_name(g_ask_num));
      ImGui::TextDisabled("The game's own change of case: what it holds stays where it is.");
      if (ImGui::Button("Make it bigger")) {
        inventory::request_case_size(g_ask_num);
        close = true;
      }
      break;
    }
    case Ask::kBoxCount: {
      const itembox::Entry& e = g_ask_entry;
      ImGui::Text("Set %s in the item box to %d?", item_label(e.id, lb, sizeof(lb)), g_ask_num);
      if (g_ask_num < e.num)
        ImGui::TextDisabled("It holds %d now: the other %d are gone for good once the game is saved.", e.num,
                            e.num - g_ask_num);
      else ImGui::TextDisabled("It holds %d now.", e.num);
      if (ImGui::Button("Apply")) {
        inventory::request_box_count(g_ask_box, e, g_ask_num);
        drop_box_edit(e);
        close = true;
      }
      break;
    }
    case Ask::kDiscardBox: {
      const itembox::Entry& e = g_ask_entry;
      const char* what = item_label(e.id, lb, sizeof(lb));
      if (e.weapon) {
        char state[160];
        itembox::describe(e, state, sizeof(state));
        ImGui::Text("Throw away %s (%s) from the item box?", what, state);
      } else {
        ImGui::Text("Throw away %s x%d from the item box?", what, e.num);
      }
      ImGui::TextDisabled("It is gone for good once the game is saved.");
      if (ImGui::Button("Throw away")) {
        inventory::request_discard(g_ask_box, g_ask_entry);
        close = true;
      }
      break;
    }
    case Ask::kStoredCount: {
      const inventory::Row& r = g_ask_row;
      ImGui::Text("Set %s in the game's storage to %d?", name, g_ask_num);
      if (g_ask_num < r.v.stored)
        ImGui::TextDisabled("It holds %d now: the other %d are gone for good once the game is saved.", r.v.stored,
                            r.v.stored - g_ask_num);
      else ImGui::TextDisabled("It holds %d now.", r.v.stored);
      if (ImGui::Button("Apply")) {
        inventory::request_stored_count(r, g_ask_num);
        drop_stored_edit(r);
        close = true;
      }
      break;
    }
    case Ask::kDiscardStored: {
      const inventory::Row& r = g_ask_row;
      if (r.weapon) {
        char state[200];
        weapon_state(r, state, sizeof(state));
        ImGui::Text("Throw away %s (%s) from the game's storage?", name, state);
      } else if (r.v.stored > 1) {
        ImGui::Text("Throw away %s x%d from the game's storage?", name, r.v.stored);
      } else {
        ImGui::Text("Throw away %s from the game's storage?", name);
      }
      ImGui::TextDisabled("It is gone for good once the game is saved.");
      if (ImGui::Button("Throw away")) {
        inventory::request_discard_stored(r);
        close = true;
      }
      break;
    }
    case Ask::kImport: {
      itembox::File file;
      const bool has = inventory::box_file(v.mode, &file);
      ImGui::Text("Move %d entr%s of %s into this game's item box?", v.box_files, v.box_files == 1 ? "y" : "ies",
                  has ? itembox::file_name(file) : "the file");
      ImGui::TextDisabled("The game's storage takes what it can, the box beside it the rest; the file goes once you "
                          "save.");
      if (ImGui::Button("Move in")) {
        inventory::request_import();
        close = true;
      }
      break;
    }
    default:
      close = true;
      break;
  }
  if (!close) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) close = true;
  }
  if (close) {
    open = g_ask = Ask::kNone;
    g_batch = BatchAsk{};
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// --- the save files ----------------------------------------------------------------------
enum { kAskNone = 0, kAskDelete, kAskCopy };
int g_file_ask = kAskNone;
int g_file_from = -1, g_file_to = -1;
savefiles::Row g_file_from_seen, g_file_to_seen;

void forget_file_dialogs() {
  g_file_ask = kAskNone;
  g_file_from = g_file_to = -1;
}

const char* slot_label(const savefiles::Row& r, char* out, size_t n) {
  const int base = r.slot >= 100 ? 100 : 0;
  if (r.slot == base) std::snprintf(out, n, "Auto-save");
  else if (r.slot > 0) std::snprintf(out, n, "Slot %d", r.slot - base);
  else std::snprintf(out, n, "?");
  return out;
}

// One save in a line: "Chapter 5 · Standard · 12 saves · 2026-10-18 21:04", or "No data".
void describe_save(const savefiles::Row& r, char* out, size_t n) {
  if (!r.used) {
    std::snprintf(out, n, "No data");
    return;
  }
  size_t k = 0;
  auto add = [&](const char* fmt, auto... a) {
    if (k + 1 >= n) return;
    if (k) k += static_cast<size_t>(std::snprintf(out + k, n - k, " \xC2\xB7 "));
    if (k + 1 < n) k += static_cast<size_t>(std::snprintf(out + k, n - k, fmt, a...));
    if (k >= n) k = n - 1;
  };
  out[0] = '\0';
  if (r.chapter >= 0) add("Chapter %d", r.chapter);
  if (r.difficulty >= 0) add("%s", game::difficulty_name(r.difficulty));
  if (r.saves >= 0) add("%d save%s", r.saves, r.saves == 1 ? "" : "s");
  if (r.clears > 0) add("NG+%s", r.clears > 1 ? "+" : "");
  if (r.unix_time > 0) {
    const time_t t = static_cast<time_t>(r.unix_time);
    tm local{};
    char when[32];
    if (localtime_s(&local, &t) == 0 && std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local)) add("%s", when);
  }
  if (!out[0]) std::snprintf(out, n, "a save");
}

void draw_save_files() {
  const savefiles::View v = savefiles::view();
  bool open_delete = false, open_copy = false;
  const bool hooked = events::save_list_hooked();
  const bool can = v.ready && hooked && v.up && !v.busy;
  char label[32], d[200];

  int used = 0;
  for (int i = 0; i < v.count; ++i) used += v.row[i].used;
  ImGui::Text("%s - %d save%s", v.separate_ways ? "Separate Ways" : "The main game", used, used == 1 ? "" : "s");
  help_marker(
      "The game's save slots, as the list beside this one shows them: the auto-save and the slots you save into "
      "(the main game's twenty, Separate Ways' ten). Delete removes a save - the game's own removal, and the slot "
      "reads No Data from then on. Copy to... puts a copy of a save into another slot of the same game, empty or not "
      "(a save already there is replaced): the game itself loads the save and writes it into the new slot, so the "
      "copy shows in the list at once and loads like the original, and the game then saves its settings, which keep "
      "the copy listed after a restart. The game's list is read again after each change. A copy made by an earlier "
      "version of the mod that shows as No Data: copy it again.\n\n"
      "Both change your saves straight away, so neither can be undone - back up your saves first if in doubt "
      "(Steam/userdata/<your id>/2050650/remote/win64_save/).");
  if (!v.ready) {
    ImGui::TextColored(kOrange, "%s", v.status[0] ? v.status : "The game's save lists were not understood - see the log.");
    return;
  }
  if (!hooked) ImGui::TextColored(kOrange, "The game's save list is not heard yet (see the log) - nothing can change.");
  else if (!v.copy_ok) ImGui::TextColored(kOrange, "The game's own copy of a save was not found - saves can be deleted, not copied.");
  if (v.count > 0 &&
      ImGui::BeginTable("saves", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Save", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int i = 0; i < v.count; ++i) {
      const savefiles::Row& r = v.row[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(slot_label(r, label, sizeof(label)));
      ImGui::TableNextColumn();
      describe_save(r, d, sizeof(d));
      if (r.used) ImGui::TextUnformatted(d);
      else ImGui::TextDisabled("%s", d);
      ImGui::TableNextColumn();
      if (r.used) {
        ImGui::BeginDisabled(!can || !v.copy_ok);
        if (ImGui::SmallButton("Copy to...")) ImGui::OpenPopup("copy to");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Delete")) {
          g_file_ask = kAskDelete;
          g_file_to = i;
          g_file_to_seen = r;
          open_delete = true;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("copy to")) {
          ImGui::TextDisabled("Copy %s to:", slot_label(r, label, sizeof(label)));
          ImGui::Separator();
          for (int j = 0; j < v.count; ++j) {
            if (j == i) continue;
            char dj[200], lj[32], item[260];
            describe_save(v.row[j], dj, sizeof(dj));
            std::snprintf(item, sizeof(item), "%s - %s", slot_label(v.row[j], lj, sizeof(lj)), dj);
            if (ImGui::Selectable(item)) {
              g_file_ask = kAskCopy;
              g_file_from = i;
              g_file_to = j;
              g_file_from_seen = r;
              g_file_to_seen = v.row[j];
              open_copy = true;
            }
          }
          ImGui::EndPopup();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  } else if (v.count == 0) {
    ImGui::TextDisabled("Reading the game's list...");
  }
  if (v.status[0]) {
    if (v.status_error) ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", v.status);
    if (v.status_error) ImGui::PopStyleColor();
  } else if (v.busy) {
    ImGui::TextDisabled("The game is busy with its saves...");
  }

  if (open_delete) ImGui::OpenPopup("delete this save?");
  if (open_copy) ImGui::OpenPopup("copy this save?");
  auto gone = [&](int row, const savefiles::Row& seen) {
    return v.up && !(row >= 0 && row < v.count && savefiles::same_save(v.row[row], seen));
  };
  if (ImGui::BeginPopupModal("delete this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskDelete || gone(g_file_to, g_file_to_seen) || v.screen < 0) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      describe_save(g_file_to_seen, d, sizeof(d));
      ImGui::Text("Delete the save in %s?", slot_label(g_file_to_seen, label, sizeof(label)));
      ImGui::TextUnformatted(d);
      ImGui::TextDisabled("It is removed from your saves (and Steam Cloud) straight away - this cannot be undone.");
      ImGui::BeginDisabled(!can);
      if (ImGui::Button("Delete")) {
        savefiles::request_delete(g_file_to, g_file_to_seen);
        logf("save files: delete %s asked for (panel)", label);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("copy this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskCopy || gone(g_file_from, g_file_from_seen) || gone(g_file_to, g_file_to_seen) ||
        v.screen < 0) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      char to[32];
      slot_label(g_file_from_seen, label, sizeof(label));
      slot_label(g_file_to_seen, to, sizeof(to));
      describe_save(g_file_from_seen, d, sizeof(d));
      if (g_file_to_seen.used) ImGui::Text("Copy %s over %s?", label, to);
      else ImGui::Text("Copy %s to the empty %s?", label, to);
      ImGui::TextUnformatted(d);
      if (g_file_to_seen.used) {
        describe_save(g_file_to_seen, d, sizeof(d));
        ImGui::TextColored(kOrange, "The save in %s is replaced: %s", to, d);
      }
      ImGui::TextDisabled("The copy is written to your saves (and Steam Cloud) straight away - this cannot be undone.");
      ImGui::BeginDisabled(!can);
      if (ImGui::Button("Copy")) {
        savefiles::request_copy(g_file_from, g_file_to, g_file_from_seen, g_file_to_seen);
        logf("save files: copy %s to %s asked for (panel)", label, to);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// --- the challenges ----------------------------------------------------------------------------------
// The tab shown (a group, or kTabAll / kTabIncomplete), the filter, whether the
// challenges the game keeps off its screen are listed; the "all" confirmation (1
// complete, 0 clear) and the challenges it names - the ones listed when it was
// clicked; a switch asked for, shown as asked until the game's data says so (or 1.5 s pass).
int g_ch_tab = challenges::kTabAll;
char g_ch_filter[64] = {};
bool g_ch_hidden = false;
int g_ch_ask = -1;
int g_ch_ask_ids[challenges::kMaxRows];
int g_ch_ask_n = 0, g_ch_ask_achievements = 0;
struct ChPending {
  int id = -1;
  bool on = false;
  DWORD at = 0;
};
ChPending g_ch_pending;

void forget_challenge_dialogs() {
  g_ch_ask = -1;
  g_ch_ask_n = 0;
}

// "1,234,567".
const char* thousands(int v, char* out, size_t n) {
  char digits[16];
  std::snprintf(digits, sizeof(digits), "%d", v < 0 ? -v : v);
  const size_t len = std::strlen(digits);
  size_t k = 0;
  if (v < 0 && k + 1 < n) out[k++] = '-';
  for (size_t i = 0; i < len && k + 1 < n; ++i) {
    if (i && (len - i) % 3 == 0 && k + 1 < n) out[k++] = ',';
    if (k + 1 < n) out[k++] = digits[i];
  }
  out[k] = '\0';
  return out;
}

// A challenge's condition with its goal in: "Defeat 50 enemies with a handgun."
void challenge_condition(const challenges::Row& r, char* out, size_t n) {
  const char* c = challenges::condition(r.id);
  if (!c) c = "";
  const char* at = std::strstr(c, "{0}");
  if (!at || r.goal < 0) {
    std::snprintf(out, n, "%s", c);
    return;
  }
  char goal[16];
  std::snprintf(out, n, "%.*s%s%s", static_cast<int>(at - c), c, thousands(r.goal, goal, sizeof(goal)), at + 3);
}

void challenge_name(const challenges::Row& r, char* out, size_t n) {
  if (const char* name = challenges::name(r.id)) std::snprintf(out, n, "%s%s", r.hidden ? "Hidden: " : "", name);
  else std::snprintf(out, n, "Challenge %d", r.id);
}

// "Costume: Jacket (Ashley)".
void unlock_text(const challenges::Unlock& u, char* out, size_t n) {
  const char* kind = challenges::bonus_kind(u.bonus);
  const char* name = challenges::bonus_name(u.bonus);
  const char* who = challenges::bonus_who(u.bonus);
  if (!name) std::snprintf(out, n, "Bonus %d", u.bonus);
  else if (who && who[0]) std::snprintf(out, n, "%s: %s (%s)", kind ? kind : "Bonus", name, who);
  else std::snprintf(out, n, "%s: %s", kind ? kind : "Bonus", name);
}

bool challenge_matches(const challenges::View& v, const challenges::Row& r, const char* needle) {
  if (!needle || !needle[0]) return true;
  char hay[1024], name[160], cond[256], line[160];
  challenge_name(r, name, sizeof(name));
  challenge_condition(r, cond, sizeof(cond));
  size_t used = static_cast<size_t>(std::snprintf(hay, sizeof(hay), "%s %s", name, cond));
  for (int k = 0; k < r.n_unlocks && used < sizeof(hay); ++k) {
    unlock_text(v.unlock[r.first_unlock + k], line, sizeof(line));
    used += static_cast<size_t>(std::snprintf(hay + used, sizeof(hay) - used, " %s", line));
  }
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (needle[i] && lower(p[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

// What a challenge unlocks, one line each (locked ones greyed); more than a few
// are counted, the list in the tooltip.
void draw_unlocks(const challenges::View& v, const challenges::Row& r) {
  char line[160];
  auto one = [&](const challenges::Unlock& u) {
    unlock_text(u, line, sizeof(line));
    if (u.bought) ImGui::Text("%s (bought)", line);
    else if (u.open) ImGui::TextUnformatted(line);
    else ImGui::TextDisabled("%s", line);
  };
  if (r.n_unlocks <= 3) {
    for (int k = 0; k < r.n_unlocks; ++k) {
      const challenges::Unlock& u = v.unlock[r.first_unlock + k];
      ImGui::PushTextWrapPos(0.0f);
      one(u);
      ImGui::PopTextWrapPos();
      if (u.kept && !u.bought && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("The game gives this otherwise too: it stays unlocked if this challenge is switched off.");
        ImGui::EndTooltip();
      }
    }
    return;
  }
  int open = 0;
  for (int k = 0; k < r.n_unlocks; ++k) open += v.unlock[r.first_unlock + k].open;
  ImGui::Text("%d shop items", r.n_unlocks);
  ImGui::SameLine();
  ImGui::TextDisabled("(%d unlocked)", open);
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    for (int k = 0; k < r.n_unlocks; ++k) one(v.unlock[r.first_unlock + k]);
    ImGui::EndTooltip();
  }
}

void draw_challenges() {
  static challenges::View v;  // large: one copy, refreshed every frame (the render thread's)
  v = challenges::view();
  if (!challenges::ready()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kOrange, "%s", challenges::status());
    ImGui::PopTextWrapPos();
    return;
  }
  if (!v.known) {
    ImGui::TextDisabled("%s", v.status[0] ? v.status : "Reading the game's challenges...");
    return;
  }
  const bool usable = v.up;
  int done_main = 0, all_main = 0, done_sw = 0, all_sw = 0, hidden = 0;
  for (int i = 0; i < v.count; ++i) {
    const challenges::Row& r = v.row[i];
    if (r.hidden) {
      ++hidden;
      continue;
    }
    if (r.group == challenges::kSeparateWays) all_sw += 1, done_sw += r.done;
    else all_main += 1, done_main += r.done;
  }
  char cp[24];
  ImGui::Text("Main story %d of %d", done_main, all_main);
  ImGui::SameLine();
  ImGui::TextDisabled("\xC2\xB7");
  ImGui::SameLine();
  ImGui::Text("Separate Ways %d of %d", done_sw, all_sw);
  if (v.cp >= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("\xC2\xB7");
    ImGui::SameLine();
    ImGui::Text("%s CP", thousands(v.cp, cp, sizeof(cp)));
  }
  help_marker(
      "Switch a challenge on and it is complete, as if you had earned it: the game's challenges screen shows it (move "
      "its cursor to see a change), it pays its CP, and what it unlocks can be bought in the Extra Content Shop "
      "(Professional mode opens straight away). A counted one's count is set to its goal. A challenge with a Steam "
      "achievement unlocks it - Steam keeps those for good. No NEW mark and no pop-up: the game shows those only "
      "for challenges you earn.\n\n"
      "Switch one off and it is incomplete again: the CP it paid is taken back (never below 0), the shop items it "
      "unlocked are locked again unless you bought them, another complete challenge unlocks them too or the game "
      "gives them otherwise, and a counted one's count drops just short of its goal, so the next one counted earns "
      "it again.\n\n"
      "Every change is saved at once.");

  if (ImGui::BeginTabBar("challenge tabs")) {
    for (int t = challenges::kTabAll; t < challenges::kGroups; ++t)
      if (ImGui::BeginTabItem(challenges::tab_name(t))) {
        g_ch_tab = t;
        ImGui::EndTabItem();
      }
    ImGui::EndTabBar();
  }

  // The rows the tab, the filter and the hidden switch show.
  static int listed[challenges::kMaxRows];
  int n_listed = 0, listed_on = 0, listed_achievements = 0;
  for (int i = 0; i < v.count; ++i) {
    const challenges::Row& r = v.row[i];
    if ((r.hidden && !g_ch_hidden) || (g_ch_tab >= 0 && r.group != g_ch_tab) ||
        (g_ch_tab == challenges::kTabIncomplete && r.done) || !challenge_matches(v, r, g_ch_filter))
      continue;
    listed[n_listed++] = i;
    listed_on += r.done;
    listed_achievements += r.achievement > 0 && !r.done;
  }
  ImGui::BeginDisabled(!usable || n_listed == 0);
  if (ImGui::SmallButton("Complete all")) {
    g_ch_ask = 1;
    g_ch_ask_n = 0;
    for (int k = 0; k < n_listed; ++k)
      if (!v.row[listed[k]].done) g_ch_ask_ids[g_ch_ask_n++] = v.row[listed[k]].id;
    g_ch_ask_achievements = listed_achievements;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear all")) {
    g_ch_ask = 0;
    g_ch_ask_n = 0;
    for (int k = 0; k < n_listed; ++k)
      if (v.row[listed[k]].done) g_ch_ask_ids[g_ch_ask_n++] = v.row[listed[k]].id;
    g_ch_ask_achievements = 0;
  }
  ImGui::EndDisabled();
  item_help("Every challenge listed below - the tab's, and the filter's when you type one.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  ImGui::InputTextWithHint("##challenges_filter", "filter", g_ch_filter, sizeof(g_ch_filter));
  if (hidden) {
    ImGui::SameLine();
    ImGui::Checkbox("Hidden challenges", &g_ch_hidden);
    item_help("Challenges the game keeps off its screen but still awards: the Assisted S+ ranks, which unlock the "
              "Wolf Tail (Leon) and the Wolf Set (Ada).");
  }
  if (!usable) ImGui::TextColored(kOrange, "The game's challenges screen is closing - nothing can change now.");
  if (!v.achievements)
    ImGui::TextColored(kOrange, "The game's achievement call was not found - challenges switched on unlock none.");

  // The confirmation for "all".
  if (g_ch_ask >= 0) ImGui::OpenPopup("challenges: all");
  if (ImGui::BeginPopupModal("challenges: all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_ch_ask < 0 || !usable) {
      // Forgotten (the screen or the panel went away while it was up), or nothing can change now.
      forget_challenge_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
      if (g_ch_ask_n == 0) {
        ImGui::Text("Every challenge listed is %s already.", g_ch_ask == 1 ? "complete" : "incomplete");
      } else if (g_ch_ask == 1) {
        ImGui::Text("Complete the %d challenge%s listed that %s not?", g_ch_ask_n, g_ch_ask_n == 1 ? "" : "s",
                    g_ch_ask_n == 1 ? "is" : "are");
        ImGui::TextUnformatted("Each pays its CP and unlocks its shop items.");
        if (!v.achievements_known)
          ImGui::TextColored(kOrange, "Every one that carries a Steam achievement unlocks it - Steam keeps them for good.");
        else if (g_ch_ask_achievements)
          ImGui::TextColored(kOrange, "%d Steam achievement(s) will be unlocked - Steam keeps them for good.",
                             g_ch_ask_achievements);
      } else {
        ImGui::Text("Clear the %d complete challenge%s listed?", g_ch_ask_n, g_ch_ask_n == 1 ? "" : "s");
        ImGui::TextUnformatted("Their CP is taken back (never below 0) and their shop items are locked again, unless "
                               "bought or unlocked otherwise. Steam achievements stay.");
      }
      ImGui::PopTextWrapPos();
      ImGui::BeginDisabled(g_ch_ask_n == 0);
      if (ImGui::Button(g_ch_ask == 1 ? "Complete all" : "Clear all")) {
        challenges::request_all(g_ch_ask_ids, g_ch_ask_n, g_ch_ask == 1);
        logf("challenges: %s %d asked for (panel)", g_ch_ask == 1 ? "complete" : "clear", g_ch_ask_n);
        forget_challenge_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_challenge_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
  if (ImGui::BeginTable("challenges", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0.0f, -footer))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Challenge", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("Rewards", ImGuiTableColumnFlags_WidthStretch, 0.33f);
    ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 0.12f);
    ImGui::TableHeadersRow();
    const DWORD now = GetTickCount();
    char name[160], cond[256];
    for (int k = 0; k < n_listed; ++k) {
      const challenges::Row& r = v.row[listed[k]];
      challenge_name(r, name, sizeof(name));
      challenge_condition(r, cond, sizeof(cond));
      ImGui::TableNextRow();
      ImGui::PushID(r.id);
      ImGui::TableNextColumn();
      bool on = r.done;
      if (g_ch_pending.id == r.id && g_ch_pending.on != r.done && now - g_ch_pending.at < 1500) on = g_ch_pending.on;
      ImGui::BeginDisabled(!usable);
      if (ImGui::Checkbox("##on", &on)) {
        challenges::request(r.id, on);
        g_ch_pending = ChPending{r.id, on, now};
        logf("challenges: \"%s\" %s (panel)", name, on ? "on" : "off");
      }
      ImGui::EndDisabled();
      ImGui::TableNextColumn();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(name);
      if (r.achievement > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kBlue, "[achievement]");
      }
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", cond);
      ImGui::PopTextWrapPos();
      ImGui::EndGroup();
      if (r.achievement > 0 && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Switching this on unlocks its Steam achievement (for good).");
        ImGui::EndTooltip();
      }
      ImGui::TableNextColumn();
      if (r.cp > 0) ImGui::Text("%s CP", thousands(r.cp, cp, sizeof(cp)));
      draw_unlocks(v, r);
      ImGui::TableNextColumn();
      if (r.progress >= 0 && r.goal > 1) ImGui::Text("%d / %d", r.progress, r.goal);
      ImGui::PopID();
    }
    if (n_listed == 0) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("No challenge here matches.");
    }
    ImGui::EndTable();
  }
  if (v.status[0]) {
    if (v.status_error) ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", v.status);
    if (v.status_error) ImGui::PopStyleColor();
  }
  if (v.saving) ImGui::TextDisabled("Saving...");
  ImGui::TextDisabled("%d of %d listed complete. The game's list shows a change once you move its cursor.  F%d hides "
                      "this panel.",
                      listed_on, n_listed, config::get().toggle_key - 0x6F);
}

// --- the Extra Content Shop --------------------------------------------------------------------------------
// The tab shown (exshop::Tab) and the filter; the "all" confirmation (1 unlock,
// 0 lock) and the items it names - the ones listed when it was clicked; a switch
// asked for, shown as asked until the game's data says so (or 1.5 s pass).
int g_shop_tab = exshop::kAll;
char g_shop_filter[64] = {};
int g_shop_ask = -1;
int g_shop_ask_ids[exshop::kMaxRows];
int g_shop_ask_n = 0, g_shop_ask_bought = 0;
ChPending g_shop_pending;

void forget_shop_dialogs() {
  g_shop_ask = -1;
  g_shop_ask_n = 0;
}

// "Leon's Jacket", "Ashley's Jacket", "Leon (Jacket)" (a model).
void shop_item_name(int id, char* out, size_t n) {
  const char* name = exshop::name(id);
  const char* who = exshop::who(id);
  if (!name) std::snprintf(out, n, "Item %d", id);
  else if (who && who[0]) std::snprintf(out, n, "%s's %s", who, name);
  else std::snprintf(out, n, "%s", name);
}

// The line under an item's name: its kind (Separate Ways' under All too) and,
// while it is locked, what the shop says unlocks it.
void shop_item_detail(const exshop::Row& r, char* out, size_t n) {
  const char* kind = exshop::kind(r.id);
  const char* lock = r.state == exshop::kLocked ? exshop::lock_text(r.id) : nullptr;
  std::snprintf(out, n, "%s%s%s%s", kind ? kind : "Item",
                g_shop_tab == exshop::kAll && r.tab == exshop::kSeparateWays ? ", Separate Ways" : "",
                lock ? " - " : "", lock ? lock : "");
}

bool shop_matches(const exshop::Row& r, const char* query) {
  if (!query || !query[0]) return true;
  char hay[400], name[160];
  shop_item_name(r.id, name, sizeof(name));
  const char* kind = exshop::kind(r.id);
  const char* lock = exshop::lock_text(r.id);
  std::snprintf(hay, sizeof(hay), "%s %s %s%s", name, kind ? kind : "", lock ? lock : "",
                r.tab == exshop::kSeparateWays ? " Separate Ways" : "");
  return item_matches(r.id, hay, query);
}

// An item the shop lists: its tab is one the shop shows (Separate Ways' items only with that DLC).
bool shop_lists(const exshop::View& v, const exshop::Row& r) { return r.tab < 0 || v.tab[r.tab]; }

void draw_shop() {
  static exshop::View v;  // large: one copy, refreshed every frame (the render thread's)
  v = exshop::view();
  if (!exshop::ready()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kOrange, "%s", exshop::status());
    ImGui::PopTextWrapPos();
    return;
  }
  if (!v.known) {
    ImGui::TextDisabled("%s", v.status[0] ? v.status : "Reading the shop's items...");
    return;
  }
  const bool usable = v.up;
  int all = 0, unlocked = 0, bought = 0;
  for (int i = 0; i < v.count; ++i) {
    const exshop::Row& r = v.row[i];
    if (!shop_lists(v, r)) continue;
    ++all;
    unlocked += r.unlocked;
    bought += r.bought;
  }
  char cp[24];
  ImGui::Text("Unlocked %d of %d", unlocked, all);
  ImGui::SameLine();
  ImGui::TextDisabled("\xC2\xB7");
  ImGui::SameLine();
  ImGui::Text("Purchased %d", bought);
  if (v.cp >= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("\xC2\xB7");
    ImGui::SameLine();
    ImGui::Text("%s CP", thousands(v.cp, cp, sizeof(cp)));
  }
  char help[720];
  std::snprintf(help, sizeof(help),
                "Tick an item and it is unlocked: the shop sells it, and you buy it there with your CP as usual - "
                "nothing is bought here, and your CP stays as it is. Untick it to lock it again (not one you have "
                "purchased).\n\n"
                "Unlock all and Lock all take every item listed below - the tab's, and the filter's when you type one "
                "- asked once for all of them.\n\n"
                "%s No NEW mark: the game puts those only on what it unlocks itself. Every change is saved at once.",
                v.instant ? "The shop's own list shows each change at once."
                          : "The shop's own list shows a change once you switch its tab (or open the shop again).");
  help_marker(help);

  if (ImGui::BeginTabBar("shop tabs")) {
    for (int t = exshop::kAll; t < exshop::kTabs; ++t)
      if (v.tab[t] && ImGui::BeginTabItem(exshop::tab_name(t))) {
        g_shop_tab = t;
        ImGui::EndTabItem();
      }
    ImGui::EndTabBar();
  }

  // The rows the tab and the filter show.
  static int listed[exshop::kMaxRows];
  int n_listed = 0, listed_on = 0;
  for (int i = 0; i < v.count; ++i) {
    const exshop::Row& r = v.row[i];
    if (!shop_lists(v, r) || (g_shop_tab != exshop::kAll && r.tab != g_shop_tab) || !shop_matches(r, g_shop_filter))
      continue;
    listed[n_listed++] = i;
    listed_on += r.unlocked;
  }
  ImGui::BeginDisabled(!usable || n_listed == 0);
  if (ImGui::SmallButton("Unlock all")) {
    g_shop_ask = 1;
    g_shop_ask_n = g_shop_ask_bought = 0;
    for (int k = 0; k < n_listed; ++k)
      if (!v.row[listed[k]].unlocked) g_shop_ask_ids[g_shop_ask_n++] = v.row[listed[k]].id;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Lock all")) {
    g_shop_ask = 0;
    g_shop_ask_n = g_shop_ask_bought = 0;
    for (int k = 0; k < n_listed; ++k) {
      const exshop::Row& r = v.row[listed[k]];
      if (r.bought) ++g_shop_ask_bought;
      else if (r.unlocked) g_shop_ask_ids[g_shop_ask_n++] = r.id;
    }
  }
  ImGui::EndDisabled();
  item_help("Every item listed below - the tab's, and the filter's when you type one.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  ImGui::InputTextWithHint("##shop_filter", "filter", g_shop_filter, sizeof(g_shop_filter));
  if (!usable) ImGui::TextColored(kOrange, "The shop is closing - nothing can change now.");
  if (v.dlc) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kOrange, "A DLC you own has every item of the main story unlocked and purchased - only "
                                "Separate Ways' items can change here.");
    ImGui::PopTextWrapPos();
  }

  // The confirmation for "all".
  if (g_shop_ask >= 0) ImGui::OpenPopup("shop: all");
  if (ImGui::BeginPopupModal("shop: all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_shop_ask < 0 || !usable) {
      // Forgotten (the shop or the panel went away while it was up), or nothing can change now.
      forget_shop_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      const bool unlock = g_shop_ask == 1;
      const int n = g_shop_ask_n;
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
      if (n == 0 && unlock) {
        ImGui::TextUnformatted("Every item listed is unlocked already.");
      } else if (n == 0) {
        ImGui::TextUnformatted(g_shop_ask_bought ? "Every unlocked item listed is purchased: they stay unlocked."
                                                 : "Every item listed is locked already.");
      } else if (unlock) {
        ImGui::Text("Unlock the %d item%s listed that %s locked?", n, n == 1 ? "" : "s", n == 1 ? "is" : "are");
        ImGui::TextUnformatted("The shop then sells them for CP. Nothing is bought, and your CP stays as it is.");
      } else {
        ImGui::Text("Lock the %d unlocked item%s listed again?", n, n == 1 ? "" : "s");
        if (g_shop_ask_bought) ImGui::Text("The %d purchased stay unlocked.", g_shop_ask_bought);
      }
      ImGui::PopTextWrapPos();
      char button[32];
      std::snprintf(button, sizeof(button), "%s %d", unlock ? "Unlock" : "Lock", n);
      ImGui::BeginDisabled(n == 0);
      if (ImGui::Button(button)) {
        exshop::request_all(g_shop_ask_ids, n, unlock);
        logf("shop: %s %d asked for (panel)", unlock ? "unlock" : "lock", n);
        forget_shop_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_shop_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
  if (ImGui::BeginTable("shop items", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0.0f, -footer))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Unlocked", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 0.66f);
    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch, 0.14f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableHeadersRow();
    const DWORD now = GetTickCount();
    char name[160], detail[320];
    for (int k = 0; k < n_listed; ++k) {
      const exshop::Row& r = v.row[listed[k]];
      shop_item_name(r.id, name, sizeof(name));
      shop_item_detail(r, detail, sizeof(detail));
      ImGui::TableNextRow();
      ImGui::PushID(r.id);
      ImGui::TableNextColumn();
      bool on = r.unlocked;
      if (g_shop_pending.id == r.id && g_shop_pending.on != r.unlocked && now - g_shop_pending.at < 1500)
        on = g_shop_pending.on;
      ImGui::BeginDisabled(!usable || r.bought);
      if (ImGui::Checkbox("##unlocked", &on)) {
        exshop::request(r.id, on);
        g_shop_pending = ChPending{r.id, on, now};
        logf("shop: \"%s\" %s (panel)", name, on ? "unlocked" : "locked");
      }
      ImGui::EndDisabled();
      if (r.bought) item_help(r.by_dlc ? "A DLC you own has this unlocked and purchased." : "Purchased - it stays unlocked.");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", detail);
      ImGui::PopTextWrapPos();
      ImGui::TableNextColumn();
      ImGui::Text("%s CP", thousands(r.cp, cp, sizeof(cp)));
      ImGui::TableNextColumn();
      if (r.state == exshop::kPurchased) ImGui::TextColored(kBlue, "%s", r.by_dlc ? "Purchased (DLC)" : "Purchased");
      else if (r.state == exshop::kShort) ImGui::TextColored(kOrange, "Not enough CP");
      else if (r.state == exshop::kForSale) ImGui::TextUnformatted("For sale");
      else ImGui::TextDisabled("Locked");
      ImGui::PopID();
    }
    if (n_listed == 0) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("No item here matches.");
    }
    ImGui::EndTable();
  }
  if (v.status[0]) {
    if (v.status_error) ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", v.status);
    if (v.status_error) ImGui::PopStyleColor();
  }
  if (v.saving) ImGui::TextDisabled("Saving...");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%d of %d listed unlocked.%s  F%d hides this panel.", listed_on, n_listed,
                      v.instant ? "" : " The shop's list shows a change once you switch its tab.",
                      config::get().toggle_key - 0x6F);
  ImGui::PopTextWrapPos();
}

// A section heading that folds, remembered in the layout file.
bool section(const char* label, bool* open) {
  ImGui::SetNextItemOpen(*open, ImGuiCond_Always);
  const bool now = ImGui::CollapsingHeader(label);
  if (now != *open) {
    *open = now;
    ImGui::MarkIniSettingsDirty();
  }
  return now;
}

// A section within a section (a list's Add): its heading and what it holds set
// in under the parent's, folding on its own.
template <typename Draw>
void subsection(const char* label, bool* open, Draw&& draw) {
  const float indent = ImGui::GetStyle().IndentSpacing;
  ImGui::Spacing();
  ImGui::Indent(indent);
  if (section(label, open)) draw();
  ImGui::Unindent(indent);
}

// The item sections: the attache case, the game's item box (`box`, null in a
// game with none), the key items, the treasures and the other items, each list
// with its Add inside it.
void draw_items(const inventory::View& iv, const itembox::View* box) {
  char head[64];
  std::snprintf(head, sizeof(head), "Attache case  (%d)###case", iv.count[inventory::kCase]);
  if (section(head, &g_case_open)) {
    draw_case(iv);
    if (iv.known) subsection("Add to the case###add", &g_add_open, [&] { draw_add(iv); });
  }
  if (box) {
    std::snprintf(head, sizeof(head), "Item box  (%d)###box",
                  iv.count[inventory::kStorage] + static_cast<int>(box->entry.size()));
    if (section(head, &g_box_open)) draw_box(iv, *box);
  }
  std::snprintf(head, sizeof(head), "Key items  (%d)###key", iv.count[inventory::kKeyItems]);
  if (section(head, &g_key_open)) {
    draw_offgrid(iv, inventory::kKeyItems);
    if (iv.known)
      subsection("Add key items###keyadd", &g_key_add_open, [&] {
        draw_picker(g_key_add, iv, false, accept_key, 7.5f,
                    "The key items of this game you do not have yet. Tick the ones you want and press Add - asked "
                    "once for all of them: they join the list above, as a pickup of each would. A key item the story "
                    "has not reached yet may not do anything until it has.");
      });
  }
  std::snprintf(head, sizeof(head), "Treasures  (%d)###treasures", iv.count[inventory::kTreasures]);
  if (section(head, &g_treasures_open)) {
    draw_offgrid(iv, inventory::kTreasures);
    if (iv.known)
      subsection("Add treasures###treasuresadd", &g_treasures_add_open, [&] {
        draw_picker(g_treasure_add, iv, false, accept_treasure, 7.5f,
                    "The treasures and gems of this game, and the ones you hold fewer of than one entry holds. Tick "
                    "the ones you want, say how many of each, and press Add - asked once for all of them: they join "
                    "the list above.");
      });
  }
  std::snprintf(head, sizeof(head), "Other items  (%d)###unique", iv.count[inventory::kUnique]);
  if (section(head, &g_unique_open)) {
    draw_offgrid(iv, inventory::kUnique);
    if (iv.known)
      subsection("Add other items###uniqueadd", &g_unique_add_open, [&] {
        draw_picker(g_unique_add, iv, false, accept_unique, 7.5f,
                    "Recipes, charms, treasure maps, attache cases (the cases' looks and perks) and armor of this game "
                    "you do not have yet. Tick the ones you want (Tick all listed ticks every one) and press Add - "
                    "asked once for all of them.");
      });
  }
}

// Once, before either window is first drawn (ImGui has read the layout file by
// then): a size kept at another scale is brought to this one.
void settle_layout() {
  if (g_layout_percent == kScalePercent) return;
  const float r = static_cast<float>(kScalePercent) / static_cast<float>(g_layout_percent);
  for (const char* name : {kPanelWindow, kFilesWindow, kChallengesWindow, kShopWindow})
    if (ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(ImHashStr(name))) {
      logf("panel: a saved size, %dx%d at %d%%, brought to %d%%", ws->Size.x, ws->Size.y, g_layout_percent,
           kScalePercent);
      ws->Size = ImVec2ih(static_cast<short>(ws->Size.x * r), static_cast<short>(ws->Size.y * r));
    }
  g_layout_percent = kScalePercent;
  ImGui::MarkIniSettingsDirty();
}

void forget_dialogs() {
  forget_item_dialogs();
  forget_file_dialogs();
  forget_challenge_dialogs();
  forget_shop_dialogs();
  forget_edits();
}

}  // namespace

// --- context --------------------------------------------------------------------------------------------
bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sRE4CabbyCodes.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  // ImGui writes the file a moment after a change: half a second, not its usual
  // five, so a game closed straight after a click loses nothing.
  io.IniSavingRate = 0.5f;
  // The panel's own lines in that file, registered before the first frame,
  // which is when ImGui reads it.
  ImGuiSettingsHandler panel{};
  panel.TypeName = "RE4CabbyCodes";
  panel.TypeHash = ImHashStr("RE4CabbyCodes");
  panel.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
    return std::strcmp(name, "Panel") == 0 ? &g_cheats_open : nullptr;
  };
  panel.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    static const struct {
      const char* key;
      bool* flag;
    } kFlags[] = {{"CheatsOpen", &g_cheats_open},       {"MercenariesOpen", &g_mercs_open},
                  {"TimeOpen", &g_time_open},           {"ResultsOpen", &g_results_open},
                  {"CaseOpen", &g_case_open},           {"AddOpen", &g_add_open},
                  {"ItemBoxOpen", &g_box_open},         {"KeyOpen", &g_key_open},
                  {"KeyAddOpen", &g_key_add_open},      {"TreasuresOpen", &g_treasures_open},
                  {"TreasuresAddOpen", &g_treasures_add_open}, {"OtherOpen", &g_unique_open},
                  {"OtherAddOpen", &g_unique_add_open}};
    char key[32];
    int v = 0;
    if (std::sscanf(line, "%31[^=]=%d", key, &v) != 2) return;
    if (std::strcmp(key, "ScalePercent") == 0) {
      if (v > 0) g_layout_percent = v;
      return;
    }
    for (const auto& f : kFlags)
      if (std::strcmp(key, f.key) == 0) *f.flag = v != 0;
  };
  panel.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out) {
    out->appendf("[%s][Panel]\nCheatsOpen=%d\nMercenariesOpen=%d\nTimeOpen=%d\nResultsOpen=%d\nCaseOpen=%d\nAddOpen=%d\n"
                 "ItemBoxOpen=%d\nKeyOpen=%d\nKeyAddOpen=%d\nTreasuresOpen=%d\nTreasuresAddOpen=%d\nOtherOpen=%d\n"
                 "OtherAddOpen=%d\nScalePercent=%d\n\n",
                 h->TypeName, g_cheats_open, g_mercs_open, g_time_open, g_results_open, g_case_open, g_add_open,
                 g_box_open, g_key_open, g_key_add_open, g_treasures_open, g_treasures_add_open, g_unique_open,
                 g_unique_add_open, g_layout_percent);
  };
  ImGui::AddSettingsHandler(&panel);
  // No keyboard navigation: the game's menus keep their keys. Dragging is
  // limited to the title bar so a click on the panel's background cannot pick
  // the window up by accident. The render thread must not set the window's
  // cursor (it belongs to the window's thread): WM_SETCURSOR does that.
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.ScaleAllSizes(kScale);  // the padding, spacing and the rest; the font has its own factor
  style.FontScaleMain = kScale;
  style.MouseCursorScale = kScale;  // the cursor the panel draws (Wine) at the panel's size
  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_wake_msg = RegisterWindowMessageA("RE4CabbyCodes.PanelVisibility");
  g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  keyboard::start(hwnd);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  char cls[64] = "?";
  GetClassNameA(hwnd, cls, sizeof(cls));
  logf("overlay ready (hwnd=%p class '%s' %ldx%ld, scale %d%%, render thread %lu, window thread %lu)",
       static_cast<void*>(hwnd), cls, rc.right - rc.left, rc.bottom - rc.top, kScalePercent, GetCurrentThreadId(),
       GetWindowThreadProcessId(hwnd, nullptr));
  return true;
}

void pointer_report(char* out, size_t n) {
  if (n) out[0] = '\0';
  const HWND w = g_hwnd;
  if (!w || !n) return;
  const HWND fg = GetForegroundWindow();
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  const bool got = GetCursorInfo(&ci) != FALSE;
  RECT client{}, clip{};
  POINT origin{0, 0};
  GetClientRect(w, &client);
  ClientToScreen(w, &origin);
  OffsetRect(&client, origin.x, origin.y);
  GetClipCursor(&clip);
  const long locks = g_clip_rect_calls.exchange(0, std::memory_order_relaxed);
  const long kept = g_clip_kept_calls.exchange(0, std::memory_order_relaxed);
  const long frees = g_clip_free_calls.exchange(0, std::memory_order_relaxed);
  const long raw = g_raw_mouse.exchange(0, std::memory_order_relaxed);
  const int last = g_clip_last.load(std::memory_order_relaxed);
  std::snprintf(out, n,
                "the game's window is %s; the cursor is %s at (%ld,%ld), %s the window (%ld,%ld)-(%ld,%ld); the clip "
                "(%ld,%ld)-(%ld,%ld), screen %dx%d; since the last report the game locked the pointer %ld time(s) (%ld of "
                "them to the clip already in place, not passed on) and let it go %ld time(s) (last: %s), and %ld raw "
                "input packet(s) reached its window",
                fg == w ? "the foreground window" : fg ? "NOT the foreground window" : "NOT the foreground window (none is)",
                !got ? "?" : (ci.flags & CURSOR_SHOWING) ? "shown" : "hidden", got ? ci.ptScreenPos.x : -1,
                got ? ci.ptScreenPos.y : -1, got && PtInRect(&client, ci.ptScreenPos) ? "inside" : "outside", client.left,
                client.top, client.right, client.bottom, clip.left, clip.top, clip.right, clip.bottom,
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), locks, kept, frees,
                last < 0 ? "none" : last ? "locked" : "let go", raw);
}

bool capturing_mouse() { return g_capture_mouse != 0; }
bool capturing_keyboard() { return g_capture_keyboard != 0; }
bool visible() { return g_visible != 0; }

void lock_imgui() {
  if (g_imgui_cs_ready) EnterCriticalSection(&g_imgui_cs);
}
void unlock_imgui() {
  if (g_imgui_cs_ready) LeaveCriticalSection(&g_imgui_cs);
}

bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const Place where = place(s);
  const int save_list = s.ticking && s.age_ms < 2000 ? s.save_list : -1;
  const bool challenges_up = s.ticking && s.age_ms < 2000 && s.challenges;
  const bool shop_up = s.ticking && s.age_ms < 2000 && s.shop;
  if (where != g_was_place || save_list != g_was_saves || challenges_up != g_was_challenges || shop_up != g_was_shop) {
    // A confirmation belongs to the screen it was asked beside.
    if (save_list != g_was_saves) forget_file_dialogs();
    if (challenges_up != g_was_challenges) forget_challenge_dialogs();
    if (shop_up != g_was_shop) forget_shop_dialogs();
    g_was_place = where;
    g_was_saves = save_list;
    g_was_challenges = challenges_up;
    g_was_shop = shop_up;
    g_user_hidden = false;  // the next time a window's screen opens, its window is back
    g_forced = false;
  }
  const bool focused = GetForegroundWindow() == g_hwnd;
  if (keyboard::active()) {
    // The panel's own keyboard: what was typed since the last frame goes to
    // the panel while it is up and the game has the focus, and the toggle key
    // is read from it (only while the game has the focus: the device hears
    // keys in the background too).
    const int presses = keyboard::pump(g_visible != 0 && focused, config::get().toggle_key);
    for (int i = 0; i < presses && focused; ++i) toggle(where, "");
  } else if (!g_legacy_keys && g_context_ready) {
    // No WM_KEYDOWN has reached the window (the game's DirectInput keyboard
    // taken exclusively?): the toggle key is read here instead.
    static bool was_down = false;
    const bool down = focused && (GetAsyncKeyState(config::get().toggle_key) & 0x8000) != 0;
    if (down && !was_down) {
      g_polled_toggle_at = GetTickCount();
      toggle(where, " (read from the keyboard)");
    }
    was_down = down;
  }
  const bool vis = g_context_ready && (where == kAtScreen ? !g_user_hidden : g_forced && where != kOutOfGame);
  if (InterlockedExchange(&g_visible, vis ? 1 : 0) != (vis ? 1 : 0) && g_hwnd) {
    if (!vis) {
      forget_dialogs();
      // What ImGui still has to write of the layout (a window or a section folded a moment ago) is written now.
      if (g_context_ready && GImGui->SettingsDirtyTimer > 0.0f && GImGui->IO.IniFilename)
        ImGui::SaveIniSettingsToDisk(GImGui->IO.IniFilename);
    }
    if (g_wake_msg) PostMessageW(g_hwnd, g_wake_msg, vis ? 1 : 0, 0);
  }
  if (!vis) {
    InterlockedExchange(&g_capture_mouse, 0);
    InterlockedExchange(&g_capture_keyboard, 0);
  }
  ImGuiIO& io = ImGui::GetIO();
  io.MouseDrawCursor = vis && g_draw_cursor;
  if (vis) {
    // The physical buttons are the truth: a press or release the window never
    // got as a message (Raw Input can take them) is still the panel's.
    // GetAsyncKeyState reads the physical buttons, the messages say the logical
    // ones: with the buttons swapped in Windows' mouse settings (left-handed),
    // the panel's left button is the physical right one.
    static const int kVk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    for (int b = 0; b < 3; ++b) {
      const int vk = swapped && b < 2 ? kVk[1 - b] : kVk[b];
      const bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
      if (io.MouseDown[b] != down) io.AddMouseButtonEvent(b, down);
    }
  } else if (g_context_ready) {
    // Input made while the panel is away is not the panel's.
    io.ClearEventsQueue();
    io.ClearInputMouse();
    io.ClearInputKeys();
  }
  return vis;
}

void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const ImGuiIO& io = ImGui::GetIO();
  InterlockedExchange(&g_capture_mouse, io.WantCaptureMouse ? 1 : 0);
  InterlockedExchange(&g_capture_keyboard, io.WantTextInput ? 1 : 0);
  if (!g_layout_settled) {
    g_layout_settled = true;
    settle_layout();
  }
  // The challenges screen has the challenges, in a window of their own (it opens
  // over the pause menu too), the Extra Content Shop its items, a load or save
  // list the save files (nothing else on the panel applies there); everything
  // else is the pause panel. Resizable, not auto-fitted, never bigger than the
  // display; each folded or open as it was left (ImGui keeps that in the layout
  // file, so a restart keeps it too).
  const bool fresh = s.ticking && s.age_ms < 2000;
  const bool chal = fresh && s.challenges;
  const bool shop = !chal && fresh && s.shop;
  const bool files = !chal && !shop && fresh && s.save_list >= 0;
  const char* const window = chal ? kChallengesWindow : shop ? kShopWindow : files ? kFilesWindow : kPanelWindow;
  const float width = chal || shop ? scaled(740) : scaled(600);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - width - scaled(20), scaled(40)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(width, scaled(chal || shop ? 680 : 640)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(scaled(360), scaled(200)), io.DisplaySize);
  if (!ImGui::Begin(window, nullptr)) {
    ImGui::End();
    return;
  }
  keep_in_reach(io.DisplaySize);
  const char* problem = nullptr;
  char why[300];
  if (!re::ready()) {
    std::snprintf(why, sizeof(why), "Game hooks: %s", re::status());
    problem = why;
  } else if (!s.ticking) {
    std::snprintf(why, sizeof(why), "Game tick: %s", app::status());
    problem = why;
  } else if (s.age_ms >= 2000) {
    std::snprintf(why, sizeof(why), "Game tick: the game has not called the mod for %lu s", s.age_ms / 1000);
    problem = why;
  } else if (!s.game_ready) {
    std::snprintf(why, sizeof(why), "Game hooks: %s", game::status_text());
    problem = why;
  }
  if (problem) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kOrange, "%s", problem);
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }
  if (chal) {
    draw_challenges();
    ImGui::End();
    return;
  }
  if (shop) {
    draw_shop();
    ImGui::End();
    return;
  }
  if (files) {
    draw_save_files();
    ImGui::Separator();
    ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
    ImGui::End();
    return;
  }
  const game::Run& run = s.run;
  if (!s.show_panel && !config::get().always_show) {
    ImGui::TextDisabled("Opened outside the pause menu - F%d closes it", config::get().toggle_key - 0x6F);
    ImGui::Separator();
  }
  if (!run.in_game) {
    ImGui::TextDisabled("Waiting for a game (start a new game or load a save)...");
    ImGui::Separator();
  } else {
    ImGui::TextDisabled("Playing %s", game::mode_name(run.mode));
  }

  const cheats::Status st = cheats::status();
  {
    // The switches that are on, counted in their section's heading, so a folded
    // section still says what it holds.
    int on = 0, results_on = 0, merc_on = 0, time_on = 0;
    for (int k = 0; k < cheats::kCount; ++k) {
      const auto kind = static_cast<cheats::Kind>(k);
      if (!cheats::enabled(kind)) continue;
      if (is_results_switch(kind)) ++results_on;
      else if (is_merc_switch(kind)) ++merc_on;
      else if (is_time_switch(kind)) ++time_on;
      else ++on;
    }
    char head[64];
    std::snprintf(head, sizeof(head), "Cheats  (%d on)###cheats", on);
    if (section(head, &g_cheats_open)) draw_cheats(st);
    if (run.mode == game::Mode::kMercenaries) {
      std::snprintf(head, sizeof(head), "The Mercenaries  (%d on)###mercs", merc_on + cheats::enabled(cheats::kFreezeCountdown));
      if (section(head, &g_mercs_open)) draw_mercs(st);
    } else {
      std::snprintf(head, sizeof(head), "Saves, time & money  (%d on)###time", time_on);
      if (section(head, &g_time_open)) draw_time_money(st);
      std::snprintf(head, sizeof(head), "Results  (%d on)###results", results_on);
      if (section(head, &g_results_open)) draw_results(st);
    }
  }
  if (run.in_game) {
    // Large: one copy, taken again only when the tick has published another.
    static inventory::View iv;
    static unsigned iv_generation = 0;
    static bool iv_taken = false;
    const unsigned generation = inventory::view_generation();  // (before the copy: a later one is taken next frame)
    if (!iv_taken || generation != iv_generation) {
      inventory::view(&iv);
      iv_generation = generation;
      iv_taken = true;
      forget_gone(iv);
    }
    g_view_gen = iv_generation;
    inventory::lists_drawn();
    g_mode = run.mode;
    const bool boxed = inventory::has_box(run.mode);
    const itembox::View box = boxed ? itembox::view() : itembox::View{};
    if (boxed) forget_gone_box(box);
    g_note_placed = false;
    draw_items(iv, boxed ? &box : nullptr);
    draw_item_dialogs(iv);
    // Only what went wrong says so here (unless it said so beside the list it
    // was asked from): what a change did shows in the lists themselves, and the
    // log keeps every one.
    if (iv.note_error && iv.note[0] && !g_note_placed) {
      ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
      ImGui::TextWrapped("%s", iv.note);
      ImGui::PopStyleColor();
    }
  }
  ImGui::Separator();
  ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  InterlockedExchange(&g_visible, 0);
}

void install_early() {
  if (!g_imgui_cs_ready) {
    InitializeCriticalSection(&g_imgui_cs);
    InitializeCriticalSection(&g_cursor_cs);
    g_imgui_cs_ready = true;
  }
  if (config::get().disable_overlay) return;
  d3d::install_early();
  // The panel's cursor (the pointer section above): its own under Wine.
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
  g_draw_cursor = config::get().disable_cursor || wine;
  if (wine && !config::get().disable_cursor)
    logf("overlay: Wine - the panel draws its own cursor and leaves the game's hidden (a cursor shown ends "
         "gamescope's mouse look)");
  if (config::get().disable_cursor) return;
  if (void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "ClipCursor",
                                 reinterpret_cast<void*>(&hk_clip_cursor), &g_clip_slot)) {
    g_real_clip = reinterpret_cast<ClipCursorFn>(prev);
    logf("overlay: re4.exe's ClipCursor import hooked (the game's lock calls for the log; one asking for the clip "
         "already in place is not passed on)");
  } else {
    logf("overlay: re4.exe's ClipCursor import not found - the pointer stays where the game holds it");
  }
}

bool install() { return d3d::install(); }

void service() {
  d3d::service();
  static DWORD last = 0;
  const DWORD now = GetTickCount();
  if (now - last >= 1000) {
    last = now;
    d3d::log_rates();
  }
}

namespace {

// The window's thread (ShowCursor counts per thread): when the panel comes or
// goes (posted from the render thread), and on its messages - the show is
// re-asserted a few times a second while the panel is up, in case the game hid
// the cursor again. The game's pointer lock is left as the game has it.
void cursor_update(bool now) {
  static bool was_visible = false;
  static DWORD checked = 0;
  if (g_draw_cursor || !g_context_ready) return;
  const bool vis = g_visible != 0;
  const DWORD t = GetTickCount();
  if (!now && vis == was_visible && (!vis || t - checked < 250)) return;
  was_visible = vis;
  checked = t;
  show_cursor(vis);
}

}  // namespace

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  d3d::uninstall();
  keyboard::stop();
  shutdown_imgui();
  mem::iat_restore(g_clip_slot, reinterpret_cast<void*>(&hk_clip_cursor), reinterpret_cast<void*>(g_real_clip));
  logf("overlay hooks removed");
}

}  // namespace re4cc::overlay
