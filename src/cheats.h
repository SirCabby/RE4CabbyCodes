#pragma once

#include "config.h"
#include "game.h"

// The cheats. Their switches are shared between the panel (drawn on the
// render thread), the game tick (dispatch.h) and the game's own threads, which
// call the mod at the moments the cheats act on - a hit, a shot, a save, a
// purchase (events.h) - so the switches are plain atomics and edits travel
// through a small request queue. What a cheat holds (the health, a countdown,
// the play time, the Mercenaries' combo) the tick writes each frame while its
// switch is on, and only then; the panel's figures are read while the panel is
// up.
namespace re4cc::cheats {

enum Kind : int {
  kGodMode = 0,      // the player takes no damage (the game's own HitPoint.NoDamage), health held at its maximum;
                     // Luis's too, the water scooter's hull, and Separate Ways' time limit to save Luis
  kAshleyGod,        // the same for Ashley, and hits no longer put her down
  kOneHitKills,      // any hit that does damage takes all of an enemy's health
  kInfiniteAmmo,     // firing uses no rounds, bolts, arrows or grenades
  kNoDurability,     // a knife loses no durability
  kNoArmorWear,      // the body armor loses no durability, so it never breaks
  kSteadyAim,        // the reticle is always fully focused, and shots go where it points
  kAutoQte,          // the dodge prompts and the grabs' button mashing succeed by themselves
  kAutoGallery,      // the shooting gallery plays itself
  kCostumes,         // the pause menu's EXTRAS changes costumes in play, not only accessories
  kFreezePlaytime,   // the play time the game shows and records stops
  kFreezeCountdown,  // the HUD's countdown stops: the Mercenaries' clock, and the main game's timed scenes
  kNoSaveCount,      // a save does not add to the save count
  kInfiniteMoney,    // the merchant takes no pesetas for a purchase or a tune-up
  kInfiniteSpinels,  // the merchant takes no spinels for a trade
  kInfiniteTickets,  // an exclusive upgrade paid with an Exclusive Upgrade Ticket keeps the ticket
  kNoMissCount,     // a shot that misses adds nothing to the shots fired (the results)
  kNoDeathCount,     // a death adds nothing to the chapter's deaths (the results)
  kNoDamageCount,    // a hit taken adds nothing to the chapter's times damaged (the results)
  kEndlessCombo,     // the Mercenaries: a combo never runs out, and what it earns is scored as it goes
  kMercMode,         // the Mercenaries: Mercenary Mode never runs out, and its gauge is always full
  kAutoKill,         // the Mercenaries: every enemy dies as it appears, to a hit of the player's (one game call:
                     // HitController.requestAttack - call.h), its kill scored as the player's
  kCount
};

// The cheats' keys in the ini (kCount of them), for config::load.
const config::CheatKey* config_keys();
void init();  // from DllMain, after config::load: the locks and the ini's start-up switches
const char* name(Kind k);
const char* ini_key(Kind k);
bool enabled(Kind k);
// The panel's switch (any thread; the events and the tick see it at once). A
// change is kept: service(), on the mod thread, writes the switches into the
// ini, so the next start begins with them as they were left.
void set_enabled(Kind k, bool on);
void service();          // mod thread, every 100 ms
bool available(Kind k);  // what it needs was found (events hooked, fields found)
const char* why_not(Kind k);

// What the panel shows (built by the tick, handed over under a lock).
struct Status {
  game::Run run;
  bool health_ok = false;
  game::Health health;
  bool ashley_ok = false;
  game::Health ashley;
  bool luis_ok = false;          // Luis is a partner in play (God mode holds his health too)
  game::Health luis;
  bool jet_ski_ok = false;       // the water scooter's ride is under way
  game::JetSki jet_ski;
  bool luis_time_ok = false;     // Separate Ways' time limit to save Luis is running
  float luis_time_left = 0.0f;   // its seconds left
  bool luis_time_held = false;   // God mode holds it full
  bool armor_ok = false;         // the unique items were read: armor.item 0 is none held
  game::Armor armor;
  int pesetas = -1, spinels = -1;
  int tickets = -1;              // Exclusive Upgrade Tickets held (the treasures')
  int saves = -1;
  bool clock_ok = false;
  long long play_seconds = -1;   // what the game shows and records
  bool play_held = false;        // held (Freeze play time)
  long long results_seconds = -1;  // the RESULTS screen's total play time (every save, menus and pauses too)
  bool results_held = false;       // held (Freeze play time)
  bool countdown_ok = false;
  game::Countdown countdown;
  bool countdown_held = false;
  bool merc_ok = false;
  game::Mercs merc;
  int kill_about = -1;           // Kill enemies as they appear: the enemies in play (-1: not read)
  bool kill_waiting = false;     // ... waiting for the clock to run (or the player to be up)
  bool stats_ok = false;
  game::Stats stats;
  int difficulty = -1;           // game::difficulty_index
  bool rank_ok = false;
  game::Rank rank;
  int enemies = -1;
  bool gallery = false;          // the shooting gallery is being played
  int gallery_score = -1;
  char last_action[160] = {};
};
Status status();

// Requests from the panel (applied by the next game tick, while the pause menu is up).
void request_armor(int durability);  // the body armor's: some mends a broken one
void request_pesetas(int v);
void request_spinels(int v);
void request_tickets(int v);  // made, set or taken out in the treasures (inventory::set_tickets)
void request_saves(int v);
void request_playtime(int seconds);
void request_countdown(int seconds);
void request_counter(game::Counter c, int v);  // this chapter's
void request_total(game::Counter c, int v);    // the whole game's
void request_difficulty(int index);

void tick(const game::Run& run);  // the game tick
void remove_hooks();              // teardown: every hold let go

}  // namespace re4cc::cheats
