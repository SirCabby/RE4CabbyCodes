#pragma once

#include <cstddef>
#include <cstdint>

#include "re.h"

// The game, as the mod sees it: every class, field and enum value it uses,
// looked up by name in the type database once the VM has loaded it
// (discover), and small readers and writers over them. Nothing here hooks or
// calls the game (events.h and call.h do); everything here is a field read or
// write, made from the game tick (dispatch.h) or an event hook, and every
// object is type-checked before it is touched (re::is_a): a wrong guess is a
// log line, never a write into the wrong object.
//
// RE4's game objects, in the names the database gives them:
//  - the managers are AppSingleton`1<T> subclasses - plain managed objects whose
//    static `_Instance` holds them (re::singleton);
//  - a character is a chainsaw.CharacterContext (Leon, Ashley, Ada, the
//    Mercenaries' characters, every enemy) with a HitPoint and a head updater -
//    the player's (PlayerHeadUpdater) holds the four inventories: the attache
//    case (CsInventoryController), the key items, the treasures and the unique
//    items (recipes, charms, maps, case skins, the body armor);
//  - where the game is - title, in play, which campaign - is share.MainModeManager's
//    current game state (a class per state) and share.ContentsManager's campaign;
//  - the pause menu is a GUI (chainsaw.GuiManager's open GUI table, GuiType
//    InGameMenu), and so are the challenges screen (RecordList) and the Extra
//    Content Shop (ExtraContentsShop).
namespace re4cc::game {

// --- discovery -------------------------------------------------------------------------------------
bool discover();  // mod thread, once re::ready(): every name; logs what is missing
bool ready();
const char* status_text();

// What each feature found; a feature missing a name is greyed out in the panel.
struct Parts {
  bool state = false;      // where the game is (game state, campaign, pause menu)
  bool players = false;    // the player's character and its HitPoint
  bool partner = false;    // the partners: Ashley beside Leon, Luis (PartnerContextList)
  bool jet_ski = false;    // the water scooter of the escape (JetSkiManager's MainJetSki): its hull's health, its grace after a hit
  bool luis_timer = false; // Separate Ways' time limit to save Luis from the flames (CustomLevelTimer's timer)
  bool inventory = false;  // the player's four inventories and the case's items
  bool armor = false;      // the body armor among the unique items: its durability, its most and its broken mark
  bool money = false;      // pesetas (InventoryManager)
  bool spinels = false;    // spinels (InGameShopManager)
  bool tickets = false;    // the merchant's exclusive upgrade: the way to pay chosen, the ticket in the treasures
  bool saves = false;      // the save count (CampaignManager)
  bool difficulty = false; // the difficulty (CampaignManager)
  bool rank = false;       // the dynamic difficulty (GameRankSystem)
  bool stats = false;      // the chapter's results (GameStatsManager)
  bool damage = false;     // the results' times damaged (each chapter's Damage count)
  bool clock = false;      // the play time (GameClock, the chapters' laps)
  bool clock_copy = false; // GameClock's second copy of its times: a load that kept the run's clock (clock_kept)
  bool system_time = false;// the RESULTS screen's total play time (GameClock's system count, ChainsawGameClock)
  bool countdown = false;  // the main game's and Separate Ways' countdown (TimerManager)
  bool mercs = false;      // the Mercenaries (MercenariesModeController)
  bool auto_kill = false;  // the enemies, their hit controllers, a shot's attack data (Kill enemies as they appear)
  bool spawn_counts = false;  // the enemies the game counts active and its limits (that cheat's log)
  bool spawners = false;      // the Mercenaries' spawners and their points' cooldowns (that cheat)
  bool action_sign = false;// the dodge prompts (PlayerHeadActionSign)
  bool aim = false;        // the gun in hand, its reticle focus and its shells' spread (Steady aim)
  bool aim_camera = false; // the camera's recoil, hand shake and shakes, and the states it aims in (Steady aim)
  bool costumes = false;   // the costume menu's entries (EXTRAS in the pause menu)
  bool case_scene = false; // whether the attache case's own screen is up (AttacheCaseManager)
  bool storage = false;    // the game's storage (ArmouryManager), its entries and its save data
};
const Parts& parts();

// --- where the game is (the tick: a handful of reads) -----------------------------------------------
enum class Mode : int { kNone = 0, kMain, kSeparateWays, kMercenaries };
const char* mode_name(Mode m);
struct Run {
  bool in_game = false;     // a game is being played (GameStateInGame - the endings too)
  bool paused = false;      // its pause menu is open (GUI InGameMenu)
  bool save_load = false;   // a save or load list is open (GUI SaveLoad)
  bool challenges = false;  // the challenges screen is open (GUI RecordList: the main menu's Bonuses, the pause menu)
  bool shop = false;        // the Extra Content Shop is open (GUI ExtraContentsShop: the main menu's Bonuses)
  bool case_scene = false;  // the attache case's own screen is up (the item window, the storage, the merchant)
  bool menus = false;       // the title or a main menu (the main game's, Separate Ways', the Mercenaries'): no game loaded
  Mode mode = Mode::kNone;  // the campaign being played (none on the title and the menus)
  int state = -1;           // the game state's class (the log): a type index
};
Run run();
const char* state_name(int state);  // the game state class's short name (the log)

// --- characters ----------------------------------------------------------------------------------
// The character in play (CharacterManager's PlayerContextList[0]): Leon, Ada in
// Separate Ways, the Mercenaries' character - or 0.
uintptr_t player_context();
// Ashley as Leon's partner (PartnerContextList, KindID 200030), or 0.
uintptr_t ashley_context();
// Luis as a partner (PartnerContextList, KindID 200032: a PartnerHeadUpdater's, as
// Ashley's - Separate Ways' Ch2a3z0HeadUpdater_cp11 beside Ada), or 0.
uintptr_t luis_context();
int kind_of(uintptr_t context);  // its CharacterKindID (-1 unknown)
uintptr_t hitpoint_of(uintptr_t context);
uintptr_t head_updater_of(uintptr_t context);
struct Health {
  int hp = -1, max = -1;
  bool no_damage = false;  // HitPoint.<NoDamage>
};
bool health(uintptr_t context, Health* out);
// God mode's (the player's, Luis's) and Ashley's hold: HitPoint.<NoDamage> - the
// game's own flag, which its damage code (addDamage, checkDead) reads and only its
// enemies' scripts otherwise write - on, and the health back to its maximum. off:
// NoDamage off, the health as it is. Returns true when something was written.
bool hold_health(uintptr_t context, bool on);
// A character kind the game counts as a player (CharacterEnumDefine.isPlayer's set).
bool is_player_kind(int kind);
constexpr int kAshleyPartnerKind = 200030;
constexpr int kLuisPartnerKind = 200032;

// --- Luis's time limit (Separate Ways) ------------------------------------------------------------
// Chapter 3's flanking tower: Luis goes into the flames, and Ada has a time limit
// to save him - the scene's CustomLevel_Timer (a chainsaw.CustomLevelTimer, the
// only one in the game's scenes), whose lateUpdate runs its timer and shows it as
// a gauge (GUI Timer_31_1011: 100 down to 0 as the time goes - it reads as Luis's
// health draining). Run out, it sets its end flag and the game is over (cause
// Other_Cp11Chp31Ch2a3z0TimeOver). The timer: a chainsaw.Timer whose _TransitTime
// runs up to its _TimeLimit (180 s on every difficulty).
struct LevelTimer {
  float limit = 0.0f, elapsed = 0.0f;
  bool done = false;  // run out (_Completed): its end flag set, the game over under way
};
bool level_timer(uintptr_t custom_level_timer, LevelTimer* out);
// God mode's hold, from the hook on its lateUpdate before the game's: the time gone
// back to none, so the gauge shows full and the time never runs out. A timer run
// out is the game's. True when something was written.
bool hold_level_timer(uintptr_t custom_level_timer);

// --- the water scooter (the escape at the main game's end) ---------------------------------------
// chainsaw.JetSkiManager's MainJetSki (a chainsaw.GmJetSki) while one is in the
// scene, or 0. Its hull has a health of its own - _CurrentHP, up to its settings'
// _MaxHP - that GmJetSki.processDamage takes a crash's damage off: at none its
// next state is Crash, it sinks, and its game-over timer asks for the game over.
// Leon's health has no part in it.
uintptr_t jet_ski();
struct JetSki {
  int hp = -1, max = -1;
  bool running = false;  // its state and its next are Run (the ride under way)
  bool crashed = false;  // either is Crash: sinking, the game over coming
};
bool jet_ski_health(uintptr_t jet_ski, JetSki* out);
// God mode's hold on it, while it runs: the hull's health at its maximum, and the
// grace the game gives after a hit held - processDamage passes over a hit no
// worse than the last while _IgnoreDamageTimer runs, so with the timer kept
// running and the last damage the most there is, no hit takes any health off and
// none sinks it. A crash under way is the game's. off: the mod's grace let go
// (the timer run out, no last damage), only where its values are still there.
// True when something was written.
bool hold_jet_ski(uintptr_t jet_ski, bool on);
constexpr int32_t kJetSkiNoWorse = INT32_MAX;  // the last damage held: no hit is worse
constexpr float kJetSkiGrace = 60.0f;           // the grace held (s), topped up below half

// --- the inventories --------------------------------------------------------------------------------
struct Inventories {
  uintptr_t context = 0;
  uintptr_t cs = 0;        // CsInventoryController (the attache case)
  uintptr_t cs_inventory = 0;  // its CsInventory
  uintptr_t key = 0, key_inventory = 0;        // KeyItemInventoryController, its KeyItemInventory
  uintptr_t treasure = 0, treasure_inventory = 0;  // TreasureInventoryController, its TreasureInventory
  uintptr_t unique = 0, unique_inventory = 0;  // UniqueInventoryController, its UniqueInventory
};
bool inventories(Inventories* out);  // the player's (false: none in play)
bool case_scene_up();  // the attache case's 3D screen (AttacheCaseSetupController past WaitOpen)

// The case's size (AttacheCaseSize: S 0, M 1, L 2, XL 3, XXL 4) and grid.
struct CaseSize {
  int size = -1, rows = 0, columns = 0;
};
bool case_size(uintptr_t cs_inventory, CaseSize* out);
const char* case_size_name(int size);  // "S (7x10)"...

// One item of a list (the case's, the key items', the treasures', the unique items').
struct ItemView {
  uintptr_t item = 0;       // chainsaw.Item (or WeaponItem)
  uintptr_t holder = 0;     // the list's entry that holds it (CsInventoryItem, KeyItemInventoryItem...)
  int32_t id = 0;           // chainsaw.ItemID
  uint8_t guid[16] = {};    // Item._ID
  int count = 0;            // _CurrentItemCount
  int durability = -1;      // _CurrentDurability (knives), -1 for none
  int condition = 0;        // _CurrentCondition (Broken 1, LimitBreak 2)
  bool weapon = false;      // a WeaponItem
  int ammo = -1, tactical = 0;  // its loaded rounds (_CurrentAmmoCount, _CurrentTacticalAmmoCount)
  int32_t ammo_id = -1;     // the ammo it is loaded with (_CurrentAmmo)
  int row = -1, column = -1;  // its place on the case's grid
  bool rotated = false;
  bool force_get = false;   // CsInventoryItem.<IsForceGet>: a scripted item the game takes back
  bool equipped = false;    // in the player's hands (the case's main or sub equip)
  // An entry of the game's storage (`holder` is its ArmouryItem): how many it
  // stacks (_CurrVirtualStackCount - only the First Aid Spray stacks, the rest
  // hold 1) and the most it stacks (_VirtualStackableCountMax).
  int stored = 0, stored_max = 0;
};
// The items of a list, at most `max`. kind: 0 the case, 1 key items, 2 treasures,
// 3 unique, 4 the game's storage (ArmouryManager's entries, any game).
int read_items(const Inventories& inv, int kind, ItemView* out, int max);
constexpr int kStorageKind = 4;
// One item, read as the lists read theirs (`holder` 0): the save's item list.
bool read_item_view(uintptr_t item, ItemView* out);
// The equipped items' Guids (main, sub) - for `equipped`.
bool equipped_guids(uintptr_t cs_inventory, uint8_t main[16], uint8_t sub[16]);

// A weapon's upgrade levels: the item's CustomLevelInWeapon, each entry's
// category and its _NowLevel (0-based: the merchant's level 1 is 0). The panel
// shows the categories the weapon has.
struct Levels {
  bool ok = false;
  int n = 0;
  struct Entry {
    int list = 0;      // 0 Common (CommonCustomCategory), 1 Individual, 2 LimitBreak (the exclusive upgrade)
    int category = 0;  // its category value in that list's enum
    int level = 0;     // _NowLevel
  } e[16];
};
bool weapon_levels(uintptr_t weapon_item, Levels* out);
const char* level_name(int list, int category);  // "Power", "Capacity"..., or nullptr for one the panel does not show

// Field writes the inventory makes itself (inventory.cpp): a weapon's loaded
// rounds and a knife's durability (the game's reload cache is put right with
// CsInventory.updateReloadInfos, a call - call.h).
bool set_loaded(uintptr_t weapon_item, int ammo);
bool set_durability(uintptr_t item, int durability);
// Item._CurrentCondition, bits: 1 broken (a knife worn out), 2 the exclusive upgrade bought.
constexpr int kConditionBroken = 1, kConditionLimitBreak = 2;
bool set_condition(uintptr_t item, int condition);
int condition_of(uintptr_t item);  // -1 unknown
// The item has the game's definition of it (Item.<_ItemDefine>, the catalog's):
// an id the game being played does not define - a DLC's the player does not
// have - gets a blank one instead, whose size is Invalid.
bool item_defined(uintptr_t item);
// An item's count off the case's grid (the key items', the treasures'): its _CurrentItemCount.
bool set_item_count(uintptr_t item, int count);
bool item_type(uintptr_t obj, uint32_t* t);  // its class (re::type_of), checked to be an Item
uintptr_t weapon_custom_levels(uintptr_t weapon_item);  // WeaponItem._CustomLevelInWeapon
bool set_item_guid(uintptr_t item, const uint8_t guid[16]);  // Item._ID

// --- the body armor (a unique item) ---------------------------------------------------------------------------
// The Body Armor is one of the unique items, and its wear is the item's
// durability (_CurrentDurability, up to its definition's _DefaultDurabilityMax).
// Every way it wears - a hit it takes for the player, fire, frost, the boulder -
// ends in the unique inventory's useDurability, which takes the cost off, marks
// the armor broken (Broken, never removed) once none is left, and then tells its
// controller (UniqueInventoryController.onDurabilityChanged, which only feeds the
// dynamic difficulty). A broken armor protects no more until it is repaired.
struct Armor {
  uintptr_t item = 0;  // its chainsaw.Item (0: none held)
  int durability = 0, max = 0;
  bool broken = false;
};
// The player's (false: the unique items could not be read; true with item 0: none held).
bool body_armor(const Inventories& inv, Armor* out);
int32_t armor_id();  // its ItemID, as the controller names it (UniqueInventoryController._ArmorItemId); 0 not found
// The editor: its durability set (0 to its most). None left marks it broken, as
// its wear does; some mends a broken one, as the merchant's repair does. True when written.
bool set_armor(const Armor& a, int durability);
// No armor durability loss (events.cpp, the controller's handler): a change of the
// armor's durability its controller is told of, from prev to curr, taken back - the
// armor (the controller's item of that id holding curr) given prev again, and the
// broken mark its wearing out put on it taken off. True when it was.
bool keep_armor(uintptr_t unique_controller, int32_t item_id, int prev, int curr);

// --- the game's storage (the typewriter's Storage: chainsaw.ArmouryManager) ---------------------------
// A list of ArmouryItems, each a copy of the item stored and a stack count;
// read_items(kStorageKind) lists it. It is kept in the game save, in the
// manager's GameSaveData - beside the list of entries, an item list
// (ArmourySaveData) the game reads back at a load but has written empty since an
// update: the mod carries the rest of its item box in it (inventory.h).
uintptr_t armoury_manager();
// An entry's stack: the First Aid Spray's count (1 to its most). True when written.
bool set_storage_stack(uintptr_t armoury_item, int count);
// The manager's save data (ArmouryManager.GameSaveData) and its item list.
bool is_storage_save(uintptr_t data);
uintptr_t storage_save_list(uintptr_t data);  // List<chainsaw.Item>, 0 when none
bool set_storage_save_list(uintptr_t data, uintptr_t list);  // the reference written as is (0: none)

// --- money --------------------------------------------------------------------------------------------
constexpr int kPesetasMax = 999999999;  // GuiMaxValueDefinition.PTAS_MAX
constexpr int kSpinelsMax = 9999;
int pesetas();  // -1 unknown
bool set_pesetas(int v);
int spinels();
bool set_spinels(int v);
// The merchant's exclusive upgrade is paid for in pesetas, with an Exclusive
// Upgrade Ticket or with a ticket bought as DLC: the way chosen in its list
// (InGameShopGuiState_CustomCommandDefault, the popup's LimitBreakSelectListGui),
// taken as the hold completes. The ticket's way takes one ticket from the first
// of the customer's treasures holding it. That ticket - its chainsaw.Item - when
// the way chosen is the ticket's, or 0 (pesetas, the DLC's ticket, none held).
uintptr_t upgrade_ticket(uintptr_t custom_command_state);
// The Exclusive Upgrade Tickets held - the treasures' entries of it added up, as
// the merchant counts them (getItemCountSum) - or -1; and its ItemID
// (it_sm75_542_00_0), 0 when it was not found.
int upgrade_tickets(const Inventories& inv);
int32_t upgrade_ticket_id();

// --- the save count (CampaignManager) ----------------------------------------------------------------------
constexpr int kSavesMax = 99999;  // ChainsawSaveDataDefinition.SAVE_COUNT_MAX
int save_count();
// The editor: the count and the current chapter's own entry, moved by the same amount.
bool set_save_count(int v);
uintptr_t campaign_manager();

// --- difficulty and the dynamic rank -------------------------------------------------------------------------
// CampaignManager.Difficulty: Rank_10 Assisted, Rank_20 Standard, Rank_30 Hardcore, Rank_40 Professional.
constexpr int kDifficulties = 4;
const char* difficulty_name(int index);  // by index 0..3
int difficulty_value(int index);         // the enum value of index 0..3
int difficulty_index();                  // the game's now, 0..3 (-1 unknown)
struct Rank {
  bool ok = false;
  int rank = 0;         // GameRankSystem._GameRank
  float action = 0.0f;  // _ActionPoint
  bool fixed = false;   // the game holds it (_RankFixedData)
};
bool rank(Rank* out);

// --- the results (GameStatsManager) ------------------------------------------------------------------------------
// The counts the results screen shows: this chapter's (OngoingStats) and the
// whole game's (every finished chapter's record, OngoingChapterStatsHistory, plus
// this one). The shots fired are the hits and the misses (GameStatsAccuracyElement's
// attempts and successes).
enum class Counter { kDeaths, kDamage, kKills, kHits, kMisses };
constexpr int kCounters = 5;
int counter_max(Counter c);  // what one chapter holds (GameStatsDefinition): 9,999; the shots 999,999
struct Stats {
  bool ok = false;
  int chapter[kCounters] = {};  // by Counter; -1: not found (the times damaged)
  int total[kCounters] = {};
  int finished = 0;             // the finished chapters' records
};
bool stats(Stats* out);
bool set_counter(Counter c, int v);  // this chapter's
// The whole game's: this chapter takes what the finished chapters do not hold
// (what it cannot hold goes onto the latest of them); a count below theirs
// leaves this chapter none and takes the latest of them down.
bool set_total(Counter c, int v);
// For the occurrence hook (events.cpp): the chapter's deaths, times damaged and
// its attempts and hits, read and written as the hook needs them.
struct StatsRefs {
  uintptr_t kill = 0, death = 0, accuracy = 0;  // the elements
  uintptr_t damage = 0;                         // 0 when the times damaged were not found
};
bool stats_refs(StatsRefs* out);
// A kill, death or times damaged element's _Count (-1 unknown): each element
// class declares its own, read by the element's class.
int element_count(uintptr_t element);
bool set_element_count(uintptr_t element, int v);
bool accuracy(uintptr_t element, int* attempts, int* hits);
bool set_accuracy(uintptr_t element, int attempts, int hits);
uintptr_t game_over_stats();  // GameStatsGameOverOngoingStats (its Death carries the count across a continue)
bool game_over_death(uintptr_t gos, bool* need_take_over, uintptr_t* death_element);

// --- the play time -------------------------------------------------------------------------------------------------
// What the game shows and records - the pause menu, the save files, the chapter
// and clear results, the rank - is not its clock but the chapters' laps: the
// finished chapters' (ChapterStatsRecord.<ChapterLapTime>._Time), and this
// chapter's, the clock's record time (elapsed - cutscenes - pauses) less the
// chapter's start (OngoingStats.<ChapterStartTime>._Time). All in microseconds.
struct Clock {
  bool ok = false;
  uint64_t now = 0;        // GameClock's record time
  uint64_t start = 0;      // this chapter's start
  uint64_t laps = 0;       // the finished chapters' laps, as the game sums them
  uintptr_t start_element = 0;  // OngoingStats.<ChapterStartTime> (GameStatsChapterLapTimeElement)
};
bool clock(Clock* out);
uint64_t play_time(const Clock& c);  // laps + (now - start), as the game shows it
bool set_chapter_start(uintptr_t start_element, uint64_t start);
// Lower the finished chapters' laps so their sum is at most `sum` (the editor
// setting less than they hold): the latest ones first.
bool cap_laps(uint64_t sum);
// Add to the latest finished chapter's lap (the editor setting more than the
// current lap can hold). False when there is none.
bool raise_last_lap(uint64_t extra);
// The run's clock across a load. GameClock keeps a second copy of its times,
// _GameSaveData_Reset, and its update moves the two alike. A load writes the
// save's times into both (GameClock.loadGameSaveData; a new game's clear makes
// both anew) - but not when the game keeps the run's clock
// (GameClock.get_IsNotClearGameElapsedTime: MainModeManager's previous game state
// no main menu - a load in play, not from the title or a menu - and
// SaveLoadMenuGuiManager.<IsLastDataLoadContinueData>, which the game over
// screen's Continue sets, and a load list's load of the save Continue would load,
// findContinueSave's): the save's times then go into the copy alone, so the play
// time does not go back for a death. The two differ from then on, until a load of
// another kind. True when they differ; `ahead`: how far the run's record time is
// past the save's.
bool clock_kept(uint64_t* ahead = nullptr);
constexpr uint64_t kMicro = 1000000ull;
constexpr uint64_t kPlayTimeMax = (999ull * 3600 + 59 * 60 + 59) * kMicro;  // 999:59:59, the game's cap
// The RESULTS screen's TOTAL PLAY TIME (the pause menu's Chapter Results, the main
// menu's RESULTS) is another count: every moment the game has run - every save and
// campaign, the menus, cutscenes and pauses too - kept in the system save, not a
// game save (GameClock._SystemSaveData._SystemElapsedTime, microseconds; shown up
// to 9999:59:59).
bool system_time(uint64_t* out);
// Set together with ChainsawGameClock.PrevTime: its update adds the count's growth
// since its last look (unsigned) to the campaign's own count, which a count set
// lower would wrap.
bool set_system_time(uint64_t t);

// --- countdowns ------------------------------------------------------------------------------------------------------
// The main game's and Separate Ways' (chainsaw.TimerManager): seconds left, while it counts.
struct Countdown {
  bool ok = false;
  bool running = false;    // Counting or Stop (the clock not moving)
  float seconds = 0.0f;    // <CurrentSecond>
  uintptr_t setting = 0;   // _CurrentFuncParamSetting: a new one is a new countdown
};
bool countdown(Countdown* out);
bool set_countdown(float seconds);

// --- the Mercenaries ---------------------------------------------------------------------------------------------------
// MercenariesModeController, found through the delegate it puts into
// MercenariesManager's _OnTimeBonusGet as it wakes (no field holds it).
struct Mercs {
  bool ok = false;
  bool active = false;      // its routine is Game (the clock runs)
  uintptr_t controller = 0;
  uintptr_t timer = 0, combo = 0, score = 0, rush = 0;
  float remaining = 0.0f;   // MercenariesTimer._RemainTimer
  bool timer_busy = false, timed_out = false;
  int combo_count = 0, max_combo = 0;
  bool combo_on = false;    // _AcceptionState Accepting
  float combo_timer = 0.0f;
  int kills = 0, score_value = 0;
  int stock_normal = 0, stock_unique = 0;  // the combo bonus not yet on the score
  bool rush_busy = false;   // Mercenary Mode
  float rush_timer = 0.0f, rush_limit = 0.0f;
  int gauges = 0, gauges_max = 0;
};
bool mercs(Mercs* out);
bool set_merc_remaining(uintptr_t timer, float seconds);
bool set_merc_combo_timer(uintptr_t combo, float t);
// The score counter's anti-cheat integers (value = _Value / (int32)(the object's
// own address)): the score, and the combo's two stocks not yet on it.
enum class MercScore { kScore, kStockNormal, kStockUnique };
bool set_merc_score(uintptr_t score_counter, MercScore which, int value);
bool set_rush(uintptr_t rush, float timer, int gauges);
constexpr int kMercScoreMax = 9999999;

// --- Kill enemies as they appear (the Mercenaries) ---------------------------------------------------------------------
// Each enemy in play takes a hit from the player the way the game's own direct
// attacks hit a character (its traps, grabs and turrets): a request on the
// player's body hit controller (HitController.requestAttack, call.h) for the
// enemy's game object, its AttackUserData naming a shot's attack data (the
// HitManager's catalog: AttackDataID Bullet, the key of the gun in hand) and its
// DamageUserData the enemy's chest. The game works the hit through the next
// frame as one of the player's - the mod's hit hook raises its damage to the
// enemy's health (events.cpp) - so the enemy dies its own death and the
// Mercenaries scores the kill as the player's.
struct KillTarget {
  uintptr_t context = 0;
  uintptr_t hit_controller = 0;  // where the hit goes: the body's (a Ganado's whose parasite is out too)
  uintptr_t parasite_waits = 0;  // a Ganado whose parasite waits for its death: its head updater (keep_parasite_in)
  int kind = -1;
  int hp = 0;
  bool parasite = false;  // a Ganado's parasite out
};
struct KillScan {
  int listed = -1;        // enemies the list holds (-1: unreadable)
  int coming = 0;         // with health but not in play yet: not started (the game still bringing them in), or a parasite coming out
  int unknown_kind = -1;  // the kind of one left alone: whether it is in play cannot be told (its hit controller has no damage effect)
};
// The enemies a hit could kill now (CharacterManager's EnemyContextList): set up,
// with health left, not invincible (as the game makes some for a moment) or held
// alive (NoDeath) - but a Ganado whose parasite waits for its death, which its
// start holds alive until its lethal hit draws the parasite's lot - and in play:
// their hit controller's game object started (hit_started). A Ganado's parasite
// once out is hit through the Ganado's body (the game hands a hit on it to the
// Ganado's hit controller; the two share one health), once its appearance has
// played (the Ganado invincible until then). At most `max` into `out`; returns
// how many.
int kill_targets(KillTarget* out, int max, KillScan* scan);
uintptr_t body_hit_controller(uintptr_t context);  // its CharacterBodyUpdater's HitController, or 0
// A Ganado whose parasite waits (ParasiteState Wait) keeps it in: its lethal hit
// draws the parasite's lot (checkParasiteAppearance: forced, else none for a
// chance of 0, else the chance plus the rank's), so with no chance and nothing
// forced the parasite fails and the hit kills the Ganado as any other
// (parasiteFailedDead: NoDeath off, the kill counted). Field writes on the
// Ganado's head updater, just before its hit.
bool keep_parasite_in(uintptr_t head);
// What the log tells of the spawns: the enemies the game counts active (EnemyManager:
// a dead one leaves the count at once) and the two limits spawning stops at - the
// game's (EnemySpawnNumLimit) and the Mercenaries' (_SpawnLimitActiveCount, but for
// spawners that skip it).
struct SpawnCounts {
  int active = -1, game_limit = -1, mode_limit = -1;
};
bool spawn_counts(uintptr_t merc_controller, SpawnCounts* out);
// The Mercenaries' spawners (MercenariesModeController._SpawnerList). Each holds a
// fixed set of enemies (<SpawnTargets>: 150 or so to a stage, all spawners together;
// one spawns once - getNextSpawnParam - and the spawner is spent, <HasAllSpawned>,
// once all have), its flags let it send (<HasSpawnEnabled>: kill counts mostly), and
// it sends its next enemy every _IntervalTime from the nearest of its points
// (_PracticalSpawnPoints) out of the camera's view where the point asks it, past its
// distance from the player, whose conditions hold - and that has cooled down since
// it last sent one (_CoolDownTimerList: a one-shot timer per point, its time the
// point's _CoolDownTime, 2 to 32 s; finished at the start, reset by each spawn).
// ready_spawn_points marks each timer of a spawner that can still send finished once
// it has run `cooldown` seconds - the game leaves a finished one-shot timer alone - so
// its points serve again that soon; returns how many it marked. Field writes.
int ready_spawn_points(uintptr_t merc_controller, float cooldown);
// What the log tells of each spawner when the kills stall: its name's hash (murmur3
// of the spawner's GameObject name, as the mode's own settings name it), whether it
// may send and whether it is spent, its requests waiting, its enemies counted active
// and its limit, how many of its enemies have come of how many it holds, its points
// and how many of them are still cooling.
struct SpawnerState {
  uintptr_t spawner = 0;
  uint32_t name_hash = 0;
  bool enabled = false, spent = false;
  int requests = -1, active = -1, limit = -1, sent = -1, holds = -1, points = -1, cooling = -1;
};
int spawner_states(uintptr_t merc_controller, SpawnerState* out, int max);
// A spawner held up with an enemy to send - none of its points out of the camera's
// view and far enough from the player (the Village's throwers: 4 to 6 points round
// the middle, out of view only, 10 m away at least) - gets both rules lifted: its
// points' _IsOutOfCameraOnly off and its _SpawnDistanceMin cut to `distance`, the
// game's values kept (for the spawners of `merc_controller`). Field writes; returns
// how many values it changed, -1 for no spawner.
int lift_spawn_rules(uintptr_t merc_controller, uintptr_t spawner, float distance);
// Every value lift_spawn_rules changed put back - where the spawners of the mode
// controller it was lifted for still reach it (the stage is up); for another or no
// controller they are let go unwritten. Returns how many it put back.
int restore_spawn_rules(uintptr_t merc_controller);
bool spawn_rules_lifted();
// Whether a hit controller's game object has started (had its first update): 1
// yes, 0 not yet, -1 cannot tell. Its damage effect (EPVExpertDamageEffect, a
// component of the same game object, which every hit asks for its blood and
// sparks) makes its 32 effect players in its start; until then the slots are
// empty, and a hit calls an empty one - the game crashed so (2026-10-26). The
// game never hits a character that has not started: its colliders are not live
// yet. No damage effect: -1.
int hit_started(uintptr_t hit_controller);
uintptr_t hit_manager();
// RE Engine's hash of a name (murmur3 of its UTF-16 text, seed 0xFFFFFFFF): the
// attack data's keys, as BulletShell.getAttackKeyNameHash hashes a gun's name.
uint32_t key_hash(const char* name);
// A shot's attack data in the HitManager's catalog (AttackDataID Bullet): the
// plain shot of `weapon_id`'s gun (the hash of its WeaponID's name) when the
// catalog holds it, else its first shot (AttackType Shot). 0 when it has none.
// `weapons_own`: the gun's own was found.
uint32_t shot_attack_key(int32_t weapon_id, bool* weapons_own = nullptr);
// The request's two objects (made by call::new_object), filled as the game's
// own direct attacks fill theirs.
uint32_t attack_user_data_type();
uint32_t damage_user_data_type();
bool fill_kill_attack(uintptr_t attack_user_data, uint32_t key, uint32_t attack_id);
bool fill_kill_damage(uintptr_t damage_user_data);

// --- the dodge prompts (Automatic QTEs) ----------------------------------------------------------------------------------
// PlayerHeadActionSign's AvoidInfo: a large action sign up and not yet
// accepted gets its reserve flag - the press the game waits for.
bool press_dodge(uintptr_t player_context);

// --- Steady aim ------------------------------------------------------------------------------------------------------------------
// The gun in the player's hands (the equipment's WeaponList entry for the weapon
// it has equipped; a chainsaw.Gun), or 0 - a knife, a grenade, nothing.
uintptr_t equipped_gun(uintptr_t player_context, int32_t* weapon_id = nullptr);
// Every gun the player's equipment holds (its WeaponList), at most `max`.
int player_guns(uintptr_t player_context, uintptr_t* guns, int32_t* ids, int max);
// The reticle focus (Gun._CurrentReticleFitPoint) up to the most its
// ReticleFitParam allows (_PointRange's end): the reticle fully shrunk, as an
// aim held still makes it. True when it was written.
bool focus_reticle(uintptr_t gun);
// The shells' spread: the BulletShellMoveInfo objects of a gun's shell generator
// (a shotgun has two: its centre pellet's and the others'; a rocket launcher its rocket's). These are the
// weapon's user data, shared by every instance of it.
int shell_move_infos(uintptr_t gun, uintptr_t* out, int max);
// The camera held still for the player's aim (PlayerCameraController, the
// player's MainCameraController._CameraControllerArray[0]): the shot's recoil
// (requestRecoil's own copy of the weapon's CameraRecoilParam - _Yaw and _Pitch
// drawn per shot) held at 0, and the hand shake's stick lerp (_OldRSOffset,
// _RSOffsetLerp, which getHandShakeRate multiplies the breathing by) held at 0,
// so the breathing layer and the camera's hand shake are flat. Per-shot and
// per-camera values: nothing shared, nothing to put back.
bool steady_camera(uintptr_t player_context);
bool steady_camera_controller(uintptr_t player_camera_controller);
uintptr_t player_camera(uintptr_t player_context);  // its PlayerCameraController, or 0 (the first failure logged)
// The hand shake a PlayerCameraController's update just handed its
// MainCameraController (_IsHandShake: the view is turned by _HandShakeRotation
// at the camera's interpolation only while it is set) taken back. True when it was set.
bool drop_hand_shake(uintptr_t player_camera_controller);
// The camera is in a state it aims in - the hold states (Hold to HoldExtraScope,
// HoldVariation), the shotgun's pump, a mounted machine gun.
bool camera_aiming(uintptr_t player_camera_controller);
bool spread(uintptr_t move_info, float* radius, float* radius_fit);
bool set_spread(uintptr_t move_info, float radius, float radius_fit);

// --- costumes (the pause menu's EXTRAS) -------------------------------------------------------------------------------
// The game's EXTRAS list is the same in the pause menu as in the main menu, but
// in-game it greys the costumes out: their entries (GuiCostumeManager.
// CostumeSettingDict, kinds Chara00/Chara01/AO_Chara00/01/02_Costume) are
// OnlyMainMenu, the accessories' ExceptEventOrBattle. `open`: the costumes'
// entries are made ExceptEventOrBattle too - with the accessories' text for the
// moments they are greyed - so the game's own list changes them in play under its
// own rule (not in combat or a cutscene), with its own swap, cutscene meshes and
// save of the choice. `held`: greyed for now, a moment a costume must not change
// though that rule allows it (costume_change_unsafe). !open: every entry put back
// as the game had it. True when something was written.
bool costume_menu(bool open, bool held);
// The player (or Ashley) grabbed, dead, on a ladder, in the boat, hidden, held by
// a gimmick, on the hook shot, or a costume already loading.
bool costume_change_unsafe();

// --- the enemies (the panel's count) --------------------------------------------------------------------------------------
int enemy_count();

// --- helpers for the other modules --------------------------------------------------------------------------------------
uintptr_t singleton(const char* type_name);  // re::singleton by name, cached
template <typename T>
bool get(uintptr_t obj, const re::Field& f, T* out);
template <typename T>
bool put(uintptr_t obj, const re::Field& f, const T& v);
uintptr_t ref(uintptr_t obj, const re::Field& f);
// Dictionary<int-like key, reference>: the value under `key`, 0 when none.
uintptr_t dict_ref(uintptr_t dict, int32_t key);
// A List<T>'s count (-1 unreadable), and the list cut back to its first `count`
// (its _size written: what was added after them is no longer in it).
int list_size(uintptr_t list);
bool truncate_list(uintptr_t list, int count);
bool guid_equal(const uint8_t a[16], const uint8_t b[16]);
bool guid_empty(const uint8_t g[16]);

}  // namespace re4cc::game

#include "mem.h"

namespace re4cc::game {
template <typename T>
bool get(uintptr_t obj, const re::Field& f, T* out) {
  const uintptr_t a = re::field_addr(obj, f);
  return a && mem::read_safe(a, out);
}
template <typename T>
bool put(uintptr_t obj, const re::Field& f, const T& v) {
  const uintptr_t a = re::field_addr(obj, f);
  return a && mem::store(a, v);
}
}  // namespace re4cc::game
