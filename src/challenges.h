#pragma once

#include <cstdint>

#include "game.h"

// The challenges window: RE4's challenges (chainsaw.GameRecordManager - the main
// story's and Separate Ways', kept in the system save) switched on and off beside
// the game's own challenges screen (the main menu's Bonuses, the pause menu),
// their CP, their Extra Content Shop unlocks and their Steam achievements
// following.
//
// A challenge is the game's data, not a switch the mod holds: its record
// (GameRecordData: done, a count and the things counted), the CP it pays
// (GameRecordManager.RecordExPoint) and the bonuses it unlocks (ExShopData:
// items the Extra Content Shop sells, or a game mode - Professional). Switched
// on, a challenge gets what the game's unlockRecord writes - done, its CP, its
// bonuses unlocked, the game asked to check "Complete all challenges" - a
// counted one its count at its goal, and its Steam achievement the game's own
// call (AchievementServiceController.unlock, user-approved 2026-10-22). Not the
// NEW mark or the game's "challenges completed" pop-up. Switched off, it is no
// longer done, its CP is taken back (never below 0), the bonuses it unlocked are
// locked again unless they are bought, another challenge that is done unlocks
// them too, or the game gives them otherwise, and a counted one's count drops
// just short of its goal (the things counted last forgotten, so they count
// again) - the game can hand it back as it is earned again. An achievement
// stays: Steam keeps them.
//
// What each challenge pays and unlocks and its goal are the game's table, read
// at run time (GameRecordManager.SettingInfoData); the names come from the
// game's text files (challenges_text.h, tools/gen_challenges.py). Each change is
// saved at once: the game's system save is asked for, as the save copy does
// (savefiles::request_system_save).
//
// The screen is GuiType.RecordList in the game's GUI table (game::Run); changes
// are made only while it is up.
namespace re4cc::challenges {

void init();      // from DllMain: the lock
bool discover();  // mod thread, once re::ready(): logs what it found
bool ready();
const char* status();  // why nothing can be switched (when it cannot)

// The window's tabs are the game's challenges screen's, in its order: All and
// Incomplete, then one per category (GameRecordSettingUserData.CategoryType).
enum Group : int { kStory = 0, kTutorials, kWeapons, kBattle, kCollectibles, kSeparateWays, kGroups };
constexpr int kTabAll = -2, kTabIncomplete = -1;
const char* tab_name(int tab);  // as the game labels it: "ALL", "INCOMPLETE", "MAIN STORY", ..., "SEPARATE WAYS"

constexpr int kMaxRows = 192;     // the game's table: 154 (the main story's 0-110, Separate Ways' 200-259)
constexpr int kMaxUnlocks = 512;  // every row's bonuses together (Chapter 1's alone are 42)

struct Unlock {
  int bonus = -1;       // ExShopBonusID
  bool open = false;    // unlocked: the shop sells it (a game mode: it can be played)
  bool bought = false;
  bool kept = false;    // undoing this challenge leaves it unlocked: bought, or given otherwise too
};

struct Row {
  int id = -1;              // GameRecordID
  int group = -1;           // Group (-1: none the window knows)
  bool done = false;
  bool hidden = false;      // kept off the game's screen, though the game awards it
  int achievement = 0;      // the game's number for its Steam achievement (0: none)
  int cp = 0;               // what it pays
  int goal = -1;            // its "{0}": the goal of a counted one, a limit for some (-1: none)
  int progress = -1;        // a counted one's count (-1: not counted, or counted in the game save)
  int first_unlock = 0, n_unlocks = 0;  // View::unlock[first_unlock .. +n_unlocks)
};

struct View {
  bool known = false;       // the game's challenges were read
  bool up = false;          // the game's challenges screen is up: changes may be asked for
  int count = 0;
  Row row[kMaxRows];        // the game's order (its table's)
  int n_unlock = 0;
  Unlock unlock[kMaxUnlocks];
  int cp = -1;              // the CP to spend in the shop (-1 unknown)
  bool achievements = false;  // the game's achievement call was found (and none failed)
  bool achievements_known = false;  // which challenges carry one was read (GameRecordIdDefinition)
  bool saving = false;      // a save the mod asked for is on its way
  char status[200] = {};    // the last change, or why nothing can change
  bool status_error = false;
};
View view();  // any thread (a copy)

// The names (challenges_text.h): nullptr when the game's files did not name it.
const char* name(int id);
const char* condition(int id);  // "{0}": the goal
const char* bonus_name(int bonus);
const char* bonus_who(int bonus);   // whose costume or accessory ("" anyone's, nullptr unknown)
const char* bonus_kind(int bonus);  // "Costume", "Model", "Mode", ... (nullptr unknown)

// From the panel: carried out by the tick while the game's challenges screen is up.
void request(int id, bool on);
void request_all(const int* ids, int n, bool on);

void tick(const game::Run& run);  // the game tick

}  // namespace re4cc::challenges
