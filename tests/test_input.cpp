// src/input.cpp's guard, on this program's own imports: input::install() hooks
// the main module's GetCursorPos, GetAsyncKeyState and GetKeyState - re4.exe's in
// the game, this test's here - so the calls below read what the game would.
// The pointer is moved with SetCursorPos and the buttons and keys pressed with
// SendInput; the panel's "has the mouse" and "has the keyboard" are this test's
// own switches (overlay::capturing_mouse/capturing_keyboard).
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static
//       tests/test_input.cpp src/input.cpp src/log.cpp src/mem.cpp src/config.cpp
//       -luser32 -o tests/build/test_input.exe
//   WINEPREFIX=<scratch> wine tests/build/test_input.exe
// Headless: set the scratch prefix's graphics driver to "null"
// (HKCU\Software\Wine\Drivers, Graphics = null), as for test_keyboard.
#include <windows.h>

#include <cstdio>

#include "../src/input.h"
#include "../src/log.h"

namespace re4cc::overlay {
bool g_test_mouse = false, g_test_typing = false;
bool capturing_mouse() { return g_test_mouse; }
bool capturing_keyboard() { return g_test_typing; }
}  // namespace re4cc::overlay

namespace {

using re4cc::overlay::g_test_mouse;
using re4cc::overlay::g_test_typing;

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
  if (!ok) ++g_fail;
}

bool at(int x, int y) {
  POINT p{-1, -1};
  return GetCursorPos(&p) && p.x == x && p.y == y;
}

void mouse(DWORD flags) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flags;
  SendInput(1, &in, sizeof(in));
  Sleep(30);
}

void key(WORD vk, bool down) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  SendInput(1, &in, sizeof(in));
  Sleep(30);
}

bool async_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

int main() {
  char dir[MAX_PATH];
  const DWORD n = GetTempPathA(sizeof(dir), dir);
  if (n == 0 || n >= sizeof(dir)) dir[0] = '\0';
  re4cc::log_init(dir);

  // A real pointer to move: if Wine has none here, nothing below means anything.
  if (!SetCursorPos(100, 120) || !at(100, 120)) {
    std::printf("SKIP  SetCursorPos/GetCursorPos do not move a pointer here\n");
    return 2;
  }
  re4cc::input::install();

  // The panel without the mouse: the game reads the pointer as it is.
  check(at(100, 120), "no capture: the pointer where it is (100,120)");
  SetCursorPos(140, 160);
  check(at(140, 160), "no capture: the pointer moved (140,160)");

  // The panel takes the mouse: the game keeps the position it last read.
  g_test_mouse = true;
  SetCursorPos(300, 320);
  check(at(140, 160), "capture: the pointer held where the game last read it (140,160)");
  SetCursorPos(500, 520);
  check(at(140, 160), "capture: still held after another move");

  // The panel lets go: the game sees the pointer where it is now.
  g_test_mouse = false;
  check(at(500, 520), "no capture: the pointer where it is again (500,520)");

  // The mouse's buttons: the game's while the panel has not the mouse, up while it has.
  mouse(MOUSEEVENTF_LEFTDOWN);
  check(async_down(VK_LBUTTON), "no capture: the left button reads down");
  check((GetKeyState(VK_LBUTTON) & 0x8000) != 0, "no capture: the left button reads down (GetKeyState)");
  g_test_mouse = true;
  check(!async_down(VK_LBUTTON), "capture: the left button reads up (GetAsyncKeyState)");
  check((GetKeyState(VK_LBUTTON) & 0x8000) == 0, "capture: the left button reads up (GetKeyState)");
  g_test_mouse = false;
  mouse(MOUSEEVENTF_LEFTUP);
  check(!async_down(VK_LBUTTON), "no capture: the left button reads up once released");

  // Keys: the game's while the panel has the mouse only; up while a panel field
  // has the keyboard, but Escape.
  key('A', true);
  g_test_mouse = true;
  check(async_down('A'), "capture of the mouse: a key still reads down");
  g_test_mouse = false;
  g_test_typing = true;
  check(!async_down('A'), "typing: a key reads up");
  key('A', false);
  key(VK_ESCAPE, true);
  check(async_down(VK_ESCAPE), "typing: Escape still reads down");
  key(VK_ESCAPE, false);
  mouse(MOUSEEVENTF_LEFTDOWN);
  check(!async_down(VK_LBUTTON), "typing: the left button reads up");
  mouse(MOUSEEVENTF_LEFTUP);
  g_test_typing = false;

  // Taken out: the real functions again.
  re4cc::input::uninstall();
  g_test_mouse = true;
  SetCursorPos(200, 220);
  check(at(200, 220), "uninstalled: the pointer where it is, whatever the panel");
  g_test_mouse = false;

  std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "all passed", g_fail);
  return g_fail ? 1 : 0;
}
