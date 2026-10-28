#pragma once

#include <cstdint>

// The save files, managed beside the game's save and load lists - the title's
// Load Game, the pause menu's, the game over screen's and a typewriter's save
// list, in the main game and in Separate Ways: while one is up the panel lists
// its slots and can delete a save or copy one into another slot of the same
// game, empty or not.
//
// - Every list is one screen, chainsaw.SaveLoadMenuGuiBehavior, whose
//   lateUpdateOnActive runs once a frame while it is up: the mod hooks its vtable
//   slot (events.cpp) to know the list is up and to read the rows it shows - for
//   each slot a SaveSlotInfoDetail (the chapter, the difficulty, the save count)
//   and an AppSaveFileDetail (the engine's detail of the slot's file: its slot
//   and when it was written). Which slots a list shows is the screen's own: its
//   campaign's auto-save slot (the main game's 0, Separate Ways' 100) and then
//   its manual slots, the auto-save row only on a load list.
// - Delete is the engine's own removal of a slot, SaveService.removeSingle: RE4's
//   save manager takes requests as objects in a queue, so no field write asks for
//   one.
// - Copy has no counterpart in the game, so the engine makes it: the source slot
//   loaded by the engine's save service (SaveService.loadSingle) into save
//   containers of the mod's own - the ones its game save fills - and those saved
//   to the target (saveSingle). A save file carries its own slot inside it, which
//   the engine files it under, so a copy of the bytes would never show as the
//   target: the engine's save writes the target's. Meanwhile the list is held
//   still (its own DialogWait phase: no input, and it cannot close) and the
//   game's saves are held back (the save manager kept busy, and its own
//   suspension of game saves holding the containers). The list's information
//   for the target then becomes the source's (the game's table of slot
//   information: field writes, or SaveLoadMenuGuiManager.getSaveSlotInfo and
//   addSaveSlotInfo for a slot it has none for yet), the engine's table of the
//   slots is read again (SaveService.updateSaveFileDetailTbl), and the game's
//   system save is asked for (SaveDataManager.requestSaveSystemData), which keeps
//   that information - as after a save of the game's own.
// - Either way the screen is then set to wait for the engine's table and to set
//   itself up again - its own SubPhase.WaitUpdateTbl, a field write - as it does
//   after a save of its own.
//
// Neither change can be undone: the panel asks first, and every request carries
// what the panel showed of the slots it names, so a slot that changed in between
// is left alone. Nothing is changed while the game's own saves are busy.
namespace re4cc::savefiles {

constexpr int kMaxRows = 24;  // the main game's load list: the auto-save and slots 1-20

struct Row {
  bool used = false;      // the slot holds a save the list shows
  int slot = -1;          // the game's slot number
  int chapter = -1;       // the chapter as the game numbers it (-1 not known)
  int difficulty = -1;    // 0 Assisted .. 3 Professional (-1 not known)
  int saves = -1;         // its save count
  int clears = 0;         // the main game cleared this many times before it
  long long unix_time = -1;  // when it was written (-1 not known)
  uint32_t hash = 0;      // of the slot and what identifies its save: a request checks it
};
inline bool same_save(const Row& a, const Row& b) { return a.used == b.used && a.slot == b.slot && a.hash == b.hash; }

enum Screen : int { kLoadList = 0, kSaveList = 1 };
const char* screen_name(int screen);  // "the load list", "the save list"

struct View {
  bool ready = false;     // everything needed was found (else `status` says why)
  int screen = -1;        // the list the panel is beside (Screen, -1: none)
  bool separate_ways = false;  // Separate Ways' slots (else the main game's)
  bool up = false;        // the list is up and idle, its rows read: changes may be asked for
  bool copy_ok = false;   // the engine's copy was found (Copy)
  bool busy = false;      // a change is under way, or the game's own saves are busy
  int count = 0;
  Row row[kMaxRows];
  char status[240] = {};  // what the last change came to, or why nothing can change
  bool status_error = false;
};

void init();       // from DllMain: the lock
bool discover();   // mod thread, once re::ready(): logs what it found
bool ready();

// The screen's frame, after the game's own (the hook, on the game's thread): the
// rows' snapshot.
void screen_frame(uintptr_t ctx, uintptr_t screen);
// The list the panel is beside, as of the screen's last frame (Screen; -1: none).
int screen_shown();

View view();  // any thread (a copy)

// Requests from the panel, carried out from the game tick. What the panel saw of
// the rows goes with them: the list can move on between the click and the tick,
// and changing a save that is not the one clicked is worse than doing nothing.
void request_delete(int row, const Row& seen);
void request_copy(int from_row, int to_row, const Row& from_seen, const Row& to_seen);

void tick();  // the game tick: the requests and the change under way

// The game's system save, for another part of the mod (the challenges keep their
// changes there): the request the copy makes, SaveDataManager.requestSaveSystemData.
// 1 asked for; 0 not now (the game's saves are busy, or a change of the save
// files' own is under way); -1 it cannot be (no save manager, or the call
// failed). The game tick only, like game_saves_busy.
int request_system_save();
bool game_saves_busy();  // SaveDataManager is busy (or not there): its process, its queue, its flows

}  // namespace re4cc::savefiles
