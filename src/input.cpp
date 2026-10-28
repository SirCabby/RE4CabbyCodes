#include "input.h"

#include <atomic>
#include <cstdint>

#include "config.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

namespace re4cc::input {
namespace {

using KeyStateFn = SHORT(WINAPI*)(int);
KeyStateFn g_real_async = nullptr, g_real_state = nullptr;
uintptr_t g_async_slot = 0, g_state_slot = 0;
using CursorPosFn = BOOL(WINAPI*)(LPPOINT);
CursorPosFn g_real_cursor_pos = nullptr;
uintptr_t g_cursor_pos_slot = 0;

// The pointer where the game last read it: held for it while the panel has the
// mouse. x in the low half, y in the high one; kNoPos until the first read.
constexpr uint64_t kNoPos = 0x8000000080000000ull;
std::atomic<uint64_t> g_game_pos{kNoPos};

bool mouse_button(int vk) {
  return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

// A key or button the game must read as up: the mouse's buttons while the panel
// has the mouse, every key while a panel field has the keyboard - but Escape,
// which stays the game's: it is how the pause menu is left.
bool kept_from_game(int vk) {
  return (mouse_button(vk) && overlay::capturing_mouse()) || (vk != VK_ESCAPE && overlay::capturing_keyboard());
}

SHORT WINAPI hk_get_async_key_state(int vk) {
  if (kept_from_game(vk)) return 0;
  return g_real_async ? g_real_async(vk) : 0;
}

SHORT WINAPI hk_get_key_state(int vk) {
  if (kept_from_game(vk)) return 0;
  return g_real_state ? g_real_state(vk) : 0;
}

// The game's menus hover by where the pointer is, read here every frame: while
// the panel has the mouse the game is told it has not moved, and it sees it again
// where it is once the panel lets go of it.
BOOL WINAPI hk_get_cursor_pos(LPPOINT pt) {
  if (pt && overlay::capturing_mouse()) {
    const uint64_t held = g_game_pos.load(std::memory_order_relaxed);
    if (held != kNoPos) {
      pt->x = static_cast<LONG>(static_cast<uint32_t>(held));
      pt->y = static_cast<LONG>(static_cast<uint32_t>(held >> 32));
      return TRUE;
    }
  }
  const BOOL ok = g_real_cursor_pos ? g_real_cursor_pos(pt) : FALSE;
  if (ok && pt)
    g_game_pos.store(static_cast<uint32_t>(pt->x) | static_cast<uint64_t>(static_cast<uint32_t>(pt->y)) << 32,
                     std::memory_order_relaxed);
  return ok;
}

}  // namespace

void install() {
  if (config::get().disable_input) {
    logf("input: disabled by config - the game sees what is typed into the panel");
    return;
  }
  HMODULE exe = GetModuleHandleA(nullptr);
  if (void* p = mem::iat_hook(exe, "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hk_get_async_key_state),
                              &g_async_slot))
    g_real_async = reinterpret_cast<KeyStateFn>(p);
  if (void* p = mem::iat_hook(exe, "USER32.dll", "GetKeyState", reinterpret_cast<void*>(&hk_get_key_state), &g_state_slot))
    g_real_state = reinterpret_cast<KeyStateFn>(p);
  if (void* p = mem::iat_hook(exe, "USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&hk_get_cursor_pos),
                              &g_cursor_pos_slot))
    g_real_cursor_pos = reinterpret_cast<CursorPosFn>(p);
  logf("input: re4.exe's GetAsyncKeyState %s, GetKeyState %s, GetCursorPos %s (the pointer held for the game while "
       "the panel has the mouse)",
       g_real_async ? "hooked" : "NOT FOUND", g_real_state ? "hooked" : "NOT FOUND",
       g_real_cursor_pos ? "hooked" : "NOT FOUND");
}

void uninstall() {
  mem::iat_restore(g_async_slot, reinterpret_cast<void*>(&hk_get_async_key_state), reinterpret_cast<void*>(g_real_async));
  mem::iat_restore(g_state_slot, reinterpret_cast<void*>(&hk_get_key_state), reinterpret_cast<void*>(g_real_state));
  mem::iat_restore(g_cursor_pos_slot, reinterpret_cast<void*>(&hk_get_cursor_pos),
                   reinterpret_cast<void*>(g_real_cursor_pos));
}

int raw_input_type(LPARAM lp) {
  RAWINPUTHEADER h{};
  UINT size = sizeof(h);
  // RID_HEADER only reads the header; the game's own read of the full packet
  // is unaffected (GetRawInputData does not consume anything).
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_HEADER, &h, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    return -1;
  return static_cast<int>(h.dwType);
}

}  // namespace re4cc::input
