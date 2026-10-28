#pragma once

#include <cstdint>

#include "cheats.h"

// The game calls the mod at its own events - a hit worked out, a weapon used, the
// body armor worn down, the merchant taking money or spinels, an exclusive upgrade
// paid for, a save's data collected, a results count, a save list's frame, a grab's
// escape check, the shooting gallery's frame, a level timer's frame - instead of the
// mod reading the game every frame. Two kinds of pointer are exchanged, both data; no code is patched:
//  - a method's entry in the type database, for the handlers the game makes
//    delegates of: the runtime reads a delegate's code from that entry when it
//    makes the delegate (TDB 71: an offset from the decoder's base, so the entry
//    is pointed at a jump of the mod's own near the game's code - re.h). The
//    entries are exchanged at start-up, before the game has made any: its
//    characters make theirs as they spawn, long after the title screen.
//  - a slot of a class's vtable (and the interface table slot holding the same
//    code), for virtual methods. A virtual call reads its slot every time, so the
//    slot is exchanged once the class has its REObjectInfo (looked for every
//    second).
// Each replacement does its cheat's work around a call to the code it replaced -
// the game's own, forwarded unchanged - on whatever thread the game called it
// from, touching only the objects the game handed it (and what they hold), and
// returning at once when that code left a managed exception pending. The
// switches it reads are atomics (cheats.h).
namespace re4cc::events {

void install();    // mod thread, right after game::discover(): the database entries; logs each
void service();    // mod thread, every second: vtable slots as classes come up; first calls to the log
bool ready(cheats::Kind k);           // the cheat's events are hooked (it works)
const char* why_not(cheats::Kind k);  // for the panel when it does not
bool save_list_hooked();              // the mod hears the save/load list's frames (the save files)
// The item box rides in the saves: the storage's save and load are hooked
// (box_hook_failed: they cannot be - not merely not yet).
bool box_hooked();
bool box_hook_failed();
void uninstall();  // a DLL unload that is not the process exiting: puts back what it exchanged

// Save without counting: the count saves keep, taken by the tick while no save
// list is open (cheats.cpp) - the save's hook writes it back over the game's +1.
void set_save_hold(int count);  // -1: none
// The last save the hook held back (a moment, stamp.h; 0 none) - the tick puts back a
// failed save's extra -1 only after one.
unsigned long last_save_held();

// Infinite spinels: the spinels as the tick saw them this frame (-1: no game). The
// merchant's screen is told only that they changed - the hook measures the change
// from this, and from what it left them at itself.
void note_spinels(int count);

// The shooting gallery's controller, as its last frame saw it (0: none for 2 s),
// and its score then.
uintptr_t gallery_controller();
int gallery_score();

// God mode: Separate Ways' time limit to save Luis from the flames (the level
// timer's frames hold it - game.h LevelTimer). The seconds left as its last frame
// left them; false when its time has not run for a second (not that scene, not
// started, stopped, run out - or the game's frames stopped: the pause menu).
bool luis_time(float* left);

// Kill enemies as they appear: an attack the tick asked for (its HitManager
// number, call::new_attack_id), noted before the game works it out - the hit
// hook raises a hit of it to the enemy's health. kill_hits: how many such hits
// have reached an enemy so far (the log's pace).
void note_kill_attack(uint32_t attack_id);
long kill_hits();
// While it acts (the tick says so each frame), every Mercenaries spawn point's
// added conditions hold - the player in its areas, its general flags, its flags -
// so every area of the stage sends enemies wherever the player stands.
// spawn_opened: how many times a condition that did not hold was taken as holding
// (the log's pace); spawn_hooked: how many of the three checks are hooked.
void spawn_everywhere(bool on);
long spawn_opened();
int spawn_hooked();

}  // namespace re4cc::events
