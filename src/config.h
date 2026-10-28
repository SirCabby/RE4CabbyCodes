#pragma once

namespace re4cc::config {

constexpr int kMaxCheats = 32;

// The cheats' keys in the ini, in the order of their switches (cheats::Kind) -
// the ini knows the cheats only by these names.
struct CheatKey {
  const char* key;
  const char* comment;  // the defaults' line comment
};

struct Settings {
  int toggle_key = 0x76;      // VK_F7: hide/show the panel while it is up
  // Each cheat's state when the game starts: what the panel left it at (the
  // panel's switches are written back here, save_cheats), by cheats::Kind.
  bool cheat[kMaxCheats] = {};
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the panel on every screen, not only the pause menu
  bool dump_methods = false;  // write every managed method's code address beside the DLL once (reverse engineering)
  // "Disable = overlay,dispatch,game,events,input,cursor,keyboard" turns whole
  // subsystems off so a fault can be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
  bool disable_events = false;  // the game's events calling the mod (events.cpp): no hooks, those cheats do nothing
  bool disable_input = false;   // keeping the panel's clicks and keys from the game
  bool disable_cursor = false;  // the panel's cursor, and the ClipCursor hook (the game's lock calls: overlay.cpp)
  bool disable_keyboard = false;  // the panel's own DirectInput keyboard (keyboard.h): it takes the window's key messages
};

const Settings& get();

// RE4CabbyCodes.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable. `keys`: the cheats' (n of them).
void load(const char* dir, const CheatKey* keys, int n);
const char* dir();  // the DLL's directory, with a trailing separator

// The cheats' switches as the panel left them, written into the ini so the next
// start begins with them. Only their values change - every other line, its
// comments and the file's order are kept - and the file is replaced whole
// (written beside it, then moved over it). False when it could not be written.
bool save_cheats(const bool* on, int n);

}  // namespace re4cc::config
