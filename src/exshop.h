#pragma once

#include <cstdint>

#include "game.h"

// The Extra Content Shop window: the shop's items (the main menu's Bonuses >
// Extra Content Shop) unlocked or locked again beside the shop's own screen, so
// that the shop sells them. Buying stays the shop's, for CP: nothing is bought
// here.
//
// What the shop sells is the game's table (chainsaw.ExContentsShopUserData,
// GameRecordManager.ExShopSettingInfoData: an InfoData per item - its
// ExShopBonusID, its price in CP, its category - in the shop's order), read at
// run time. Whether an item is unlocked and bought is its ExShopData
// (GameRecordManager.ExShopDataList: _IsUnlock, _IsBuy), which the game's own
// unlock writes (GameRecordManager.setUnlockBonus: a challenge earned, an item
// found, a request done) and the shop reads for every item it shows
// (isUnlockBonus, checkBuyBonus): locked, for sale, more than the CP held, or
// purchased. Unlocking writes _IsUnlock as the game's unlock does - not its NEW
// mark, which is a call (NewMarkGuiManager) - and locking clears it again on an
// item not bought. The game's system save keeps them: every change is saved at
// once (savefiles::request_system_save), as the challenges window's are.
//
// The shop's tabs are its screen's (chainsaw.gui.ExContentsShopGuiBehavior,
// whose lateUpdateOnActive the mod hooks - events.cpp): Separate Ways' only
// with that DLC, as the shop shows them. Changes are made only while the shop
// is up (GuiType.ExtraContentsShop in the game's GUI table, game::Run). The
// shop's page shows them at once: its screen's next frame has the shop's own
// code set the entries in view up again from the items' state, as after its
// own purchase, and the selected item's name and text (call::shop_page,
// shop_caption) - without that, only once its tab is switched.
namespace re4cc::exshop {

void init();      // from DllMain: the lock
bool discover();  // mod thread, once re::ready(): logs what it found
bool ready();
const char* status();  // why nothing can be unlocked (when it cannot)

// The window's tabs are the shop's, in its order: All, then its categories
// (Apparel holds the costumes and the accessories).
enum Tab : int { kAll = 0, kWeapons, kApparel, kModels, kArt, kSeparateWays, kTabs };
const char* tab_name(int tab);  // as the shop labels it: "ALL", "WEAPONS", "APPAREL", ..., "SEPARATE WAYS"

constexpr int kMaxRows = 320;  // the shop's table: 257 in this build

// What the shop shows for an item (chainsaw.gui.ExShopItemState, as
// ExShopGridItemGui.setupItem picks it): bought wins, then locked, then whether
// the CP held pays for it.
enum State : int { kLocked = 0, kForSale, kShort, kPurchased };

struct Row {
  int id = -1;          // chainsaw.ExShopBonusID
  int tab = -1;         // its tab (Tab; -1: a category the shop has no tab for - listed under All)
  int cp = 0;           // its price
  bool unlocked = false;
  bool bought = false;
  bool by_dlc = false;  // a DLC the player owns has it unlocked and bought, whatever its data says
  State state = kLocked;
};

struct View {
  bool known = false;     // the shop's items were read
  bool up = false;        // the shop is up: changes may be asked for
  int count = 0;
  Row row[kMaxRows];      // the shop's order
  bool tab[kTabs] = {};   // the tabs the shop shows (All always)
  int cp = -1;            // the CP held (-1 unknown)
  bool dlc = false;       // that DLC is owned: every main-game item is unlocked and bought
  bool instant = false;   // the shop's page shows each change at once (else once its tab is switched)
  bool saving = false;    // a save the mod asked for is on its way
  char status[200] = {};  // the last change, or why nothing can change
  bool status_error = false;
};
View view();  // any thread (a copy)

// An item's name, whose it is ("Leon", "": anyone's), its kind ("Weapon",
// "Costume", ...) and what the shop says while it is locked - from the game's
// text files (challenges_text.h); nullptr when they did not name it.
const char* name(int id);
const char* who(int id);
const char* kind(int id);
const char* lock_text(int id);

// From the panel: carried out by the tick while the shop is up.
void request(int id, bool on);
void request_all(const int* ids, int n, bool on);

// The shop screen's frame, after the game's own (its hook, on the game's
// thread, inside a call::InHook): the tabs it shows; after a change of the
// tick's, the shop's page shown again by its own code.
void screen_frame(uintptr_t screen);

void tick(const game::Run& run);  // the game tick

}  // namespace re4cc::exshop
