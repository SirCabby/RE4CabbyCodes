#pragma once

#include <cstdint>

#include "game.h"
#include "itembox.h"

// The attache case and the lists off it - the key items, the treasures, the
// unique items (recipes, charms, treasure maps, case skins, armor) - of the
// character in play, edited from the panel, and the item box's Store and Take.
// RE4's case is a grid of pieces, so nothing goes in by writing a slot: an item
// is made by the game's own factory and put in where the game's own placement
// finds it room (topping up the stacks of it already there first, turning a
// piece if it has to), or not at all; it is taken out by the game's own discard,
// which unequips a weapon going out. The lists off the grid take an item through
// their own add and remove. What an item holds - its count, a gun's loaded
// rounds, a knife's durability, a weapon's upgrades - goes through the game's own
// setters where it has them (call.h), else is a field write. Everything is done
// by the game tick while the pause menu has the game stopped, and is checked
// against what the panel was showing: acting on the wrong item is worse than not
// acting.
//
// The item box is two places, both in the game's save. What the game's storage
// (the typewriter's Storage) takes - by its own rule, one of each, the First Aid
// Spray stacked to its most - goes in it through its own store and comes out
// through its own take, so a typewriter reaches it too. Everything else, and what
// the storage has no room for (a second copy of a weapon), goes in the mod's own
// part (itembox.h), which the storage's save carries (box_into_save) and its load
// hands back (box_from_save).
namespace re4cc::inventory {

enum Kind : int { kCase = 0, kKeyItems, kTreasures, kUnique, kStorage, kKinds };
const char* kind_name(int kind);

struct Row {
  game::ItemView v;        // as the game has it (the game's storage: v.stored, v.stored_max)
  int kind = kCase;
  int stack = 1;           // how many one piece (or entry) holds - the game's own number
  bool weapon = false;     // a gun or a knife: a state of its own (items::keeps_state)
  bool gun = false, knife = false;
  int durability_max = 0;  // a knife's as made (an upgraded one holds more)
  game::Levels levels;     // a weapon's upgrades
  int max_level[16] = {};  // the most each of `levels` goes to, 0-based (-1 unknown)
};

constexpr int kMaxRows = 320;

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool known = false;      // the player's inventories were found (a game is being played)
  bool editable = false;   // a change can be made now
  char why[160] = {};      // ...or why not
  game::Mode mode = game::Mode::kNone;
  game::CaseSize size;     // the case's grid
  int count[kKinds] = {};  // rows of each kind, in `row` one kind after the other
  int nrows = 0;
  Row row[kMaxRows];
  // The item box of the game being played (none in the Mercenaries): the game's
  // storage is in `row` (kStorage), the mod's part is itembox::view().
  bool has_box = false;
  bool storage = false;      // the game's storage is read and its calls are there
  char storage_why[160] = {};
  bool box_ok = false;       // the mod's part takes items: it is the game's, and the saves carry it
  char box_why[160] = {};
  int box_files = 0;         // entries waiting in this game's files beside the mod (Move into this game)
  char note[400] = {};     // what the last change came to (logged; the panel shows a refusal)
  bool note_error = false;
  int waiting = 0;         // the panel's requests not done yet (a batch is worked through a few a frame)
};

void init();             // from DllMain: the lock
void view(View* out);    // any thread
// How many times the tick has published the view: the panel copies it (it is
// large) only when this has moved since its last copy.
unsigned view_generation();
// The panel drew the lists this frame: the tick keeps them current while it
// does (four times a second), and lets them be while nothing shows them.
void lists_drawn();

// Which list an item lives in: the case for what takes room in it, else the
// list off the grid its type belongs to.
int kind_of_item(int32_t id);

// Requests from the panel, applied by the game tick while the pause menu is up,
// in the order they were made - a few each frame, so a long list of them holds
// no frame up. `seen` is the row as the panel showed it: an item that has
// changed since is left alone.
//
// A batch: the requests the panel makes between begin_batch() and end_batch()
// are one change of many items, queued together and summed up in one note once
// the last is done - "7 of 9 added - not added: no room in the case for ...".
// `done` is the word the note has for each one done ("added", "thrown away").
void begin_batch();
void end_batch(const char* done);
void request_count(const Row& seen, int count);
// A weapon's rounds (a gun's; -1 unchanged), durability (a knife's; -1
// unchanged) and upgrades (one level per seen.levels entry, 0-based; nullptr
// unchanged).
void request_weapon(const Row& seen, int ammo, int durability, const int* levels);
void request_remove(const Row& seen);
void request_add(int32_t id, int count);
void request_case_size(int size);  // grows only
// The Exclusive Upgrade Tickets held, set to `count` (0 to what one entry holds)
// by the game tick, for the cheats' editor (cheats.cpp): the first of the
// treasures' entries of it takes the count and any other is taken out; one is
// made and added when none is held, and every one taken out for 0. False, with
// why, when it could not be done.
bool set_tickets(const game::Run& run, int count, char* why, size_t n);
// The item box of the game being played (has_box):
//   store         a piece of the case into it: the game's storage when it takes
//                 it and has room, else the mod's part.
//   take          the mod's part: one piece's worth at a time (its count has no
//                 limit) or a weapon; take_stored: the game's storage, one at a
//                 time as the game's own take (the item itself, or one off a stack).
//   box_count     an entry's count (the mod's part); stored_count a stack's (the
//                 game's storage: the First Aid Spray, up to its most).
//   discard       an entry thrown away; discard_stored a storage entry, whole.
//   import        the game's files beside the mod (itembox.h) into the box, each
//                 item where a Store would put it; the files put aside after.
// An entry of the mod's part is the one at `box_index` while it is still `seen`,
// else the first one like it: a batch's earlier change may have taken an entry
// before it out.
void request_store(const Row& seen);
void request_take(int box_index, const itembox::Entry& seen);
void request_take_stored(const Row& seen);
void request_box_count(int box_index, const itembox::Entry& seen, int num);
void request_stored_count(const Row& seen, int num);
void request_discard(int box_index, const itembox::Entry& seen);
void request_discard_stored(const Row& seen);
void request_import();
// A game with an item box: the main game and Separate Ways (the Mercenaries,
// played in one sitting with no saves, has none), and the files beside the mod
// that are its own.
bool has_box(game::Mode m);
bool box_file(game::Mode m, itembox::File* out);

// The item box and the saves (events.cpp, on the game's thread).
//   box_into_save   the storage's save data being made (ArmouryManager.
//                   GameSaveData): the mod's part goes into its item list, as
//                   items the game's factory makes (inside the hook's InHook),
//                   each marked as the box's.
//   box_from_save   the storage's save data being loaded: a list holding the
//                   box's items replaces the box, and is returned for the hook to
//                   keep out of the game's sight while it reads the rest; any
//                   other list (an old save's own items, or none) is the game's
//                   (0), and the box comes out empty.
void box_into_save(uintptr_t save_data);
uintptr_t box_from_save(uintptr_t save_data);

void tick(const game::Run& run);  // the game tick

}  // namespace re4cc::inventory
