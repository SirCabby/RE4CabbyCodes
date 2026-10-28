#pragma once

#include <cstdint>
#include <vector>

// The item box's own part: what the game's storage does not take, kept by the
// mod for the game being played and carried in its save.
//
// The item box is two places. What the game's storage (the typewriter's Storage,
// chainsaw.ArmouryManager) takes - guns, attachments, some knives, First Aid
// Sprays, one of each - goes into it (inventory.h), where a typewriter reaches it
// too. Everything else that takes room in the attache case - ammo, herbs,
// grenades, resources, a second copy of a weapon - is kept here, with no limit:
// the box of the game being played. It goes into the game's own save with the
// storage (the storage's save data has an item list the game reads back at a
// load but no longer writes), and comes back out of a save as it is loaded,
// before the game reads it (events.cpp). So each save has the box it was saved
// with: a load - the title's, the pause menu's, a game over's, Continue - brings
// that save's back, and a new game starts with none, as the title and the main
// menus let the box go. The Mercenaries, with no saves, has none.
//
// Everything of one item is one entry, however much of it is stored - its counts
// are added up, with no stack size - except a weapon (a gun or a knife), whose
// loaded rounds, durability and upgrades are a state of its own: it keeps an entry
// of its own, as it keeps a piece of its own in the case, and has no count to set.
// The box keeps itself in the panel's order (the Add list's: bank, group, the
// group's own order; one item's weapons the most upgraded first), so an index the
// panel draws is the box's own.
//
// Before the box went into the saves it was a file beside the mod, one per game
// that saves (RE4CabbyCodes.itembox.ini, RE4CabbyCodes.itembox.separate-ways.ini).
// Such a file is now only read to bring its items into a game (the panel's Move
// into this game), and goes once a save keeps them: no copy is left. The mod
// writes one again only when a save could not carry the box (its calls off), so
// nothing is lost.
namespace re4cc::itembox {

// The most of one item an entry counts. The box has no limit of the game's: this
// only keeps a count that is typed, added up or read within what makes sense.
constexpr int kMaxCount = 999999999;

// A weapon's upgrade: one of its levels (game.h Levels: list 0 common, 1
// individual, 2 the exclusive; the category in that list's enum; the level 0-based).
struct Level {
  int8_t list = 0, category = 0, level = 0;
};
constexpr int kMaxLevels = 12;

struct Entry {
  int32_t id = 0;
  int num = 0;          // how many, 1..kMaxCount (a weapon: 1)
  bool weapon = false;  // one entry each, with its state below (items::keeps_state)
  int ammo = 0;         // a gun's loaded rounds (the magazine and its tactical rounds as one)
  int32_t ammo_id = -1; // the ammo it is loaded with (-1: its own)
  int durability = -1;  // a knife's (-1: none)
  int condition = 0;    // game.h kCondition* bits: broken, the exclusive upgrade bought
  int nlv = 0;
  Level lv[kMaxLevels];
};
bool same(const Entry& a, const Entry& b);
int upgrades(const Entry& e);  // the levels added up (the box's order)
// What the box keeps of `e`: a state only for an item that has one.
Entry kept(Entry e);

// A copy of the box for the panel, taken under the lock.
struct View {
  bool live = false;         // the box is the game's being played: emptied at a menu or loaded since
  bool save_failed = false;  // the last save could not carry it (see the log): it went to a file beside the mod
  std::vector<Entry> entry;  // in the panel's order
};

void init(const char* dir);  // from DllMain: the lock, the files beside the mod
View view();                 // any thread
bool live();                 // view().live, without the copy

// The box's life. clear: at the title and the main menus (the tick) - a new game
// starts with none. loaded: a save's box, as the storage's load reads it from the
// save (events.cpp, on the game's thread) - in place of the one there was.
// contents: what a save is to carry (events.cpp, on the game's thread).
// saved: after that save - `carried` false when it could not take the box, which
// then goes to the game's file beside the mod (kept until it is moved back in).
enum class File { kMain, kSeparateWays };
void clear();
void loaded(const std::vector<Entry>& entries);
std::vector<Entry> contents();
void saved(bool carried, File game);

// The box's changes, game tick only (inventory.cpp, which moves the items in and
// out of the game). They are in the game's save at its next save.
//   put        `e` into the box. false: no box is live (nothing changed - the
//              item must stay where it is).
//   unput      takes back what a put put in (a store the game then refused).
//   take       `num` out of the entry at `index`, which must still be `seen` (a
//              weapon's entry goes whole). false when the entry is not there (or
//              not `seen`) any more.
//   set_count  the count of the entry at `index` (still `seen`), 1..kMaxCount; not
//              a weapon's.
//   discard    throws the entry at `index` (still `seen`) away.
bool put(const Entry& e);
void unput(const Entry& e);
bool take(int index, const Entry& seen, int num);
bool set_count(int index, const Entry& seen, int num);
bool discard(int index, const Entry& seen);

// The files beside the mod, one per game that saves: the box from before it went
// into the saves, or one a save could not carry. Read for the panel's Move into
// this game; moved_in() then holds them back (no count, nothing read: never moved
// in twice) until a save keeps the box - then saved() removes them. A load or the
// menus first: offered again (their items went with the box). A save that cannot
// carry the box writes the unsaved file with the box alone and removes the other.
const char* file_name(File f);
int file_entries(File f);  // how many entries it holds (0: none, in a box already, or unreadable); any thread
std::vector<Entry> read_file(File f);  // the tick
void moved_in(File f);                 // the tick

// What the panel and the files say of a weapon's state: "12 rounds, Power 3, ...".
void describe(const Entry& e, char* buf, int n);

}  // namespace re4cc::itembox
