#pragma once

#include <cstdint>

// Calls into the game's own code. Everything else the mod does is a field
// write or an event hook; these are not, each because only the game's code
// does the job whole:
//  - the attache case and the lists off it (the panel's editor, its Add lists,
//    the item box's Store and Take): an item is made by the game's own factory
//    (ChainsawGuiUtil.generateItem - the merchant's, the storage's), put in the
//    case where the pickup's own placement finds it room
//    (CsInventoryController.add - topping up stacks first, turning a piece if it
//    has to; a weapon equipped and given its shortcut as a pickup's is), and
//    taken out by the game's own discard (CsInventoryController.remove: it
//    unequips, clears shortcuts and frees the cells). A count is changed by
//    CsInventory.changeCount, and the reload cache the HUD and reloads read is
//    rebuilt by CsInventory.updateReloadInfos after the editor writes a weapon's
//    rounds or a knife's durability. Key items, treasures and unique items
//    (recipes, charms, maps) go through their own lists' add and remove.
//  - a weapon's upgrades: the merchant's own setters on WeaponCustomManager
//    (setCommomCustomLevel, setIndividualCustomLevel, setLimitBreakCustomLevel),
//    which fill in the levels the combat code reads, and getMaxCustomLevel for the
//    levels a weapon has; WeaponItem.onCustomized then clamps its rounds, as the
//    merchant's upgrade does.
//  - the difficulty switch: CampaignManager.setDifficulty and
//    GameRankSystem.recalcRank, the two calls the game makes when a game over's
//    "lower the difficulty" takes effect.
//  - the save files: the engine's own removal of a slot
//    (via.storage.saveService.SaveService.removeSingle) - RE4's save manager takes
//    requests as objects in a queue, so no field write asks for one - its table
//    read again after a copy (updateSaveFileDetailTbl), and the load list's slot
//    information for a copied slot (SaveLoadMenuGuiManager.getSaveSlotInfo,
//    addSaveSlotInfo).
//  - a challenge's Steam achievement (AchievementServiceController.unlock: the
//    platform's request, queued for the service as the game's own unlock does).
//  - the item box: the game's storage's own store and take
//    (ArmouryManager.addArmouryItem, reduceArmouryItem) and its rule for what it
//    takes (ItemExtension.isEnableMoveToArmoury); and, from the hook on the
//    storage's save, the rest of the box put into the save's item list
//    (List<chainsaw.Item>.Add) as items the game's factory makes.
//  - Kill enemies as they appear (the Mercenaries): a hit from the player, as
//    the game's own direct attacks make theirs (its traps, grabs and turrets) -
//    HitController.requestAttack on the player's body hit controller, numbered by
//    HitManager.generateAttackID, its two data objects made by
//    System.Activator.CreateInstance; the enemy's game object through
//    via.Component.get_GameObject (the shooting gallery's call).
//  - the Extra Content Shop: its page shown again by its own code once the shop
//    window has unlocked or locked an item - each entry in view set up again
//    (ExContentsShopGuiBehavior.updateCurrentGrid, as after its own purchase) and
//    the selected item's name and text (setCaptionMessage).
// These are the counterparts of what the sibling mods' user approved: the
// attache case's own functions (RE4OGCabbyCodes), the item box's Take and Store
// and the difficulty switch (RE2/RE3CabbyCodes), the save files' delete and
// list refresh (RE2/RE3CabbyCodes), the records' achievements (RE2CabbyCodes;
// for the challenges, 2026-10-22), the game's storage and the save's item list
// for the item box (2026-10-23), the hit from the player for Kill enemies as
// they appear (2026-10-26), and the shop's own refresh of its page for the shop
// window (2026-10-28).
//
// A managed method is called as the engine calls one from native code:
// `method(thread context, [this,] args)`, inside the VM's frame (begin on the
// first reference, and on the last one the pending exception handled, the local
// frame collected, the frame ended). Every helper is found by the code that
// uses it hundreds of times over - no address is assumed - and the methods by
// name and parameter types. A call that leaves an exception behind is handed to
// the VM's own handler and turns every further call off for the session.
// Calls are made from the game tick (dispatch.h), or from a hook holding InHook.
namespace re4cc::call {

bool init();  // mod thread, once re::ready(): the helpers and the methods; logs what it found

// A hook the game called (events.cpp) may call game methods too, while it holds
// one of these: it runs inside the game's own managed call, on the game's
// thread, so the calls share that call's VM frame.
class InHook {
 public:
  InHook();
  ~InHook();
  InHook(const InHook&) = delete;
  InHook& operator=(const InHook&) = delete;
};

// One VM frame held open across several calls (the game tick only). An object
// a call makes is referenced by the frame it was made in until that frame
// ends, so a new item and the call that puts it into the case go inside one:
// the case then holds it.
class Frame {
 public:
  Frame();
  ~Frame();
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  bool open() const { return ctx_ != 0; }

 private:
  uintptr_t ctx_ = 0;
};

bool ready();          // the VM's helpers were found and no call has failed
const char* status();
bool items_ready();    // the attache case's and the lists' calls
bool levels_ready();   // the upgrade setters
bool difficulty_ready();
bool saves_ready();    // the save files' calls
bool copy_ready();     // and the engine's copy of a save (the save service, the save manager)
bool system_save_ready();   // the game's system save (request_system_save)
bool achievements_ready();  // the challenges' Steam achievements
bool storage_ready();       // the game's storage: its store, its take and its rule
bool box_save_ready();      // the rest of the item box into a save: the factory, the upgrades, the list's Add
bool kill_ready();          // a hit from the player (Kill enemies as they appear): the request, its number, its objects
bool shop_ready();          // the Extra Content Shop's page shown again (shop_page, shop_caption)

// An event hook forwarding the game's call (events.cpp): true when the original
// left a managed exception pending in `ctx` - the hook must then return at once,
// as the game's own code does.
bool exception_pending(uintptr_t ctx);

// --- items (game tick only; false: not made, or it threw) ---
// A new item (ChainsawGuiUtil.generateItem): a weapon loaded with `ammo_count`
// of `ammo_id` (-1: its first usable ammo; the count clamped to its magazine),
// `durability` > 0 for a knife. 0 when the game made none.
uintptr_t generate_item(int32_t id, int count, int durability, int32_t ammo_id, int ammo_count);
// Into the case where the pickup's placement finds room: how many went in
// (AddItemResult.AddCount; 0 = no room), -1 when the call failed.
int case_add(uintptr_t cs_controller, uintptr_t item);
bool case_remove(uintptr_t cs_controller, const uint8_t guid[16]);
bool case_change_count(uintptr_t cs_inventory, const uint8_t guid[16], int count);
bool case_update_reload(uintptr_t cs_inventory);
bool case_change_size(uintptr_t cs_controller, int size);  // grows only (checkSize: nothing that does not fit is lost)
// The lists off the grid (kind: 1 key items, 2 treasures, 3 unique), on their controllers.
int list_add(int kind, uintptr_t controller, uintptr_t item);  // AddItemResult.AddCount, -1 failed
bool list_remove(int kind, uintptr_t controller_or_inventory, const uint8_t guid[16]);

// --- the game's storage (ArmouryManager; game tick only) ---
// Its own store: a copy of `item` goes in (a weapon's fitted parts are let go,
// as the game's own store does). It keeps one entry per item: a stacking one
// (the First Aid Spray, up to its most) takes the count, and an item of an id it
// holds already is dropped without a word - so the caller checks first.
bool storage_add(uintptr_t armoury_manager, uintptr_t item);
// Its own take: `n` off the stack of the entry holding `item` (by its Guid), an
// emptied entry gone - not one with fewer than `n`.
bool storage_reduce(uintptr_t armoury_manager, uintptr_t item, int n);
// The game's rule (ItemExtension.isEnableMoveToArmoury): 1 the storage takes
// it, 0 not, -1 the call failed.
int storage_accepts(uintptr_t item);
// The save's item list (from the hook on the storage's save, inside an InHook):
// List<chainsaw.Item>.Add.
bool item_list_add(uintptr_t list, uintptr_t item);

// --- a weapon's upgrades (WeaponCustomManager) ---
// list: 0 Common, 1 Individual, 2 LimitBreak (game.h Levels).
int max_level(int list, int category, int32_t weapon_id);  // -1 unknown
bool set_level(int list, int category, int level, uintptr_t custom_levels, int32_t weapon_id);
bool weapon_customized(uintptr_t weapon_item);  // WeaponItem.onCustomized: clamps its rounds

// --- the difficulty ---
bool set_difficulty(uintptr_t campaign_manager, int value);  // CampaignManager.setDifficulty, then GameRankSystem.recalcRank

// --- Automatic QTEs (from the hooks on the prompts, inside an InHook) ---
// A button pressed for one frame, as the game's input does it
// (share.hid.Command.setValue: down, and a trigger unless it was down last frame).
bool press_command(uintptr_t command, uint32_t hash);
// What the laser trap reads for its prompt (GmQTELaserTrap.calcAvoidCommandHash:
// Separate Ways' hookshot buttons remapped as the options say). 0 failed.
uint32_t laser_avoid_hash(uintptr_t trap, int32_t command_type);
// Ada's hookshot button for the Ch4fez0 fight, as the options set it
// (AdditionalManager's AdditionalOptionManager.getCp11HSCommandHash). 0 failed.
uint32_t hookshot_hash();
// The prompt's button "pressed": PlayerHeadActionSign.requestReserveAvoid, and
// requestReserveParry (virtual: the driver's own) - with the game's own checks
// (a knife in the case, the parry's cool-down, the lock).
bool reserve_avoid(uintptr_t action_sign);
bool reserve_parry(uintptr_t action_sign);
// ActionSignGuiUnit.checkNormalActionSignDispLargeUI: a normal parry sign the
// game draws as the big prompt. -1 failed.
int large_normal_sign(uintptr_t gui_unit);
// A trigger of `hash` put straight into a Command's _Triggers for this frame
// (ConcurrentDictionary<UInt32,Boolean>.TryAdd): seen by every reader until the
// next frame's input update, every frame it is put there.
bool trigger_command(uintptr_t triggers, uint32_t hash);
// A grab's knife escape can be taken (KnifeEscapeCondition.canOpenGui: the
// player has a knife). -1 failed.
int knife_can_escape(uintptr_t knife_condition, uintptr_t player_head);

// --- the shooting gallery (from its frame's hook, inside an InHook) ---
// A component's world position (via.Component.get_GameObject, via.GameObject.
// get_Transform, via.Transform.get_Position): where a target's score is shown.
bool component_position(uintptr_t component, float out[3]);

// --- Kill enemies as they appear (game tick only) ---
// The game object a component sits on (via.Component.get_GameObject), or 0.
uintptr_t component_game_object(uintptr_t component);
// A new attack's number (HitManager.generateAttackID), 0 when it failed: a hit
// lands once per attack and target (the HitManager keeps the numbers it saw).
uint32_t new_attack_id(uintptr_t hit_manager);
// A hit from `attacker` (a HitController) on `target` (the game object holding
// the target's HitController): HitController.requestAttack, which queues it for
// the attacker's next update; the HitManager works it out the same frame, as
// every other hit. The two objects are the request's (call::new_object, inside
// the same Frame: the request keeps them).
bool request_attack(uintptr_t attacker, uintptr_t target, uintptr_t attack_user_data, uintptr_t damage_user_data);

// --- the save files ---
bool remove_slot(int slot);              // SaveService.removeSingle: false when the service refused (busy)
bool refresh_save_table();               // SaveService.updateSaveFileDetailTbl
uintptr_t slot_info_copy(uintptr_t save_load_menu_gui_manager, int slot);  // getSaveSlotInfo: a fresh copy, or 0
bool add_slot_info(uintptr_t save_load_menu_gui_manager, uintptr_t info);    // addSaveSlotInfo
// SaveLoadMenuGuiManager.clearInfo: the campaign's last loaded slot forgotten,
// as after a save of the game's own (Continue then takes the newest save).
bool clear_load_info(uintptr_t save_load_menu_gui_manager, int slot);
// The engine's copy of a save: the source loaded into containers of the mod's
// own and saved to the target by the engine's save service, which writes the
// target's own slot into the file.
// A new object of class t, its default constructor run (System.Activator.
// CreateInstance on System.Type.GetTypeFromHandle). Inside a Frame: the frame
// holds it until something else does.
uintptr_t new_object(uint32_t t);
// share.SaveDataUtil.isEnableAccessSaveFileDetailTbl: no table rebuild, the
// service idle and the save manager not busy.
bool save_access_ok();
// SaveDataManager.entrySuspendGameSave / exitSuspendGameSave: the manager holds
// `caller` (and game saves wait) until it is let go.
bool suspend_game_saves(uintptr_t save_manager, uintptr_t caller, bool enter);
uintptr_t save_key(int save_load_key);   // SaveDataManagerEnumExtension.getKeyString (inside a Frame)
// via.storage.saveService.SaveService's statics.
bool service_clean();                     // cleanData
bool service_add(uintptr_t key, uintptr_t container);  // addDataClass
bool service_load(int slot);              // loadSingle: false when not started
bool service_save(int slot, uint64_t size);  // set_SaveDataSize, saveSingle: false when not started
int service_state();                      // get_SaveState (-1 failed)
int service_result();                     // get_LastSaveResult (-1 failed)
// SaveDataManager.requestSaveSystemData(Share_SystemSave, new SystemSaveRequestArgs):
// the game's system save, which keeps the lists' slot information.
bool request_system_save(uintptr_t save_manager, int32_t system_slot);

// --- the challenges ---
// A Steam achievement by the game's number for it (GameRecordIdDefinition's
// table): AchievementServiceController.unlock, on NetworkManager's controller -
// the request the game's GameRecordManager.unlockRecord makes as a challenge is
// done. The service sends it to the platform from its own update.
bool unlock_achievement(uintptr_t achievement_controller, int32_t number);

// --- the Extra Content Shop (from its screen's frame's hook, inside an InHook) ---
// The shop's own code on its own screen (chainsaw.gui.ExContentsShopGuiBehavior).
// Its page: every entry in view set up again from isUnlockBonus, checkBuyBonus
// and the CP, the cursor and the scroll left where they are - updateCurrentGrid,
// as the shop shows its own purchase (which passes CurrentListIndex and
// GridScrollDirection.None; it reads neither).
bool shop_page(uintptr_t screen, int32_t index);
// The selected item's name and text, as a move of the cursor shows them
// (setCaptionMessage: "???" and its lock text while it is locked).
bool shop_caption(uintptr_t screen, int32_t bonus_id);

}  // namespace re4cc::call
