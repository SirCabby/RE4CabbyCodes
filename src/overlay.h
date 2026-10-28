#pragma once

#include <windows.h>

// The Dear ImGui panel and the machinery that gets it on screen. RE4 renders
// through Direct3D 12 (RE2 and RE3 also offered 11, which the panel still
// draws with), and both present through a DXGI
// swap chain - DXVK's under Proton, dxgi.dll's on Windows, one class for both
// APIs - so the panel hooks Present, Present1 and ResizeBuffers of the game's
// own swap chain, taken as its factory makes it and given a vtable of its own
// (or, failing that, in the class's vtable through a throwaway swap chain), and
// draws with the DX11 or the DX12 backend depending on which device the game's
// swap chain belongs to.
namespace re4cc::overlay {

void install_early();  // from DllMain: the locks, and re4.exe's ClipCursor import (watched)
bool install();        // mod thread: the swap chain hooks
void uninstall();
void service();        // mod thread, every 100 ms: work Present asked for (the DX12 queue's discovery)

bool ensure_context(HWND hwnd);  // ImGui context + Win32 backend + window procedure, once
bool wants_draw();               // the visibility rule for this frame (lock held)
void draw_panel();               // between ImGui::NewFrame() and ImGui::Render() (lock held)
void shutdown_imgui();

// One ImGui context; the window procedure (the window's thread) feeds it
// messages and the present hook (the render thread) draws from it, so every
// touch of the context is taken under this lock (RE0 learned it the hard way).
void lock_imgui();
void unlock_imgui();
struct ImGuiLock {
  ImGuiLock() { lock_imgui(); }
  ~ImGuiLock() { unlock_imgui(); }
  ImGuiLock(const ImGuiLock&) = delete;
  ImGuiLock& operator=(const ImGuiLock&) = delete;
};

// The pointer as Windows has it, for the log (the tick, at the pause menu's open
// and close): whether the game's window is the foreground window, the cursor
// shown and inside it, the pointer's clip, and - since the last report - the
// game's own pointer lock calls and the raw mouse input that reached its window.
void pointer_report(char* out, size_t n);

// What the panel is using this frame, for the input guard (input.cpp).
bool capturing_mouse();     // the pointer is over the panel, or one of its dialogs is open
bool capturing_keyboard();  // a panel field has keyboard focus
bool visible();             // the panel was drawn in the last frame

namespace d3d {  // overlay_d3d.cpp
void install_early();  // DllMain: re4.exe's CreateDXGIFactory imports, so the game's swap chains are adopted as they are made
bool adopting();       // those hooks are in
bool presenting();     // the game's swap chain has presented through the mod
bool install();        // the fallback: the swap chain class's vtable, through a throwaway swap chain
void uninstall();
void service();
void log_rates();  // mod thread, once a second
}  // namespace d3d

}  // namespace re4cc::overlay
