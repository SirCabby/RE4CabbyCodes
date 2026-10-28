#pragma once

#include <windows.h>

// The input guard. RE4 (as RE3) reads the mouse's motion and buttons as Raw Input
// (WM_INPUT through its window procedure, GetRawInputData) and where the
// pointer is with GetCursorPos, every frame (RE3's mouse update, the engine's
// via.hid.Mouse.ScreenCursorPosition, which its menus hover by);
// the keyboard through DirectInput and some keys with
// GetAsyncKeyState/GetKeyState; the pad through XInput/DirectInput. While the
// panel is using the mouse (the pointer is over it, or one of its dialogs is
// open) or the keyboard (a text field has focus), the window procedure hook
// (overlay.cpp) keeps those messages from the game; the exe's GetCursorPos
// import, hooked here, tells the game the pointer is still where it last saw it,
// so its menus do not follow the pointer across the panel (a save list's
// highlight moved under the save files' Delete dialog, 2026-10-11); the exe's two
// key-state imports read the mouse buttons as up while the panel has the mouse,
// and every key while a text field has focus; and the game's DirectInput
// keyboard is told no key went down (keyboard.h) - except Escape, which always
// reaches the pause menu.
namespace re4cc::input {

void install();    // from DllMain: re4.exe's GetAsyncKeyState/GetKeyState imports
void uninstall();

// For the window procedure: a WM_INPUT message's device (RIM_TYPEMOUSE,
// RIM_TYPEKEYBOARD, ...), or -1.
int raw_input_type(LPARAM lp);

}  // namespace re4cc::input
