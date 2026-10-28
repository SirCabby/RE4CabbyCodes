# RE4CabbyCodes — project guide

A client-side mod for **Resident Evil 4 (2023)** — Steam app 2050650, `re4.exe` build **22377325** (PE32+,
SteamStub `.bind` section, `.text` plain on disk, Direct3D 12) — cross-built on Linux with mingw-w64. It
draws a Dear ImGui panel beside the game's **pause menu** with God mode, Ashley takes no damage, One hit
kills, Infinite ammo, No durability loss, No armor durability loss, Steady aim, Automatic QTEs, Automatic shooting gallery, Change
costumes in game, the Mercenaries' Endless combo, Mercenary Mode always on and Kill enemies as they appear (shown only there), Save without counting, Freeze
play time, Freeze countdown timer, Infinite money, Infinite spinels, Infinite upgrade tickets, Misses don't count, Deaths don't count, Damage doesn't count,
editors for the body armor's durability, the save count, play time, a timed scene's countdown, pesetas, spinels, upgrade tickets and the results (the chapter's and the whole game's), the difficulty switch, an
**attache case editor** (counts, loaded rounds, knife durability, upgrades, throw away, grow the case), an
**Add to the case** list (tabs, group headings, search), the **key items**, **treasures** and **other items**
(unique: recipes, charms, maps, case skins, armor; sorted under group headings) each with its own Add - every list
and Add list ticked **many at once**, one confirmation for the lot - and a limitless **item box** kept in
each save: the game's own storage for what it takes (weapons, attachments, First Aid Sprays, one of each), the
rest beside it in the storage's save data (main game, Separate Ways; none in the Mercenaries). Beside the
game's save and load lists it is a **save file manager**
(delete, copy), beside its challenges screen it switches **challenges** on and off (their CP, the Extra
Content Shop's unlocks, their Steam achievements), and beside its **Extra Content Shop** it unlocks each of the
shop's items - or locks it again - without buying it. Every cheat switch is written back to the ini when it changes. `README.md` is the user doc
(`README_BBCode.txt` the same for forums); this file records **what was reverse-engineered** and how the
mod is put together.

The engine side - the steam_api64.dll proxy, the type-database reader, the frame's entry hooks, the call
bridge into managed code, the DXGI swap-chain overlay, the DirectInput keyboard, the input guard, the crash
logger - comes from `../RE3CabbyCodes` and `../RE2CabbyCodes` (RE Engine, TDB 70), moved to TDB 71. The
panel's layout, the item box and the feature set follow `../RE4OGCabbyCodes` (the 2005 game).
REFramework (`praydog/REFramework`) documents TDB 71 and the engine's managed objects; a clone lives in the
session scratchpad when needed - it is reading material, never a dependency.

## Build & deploy

```sh
make            # -> build/steam_api64.dll (config.mk sets GAME_DIR; gitignored)
make install    # rename the stock steam_api64.dll -> steam_api64_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make proxy      # regenerate steam_api64.def + src/proxy_exports.inc from the stock DLL's exports
make package    # dist/RE4CabbyCodes_v<version>.zip (no VERSION file: 0.0.0)
python3 -I tools/gen_items.py --game "$GAME_DIR" --out src/items.h     # the item table (reads the paks)
python3 -I tools/gen_challenges.py --game "$GAME_DIR" --out src/challenges_text.h   # the challenges' and the shop's names, its lock texts, both screens' tabs
python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --type chainsaw.CsInventory   # the type database
python3 -I tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
```

**Always install after a change.** Once a change builds and its checks pass, run `make install` yourself
(check `pgrep -x re4.exe` first; if the game is running, say so and install as soon as it closes), confirm
with `cmp build/steam_api64.dll "$GAME_DIR/steam_api64.dll"`, and say in the reply that it is installed.
Installing is not a release: the version stays as it is.

The VERSION file is the user's: never create or change it, never run `make version`/`make rev`, never
suggest a bump.

### Tests (offline, under Wine)

Run them in a scratch prefix with `Graphics = null` (no display), never the user's. Each header gives its
build line; the main ones:

| test | what it checks |
|---|---|
| `test_discover` | the mod's own discovery over the real `re4.exe` mapped as an image with its TDB rebased and every method linked from `methods.bin`: `re::init`, `game::discover` (every class, field and enum value - "discovery: state 1 ... steady aim 1"), `savefiles::discover`, `challenges::discover` ("the challenges 1, their achievements 1"), `exshop::discover` ("the shop's items 1, its tabs 1, its page shown again 1, the DLC with every item 1"), `call::init` (the VM helpers and every called method - "the shop window's calls: the shop's page shown again 1" among them), `events::install` (the database-entry hooks exchanged: the hit calculation, the merchant's money, spinels and exclusive upgrade, the armor's wear, the laser), `app::find`, the save list's getters. Link it with every `build/*.o` but `dllmain.o`. |
| `test_stats` | the results' reads and writes (`stats`, `set_counter`, `set_total`, the elements the occurrence hook counts) and the play time's (`clock`, the laps as the game adds them up in Separate Ways, `cap_laps`, `raise_last_lap`, `clock_kept` - the clock's second copy telling a load that kept the run's clock - the RESULTS screen's total with `ChainsawGameClock.PrevTime`) over a GameStatsManager, its OngoingStats, each element class, finished chapters' records, a GameClock and a ChainsawGameClock made the VM's way from the real database (`re4.exe` mapped as for `test_discover`; link it the same way). Each element is read through its own class's `_Count` - the deaths and times damaged read through the kills' field kept the results table from showing (2026-10-23). |
| `test_results` | the results' rules (`results_rules.h`): a whole game's count spread over the chapters, the hits and misses set apart |
| `test_aim` | Steady aim's camera (`player_camera`, `steady_camera_controller`, `drop_hand_shake`, `camera_aiming`) over a CharacterContext, MainCameraController, PlayerCameraController, its recoil copy and state made the VM's way from the real database (link it as `test_stats`); and the game's code the arms' sway hooks lead (each shake action asks the equipment's scope check, then sets its layer's blend to the camera's rate) |
| `test_spinels` | Infinite spinels' rule (`spinels_rules.h`): a trade given back, a case trade's placement noted and its cancelled refund taken back out, every other rise kept |
| `test_playtime` | Freeze play time's hold (`playtime_rules.h`) through the game's sequences: the lap held as the clock runs; a Continue (the checkpoint's start, the run's clock run on through the play since the checkpoint and the game over screen) keeps the play time held - the old hold, taking the lap afresh, gains those 2 min 20 s in the test; a chapter's save, whose start the game puts back as play begins (before or after the first look); another save's load and a new game bring their own time; a new chapter; the editor's time across a Continue |
| `test_tickets` | Infinite upgrade tickets: `game::upgrade_ticket` over an exclusive upgrade's state, its controller, the item popup's list of ways to pay and the customer's treasures made the VM's way from the real database (link it as `test_stats`) - the ticket only for the ticket's way, the first valid entry, the editor's count (`upgrade_tickets`: every entry added up); and the game's code the hook relies on (the state's getters read the same fields, the ticket's way calls `reduceTicket(id, 1)`, the reduce takes the first valid entry and removes one it empties) |
| `test_armor` | No armor durability loss and the armor's editor: `body_armor`, `set_armor`, `keep_armor` over a UniqueInventoryController, its UniqueInventory, entries and the Body Armor's item and definition made the VM's way from the real database (link it as `test_stats`) - a wear put back (a break's mark taken off), a rise, another item, a second armor left alone; the editor's mark and mend; and the game's code the hook relies on (`useDurability` marks the armor broken and tells the controller after the change; every way it wears reaches it; the controller's handler does nothing for no change and only feeds the rank; a hit reads the durability after; the repair is the editor's) |
| `test_jetski` | God mode on the water scooter: `jet_ski`, `jet_ski_health`, `hold_jet_ski` over a JetSkiManager (its `_Instance`), its MainJetSki and the scooter's settings made the VM's way from the real database (link it as `test_stats`) - the hull held only while it runs (not waiting, not crashing), the grace topped up, a hit every step for 40 s taking nothing under the game's rule, the switch off letting only the mod's values go; and the game's code the hold relies on (processDamage passes over a hit no worse than the last while the grace runs, before it writes anything; only processDamage sends it to Crash; the game over only from Crash; a ride starts with the game's own values; the Smooth Escape check reads a full hull) |
| `test_luis` | God mode for Luis: `luis_context` over a CharacterManager's PartnerContextList (Ashley's and Luis's PartnerBaseContexts with their HitPoints, one that is no context), `hold_health` on his HitPoint (on, held already, none left, off); `level_timer` and `hold_level_timer` over a CustomLevelTimer and its Timer made the VM's way from the real database (link it as `test_stats`) - no time gone, nothing to write, run out left to the game, not a level timer, before its start; ten minutes of the game's frames held never run out, off it runs its 180 s from full; and the game's code it relies on (isPartner's two kinds, onFrameHeadUpdate rebuilding the partner list, PartnerBodyHitDriver's cut out of battle, ForceDown's death at no health from Luis's frame, addDamage/checkDead returning on NoDamage, the level timer's frame - its _Timer run, ProgressRate, the Timer_31_1011 gauge, the end flag once done - and Timer.update/updateCompleted) |
| `test_autokill` | Kill enemies as they appear: `kill_targets`, `hit_started`, `body_hit_controller`, `keep_parasite_in`, `shot_attack_key`, `fill_kill_attack`/`fill_kill_damage` over a CharacterManager's enemies (contexts, health, body updaters, hit controllers with their damage effects; a Ganado's head, body and parasite updater in each ParasiteState) and a HitManager's attack data catalog (its merged dictionary, RefCounters, an AttackHitUserData's list) made the VM's way from the real database (link it as `test_stats`) - who a hit could kill (set up, health left, not Invincible or NoDeath - but a Ganado whose parasite waits, held alive by its start, is hit with the parasite noted; in play: the damage effect's players made - the 2026-10-26 crash's Ganado, whose were not, is coming, not hit; one with no damage effect left alone; a parasite coming out is coming, one out is hit through the Ganado's body once the Ganado is no longer invincible and its appearance played), the parasite kept in (no chance, none forced; only while it waits), the gun in hand's shot or the first shot, the request's fields; `game::key_hash` against the game's own string hash run in the mapped exe; and the game's code the mod relies on (requestAttack reads the ID and key where the mod writes them and looks the key up; the request pool handed on by the hit controller's update to the HitManager; calcInfo's OnCalculateDamage before hitSetting's OnDamageHit; CalculateInfo carries the attack's number; the game's own direct damage numbers its attack the same way; onEnemyDead counts the combo before the reason and scores on both of its paths; the damage effect's constructor leaves its `_Players` empty, its doStart makes them, its request calls each with no null check, `callbackDamageHit` asks `<DamageEffect>`, `HitController.awake` takes it from its own game object; a parasite's `onAppearance` sets `<IsFinishParasiteAppearance>`, its start clears it; the Ganado's `onStart` holds a waiting parasite's Ganado alive, `parasiteCheck` draws the lot through the head's `checkParasiteAppearance` with its chance and loses to `parasiteFailedDead` (NoDeath off), each `checkParasiteAppearance` - the base's and the three Mercenaries' - lets force win and a chance of 0 lose before any draw; `CustomConditionBase.check` asks each spawn point condition through its table's slot 7, the three classes' checks are that slot, `tryGetSpawnPoint` checks the camera, then the conditions; and the spawners: `spawner_states` and `ready_spawn_points` over a MercenariesModeController's `_SpawnerList` of spawners with their cooldown timers, targets and sent lists - only an open, unspent spawner's timers that have run a second are marked finished; the game's code: a point waits on its own one-shot timer before the camera and the conditions, `start` makes each timer one-shot and finished, `Timer.update` keeps a finished one-shot timer finished, `requestSpawn` resets the used one and marks the spawner spent once `getNextSpawnParam` finds none, `IsExterminated` clears the stage at `_SpawnEnemyMax`; `lift_spawn_rules`/`restore_spawn_rules`: a held-up spawner's out-of-view-only points and distance lifted once, put back exactly, nothing written for another controller; the game's code: the camera test only for a point marked out of view only, then the spawner's distance, then the conditions) |
| `test_challenges` | the challenges' rules (the CP's sum, a counted one's count and bits) and names (`challenges_text.h`), and - given `re4.exe` - the game's code they follow: `unlockRecord`'s CP clamp, `getProgressNum`'s game-save ones, the bonuses given outside the challenges |
| `test_exshop` | the Extra Content Shop window over a GameRecordManager (its `_Instance`) with the shop's table (an ExContentsShopUserData's InfoData, in the shop's order), the items' ExShopData and the CP, `share.ExtraContentsManager`'s list of DLC owned and the shop's screen with its tabs, its page's list, cursor and grid, made the VM's way from the real database (`exshop.cpp` built in - its insides reached by name; link every `build/*.o` but `dllmain.o` and `exshop.o`): the states the shop shows (locked, for sale, not enough CP, purchased - by the DLC with every main-game item too), its tabs (Separate Ways' only while its screen shows them; every tab with an item when no frame of it came for 2 s), unlock and lock (only while the shop is up, never a purchased item, never the CP, nothing for a bonus it does not sell; a batch summed up once; the system save asked for); the shop's page shown again (the shop's code it calls recorded, not run: after a change the screen's next frame asks the selected item's caption, then the page at its cursor - once; nothing without a change; not while the player holds to buy, then at the hold's end; the page alone with the cursor past the list; nothing with its grid missing, after the shop closed, or without game calls - the window saying so); and the game's code it relies on (isUnlockBonus and checkBuyBonus read `_IsUnlock`/`_IsBuy` but for that DLC's items - not CP11, GameMode01, CP12; updateCurrentGrid sets each entry from them and the CP against the price, calls nothing but the entries' set-up and reads the grid and its lists; setupItem's states; a purchase starts only on an entry for sale, buyBonusItem asks nothing more and shows itself with updateCurrentGrid(CurrentListIndex, None), from the screen's frame through the hold's gauge; a hold lasts frames and the shop's scroll ends one; setCaptionMessage asks the unlock before the text; onSelectionChanged's caption then page; a tab switch runs setupList -> updateCurrentGrid; setupList's and setup's tabs; setUnlockBonus writes the same flag after its NEW mark). Broken versions of the module fail it: one that buys, one that locks a purchased item (2026-10-27); one that draws during a hold, page before caption, twice, after the shop closed (2026-10-28) |
| `test_re` | the TDB 71 reader against the real database |
| `test_proxy` | the proxy's checks of the exe's delay-load table and the original DLL |
| `test_load` | the built DLL loaded as the game loads it: thunks, the data forwarder, the mod thread finding no game, a clean unload |
| `test_config` | the ini: defaults, reading, the switches' write-back (line-preserving, retries while another program holds the file) |
| `test_itembox` | the mod's part of the item box: its life (no box before a menu or load, a load replaces it, the menus empty it), stacks and weapons, the panel's order, take/count/discard/unput only as seen; a save that cannot carry it writes the unsaved file once per box; the files read for Move into this game (one unreadable: neither), then held back (never moved in twice) until a save keeps the box and removed then - no copy left; a load or the menus first: offered again; a failed save: the unsaved file holds the box once |
| `test_panel` | the panel's item lists drawn headless (ImGui built with its test-engine hooks, which report each widget's label and place; `overlay.cpp` and `inventory.cpp` built into the test) over a made-up game and driven by clicks and typing: the off-grid lists' order and headings; ticks, All and a group's tick, Store (no question) and Remove (one question) for the ticked rows; two counts typed and Apply all (one question, one batch), a count typed back being no change, Undo all; ticks and a count in an Add list, Add (one question, one batch), Tick all listed; the Add list's tabs (every knife that can be upgraded under Weapons, after the rocket launchers - the Combat, Fighting and Primal Knives, in Separate Ways the Tactical and Elite Knives too; the Kitchen and Boot Knives under Sub-weapons); the box's Take and Throw away; the tick working a batch through a few requests a frame with one note (each reason once), the box's entries found again after an earlier one went; the Extra Content Shop's window (`exshop.cpp` built in too, its view made up): the shop's tabs, a row's tick asking for its item at once (a purchased one's fixed), Unlock all and Lock all one question each for what is listed (a tab's, the filter's), Separate Ways' items only with its tab, nothing while the shop closes. `shots` draws each step to a .ppm |
| `test_savefiles` | the getters' decoding, the save list's own wait-and-rebuild code the refresh relies on, and its DialogWait (where a copy holds it still: no update, no closing); the moments a list is seen at (`stamp.h`: one read in its own millisecond is recent, even or odd) |
| `test_app`, `test_adopt`, `test_input`, `test_keyboard`, `test_mem` | the frame hooks beside REFramework's, the DXGI adoption, the input guard, the panel's keyboard, guarded memory |

### Testing without playing

The user play-tests: never drive the game in their session, never send it input or take screenshots there.
A headless run of the game is only ever in a gamescope of our own with its own prefix, and must never reach
the user's saves: the game runs as app 2050650 under the logged-in Steam client, so its Steam Cloud reads and
writes are the user's real saves. Make sure no headless gamescope is left afterwards.

## ⛔ Rules

- **Never patch `re4.exe`**, on disk or in memory. The mod changes data only: field writes, the method
  entries of the type database and vtable slots it exchanges (`events.cpp`), the application's entry table
  (`app.cpp`), import slots.
- **Never use absolute addresses or hard-coded offsets in code.** Everything is found by name in the type
  database (classes, fields up the class chain, enum values, methods by name and parameter types), or by
  code patterns cross-checked across many sites (the VM global, its helpers, the app's entry table). The
  RVAs and offsets in this file document build 22377325 only.
- **Game memory from the tick or a hook only.** The panel runs on the render thread: it flips atomics and
  posts requests; the tick applies them (`dispatch.cpp`). Hooks touch only the objects the game handed them.
- **Edits only while the pause menu is up** (and never while the attache case's own screen is), save-file
  changes only while the game's list is idle and its saves are not busy, challenges only while the game's
  challenges screen is up, the shop's items only while the Extra Content Shop is up.
- **Game calls: only the approved kinds.** The siblings' user approved: the attache case's own functions
  (RE4OGCabbyCodes), the item box's Take and Store and the difficulty switch (RE2/RE3CabbyCodes), the save
  files' delete and list refresh (RE2/RE3CabbyCodes), and event hooks. For Steady aim, Automatic QTEs, the
  save copy, costumes in game and the automatic shooting gallery the user said (2026-10-20) "add whatever
  game hooks you need": the hooks and calls those features make are listed below. For the challenges window
  the user chose (2026-10-22) "flags and achievements": switching a challenge on unlocks its Steam
  achievement through the game's `AchievementServiceController.unlock`, as RE2's records panel does; its
  changes are saved through the save copy's `requestSaveSystemData`. For the item box the user chose
  (2026-10-23) "both, each save its own": the game's storage for what it takes, the rest carried in the
  save, approving the storage's `addArmouryItem`, `reduceArmouryItem`, `ItemExtension.isEnableMoveToArmoury`,
  `List<chainsaw.Item>.Add` and the hooks on the storage's `saveGameSaveData`/`loadGameSaveData`. For Kill
  enemies as they appear (the Mercenaries) the user chose (2026-10-26) "a hit from you": the game's own direct
  attack request `HitController.requestAttack` from the player's body hit controller, numbered by
  `HitManager.generateAttackID`, its two data objects made by `System.Activator.CreateInstance` (the save
  copy's), the target's game object through `via.Component.get_GameObject` (the gallery's) - over the other
  offer, `Ch1c0HeadUpdaterCommon.externalDead` (Ganados only). For the Extra Content Shop window the user asked
  (2026-10-28) "add the instant refresh call so the shop updates immediately": the shop screen's own
  `ExContentsShopGuiBehavior.setCaptionMessage` (the selected item's name and text) and `updateCurrentGrid` (the
  entries in view, as after its purchase), from the hook on its frame. Every call
  this mod makes is listed under "The game's calls" below. **Ask before adding any other call.** Field writes
  need no approval.
- Back up `Steam/userdata/<id>/2050650/remote/` before experiments that write saves.
- The user commits every repo themselves - do not `git commit`/`push` unless asked.

## Why steam_api64.dll

`re4.exe` delay-loads `steam_api64.dll` (12 functions by name), the DLL ships beside it, and it is not a
Wine builtin - so Proton loads a native one with no launch option, on Linux and Windows alike, and
`dinput8.dll` stays free for REFramework. The proxy (`src/proxy.cpp`, exports generated by
`tools/gen_proxy.py`) forwards all 1058 functions by jump thunks and the one data export
(`g_pSteamClientGameServer`) through a PE forwarder to `steam_api64_orig.dll`. It is tolerant: the stock
DLL is recognised (295336 bytes, 1059 exports), and any other original with every function the exe imports
still works (logged); a missing original, one lacking an imported function, or an exe importing one the
proxy does not export, stops the game with a message box that says what to do.

## Threads and the tick

- **DllMain** (the game's first Steam call, early in WinMain, after SteamStub unpacked the image): loads the
  original, the crash logger (the exe's `SetUnhandledExceptionFilter` import), the ini, the item box files
  waiting to be moved in (counted), the
  DXGI factory hooks (`overlay_d3d.cpp`: the game's swap chains adopted as its factory makes them), the input
  guard's import hooks, then starts the mod thread.
- **The mod thread** waits for the VM (`re::init`: the type database in `.data`, rebased by the VM; the VM
  global, found as the `mov rcx,[rip+X]; mov edx,-1; call` target loaded dozens of times; the database
  pointer at VM+0x3000, the static table 0x30 before it), then `game::discover`, `savefiles::discover`,
  `challenges::discover`, `events::install` (before any character exists: the database-entry hooks must be in before the game makes
  delegates of them), `call::init`, and hooks the frame (`app.cpp`).
- **The tick** (`dispatch.cpp`): via.Application's entry table (found from its `add_function`: count +0x3AC,
  table +0x808, 0xD0-byte entries, function +0x8, name +0x18, priority +0x20) - the mod's function goes in
  front of `UpdateBehavior` and `EndRendering` (the latter only for frames whose behaviours did not
  update). The entries run as jobs on the engine's thread pool: the tick is serialised and never waits.
  REFramework hooks the same entries; the mod goes in front of it and calls on (`test_app`).
- **Events** (`events.cpp`) run on whatever game thread calls them; they read switches (atomics) and touch
  only their own objects (and what hangs off them). A hook that calls the game opens a `call::InHook` first:
  the bridge refuses calls outside the tick otherwise.
- **The panel** draws in the swap chain's Present (render thread), from snapshots the tick publishes
  (`dispatch::snapshot`, `cheats::status`, `inventory::view`, `savefiles::view`, `itembox::view`,
  `challenges::view`, `exshop::view`). Four windows, each shown by itself at its screen, folded or open as the player left it
  (the user, 2026-10-23: "make all imgui menus remember collapsed state across restarts" - ImGui keeps a window's
  `Collapsed` in `RE4CabbyCodes.imgui.ini`, the panel's sections are the mod's own lines there; until then each
  window was forced open as it came up):
  the pause panel, the save files beside a load or save list, the challenges beside the challenges screen
  (over the pause menu too), the shop's items beside the Extra Content Shop. F7 hides and shows the window of the screen it is pressed at (back by itself
  the next time that screen opens); in play it opens the pause panel over the game; out of a game (the
  title, the main menu) it does nothing - unless the mod cannot see the game (no tick, not understood),
  when it opens the panel to say why.
- **Moments** one thread notes for another (`stamp.h`: a screen's frame seen, a save held, the gallery's
  update): `GetTickCount() | 1` (0 is none), aged on a clock with bit 0 set too, the moment loaded before
  the clock is read. Aged on the bare clock, a moment of an even millisecond read while GetTickCount still
  showed it (it holds a value up to 16 ms) was 49 days old: the save list read as closed in those frames
  and the save files window flashed (2026-10-23). The log says when a save or load list comes up and closes.
- **What a frame costs** (the user, 2026-10-24: look for checks that run every frame instead of on demand).
  The tick and the hooks run inside the game's frame, a few hundred field reads of it. A guarded read is a
  copy (`mem.h`): `VirtualQuery` scans Wine's page table (5-15 us in the game's heap; one per read once took
  the game from 120 fps to 30), so it is for rare checks - the call bridge checks a thread's VM context once
  (`call.cpp`). A field's address is one read of the object's REObjectInfo (its class, its fieldptr offset
  just before it) with no lock and no table (`re.cpp`); a List's two fields sit in a lock-free table per class.
  Names are found once at discovery - a field looked up by name every tick (`re::find_field` is a string
  scan up the class chain) is kept per class instead. A switch that is off reads nothing; the save files'
  busy check runs only while a list is up or a change is under way; the item lists are rebuilt (4/s) only
  while the panel draws them, and the panel copies the view only when the tick published another. Raw Input
  (the mouse's every packet) never waits for the ImGui lock, and Present waits for the GPU without it. The
  panel's item requests are worked through at most 12 a tick and 4 ms (a batch of hundreds spreads over a
  few frames, the pause menu up).

## Windows (audited offline; never run there)

- **DXGI**: the game's swap chains are adopted as its factory makes them, each with a vtable of its own, so
  the Steam overlay or a DXGI wrapper hooking the class never meets the mod's functions (`test_adopt`). The
  fallback (the class vtable through a throwaway swap chain) only if none presents within 10 s.
- **REFramework** (dinput8.dll): no file clash; both hook the application's entries, and the mod chains
  (`test_app`). The database-entry hooks check that the entry still holds the game's own code before taking
  it, and re-hook if the runtime writes the entry again.
- **Input**: the game reads the mouse as Raw Input and the keyboard through DirectInput (`keyboard.h`: the
  panel has its own non-exclusive DirectInput keyboard); `GetCursorPos`/`GetAsyncKeyState`/`GetKeyState`
  imports guarded while the panel has the pointer; `ClipCursor` calls that change nothing are not passed on.
  Swapped mouse buttons are read physically (`SM_SWAPBUTTON`).
- **Exit**: DllMain does nothing on a process exit; the DLL pins itself.
- **ASLR**: nothing is an address; the discovery test runs the exe at whatever base Wine maps it.
- **Files beside the DLL** (the ini, an item box a save could not carry): written whole to a temporary and
  moved over, retried 5 times 100 ms apart while a virus scanner or the indexer holds them (`files.h`;
  `test_config`, `test_itembox`).
- **The game folder** under Program Files: Steam's library folders are user-writable; the log, ini and item
  box files live there, as the siblings' do.
- **Open**: never run on Windows; paths are ANSI.

## What the game does (build 22377325; offsets are from the field pointer, +0x10 plain objects, +0x50 via.Behavior)

### Where the game is (`game::run`)
- `share.MainModeManager._Instance.CurrentGameState.GameState`: its class - `chainsaw.GameStateInGame`,
  `GameStateInGameChapterChange`, `GameStateInGameInterlude` count as in a game.
- `share.ContentsManager._CurrentCampaign` (`share.CampaignController`): `<CurrentPhase>` Active (3) and
  `<CampaignID>` Campaign1001 main / 1011 Separate Ways / 1021 Mercenaries.
- The pause menu: `chainsaw.GuiManager.<GuiOpenCloseData>.<ActiveGuiTypeDatas>[GuiType.InGameMenu 53]
  .<ActiveNum>` > 0; a save or load list: GuiType.SaveLoad (8); the challenges screen: RecordList (134); the
  Extra Content Shop: ExtraContentsShop (17).
- The attache case's own screen: `AttacheCaseManager.<AcSetupController>.<CurrPhase>` past WaitOpen.

### Health (God mode, Ashley)
- `CharacterManager._Instance.<PlayerContextList>[0]` (Leon 100000, Ashley-as-player 110000, Ada 380000,
  the Mercenaries 600000-600005), `<PartnerContextList>` (Ashley 200030). `CharacterContext.<HitPoint>`:
  `<DefaultHitPoint>` (max), `<CurrentHitPoint>`, `<NoDamage>`.
- `HitPoint.addDamage` takes nothing and `checkDead` says no while `<NoDamage>` is set - and the game's code
  writes NoDamage only on enemies' HitPoints. God mode holds it on the player (and HP at max) every tick;
  switching off clears it on that character only.
- Ashley goes down on any hit with damage (`PartnerNpcCautionWaitDriver`): the hit-calc hook (below) zeroes
  the damage of a `PartnerBodyHitDriver` hit whose context is KindID 200030.
- Luis (KindID 200032, a partner as Ashley is) and Separate Ways' time limit to save him: God mode holds both (below).
- The water scooter of the escape has a health of its own, which God mode holds too (below).
- Not covered: scripted deaths without a hit (falls, traps' own death scenes, QTE-failure scenes), and Del
  Lago's boat (`GmBoat.onHitDamage` asks `requestGameOver` itself; not read).

### The water scooter (God mode) - field writes (`game.cpp` jet_ski, hold_jet_ski; `cheats.cpp` hold_jet_ski)
The user (2026-10-26): "the god mode cheat needs to work for the escape sequence on the jetski at the end of the
game". The scooter's crashes never touch Leon's HitPoint: God mode's NoDamage did nothing there.
- `chainsaw.JetSkiManager` (AppSingleton) `<MainJetSki>k__BackingField` = `_JetSkis[0]` (`registerJetSki`), a
  `chainsaw.GmJetSki` (via.Behavior: fields +0x50). Its hull: `_CurrentHP`, up to `_JetSkiSetting`
  (`GmJetSkiUserdata`, a via.UserData: fields +0x20) `._MaxHP`; the HUD's Vehicle gauge (GuiType 94) is told it by
  `onUpdate` whenever `_LastGuiHP` differs. The health is 0 until a ride starts: `resetCarStartPosture` (from
  `set_NextGameState(Run)` and `onUpdate`'s own start of the ride) sets it to the max and zeroes
  `_LastDamage`/`_LastShock` and both grace timers.
- `onUpdate` (JetSkiManager's update) runs `updateCar3` once `_NextGameState` >= Run: a fixed-step loop (several steps
  a frame at a low frame rate), each step copying `_NextGameState` into `<CurrentGameState>` (GameState Standby 0,
  Run 1, Crash 2). A collision's damage and shock go to `processDamage(Int32 damage, Int32 shock, DamageTier, Single
  dt)` [217083] (exe+0x101ED20; its only caller updateCar3): both timers (`_IgnoreDamageTimer`,
  `_IgnoreDamageReactionTimer`) lose dt (to no less than 0); a hit with damage while `_IgnoreDamageTimer` > 0 that is
  no worse than `_LastDamage` returns 0 at once - nothing written, no camera shake, rumble or crew reaction;
  otherwise `_LastDamage`/`_LastShock` = the hit's, both timers = the settings' `_IgnoreDamageTime`, the shake and
  rumble, then the health: max(hp - damage, 1) inside `_ImmortalArea` (an OBB) or with the difficulty's
  `getSurviveWithGuts` and hp > 1, else max(hp - damage, 0); none left: `set_NextGameState(Crash)` (its only caller
  in the exe) - the crash timer from 0, the buoyancy lost, the sinking; then `GameOccurrenceManager.cause(
  VehicleDamaged 82, ..., damage)` (the results' times damaged). A shock without damage has its own timer.
- `updateGameOverTimer` (exe+0x10842F0): in Crash, `_CrashElapsedTimer` grows to `CharacterManager.requestGameOver
  (YouAreDead 1, cause 1)`. Nothing else ends the ride: no other jet ski class writes the health or the state.
- The hold (the tick, God mode on, only while both states are Run): the health at its max, `_LastDamage` = INT32_MAX
  and `_IgnoreDamageTimer` = 60 s (topped up below 30): processDamage's own grace passes over every hit with damage,
  however hard. Not while waiting for the ride (Standby) nor in a crash under way. Off: where `_LastDamage` is still
  the mod's, it and the timer go back to 0 (the next hit counts). No call of the mod's, no hook. The panel shows
  "(water scooter n / max)" beside God mode during the ride. The log: "god mode: the water scooter takes no damage
  either - its hull held at n of max" once per scooter. `test_jetski` checks the hold and the game's code.
- Side effect: `tryCheckNoDamageRecord` (no direct caller: invoked by name) unlocks challenge 072 Smooth Escape
  ("Escape on the water scooter without taking any damage") when `_CurrentHP >= _MaxHP` - its achievement too
  (`GameRecordIdDefinition._Table`: 72 -> achievement 20). The hull finishes full with God mode on, so the ride
  earns it. Holding it one point short would not; the user was told (2026-10-26).
- A countdown in that part (`TimerGuiBehavior.isOpenTimerStopGui` reads JetSkiManager's `IsPauseTimerByInteract`)
  is TimerManager's: Freeze countdown timer's, not God mode's.

### Luis (God mode) - field writes (`game.cpp` luis_context, hold_health; `cheats.cpp`) and a vtable hook (`events.cpp` kLevelTimer)
The user (2026-10-27): "the god mode cheat should also work for Luis's health in separate ways mode", then "the area
where I noticed this issue was in a burning building and luis was slowly draining health". Two things, both God mode's.
- His health. Luis is `CharacterKindID` ch2_a3z0 (200032; ch2_a3z1 200033 is a scripted DollNpc), a partner as Ashley is
  (`CharacterEnumDefine.isPartner`: 200030 or 200032): `Ch2a3z0HeadUpdater` (Separate Ways' `..._cp11`) derives from
  `PartnerHeadUpdater`, his context is a `PartnerBaseContext`, and `CharacterManager.onFrameHeadUpdate` rebuilds
  `<PartnerContextList>` every frame from the partner contexts - so `game::luis_context` is Ashley's lookup by his kind.
  A hit: `PartnerBodyHitDriver.onHitDamage` [720321] (exe+0x1203620) cuts its damage to his health less one unless his own
  context's `PlayerBaseContext.<State>` holds `PlayerDefine.State` Battle (bit 60, set from `PartnerBattleUpdater` by
  `PartnerHeadUpdater.updateBattleMode`); then `PlayerBodyHitDriver.onHitDamage` and the head's `onHitDamage`
  (`Ch2a3z0HeadUpdater` [672088] -> `PartnerHeadUpdater` -> `PlayerHeadUpdater` -> `CharacterHeadUpdater`:
  `HitPoint.addDamage`, the hit's `<IsKill>` -> `PlayerHeadUpdater.onDead` [342603], the dead action and the game over).
  And every frame `Ch2a3z0HeadUpdater.update` calls `ForceDown` [672089] (exe+0x20A03E0): not `<Invincible>` and no health
  left - `PlayerHeadUpdater.requestDead(Normal)`. Burning takes nothing of his (`PlayerHeadUpdater.calculateBurning` damages
  only `CharacterEnumDefine.isPlayer` kinds); his HUD controller `PartnerNPCGui_ch2_a3z0_Controller` is the ammo he hands
  over, not his health. The game's own `NPCNoDamageZone` (Luis only) uses the body's `requestInvinsible` (HitPoint
  `<Invincible>`), not NoDamage: nothing to fight with.
  The hold (the tick, God mode on): the player's - `<NoDamage>` on (addDamage takes nothing, checkDead says no), health
  back at its maximum; off: NoDamage off on that context only. The log: "god mode (Luis): no damage, health held at its
  maximum" once per context; the panel shows "(Luis n / max)" beside God mode while he is a partner.
- The time limit to save him (what drained). The only `chainsaw.CustomLevelTimer` in the game's scenes (every SCN/PFB/USR
  in the paks scanned for its type id): a Separate Ways scene in re_dlc_stm_2109300.pak (path key ca8d9bfd089c890e;
  loc51, chapter 3's flanking tower, 側防塔), GameObject `CustomLevel_Timer_ルイスを助ける制限時間` ("time limit to help
  Luis"), beside `LevelAreaHit_cse305_ルイス炎の中へ_FlagSet` ("Luis into the flames"), `ルイス無敵処理` (a LevelFlagCheck
  "down_invincible") and an `NpcNoDamageZone`. Its settings: `_UseDefficulty` on, `_TimerSecond10/20/30/40` 180 s (every
  difficulty; `_TimerSecond` 45 unused). `start` [675966] makes `_Timer` a `chainsaw.Timer(limit, FunctionType 0, CountMode
  0)`; `lateUpdate` [675967] (exe+0x3127600, slot 6, no direct caller, no subclasses) returns while its `_EndFlag` or
  `_StopFlag` is set or its `_StartFlag` is not (`ScenarioFlagManager.checkFlag`), and while the player has no health;
  else `Timer.update(elapsed)` (`_TransitTime` += dt; `updateCompleted`: done once `_TransitTime` >= `TimerBase._TimeLimit`),
  opens GUI `Timer_31_1011` (156; `Cp1011Timer31GuiBehavior`, a VehicleGuiBaseBehavior) with MaxHP 100, CurrentHP
  (1 - `get_ProgressRate`) x 100, CautionHitPointRate 0.2 - the bar that reads as Luis's health draining - and once the
  timer is done sets `_EndFlag`; the game over (`GameOverCauseType` Other_Cp11Chp31Ch2a3z0TimeOver, 75) follows from that
  flag in data (which code asks for it was not read). No property exposes `_Timer`: start and lateUpdate are its only users.
  kLevelTimer, the vtable hook on lateUpdate: with God mode on and the timer not done, `_TransitTime` = 0 before the
  game's frame (`game::hold_level_timer`) - the gauge shows full, the time never runs out; switched off, it runs its 180 s
  from full. A frame that moved the time on says it is under way: the hook notes the seconds left
  (`events::luis_time`), the tick keeps them while unpaused (the game's frames stop under the pause menu), and the panel
  shows "(time to save Luis held)" beside God mode, or "(m:ss to save Luis)" with it off. The log: "events: CustomLevelTimer.
  lateUpdate ... hooked" once the scene is up, "god mode: the time limit to save Luis held - 180 s, n gone when it took
  hold ..." once per timer. No call of the mod's. `test_luis` checks the hold, the timer and the game's code.

### Hits (One hit kills) - `events.cpp` kHitCalc, kParasiteCalc
- `chainsaw.CharacterBodyHitDriver.onCalculateDamage(HitController.DamageValue, HitController.CalculateInfo)`
  [333068] (non-virtual, exe+0x2695BB0): every character makes a delegate of it as it is configured, from
  the method's database entry. The mod exchanges the entry (TDB 71 holds an int32 offset from the decoder's
  base exe+0x39F1110, so it points at a near jump stub of the mod's) before any character exists. After
  the original: an `EnemyBodyHitDriver` hit with damage is raised to the enemy's `<CurrentHitPoint>`; the
  game's own checkDead/NoDeath/Immortal decide the rest (a boss held alive keeps its last point).
- A Ganado's parasite (the plaga out of a broken head; the user, 2026-10-24: "the parasite portion has normal
  hp while the zombie body still works in one hit"): `Ch1c0ParasiteUpdaterCommon` (subclasses `Ch1e2z0Updater`,
  `Ch1e3z0Updater`) has a `HitController` of its own, and its `start` (and `Ch1e2z0Updater.start`, for its
  option's hit controller) adds `OnCalculateDamage += Ch1c0ParasiteUpdaterCommon.onCalculateDamage(DamageValue,
  CalculateInfo)` [188978] (exe+0x1FA6B80; the damage x the rank's and the part's rates) - not the body
  driver's. Its health is the host's: `Ch1c0HeadUpdaterCommon.setupParasiteParam` writes the parasite's max HP
  (`Ch1c0ParamUserDataCommon.getParasiteA/BHitPointMax`) into the Ganado's context `<HitPoint>` Default and
  Current as it appears, and the parasite's hit controller measures against its context -
  `HitController.<ContextAccessor>` is the IContextAccessor component on its own object (the parasite updater),
  whose `get_Context` is `_OwnerHeadUpdater.<Context>` (`chainsaw.HeadUpdater`). kParasiteCalc, the same kind
  of entry hook, raises a hit with damage to that context's `<CurrentHitPoint>` (logged the first time).
- Other `OnCalculateDamage` handlers, read and left alone: the Regenerador's weak points and Ch1f8z0's eye forward
  to their parent's (`HitController.callbackCalculateDamage`: the body's, hooked); `GmGondolaEnemyBase.
  onCalculateDamageEnemy` only multiplies (x10000 for one weapon category); Separate Ways'
  `Ch1c0z1HeadUpdater_AO.onCalculateDamage` (its `registerDelegateBody` adds it to the body updater's hit
  controller) scales two attacks (by their hash) by rates of its own - should one of those not kill in one hit
  there, its order against the driver's is the place to look.

### Ammo and durability - vtable hooks on `chainsaw.CsInventoryController`
- `useWeapon(EquipType)` slot 40 [104800] - the only caller is `PlayerHeadUpdater.useWeapon`: Infinite
  ammo notes the equipped item's `_CurrentAmmoCount`, `_CurrentTacticalAmmoCount` and `_CurrentItemCount`,
  raises the count by one (so the last grenade is not removed inside the call), calls the game, and writes
  all three back. Only the player's controller (`InventoryControllerBase.<_CharacterContext>.<KindID>` a
  player's - Luis has a case too); an `<IsForceGet>` item (scripted) is left alone.
- `useWeapon(EquipType, Int32 cost, Int32&)` slot 41 [104801] - every knife wear (slash, parry, stealth kill,
  grab break) ends here with EquipType.Sub: No durability loss passes cost 0.

### Body armor (No armor durability loss, the editor) - database-entry hook `UniqueInventoryController.onDurabilityChanged`
The user (2026-10-24): "Add a durability cheat for body armor".
- The Body Armor is a unique item, ItemID it_sm77_501_00_0 (124001600) - the mod reads it as the controller's own
  literal `UniqueInventoryController._ArmorItemId`. Its wear is the item's `_CurrentDurability`, up to
  `Item.get_CurrentDurabilityMax` (the definition's `_DefaultDurabilityMax`, 1000, for what is not a knife); `_CurrentCondition`
  Broken (1) once none is left. `PlayerEquipment.existsArmor()` = `UniqueInventory.existsItem(armor, ignore Broken)`:
  a broken armor is never used.
- What it does: `PlayerBodyHitDriver.onCalculateDamage(Int32, CalculateInfo)` (virtual 23, called from
  `CharacterBodyHitDriver.onCalculateDamage` [333068], kHitCalc's) - while the armor exists, a hit whose attack type is in
  `PlayerCommonParamUserData.<MeleeAttackTypes>` loses `_ArmorDamageRate`% of its damage to it, any other hit all of it;
  the cost (what it took / `_ArmorDivideValue`) goes into the driver's `ProtectedAttackIds[the hit's key]`. `onHitDamage`
  (virtual 21) reads it back, calls `UniqueInventory.useDurability(armor, cost)` directly, then compares
  `PlayerEquipment.getArmorDurabilityProportion()` (a percentage; -1 none) before and after: from above 10 to 10 or less,
  `CharacterTalkManager.requestSpeech(.., LeonArmorFew 4)`; none left, `requestCancel`. Fire and frost
  (`PlayerHeadUpdater.calculateBurning/calculateFreezing`) and the boulder (`GmRollingStone.playDamageMot`) give the armor
  a share of their damage through `PlayerEquipment.useArmor(cost)` (a tail jump to useDurability;
  `UniqueInventoryController.useArmor` the same, never called). No caller reads useDurability's return.
- `UniqueInventory.useDurability(ItemID, Int32)` [92497] (exe+0x2E947E0): under the list's lock, the first valid entry
  of the id: `Item.setDurability(d - count)` (clamped to 0..max); none left: the armor marked Broken (any other item is
  removed); unlock; then `OnDurabilityChanged(id, before, after)`, an `Action`3` whose only subscriber is the controller's
  `onDurabilityChanged(ItemID, Int32 prev, Int32 curr)` [106248] (exe+0x2FBA770), a delegate `registerInventoryEvent` makes
  from the method's database entry. That handler returns at once for prev == curr, else (the armor's id only) moves
  the dynamic difficulty's points (`GameRankSystem.getRankPointItemDurabilityData` + `addRankPointDirect`; a rise
  `ChainsawGuiUtil.addRankPointRecoverItemDurability`).
- The hook (kArmorWear): with the switch on, a fall on the player's controller (`player_controller`) is put back inside
  that call - `game::keep_armor`: the controller's item of the armor's id holding `curr` gets `prev` again, and a Broken
  mark (this wear's: it had some left) comes off - and the handler is told (prev, prev). So the "armor nearly gone" line,
  the HUD's armor gauge (`ArmorGaugeGuiWork.applyGuiParam` reads `Item.get_CurrentDurabilityProportion` as it opens) and
  the rank find it as it was. A rise is the game's (`UniqueInventory.repair(Guid)`: the max set, Broken cleared, then
  told). The switch mends nothing: an armor broken before stays broken. No call of the mod's.
- The editor (`cheats::request_armor`, applied by the tick while the pause menu is up and the case's screen is not):
  `_CurrentDurability` written (0..max); 0 marks it Broken as its wear does, more takes the mark off as the repair does.
  Beside the switch the panel shows (n / max), (broken) or (you have no body armor).

### Steady aim (field writes, `game.cpp`; the camera's hooks, `events.cpp`)
- The gun in hand: `PlayerHeadUpdater.<Equipment>` (`chainsaw.Equipment`) `WeaponList[_EquipWeaponID]`, a
  `chainsaw.Gun`. Focus: `Gun._CurrentReticleFitPoint` held at `Arms.<ReticleFitParam>._PointRange` s + r
  (via.Range: start and range; `Gun.updateReticleFitPoin` clamps to it).
- Spread: `Gun.<ShellGenerator>._UserData` - a `BulletShellGeneratorUserData._ShellInfoUserData` or a
  `ShotgunShellGeneratorUserData` `_CenterShellInfoUserData` / `_AroundShellInfoUserData` - `._MoveInfo`
  (`BulletShellMoveInfo`) `_RandomRadius` / `_RandomRadius_Fit` set to 0. That is the weapon's user data,
  shared: the originals are kept by object (and the weapon they were taken from) and put back when the
  switch goes off, on the player's guns that still hold the mod's zeros.
  The rocket launcher's: `RocketLauncherShellGeneratorUserData._RocketLauncherShellInfoUserData._MoveInfo`.
- Recoil: `PlayerCameraController.requestRecoil` keeps its own copy of the gun's `CameraRecoilParam` in
  `_RecoilParam` (`init()` draws `_Yaw`/`_Pitch`), and `updateRecoil` turns the camera by them along the
  curve. The camera: `CharacterContext.<CameraController>` (`MainCameraController`)
  `_CameraControllerArray[0]` (a `chainsaw.PlayerCameraController`). The copy's `_Yaw` and `_Pitch` are held
  at 0 every tick and at once after `PlayerBehaviorTreeAction_MFSM_GunFire.onStart` (slot 10, kGunFire):
  nothing moves, nothing is folded in. Nothing shared, nothing to put back.
- Sway: `getRStickOffset()` (exe+0x13BA950) = `_OldRSOffset` + (target - old) * `_RSOffsetLerp` (target 1, or
  `WeaponHandShakeParam.RStickOffset` while the stick moves; each read adds 2 x the frame's seconds to the lerp)
  scales both the camera's hand shake and the breathing layer (`MFSM_HoldShake` / `ScopeDangerHoldShake` blend
  rates, `getHandShakeRate` = the curve x it); both held at 0 every tick. Never touch `WeaponHandShakeParam`:
  its `Time` = 1e9 freezes the curve at its largest.
- The frame (the engine's module entries): UpdateBehavior (the tick) -> UpdateBehaviorTree/UpdateFSM (the
  HoldShake actions, onUpdate slot 11: they set their motion layer's blend to the rate only for a weapon with a
  sight or scope - `PlayerEquipment` virtuals 20-23) -> LateUpdateBehavior: `MainCameraController.lateUpdate`
  (exe+0x3452700) -> `CameraControllerRoot.advancedTime` (the busy controller's vtable slot 20,
  `PlayerCameraController.onCameraUpdate`, exe+0x13E4230: `updateRecoil`, `updateHandShake` exe+0x1470C60) ->
  `interpolateCamera` (exe+0x344EA80), which applies the `_ShakeController` (`CameraShakeController.apply`) at
  the busy controller's slot 17 `get_CameraShakeApplyRate` (exe+0x5B6EF0: 0.05 in a sight or scope - `checkScope`,
  states 7-10 - else 1), then, only while `_IsHandShake`, turns the view by `_HandShakeRotation` and clears it.
  `updateHandShake` hands it over (`_IsHandShake` = 1, `_HandShakeRotation` = the damped `_HandShakeAngle`, from
  the body's `Null_Offset` joint x the rate) in the states `checkHandShake` names - Hold 6 to HoldExtraScope 10,
  HoldVariation 33, Gimmick 15 with GimmickType InstalledMachineGun 6 - when the busy controller is the player's.
- A shot's own camera shake: `PlayerEquipment.execFire` (exe+0x11A8C80) requests the weapon's
  (`WeaponCustomManager.getCameraShakeParam` -> `CameraShakeController.request` on the main camera's
  `_ShakeController`); the mod never touched it before 2026-10-23 - the jolt left when firing.
- The hooks (2026-10-23; the user: "Steady aim doesn't seem to do anything, aim still wobbles" - and no log
  line ever said the tick's camera writes landed): `PlayerCameraController.onCameraUpdate` slot 20 (kCameraUpdate)
  - recoil and the stick lerp zeroed on the controller it is called on, before; the hand shake it handed over
  taken back after (`MainCameraController._IsHandShake` = 0 through `CameraControllerRoot._MainCameraController`);
  `get_CameraShakeApplyRate` slot 17 (kShakeRate, a float return) - 0 while the camera aims
  (`game::camera_aiming`: `_CurrentStateParam.<State>` in checkHandShake's states or PumpAction 21); the two
  HoldShake actions' `onUpdate` slot 11 (kHoldShake, kScopeShake) - the lerp zeroed just before they read the
  rate. All reached only through their vtables (no direct calls); the camera class has no subclasses. The tick
  keeps its writes; `game::player_camera` logs once where the way to the controller fails, and once that it was
  reached. `test_aim` checks the helpers on game-shaped objects.
- The arms' sway (2026-10-24; the user: "steady aim could be further improved by stopping the moving animation of
  the player while aiming"). The two shake actions are nodes of the player's shoot layer (its motfsm2 - six in the
  archives hold them, all alike: root -> Shoot, IDLE, Locked, Hold, HoldEnd, Dead, ...; Hold ->
  `wp4400H_0162_hold_breath_loop_add` with HoldShake, `wp4000H_0161_hold_danger_loop` with a joint mask and
  ScopeDangerHoldShake, and Idle, which plays nothing). The aiming pose is another layer's (`ch0_160_HOLD_LOOP`).
  Each action's onUpdate sets its own layer's blend (`TreeLayer.set_BlendRate`, inlined: +0x350's setter, clamped
  to 0..1) to `getHandShakeRate` - but only when the equipment answers yes to `get_IsEnableIronSight` (a rifle,
  WeaponCategory 4, without its three scopes) or a scope (`get_IsEnableOpticalScope`, `...SpecialOpticalScope`,
  `...ThermalScope`; ScopeDanger asks only these); onEnd sets it back to 1. With any other gun the breath loop
  played at full weight over the pose - the arms and the gun swayed in view (the danger loop, at low health, shook
  them). kOpticScope: `PlayerEquipment.get_IsEnableOpticalScope` slot 21 (no subclasses) answers yes while either
  action's hook runs with Steady aim on (a thread-local, set only once the player's camera is reached and its lerp
  zeroed): the game then sets the blend for every gun, to the flat rate - the layer as its Idle node leaves it. The
  game's answer everywhere else. Switched off mid-aim, the layer stays flat until the hold ends. No call of the
  mod's. `test_aim` checks the game's code this leads: each action's onUpdate asks the scope check through the
  table, then calls `getHandShakeRate`, then the blend's setter; its onEnd calls the setter.

### Automatic QTEs
The prompt GUIs judge nothing: each opener reads its own input. Two mechanisms cover every prompt.
- The ActionSign system (every enemy, boss and projectile prompt and the boulder; Leon, Ada, Ashley):
  `PlayerHeadUpdater.onHitActionSign` hooked in every head class's own vtable (a family hook: one slot per
  subclass, each with its own original; `re::vtable_of_object`). After the original, a sign at its act or
  "just" timing whose driver (`<ActionSignDriver>`, `PlayerHeadActionSign`) has taken it
  (`<RequestEnable>`, not `<Accepting>`/`<RequestReserve>`, not `_CantParry`) is pressed by the driver's
  own `requestReserveAvoid()` or (virtual) `requestReserveParry()` - when it is a QTE: a large sign
  (SignType > ParrySignTypeMax), Krauser's knife fight (1/2), or a normal sign drawn large
  (`ActionSignGuiUnit.checkNormalActionSignDispLargeUI`). Ordinary parries are left alone.
- `share.hid.Command` triggers (rebuilt in `PlayerHeadUpdater.updateCommand` every frame, read after it):
  injected in a hook on the reader right before the original - `Command.setValue(hash)` (a one-frame tap)
  or `_Triggers.TryAdd(hash, true)`; never from the tick (it runs before updateCommand). Hashes are
  `PlayerCommandType` literals (Avoid, MeleeHold, Escape_Knife, Escape_ButtonRapid); `GuiCommandType.Decide`.
  - the laser corridor: `GmQTELaserTrap` (database entry, kLaser) - `calcAvoidCommandHash` (Separate Ways'
    remap); the second laser `GmQTELaserTrap3.updateGimmick`; Ada's hookshot `GmHookShot.updateGimmick`
    (`AdditionalOptionManager.getCp11HSCommandHash`); `GmTentacleAvoid.updateGimmick` (Decide);
    mutated Krauser's grab `Ch1f7z0...CheckGrappleParryEscape.update`.
- Grabs: `ButtonMashingEscapeCondition.update` (slot 13): `<CurrentCount>` set to `<RequiredCount>`;
  `KnifeEscapeCondition.update`: the knife's button when `canOpenGui(head)` (a knife to hand); Ganados'
  holds (`Ch1c0z0...CheckEscapeGrapple.update`) and Regeneradors' (`Ch1d4z0...`): their own counts made
  enough, or the knife's button where only the knife frees; the swim back to the boat
  (`Ch1f1z0...SearchEvent_Start.update`): its stroke count made its maximum.
- The timed lever with Ashley (`GmTimingShutterGauge.updateGimmick`): `_CorrectAreaSize` widened to the
  gauge (kept and put back when the switch goes off).
- No cutscene/timeline QTEs exist (no timeline reads input); the minecart, jet-ski and statue have none.

### Shooting gallery - vtable hook `ShootingGallery2Controller.update` slot 15
- After the original, while `_Routine` is Game (4): every `_TargetMove` animator
  (`ShootingGallery2Animator`) in Move, `<IsDisplay>`, not `<IsBroken>`/`<NeedDown>`, whose kind scores
  (`UserdataScore.TargetScores[]`, the sailor -500 never) gets the record a head shot leaves:
  `<DispPos>` (its transform's position + (0, Fly ? -1.5 : 1.5, 0): `via.Component.get_GameObject`,
  `via.GameObject.get_Transform`, `via.Transform.get_Position` - a vec3 returned through a buffer passed
  first), `<HitHead>`, then `<IsBroken>` last. The animator's next update breaks it through the game's
  `onBrokenTarget` (score, popup, counts). Medals in `_MedalMove` (Display, shown): `<IsBroken>`,
  `<NeedDisplay>` = false. Never `<HitWeapon>` (a dynamite stick's blast breaks sailors).
- No shot is fired, so the ShotLessThan/NoMissShot bonus units all appear. A wave ends 0.1 s after its last
  target breaks and a broken target is back in its pool a second later (`checkRemoveMove`, every frame): the
  hit that would end a wave waits (at most 1.2 s, never past `TimeLife` - 0.2 or the wave's `Time` - 0.2)
  while the next wave needs more of a kind than `_TargetStock` holds and some are on their way back.
- Never write `_Score`, and nothing outside Game (`openResult` adds the time bonus inside the update).

### Costumes in game - field writes (`game.cpp`)
- The pause menu's EXTRAS already changes accessories in game; costumes are greyed ("only from the main
  menu") by data: `GuiCostumeManager.CostumeSettingDict[CostumeMenuKind]` (`CostumeSelectMenuInfo`)
  `_SelectableSituation` OnlyMainMenu (2) for the costume kinds (Chara00/01_Costume, AO_Chara00/01/02).
- While the switch is on and the game is paused, those entries get ExceptEventOrBattle (3) - the
  accessories' rule - and the accessories' `_UnselectableReason` text; OnlyMainMenu again while a change is
  unsafe (`CharacterContext.<IsCostumeChanging>`, or `PlayerBaseContext.<State>` - an int64 of flags - in
  Jacked, Dead, Ladder, Boat, BodyDrawOff, ParentGimmick or HookShot). Off: the game's own values put back. The change itself is the
  game's (its loading, cutscene meshes, persistence).

### Play time (Freeze play time, the editor)
- What the game shows and records - the pause menu's clock (`InGameMenuGuiBehavior.initLayout` ->
  `ChainsawGuiUtil.getViewRecordTime`, set as the menu opens), the challenges' detail, a save's detail text, the
  chapter end's lap, the game clear's TOTAL PLAY TIME (`createCampaignClearStatsCurrent`: `GameClearTime`), the
  rank - is not `share.GameClock` but `GameStatsManager.calcTotalChapterStatsClearTime`: the finished chapters'
  laps (`calcPrevTotalChapterStatsClearTime`: `<OngoingChapterStatsHistory>[].<ChapterLapTime>._Time`) plus this
  chapter's: the clock's record time (`_GameSaveData` `_GameElapsedTime - _DemoSpendingTime - _PauseSpendingTime`,
  microseconds) less `<OngoingStats>.<ChapterStartTime>._Time`. A lap not above 0 shows 999:59:59.
- Separate Ways records chapters 1 and 2 in two parts, the second part's lap holding the first's: `calcPrev` skips
  a record of `ChapterID` cp11_chp1_1 / cp11_chp2_1 (31100/32100) when the latest record's chapter is higher. The
  mod adds the laps the same way (`game.cpp` counted_laps; until 2026-10-23 it added every record, so its play
  time in Separate Ways was off by those laps - `test_stats`).
- The RESULTS screen (the pause menu's Chapter Results, the main menu's RESULTS: `ChapterDetailResultGuiBehavior`,
  GuiType 114) shows another TOTAL PLAY TIME (`updateTotalTime`, every frame, up to 9999:59:59):
  `GameClock._SystemSaveData._SystemElapsedTime`, which `GameClock.update` grows by every frame's time before
  anything else - menus, cutscenes, pauses, every save and campaign - and only the system save keeps (a load
  leaves it alone). `ChainsawGameClock.onUpdate` adds its growth since `PrevTime` (unsigned) to `TimeDic[campaign]`
  (system save, never shown) and sets `PrevTime`: the mod writes both together. The editor sets it to the play time
  it set (whole seconds); the freeze holds it at a whole second every tick, in a game or not. (User, 2026-10-23:
  the RESULTS screen showed "a much larger total play time" than the editor's.) Its chapter table and TOTAL
  RESULTS tab are the best records (`BestChapterStatsHistoryCp1001/1011`, `BestCampaignStatsHistory...`), untouched.
- The clock is never touched (its countdowns and the HUD go by it). The freeze holds this chapter's lap: each
  tick the chapter's start is put that far behind the clock (at least 1 s). A start the mod did not write - a
  new chapter, a load - is the game's, and its lap is held from then on (`playtime_rules.h`, `test_playtime`).
- A Continue does not take the play time back, and the freeze must not take it from the game there (the user,
  2026-10-25: "if the character dies and then hits continue, it bleeds time into their playtime even if
  playtime is frozen"). `GameClock` keeps two copies of its times, `_GameSaveData` and `_GameSaveData_Reset`,
  and `update` moves both alike. `get_IsNotClearGameElapsedTime` = `MainModeManager.PreviousGameState` no main
  menu (`GameStateMainMenuBase.isMainMenuState`: a load in play) && `HighwayGuiManager.<SaveLoadMenuGuiManager>
  .<IsLastDataLoadContinueData>` - set by `GameStateGameOver.update` on its Continue, and by
  `SaveLoadMenuGuiBehavior.lateUpdateOnActive` for a load of `ContinueSlotId` (`setup`: `findContinueSave`'s
  slot, in the load modes); cleared by `GameStateGameOver.enter`, `initTakeOver_Continue` and
  `SaveLoadMenuGuiManager.onIngameStart`. Then `GameClock.loadGameSaveData` writes the save's four times into
  `_Reset` alone (any other load into both; `clear` makes both anew unless it is true): the run's clock goes
  on. The save's own chapter start comes with it (the setup's ApplyLoadedGamedata ->
  `GameStatsManager.loadGameSaveData` -> `OngoingStats.load`, into a new element: `onIngameSetupStart`'s
  `init`), and the clock ran through the game over screen (`GameStateGameOver.enter` leaves
  `_MeasureGameElapsedTime` on; `GameStateInGameCleanup.enter` stops it, the setup's last phase starts it as
  it hands over to GameStateInGame). So the lap the game shows after a Continue takes in everything since the
  checkpoint's save. A save that starts a chapter (the setup's LoadChapterClearGamedata /
  TakeOverLoadedGamedata) calls `onPrepareNewChapterStartTime` (a new OngoingStats, its start 0), which with
  the same flag sets `NeedTakeOverChapterStartTimeBackup`; `onIngameStart` (one of the handlers `onStart`
  subscribes through `MainModeManager.addEvent`; whether before or after the mod's first tick in play was not
  read - the hold takes either) then writes `<ChapterStartTimeBackup>` - the start `onIngameCleanupStart` read
  as the cleanup began, the mod's last write. Until 2026-10-25 the mod dropped its hold at every game start and
  took that lap afresh (and this file said a continue brought back the mod's own start). Now, when the two
  copies differ as play begins (`game::clock_kept`), the play time held carries over: the first look sets this
  chapter's lap to the time held less the laps the load brought, and the start written last before the load
  stays the mod's own should the game put it back. The log: "freeze play time: the game kept its clock across
  this load (m:ss past the save it loaded ...) - the play time held stays h:mm:ss". Any other load, a new
  game: that save's or the game's own time, taken afresh.
- The editor: this chapter's lap = T - laps; a T below the finished laps takes the latest laps down; above
  what this chapter's clock can hold goes onto the latest finished lap.

### Save count - vtable hook `CampaignManager.saveGameSaveData` slot 9 (+ its interface slot)
- `<SaveCount>` and `<ChapterSaveCount>` (Dictionary<ChapterID, Int32>). The save list's request adds 1
  before the save collects `CampaignManager`'s data in the same call: the hook puts the count the tick took
  while no save list was open back (the chapter's entry moved with it). A failed save's extra -1 is put back
  by the tick after a held save.

### Countdowns
- `chainsaw.TimerManager._Instance`: `<CurrentSecond>` (seconds left) held while `_CurrentState` is Counting
  or Stop; a new `_CurrentFuncParamSetting` (or the countdown starting) is taken afresh.
- The Mercenaries: `MercenariesModeController` (found as the target of the delegate it puts in
  `MercenariesManager._OnTimeBonusGet` - nothing holds it), `_PlayTimer` (`MercenariesTimer`)
  `_RemainTimer` held while `_IsBusy` and not `_IsTimeOut`; a bonus that raises it is kept. No editor for it (the
  user, 2026-10-27: "remove the option to change the current time left for mercenary mode"): the panel shows the time
  left beside Freeze countdown timer, nothing more. A timed scene's countdown keeps its editor.

### The Mercenaries (Endless combo, Mercenary Mode always on)
- `_ComboCounter` (`MercenariesComboCounter`): `_Timer` counts up to the combo's limit while
  `_AcceptionState` is Accepting - held at 0.
- `_ScoreCounter` (`MercenariesScoreCounter`): `_Score`, `_AddEntryComboBonusScoreNormal/Unique` are
  `SimpleAntiMemoryCheatInteger` objects: `_Value` = (int64)(int32)(low 32 bits of the object's address) x
  value, a new object on every change. The combo's stocks are credited to `_Score` each tick (capped at
  9,999,999) and zeroed - the game would add them only at the combo's end, and loses them at a death.
- `_BulletRushMultiCounter`: `_RushTimer` counts down from `_RushLimitTime` while `_IsRushTimeBusy` - held
  at the limit; `_FullGaugeNum` held at `_MaxGaugeNum`. Starting it stays the player's button.

### Kill enemies as they appear (the Mercenaries) - a hit from the player (`cheats.cpp` auto_kill, `game.cpp`, `events.cpp` kill_attack)
The user (2026-10-26): "the mercenaries mode doesn't have an auto kill mode that works like the re4 original mod
version" (RE4OGCabbyCodes' Kill enemies as they appear: every enemy dies as it appears, its kill scored as a shot's).
- One match at a time (the user, 2026-10-26, once it worked: "update the auto kill cheat to be disabled after each game
  so that the player must turn it on after starting a new mercenaries match"): the tick switches it off
  (`set_enabled`, so the ini is written as for the panel's switches) whenever the run enters or leaves a Mercenaries
  match (`Run.in_game` with `Mode::kMercenaries`): the end - the results (`GameStateInGameMSClearResult`), a game over,
  a quit, a retry - and the next match's start, where nothing the player set is undone (the switch is only shown in a
  match's pause menu) and an ini kept on by an older build or a crash does not carry over. The log: "kill enemies as
  they appear: switched off - the match is over (it lasts one match)" / "... off as the match begins ...".
- Where a kill scores: `MercenariesModeController.onEnemyDead(DamageInfo, EnemyBaseContext)` [551886] (exe+0x1D39760;
  `IEnemyManagerEnemyDeadListener`), from `EnemyManager.notifyDead` [77129], from `EnemyHeadUpdater.onDead` (virtual 51),
  which `CharacterHeadUpdater.onHitDamage` (45) reaches once `HitPoint.addDamage`/`checkDead` empty the health. It
  returns at once on `_bGameOver`, the timer's `_IsTimeOut`, `_Routine` not Game, an invalid context; else
  `_DeadEnemyCount` +1, the defeat bonus, `MercenariesComboCounter.requestAddCombo(1)`, then the reason -
  `getEnemyDeadReason(DamageInfo)` by the AttackData's `_KeyNameHash` (the Mercenaries characters' special attacks) and
  `_AttackType` (Normal 0, Knife, HeadShot - a head part, Explosion, Flash...), or without one `getEnemyDeadReason
  (context)` from `<LastDamageInfo>` (no attack owner and AttackType 0: RequestDie 15) - `requestAddScore` on both
  paths, the time (`MercenariesTimerUserData.tryGetEnemyDeadReasonTime`; added when `<LastDamageInfo>`'s
  `<AttackOwnerGameObject>` is a player's - or for an explosion, a unique enemy, the player's mounted gun), and
  `getBulletRushAddPoint` (Mercenary Mode's gauge).
- No field makes a death: HitPoint is plain data and `checkDead` is only read inside a hit; the scripted deaths are
  per class (`Ch1c0HeadUpdaterCommon.externalDead(deadActionCall, parasiteCheck)` - Ganados only: NoDeath off, HP 0,
  `onDead(null)`; the partner's finisher, a fall below the kill height in its `onUpdate`). So it takes a call: the
  user chose the hit (above, Rules).
- The request: `HitController.requestAttack(GameObject damage, AttackUserData, DamageUserData)` [359058]
  (exe+0x20113C0), what the game's own direct attacks use (`BehaviorTreeAction_DirectDamage.requestAttack`,
  `GmLaserTurret.callDirectAttack`, `RailCarCrewEM.dead`: each makes the two objects, numbers the attack with
  `HitManager.generateAttackID` [8262] (exe+0x233F340, a lock and a counter) into `<AttackID>`). It looks the
  AttackUserData's `_AttackHitDataID` up in `HitManager._Instance.AttackDataCatalog` (`DictionaryCatalog`2`:
  `_MergedCatalog`, a Dictionary of `RefCounter`1._Item` AttackHitUserData) and then `getAttackData(_KeyNameHash)` -
  none: nothing happens; else a new HitInfo (AttackGameObject this controller's game object, the target, both
  objects, the AttackData; the target's position) into this controller's `<RequestDamagePool>` (`ContactPool`, 32 at
  most). `HitController.update` hands that pool to `DamagePools[0]`, clears it and calls `updateAttack` ->
  `HitManager.registerDamage` -> `HitManager.onUpdate` -> `updateDamage`: the damaged controller by its game object,
  `get_Invincible` (its context's `HitPoint.<Invincible>`) turns the hit away, one hit per attack number and target
  (the contact history), `calcInfo` (the DamageValue from the AttackData, the attacker's
  `OnCalculateDamageByAttacker`, the damaged one's `OnCalculateDamage` - kHitCalc's), `hitSetting` (`checkKill`,
  `callbackDamageHit` - its `onHitDamage`, the death). No shielding or terrain check on a request (`shieldingDecision`
  is the physics path's, `poolDamage`).
- In play only (the first build crashed the game, 2026-10-26: "enabling the cheat crashed the game" - the log: the
  first hit on a Ganado, kind 200000, just after the pause menu closed, then an access violation reading 0x10 in
  `EPVExpertDamageEffectPlayer.request` called with null from `EPVExpertDamageEffect.request` from
  `HitController.callbackDamageHit` from `HitManager.hitSetting`). Every hit asks the damaged hit controller's
  `<DamageEffect>` (an `EPVExpertDamageEffect`, the component of its own game object `HitController.awake` takes) for
  its blood and sparks, and the effect calls its `_Players` in turn with no null check; its constructor makes the 32
  slots empty (`EffectPlayerNum`), its `doStart` - from the component's start, at the game object's first update -
  fills them. An enemy in the list, set up, with health, can be one whose game object has not had its first update
  (spawned in a frame paused or not yet updated); a real bullet never meets one (its colliders are not live). So
  `game::hit_started`: the effect's first and last players made (1), not yet (0), no damage effect to tell (-1 - left
  alone, the kind logged once); a Ganado whose parasite is out only once its appearance has played
  (`<IsFinishParasiteAppearance>`, set by `onAppearance`, cleared by its `start`; below). And 12 frames in play before
  the first hit. With no collider on the hit (a request's `DamageCollidable` is null, as the game's own direct attacks'),
  the effect emits at the character's `_RootJoint` (`findTargetJoint`'s null path), and the enemies' hit handlers read
  the request's DamageUserData parts, not a collider.
- What the mod asks for (the tick, in the Mercenaries, the clock running with time left, the pause menu down, the
  player's health above 0): for each enemy of `CharacterManager.<EnemyContextList>` set up (`<Setupped>`), with health,
  not `<Invincible>` or `<NoDeath>` (but a Ganado whose parasite waits, below), in play (`game::kill_targets`), at most
  8 a tick, again after 400 ms while it
  stands, 25 times (then logged and left): the player's body hit controller (`CharacterContext._BodyUpdater.<HitController>`)
  requests on the enemy's body hit controller's game object, `AttackUserData` {`_AttackHitDataID` Bullet 2,
  `_KeyNameHash` a shot's, `<AttackID>`}, `DamageUserData` {HitGroup Enemy 2, Priority 0, Parts Chest 1, Center} -
  the laser turret fills its own so (Parts 1). Not the head: HeadShot is a head part's, and a Ganado's head would
  burst. The attack's owner is the player's body (`getAttackOwner`: the controller's `<AttackOwner>`, else its game
  object), so the kill is the player's: reason Normal, the combo, the score, the kill count, the player's time.
- The shot: `BulletShell.getAttackKeyNameHash` (virtual 61) hashes the gun's WeaponID name (`Enum.ToString`, "wp4000",
  plus suffixes for a fitted reticle, a critical, a partner's shot) with the game's string hash (exe+0x3AABEC0: murmur3
  of the UTF-16 text, seed 0xFFFFFFFF - `game::key_hash`, `test_autokill` runs the game's). The mod takes the gun in
  hand's (`re::enum_name`) when the catalog's Bullet entry holds it, else the entry's first AttackType Shot
  (`Default`: damage 200, power 2). The archives' bullet attack data (three files) hold wp4000 at 200, Shot, power 2.
- The damage: `CalculateInfo.<AttackID>` is the AttackUserData's (its constructor copies it); kHitCalc and
  kParasiteCalc raise a hit of a number the tick noted (`events::note_kill_attack`, a ring of 128) to the enemy's
  health whatever its damage came to, One hit kills on or off.
- A Ganado's parasite: `Ch1c0HeadUpdaterCommon.parasiteState` (None 0, Wait 1 - one inside, Start 2 - coming out,
  Appeared 3, Dead 4, Failed 5, DoubleDamage 6, TypeFailed 7). The user's second report (2026-10-26, after the crash
  fix): "some spawns didn't die and were attacking me" - two kinds:
  - Wait: `setupParam` sets it from the spawn param (`ParasiteSettingDef` not None), with `parasiteAppearanceProbability`
    and `forceParasiteAppearance`; `onStart` then sets the context's `HitPoint.<NoDeath>` - the Ganado cannot die until
    its lethal hit draws the parasite's lot. The first build passed every NoDeath enemy over (a hit that could not kill),
    so these were never hit. The lethal hit's `<onHitDamage>g__parasiteCheck|420_5` (exe+0x18FD2F0): state Wait,
    `parasiteDamageCheck` (the hit empties the health, or breaks a head-category part), `checkParasiteAppearanceAttack`
    (some attack types fail: TypeFailed), then the head's own `checkParasiteAppearance(probability, 0)` through its
    table (virtual 259; overridden by `Ch1c0z0/z1/z2HeadUpdater_Mercenaries` the same way): `forceParasiteAppearance` wins,
    a probability of 0 loses at once, else the rank's points and a lottery; lost - `parasiteFailedDead(Failed 5)`: NoDeath
    off, the state written, the DamageInfo's `<IsKill>` - the hit kills it; won - `setupParasiteParam` (the parasite's HP
    into the context's HitPoint), `parasiteAppearance`, the hit's damage zeroed, the HitPoint `<Invincible>`. Now
    `kill_targets` takes a Wait Ganado despite its NoDeath, and just before its hit `game::keep_parasite_in` writes the
    head's `parasiteAppearanceProbability` 0 and `forceParasiteAppearance` false: the hit kills it outright (field writes,
    no call). Log: "a Ganado carrying a parasite - the parasite kept in, so the hit kills it (first time)".
  - Appeared (one that came out anyway: the player's own shot, a group's `callParasite`): the first build hit the
    parasite's own `HitController` (`parasiteUpdater._HitController`), which has no OnDamageHit subscriber - the mod's
    hits reached its calc and did nothing ("still stands after 25 hits asked for ... its parasite out", twice in the
    log). A real hit on the parasite comes through physics: `HitController.poolDamage` re-pools it on the
    `<ParentHitController>` (the Ganado's) when `<DamageToParent>`, and the Ganado's handlers take it (one health, the
    parasite's). So it is now hit on the Ganado's body hit controller, as any enemy, once out: the Ganado is
    `<Invincible>` from `parasiteCheck` until `Ch1c0BodyUpdaterCommon.<onAppearedParasite>b__411_0` takes it off (NoDeath
    with it, `onAppearedParasite`), and `<IsFinishParasiteAppearance>` set when its parasite updater can be found.
  - Start (coming out): counted as coming, not hit.
- Every area sends (the user's first point: "it seems I needed to walk around to trigger new spawns to come out"). The
  four stage scenes (patch_003; read offline with a scratch RSZ reader) and the code: `MercenariesModeController.
  updateEnemySpawn` walks `_SpawnerList` (`CharacterSpawnPointControllerMercenaries`, `_ManagedByMercenariesModeController`),
  skipping a spawner whose game object does not update; `canUpdateSpawn`: not `<IsGameOver>`, `<HasSpawnEnabled>` (its
  `_SpawnCondition` flags - in the Village every spawner's are kill counts: `DeadEnemyCounter` sets a flag at 4, 10, 25,
  40, 50, 70, 100, 130... kills), not `<HasAllSpawned>`, not `_InhibitSpawnCondition`; one request per `_IntervalTime`;
  `requestSpawn`: `<ActiveCount>` below `_ActiveCountLimit`, `EnemyManager.<EnemyActiveContextCount>` (+ the preparing
  ones) below `EnemySpawnNumLimit` (and the Mercenaries' `_SpawnLimitActiveCount` unless the spawner's setting skips it);
  `tryGetSpawnPoint`: the nearest point (no farthest limit) that is cool (`_CoolDownTime`, 0-6 s), out of the camera's
  view when `_IsOutOfCameraOnly` (`MainCameraController.isInside`, the point and 2 m above it), at least
  `_SpawnDistanceMin` (8-20 m) from the player - and whose `_AddConditions` (`CharacterSpawnConditionMercenaries`: Logic
  and/or over `Matters`) hold: `ConditionArea` (the player inside one of its boxes - `CompareValue` true everywhere
  read: a point serves the player standing in one part of the stage), `ConditionFlagMer` (Mercenaries general flags),
  `ConditionFlag`. A dead enemy leaves the active counts at once (`EnemyManager.updateActiveCount` and the spawner's
  skip a context flagged dead), so bodies block nothing - nothing to hurry. Standing still, the few points serving
  that spot (in view, cooling) left a trickle. The hooks: slot 7 of each condition class's table (`kSpawnArea` on
  `ConditionArea`, its check `ConditionAreaBase<...>.check` exe+0x1B0EA10; `kSpawnFlagMer` exe+0x3D1720; `kSpawnFlag` on
  `ConditionFlag`, `ConditionFlagBase<...>.check`) - `CustomConditionBase<...>.check` (exe+0xF9BCB0, called directly
  by `tryGetSpawnPoint`) asks each `_Data` through its table: the game's answer, but true while the tick says the cheat
  acts (`events::spawn_everywhere`: on, available, the Mercenaries' routine Game) and the switch is on, for an object of
  that class. The classes are nested in `CharacterSpawnConditionMercenaries`: only the spawn points use them. The camera
  rule, the distance and the cooldowns stay the game's. Log: "a spawn point's area (the player not in it) taken as
  holding - it sends wherever the player stands (first time)" (and the same for general flags, flags); the pace line
  adds "N active (spawning stops at L, the Mercenaries' M); spawn points opened K times (3 of 3 checks hooked)".
- The third report (2026-10-26, after the hooks: "it still requires me to walk around to trigger enemies to keep
  spawning, including some that jump out of closets etc for being near them"; the log: kills in bursts, the points'
  conditions taken as holding only 0-17 times in 15 s, the active counts far below both limits). What the stage data
  and the code say (the scenes read offline, scratch RSZ reader):
  - A stage holds a fixed set of enemies: every Mercenaries map's spawner scene carries 150 or 151 spawn params
    (`EnemySpawnParam` - Village 151, the others 150), each spawned once: `getNextSpawnParam` takes the first of a
    spawner's `<SpawnTargets>` that no `SpawnedDataList` entry carries (by the param's context ID; entries are never
    taken out), and `requestSpawn` marks the spawner spent (`<HasAllSpawned>`) when none is left. The stage ends -
    `MercenariesModeController.IsExterminated`, `_DeadEnemyCount >= _SpawnEnemyMax` - once every enemy is down: the
    user's run of 22:02-22:05 killed all of them in about 3.5 min and went to `GameStateInGameMSClearResult`.
  - Spawners open by kill counts: `SpawnController.onUpdate` (virtual 16, every frame) sets `<SpawnEnabled>` from its
    `_SpawnCondition` flags, the difficulty bits and `_SpawnSkipCondition`, calling `onSpawnEnabled`/`onSpawnDisabled`
    (`<HasSpawnEnabled>`) on a change; the flags are `DeadEnemyCounter` data (in the Village: 4, 8, 10, 16, 20, 25, 40,
    50, 70, 80, 90, 100, 110, 115, 120, 130, 140, 150 kills). Village waves: three start groups (`CSC_Start_02`, a plain
    `CharacterSpawnController`, 4 each), Reinforce02_01 at 4 kills (5), Reinforce02 at 10 (81; limit 12 at once,
    0.3 s apart), bosses at 25/50/90/130, Dynamite/Bow at 70, PlagaA03/Dynamite4 at 100, Reinforce04 at 130 (10),
    `CSPSCM_ NoCloset` once a level flag (115 kills and no closet event) is set, and one closet Ganado (`CSC_Closet`)
    the closet gimmick's sensor (gm84_844, `FireGanadoAppearanceLocker`, its own scene) lets spawn when the player comes
    near. The other maps' zone flags (`GlMercenariesGeneralFlagController`, 7 "player in area" flags in one) gate
    points (`ConditionFlagMer`, held by the hooks), not spawners.
  - The throttle while standing still was the points: a spawner's `_PracticalSpawnPoints` are its preset's
    (`CharacterSpawnPointsMercenariesPresetUserdata`, 18 user files; the Village's three: 14 points with a 32 s
    cooldown, 14 with 8 s, 8 with 3 s, out of the camera only) or its own, and each point waits its `_CoolDownTime`
    after sending (`_CoolDownTimerList`: `new Timer(_CoolDownTime, FunctionType 0, CountMode 0)` + `finish()` in
    `start`; `tryGetSpawnPoint` asks `get_Completed` - virtual 6, `_Completed` - first of all; `requestSpawn` resets the
    used one; `Timer.update` un-finishes a finished timer only of FunctionType 1). Out of view and far enough, few points
    served the spot the player stood at, each once per 8-32 s. Dead bodies are not it: `EnemyManager.updateActiveCount`
    and the spawner's `updateActiveCount` drop an enemy once its context is gone or `<IsEliminated>` (set by
    `EnemyHeadUpdater.onEndManagementDeadBody` when `EnemyDeadBodyManager.updateDeadBody` lets a body go - one a frame,
    only over its `_DeadBodyCountLimit`/`_DeadBodyCountCameraOutLimit`, an in-view one only beyond
    `_CameraInFarDistance`), and the log's counts stayed low.
  - The fix (field writes): each tick while the cheat acts, `game::ready_spawn_points` marks finished every cooldown
    timer of a spawner that may send (`<HasSpawnEnabled>`, not spent) that has run 1 s (`kKillPointCooldown`): the
    points serve again a second after sending. The camera rule and the distance stay the game's. The log: "N spawn
    point(s) cut short their cooldown - each sends again a second after its last (first time)", the pace line's "R
    taken off their cooldown", and when a 15 s window adds 2 kills or fewer (at most every 30 s) "the kills stalled -
    n spawner(s) may send: <name hash> requests r active a/l sent s of h, p point(s), c cooling; ... m spent: ...; k not
    let send yet: <hash> (s of h sent) ..." - the hash is murmur3 of the spawner's GameObject name (the mode's
    `SpawnerSetting._SpawnControllerKeyNameHash`; "CSPCM_Defeat25" 0x248FF036 checks out against the scene).
  - The fourth report (2026-10-26 22:54-22:56: "it did better but stopped at 122 enemies this time. The rest spawned
    after I started moving around"). The stall line named them: seven spawners that may send, each "requests 1 active
    0/1 sent 0 of n, 4-6 point(s), 0 cooling" - the Village's throwers `CSPSCM_Molotov1` (5), `CSPSCM_ Dynamite1-4`
    (1-2), `CSPSCM_Bow1/2` (2) - 10 spent, and `CSPCM_Defeat130`/`CSPCM_Reinforce04` waiting for 130 kills (122 + 15 +
    13 = 150). Their points are their own (no preset): 4 to 6 within ~16 m of the village's middle (e.g. (-7,4,-8),
    (11,3,-9), (-15,0,4), (-2,3,-13), (0,0,9), (16,1,6)), every one out of view only, `_SpawnDistanceMin` 10, conditions
    `ConditionArea` (none forced the whole stall: the camera test - the point and 2 m above it, `MainCameraController.
    isInside`, a frustum without walls - and the distance turned each away first). Fix (field writes): four times a
    second `watch_spawners` (`cheats.cpp`) finds a spawner that may send, has a request waiting and room for another
    enemy (active below its limit, fewer sent than it holds) and has sent nothing for 2 s (`kKillStuckMs`): once,
    `game::lift_spawn_rules` turns its points' `CharacterSpawnPoint._IsOutOfCameraOnly` off and cuts its
    `_SpawnDistanceMin` to 2 m (`kKillStuckDistance`), keeping the game's values (512 points, 96 spawners, with the mode
    controller they belong to); `restore_spawn_rules` puts them back where that controller's spawners still reach them
    the first tick the cheat stops acting (switched off, the stage over - the results screen is still the stage's - or
    out of the Mercenaries); for another controller nothing is written. A preset point is shared by the spawners using
    the preset (user data): it is put back the same way. The log: "spawner <hash> held an enemy back for 2 s (s of h
    sent) - none of its p point(s) out of view and far enough; their own rules lifted (v value(s)): it sends in view
    too, from 2 m", and at the end "the held-up spawners' own rules put back (v value(s))".
  - Not done: the closet Ganado and any other sensor-triggered spawn still waits for the player; 150 enemies is all a
    stage holds (the stage clears when they are down).

### Money
- Pesetas `InventoryManager.<CurrPTAS>` (max 999,999,999); spinels `InGameShopManager.<CurrSpinelCount>`
  (9,999).
- Infinite money - database-entry hook `chainsaw.gui.shop.InGameShopRootGui.onChangedPtas(Int32, Int32)`
  [310720]: the merchant's screen hears every change of the money through it; a fall is given back and the
  screen told nothing changed. A case bought is placed in a screen whose cancel refunds its price:
  `InGameShopManager.<FlowController>._StateController` (an `InGameShopStateController`)
  `.<InGameShopGuiParamHub>.<CasePurchaseParamHolder>.<PurchasePrice>` is cleared when it is what was given
  back.
- Infinite spinels (the user, 2026-10-23: "we need an infinite spinels cheat") - database-entry hook
  `InGameShopRootGui.onSpinelCountChanged()` [310721] (exe+0x14A1C0: `SpinelGui.setSpinelCount(InGameShopManager
  .CurrSpinelCount)`), the delegate the root GUI's `setup` subscribes to `InGameShopManager.OnSpinelCountChanged`
  (an `Action`: told only that they changed). Every change of the count fires it: `reduceSpinelCount` (lock,
  subtract to no lower than 0, unlock, the delegates) from `InGameShopGuiState_RewardDefault.onHoldGaugeCompleted`
  (a trade: `RewardItem._Setting` (`InGameShopRewardSingleSetting`) `._SpinelCount`, refused while it is above
  the count) and `InGameShopGuiState_CaseRewardEnter.onEnter` (a trade whose item must be placed in the case:
  taken as the placement opens, `CaseRewardParam.<RewardItem>`'s); `addSpinelCount` (up to `SPINEL_MAX`) from
  `TreasureInventoryController.add` (a spinel found), `InGameShopGuiState_RewardAnnounceDefault.onEnter` (the
  merchant's rewards) and `InGameShopGuiState_CaseRewardExit.onEnter` (a cancelled placement - its
  `EnterParam.IsPayBack`, set by `CaseRewardConfirm` - refunds the same `_SpinelCount`); `setSpinelCount` only from
  `InventoryManager.setupInitialSetting`. The hook measures a change from the count the tick notes every frame
  (`events::note_spinels`, switch on or off) or the one it left them at: a fall is given back before the screen
  reads the count; the screen's state - `InGameShopStateController._CurrState`, set by `requestTransitState`
  before it calls the state's `onEnter` - tells the case trade's opening (noted: what it gave back) and closing
  (a rise of exactly that is taken back out). The rule is `spinels_rules.h` (`test_spinels`). Nothing else in
  the game subscribes to the delegate.
- Infinite upgrade tickets (the user, 2026-10-24: "add a new cheat for infinite upgrade tickets") - database-entry
  hook `chainsaw.gui.shop.InGameShopGuiState_CustomCommandDefault.onHoldGaugeCompleted()` [740212] (exe+0x1FBA510),
  the `Action` the state's `onEnter` makes of it each time and writes into its `_HoldGaugeModule.OnHoldGaugeCompleted`
  (the inlined `HoldGaugeModule.setup`). It pays for the exclusive upgrade by the way chosen in the item popup's list
  (`get__LimitBreakSelectListGui`: `<_Controller>.<BehaviorHub>._InventoryPopupGuiBehavior._LimitBreakSelectListGui`;
  `get_CurrSelectable`/`get_CurrSelectedCommand`: `_AppSelectItems[<CurrSelectedSlotIndex>]` `<Selectable>`,
  `<CommandType>`): ItemCommandType MoneyLimitBreak 24 - `InventoryManager.reducePTAS` (Infinite money's);
  TicketLimitBreak 25 - `TreasureInventoryController.reduceTicket(ItemID it_sm75_542_00_0 120867200, 1)`, the
  Exclusive Upgrade Ticket (result unchecked); TicketLimitBreak01 26 - `reduceTicket01(1)`, the ticket sold as DLC
  (ItemID it_sm75_600_00_0, 120960000), refused when it fails; each then `CsInventory.setLimitBreakCustomPattern`
  (1, 2, 3). The controller is the customer's (`get__TreasureInventoryController`: `<_Controller>
  .<InGameShopGuiParamHub>.<TreasureInventoryController>`). `reduceTicket` takes only those two ids:
  `TreasureInventory.getInventoryItem(ItemID)` - the first entry of `_Items` its virtual `get_IsValid` passes (an item
  whose Guid is not empty, a slot with row and column >= 0) with the id - then `operation(ReduceItemCountOperation)`:
  under the list's lock, `_CurrentItemCount` less the count (clamped to its define's `_StackMax` and 0), then
  `OnItemCountChanged`; one it empties is taken out of its slot and the list (`RemoveAll`) and `OnRemoveItem` told.
  The hook (`game::upgrade_ticket` finds that entry by the same rules, only for TicketLimitBreak and a selectable
  way) raises its count by one before the original - so the last ticket is never removed - and takes the one added
  off again if the game took none (the original returned early). No call of the mod's. The DLC's ticket is not
  touched: it goes through the obfuscated `chainsaw.gui.AppAugustModule` (an instance in a static):
  `getTicket01CountSum` is its `askTheWay()`, `reduceTicket01(n)` takes its `getAsSellableItems()` (false when fewer
  than n) and calls `shotemAll(Item)` on n of them; `TreasureInventory.removeTicket` takes entries of that id out of
  the treasures (from `ItemWindowGuiState_Close.closeAllGui` and `getInventoryItemsForGui`). `test_tickets` checks
  the hook's side and the game's.
- The tickets editor (the user, 2026-10-24: "you didn't add a ticket modifier like the other currency"): the count
  shown is `game::upgrade_tickets` - every treasure entry of the ticket's id added up, as the merchant's
  `getTicket00CountSum` (`TreasureInventory.getItemCountSum`) adds them. Set (`cheats::request_tickets`, applied by
  the tick through `inventory::set_tickets` while items may change): up to the ticket's `_StackMax` (999, one
  entry); the first entry's `_CurrentItemCount` written and any other entry of it taken out
  (`TreasureInventoryController.remove`); none held: one made (`generateItem`) and added
  (`TreasureInventoryController.add`); 0: every entry taken out. The treasures list's own calls - nothing new.

### Results (Misses/Deaths/Damage don't count) - vtable hook `ChapterStatsGameOccurrenceObserver.catchGameOccurrence` slot 5 (+ interface)
- `GameStatsManager.<OngoingStats>`: `<Kill>`, `<Death>`, `<Damage>` (`_Count`), `<Accuracy>` (`_HitAttempt`,
  `_HitSuccess`); each finished chapter's the same in `<OngoingChapterStatsHistory>[]` (`ChapterStatsRecord`).
  The element classes are siblings (`GameStatsElementBase`), each declaring its own `_Count`, and `re::field_addr`
  reads a field only on its declaring class and subclasses: each element is read through its own class's field
  (`game.cpp` count_field; until 2026-10-23 the deaths and times damaged went through the kills' field, so the
  panel's results never showed and Deaths/Damage don't count never acted - `test_stats`). Every count of the chapter comes through the
  observer (`check`, then `addCount(ChapterStatsType)`; nothing else adds a kill, death or hit taken): the hook
  notes deaths, times damaged and attempts first, puts a death or a hit taken back, and keeps attempts = old
  attempts + new hits. The game over's `GameOverOngoingStats.<Death>` copies the held count across a continue.
- Times damaged (the results screen's TIMES DAMAGED): ChapterStatsType Damage (4) and VehicleDamaged (5) add 1
  (up to `GameStatsDefinition.DamageCountMax` 9,999) for a value > 0 whose target is a player
  (`CharacterEnumDefine.isPlayer`); a burn, a freeze or a grab hurting over time counts once
  (`PlayerBaseContext.<IsInBurnDamage>`/`<IsInFreezeDamage>`/`<IsInGrappleDamage>`, the observer's
  `<IsPlayerContinuousDamage>`). Nothing carries it across a continue: it goes back to the checkpoint's. The
  maxima: kills and deaths 9,999, shots and hits 999,999 (`GameStatsDefinition`'s .cctor).
- The editors (field writes): deaths, times damaged, kills, hits and misses (misses = attempts - hits: the hits
  keep the misses and the misses the hits, the shots following), this chapter's and the whole game's. A whole
  game's count: this chapter takes what the finished chapters do not hold (what it cannot hold goes onto the
  latest finished ones); a count below theirs leaves this chapter none and takes the latest finished records
  down (`results_rules.h` spread_total, as the play time editor does with the laps). The records are in the save.

### Difficulty
- `CampaignManager.<CurrentDifficulty>` (Difficulty Rank_10 Assisted, 20 Standard, 30 Hardcore, 40
  Professional). The switch is the game's own pair of calls for a game over's "lower the difficulty":
  `CampaignManager.setDifficulty` then `GameRankSystem.recalcRank` - only while the pause menu is up.

### Items
- The player's `PlayerHeadUpdater`: `<InventoryController>` (`CsInventoryController`, `<_CsInventory>`
  `chainsaw.gui.inventory.CsInventory`: `_InventoryItems` of `CsInventoryItem` (`<Item>`, `<SlotIndex>`,
  `<CurrDirection>`, `<IsForceGet>`), `_EquipInfos[EquipType].<ID>`, `<CurrInventorySize>`),
  `<KeyItemInventoryController>`, `<TreasureInventoryController>`, `<UniqueInventoryController>` (each
  `<_Inventory>._Items`).
- `chainsaw.Item`: `<_ItemDefine>`, `_ID` (Guid), `_ItemId`, `_CurrentCondition` (flags: Broken 1,
  LimitBreak 2), `_CurrentDurability`, `_CurrentItemCount`; `WeaponItem`: `_CurrentAmmo` (the ammo's
  ItemID), `_CurrentAmmoCount`, `_CurrentTacticalAmmoCount`, `_CustomLevelInWeapon` (`_Common`/
  `_Individual`/`_LimitBreakLevelInWeapon` lists, one entry per category the weapon has, `_NowLevel`
  0-based). The panel shows Power (Common 0), Capacity (Common 2), Reload speed (Individual 7), Rate of fire
  (Individual 8), a knife's durability upgrade (Individual 6, Strength) and the exclusive (LimitBreak).
- An item whose id the game being played does not define (a DLC's) gets a blank `ItemDefiniition` with
  `_ItemSize` Invalid (-1) from `Item.setItemId`: the mod refuses to add one.
- Names (`tools/gen_items.py`): `ItemMessageIdSettingUserdata` (`_ItemId` -> `_NameMsgId`), and over it each DLC's
  own `ItemMessageIdOverwriteSettingUserdata` (in its `re_dlc_stm_*.pak`, with its message file): the Attache Case
  Gold and Classic (it_sm77_620/621 - the main naming points them at messages no file holds) and the charms Green
  Herb and Handgun Ammo (it_sm79_600/601, named without "Charm: ", which the table adds as for every charm). Until
  2026-10-23 the overwrite was not read and those four showed as "item <id>" (the user's report).
- The panel's tabs (`gen_items.py` BANKS, GROUPS): the knives are filed with the grenades and eggs under
  Sub-weapons, RE2's word for both (the user, 2026-10-24) - but for every knife that can be upgraded, which has a
  heading of its own in Weapons, Special knives, after the rocket launchers: the Combat (it_wp5000), Fighting (5001)
  and Primal (5006) Knives, Separate Ways' Tactical (6107) and Elite (6108) Knives (the user, 2026-10-25: "put the
  combat knife into the weapons tab, the other knives are sub weapons"; 2026-10-26: "the fighting knife is another
  special knife type that belongs in the weapons tab"; 2026-10-27: "the tactical knife needs to go under the weapons
  tab instead of sub-weapons. Review other items and ensure any that can be upgraded go under weapons"). Under
  Sub-weapons stay the Kitchen and Boot Knives (no upgrades), the Mercenaries' unnamed wp6305, the grenades and eggs.
  What can be upgraded is the game's upgrade catalogs, `chainsaw.WeaponCustomUserdata` (three, found by their
  contents: two in patch_003 - the main game's and the Mercenaries' - and Separate Ways' in re_dlc_stm_2109300.pak):
  `_WeaponStages` (`_WeaponID`, `_WeaponCustom`: `_Commons`, `_Individuals`, `_LimitBreak`) and `_ItemStages`
  (`_ItemID`: only the Body Armor). A category's levels are the `_...CustomStages` of the element it names
  (`_CustomAttackUp` for AttackUp; stage 0, cost -1 or 0, the level it comes with). `upgrades_in`: a category with a
  second stage, or an exclusive with one, is an upgrade; Repair and Polish (one stage each: the merchant's mending of
  a knife, and all the Body Armor has) are not. 37 weapons can be upgraded - every gun but the rocket launchers, the
  Mercenaries' 6301-6304 and set pieces' (under Weapons anyway, the named ones), and those five knives - and no other
  item: the Body Armor stays under Other items (off the case's grid). The generator stops if anything that can be
  upgraded is filed outside Weapons. Until 2026-10-27 the special knives were a list in the generator
  (`SPECIAL_KNIVES`: Combat, Fighting). The Broken Knife (it_sm73_503) is a resource (ItemType Powder; "no longer fit
  for use").
- Never write `_NowLevel` alone: `WeaponCustomManager.check` replaces a mismatched level object with zeros
  on every item copy. The setters below fill in the derived levels.

### The game's calls (`call.cpp`; all from the tick, inside the VM's frame)
The approved kinds only (see Rules). The call bridge: `get_thread_context` (exe+0x39D9E30), begin frame,
the exception handler, local frame gc, end frame - all found by pattern votes; an exception left behind is
handed to the VM's handler and turns every further call off for the session. A `call::Frame` holds one VM
frame across several calls: an item made by one call stays referenced by the frame until the next puts it
in the case.
- The attache case's own: `ChainsawGuiUtil.generateItem(ItemID, count, durability, ammoId, ammoCount,
  changeId)` (the merchant's and storage's factory), `CsInventoryController.add(Item, InventorySlotType)`
  (the pickup's placement; `AddItemResult.AddCount`), `.remove(Guid)` (unequips, frees the cells),
  `.changeSize(AttacheCaseSize, checkSize=true)`, `CsInventory.changeCount(Guid, Int32)`,
  `.updateReloadInfos()`, `KeyItemInventoryController.add/remove`, `TreasureInventoryController.add/remove`,
  `UniqueInventoryController.add`, `UniqueInventory.remove(Guid)`.
- A weapon's upgrades (the merchant's): `WeaponCustomManager.setCommomCustomLevel`,
  `setIndividualCustomLevel`, `setLimitBreakCustomLevel`, `getMaxCustomLevel` (three overloads),
  `WeaponItem.onCustomized()` (clamps rounds).
- The difficulty: `CampaignManager.setDifficulty`, `GameRankSystem.recalcRank`.
- The save files: `via.storage.saveService.SaveService.removeSingle(Int32)`, `updateSaveFileDetailTbl()`,
  `SaveLoadMenuGuiManager.getSaveSlotInfo(Int32)`, `addSaveSlotInfo(SaveSlotInfo)`, `clearInfo(Int32)`.
  The copy: `System.Type.GetTypeFromHandle(RuntimeTypeHandle)` (the handle by reference: the type's
  database record, whose first 19 bits are its index) and `System.Activator.CreateInstance(Type)`;
  `share.SaveDataUtil.isEnableAccessSaveFileDetailTbl()`; `SaveDataManager.entrySuspendGameSave(Object)`,
  `exitSuspendGameSave(Object)`, `requestSaveSystemData(AppSaveSlot, SystemSaveRequestArgs)`;
  `SaveDataManagerEnumExtension.getKeyString(SaveLoadKey)`; SaveService's statics `cleanData()`,
  `addDataClass(String, Object)`, `loadSingle(Int32)`, `saveSingle(Int32)`, `get_SaveState()`,
  `get_LastSaveResult()`, `set_SaveDataSize(UInt64)`.
- Automatic QTEs (from their hooks, inside a `call::InHook`): `share.hid.Command.setValue(UInt32)`,
  `ConcurrentDictionary<UInt32,Boolean>.TryAdd`, `GmQTELaserTrap.calcAvoidCommandHash`,
  `AdditionalOptionManager.getCp11HSCommandHash`, `PlayerHeadActionSign.requestReserveAvoid` and the
  virtual `requestReserveParry` (through the object's own vtable),
  `ActionSignGuiUnit.checkNormalActionSignDispLargeUI`, `KnifeEscapeCondition.canOpenGui(PlayerHeadUpdater)`.
- The shooting gallery (from its hook): `via.Component.get_GameObject`, `via.GameObject.get_Transform`,
  `via.Transform.get_Position`.
- The challenges: `AchievementServiceController.unlock(Int32)` on `NetworkManager._AchievementServiceController`
  (a challenge switched on that carries an achievement); their system save is the copy's
  `SaveDataManager.requestSaveSystemData(-1, new SystemSaveRequestArgs)` (`savefiles::request_system_save`).
- The item box (2026-10-23): the game's storage's own `ArmouryManager.addArmouryItem(Item)` (exe+0x2309570),
  `reduceArmouryItem(Item, Int32)` (exe+0x238E090) and its rule `ItemExtension.isEnableMoveToArmoury(Item)`
  (static, exe+0x2C66CD0), from the tick; and from the hook on the storage's save (inside a `call::InHook`), the
  item factory and the upgrade setters above and `List<chainsaw.Item>.Add(Item)` (exe+0x101100) on the save
  data's `ArmourySaveData`.
- Kill enemies as they appear (2026-10-26; from the tick): `HitManager.generateAttackID()` (exe+0x233F340),
  `HitController.requestAttack(GameObject, AttackUserData, DamageUserData)` (exe+0x20113C0) on the player's body hit
  controller, its two objects made by the copy's `System.Activator.CreateInstance`, the target's game object by the
  gallery's `via.Component.get_GameObject` - one `call::Frame` for a tick's requests.
- The Extra Content Shop (2026-10-28; from the hook on its screen's frame, inside a `call::InHook`, on the screen
  the hook was handed): `ExContentsShopGuiBehavior.setCaptionMessage(ExShopBonusID)` (exe+0x1B91620) for the
  selected item, then `updateCurrentGrid(Int32, GridScrollDirection)` (exe+0x1BCA6A0) with `CurrentListIndex` and
  None, as its purchase passes them.

### The panel's lists: many at once (`overlay.cpp`; the queue in `inventory.cpp`)
The user (2026-10-25): "sort the other items list so it's easier to read. Also we need a way to add multiple
items at once since there's a confirmation window, it's currently very slow to approve each edit. The same goes
for other parts of the cheat menu". No new game call: the same requests, made together.
- **Order**: the lists off the grid (key items, treasures, other items) come in the Add list's order
  (`offgrid_rows`: `items.h` rank - bank, group, then the game's own) under a heading per group, each heading with
  a tick box for its rows; the game holds them in the order they came. The case keeps the game's order.
- **Ticks**: every row of the case, the off-grid lists and the item box (both parts) has a tick box, kept by
  item - list, ItemID and Guid (`ItemKey`; a box entry by its id, a weapon's by its whole state) - so it stays
  with the item as rows come and go. A line over each list: All (half ticked when some are; the box's only
  covers the tab and filter shown), "n ticked:" and what they can be given to - the case: Store (no question,
  as Store beside a row) and Throw away; the off-grid lists: Remove; the box: Take and Throw away (ticks under
  other tabs counted). A scene's weapon (`<IsForceGet>`) is never ticked.
- **Edits** are kept by item too (`RowEdit`, `BoxEdit`, `StoredEdit`) until Apply, Apply all or Undo; a count
  typed back to what the game has is no edit. The line says "n changed:" with Apply all and Undo all.
- **Add lists**: a tick box per item (Enter in the search ticks the first match), a count beside a ticked item
  that stacks, Tick all listed (the tab and search), Untick all, Most of each, Add - for every ticked item.
- **One question** for a batch (`BatchAsk`: a line per item, its button counting them - "Add 12", "Throw away
  3"); a single row's Apply and x ask as before. Ticks, edits and questions are let go when the panel goes; an
  Add list's ticks stay (by item type) until added.
- **The queue**: `inventory::begin_batch`/`end_batch` queue a batch's requests whole; the tick works through
  the queue at most 12 a tick and 4 ms, and sums a batch up in one note once its last is done ("7 of 9 added -
  not added: no room in the case for Rifle Ammo"; each reason once, three at most, the rest "see the log"; a
  batch of one is a plain request). The view's `waiting` counts what is left. An entry of the box's own part is
  found at its index while it is still as seen, else the first one like it (an earlier change of the batch took
  one before it out). The note shows under the list or Add list it was asked from (`place_note`), "Working on
  it" until a view published after the request says it is done - and at the bottom only when not shown there.

### The item box (`itembox.cpp` the mod's part; Store / Take / count / throw away and the saves in `inventory.cpp`)
The user's choice (2026-10-23): "both, each save its own". Two places, both in the game save; the panel lists
them as one (the banks' tabs, the group order, a search), a row of the game's storage marked "storage".
- **The game's storage** (below) for what its own rule takes and has room for - one entry per item, the First
  Aid Spray stacked to 999. Store: `isEnableMoveToArmoury(item)`, the entry checked for room (its `addArmouryItem`
  drops a second of an id without a word), `addArmouryItem(item)` (copies it, lets a weapon's fitted parts go),
  the storage re-read to see it went in, then the case's `remove`. Take, as the typewriter's own: a stack of more
  than one gives a new item of count 1, else the stored item itself goes to the case (`CsInventoryController.add`),
  then `reduceArmouryItem(stored, 1)`. Throw away: `reduceArmouryItem(stored, its count)`. The spray's count:
  `_CurrVirtualStackCount` written (1..its most).
- **The mod's part**: everything else (ammo, herbs, grenades, resources, a second copy of a weapon, a knife in a
  stage that refuses knives), with no limit, in memory for the game being played. One entry per item (counts
  added up), a weapon (gun or knife) an entry of its own with its rounds, ammo id, durability, condition and
  levels. Store: the entry first, then the case's remove (a refused remove takes the entry back out). Take: one
  piece's worth (`items.h` stack) or the weapon - made by generateItem, levels by the setters, condition and
  rounds/durability written, added by the case's add.
- **In the save** (`events.cpp` kArmourySave/kArmouryLoad: vtable slots 4/5 of `ArmouryManager` + its
  `share.IGameSaveData` slots; every save is `SaveDataManager.execSave` -> `collectGameSaveData`, every load
  `GameStateIngameSetup.update` -> `applyLoadedGameSaveData`, Continue included). The storage's
  `GameSaveData.ArmourySaveData` (`List<chainsaw.Item>`, serialized: field flag 0x4000) is what the game kept
  its storage in before an update; it still reads it at a load (each item through `addArmouryItem`) but writes
  it empty. Save: after the original, each entry made by generateItem (+ state; `_CurrentItemCount` written
  whole past the piece's limit), its Guid "RE4CCBOX" + entry number + save number, `List.Add`ed (inside the
  hook's `call::InHook`); a failure cuts the list back (`_size`) and the box goes to the game's unsaved file
  instead. Load: before the original, a list of nothing but marked items replaces the box and is written out of
  the save data's field for the original's call (put back after - the object may be read again); a list with
  any unmarked item is an old save's own storage, left to the game (the box comes out empty). A new game loads
  nothing: the tick empties the box while the game state derives from `GameStateMainMenuBase` (the title, the
  main game's, Separate Ways' and the Mercenaries' menus). Without the mod a save's marked items go into the
  storage (one of each) at the next load.
- **Files beside the mod**, one per game: `RE4CabbyCodes.itembox.ini` / `.separate-ways.ini` (the box before
  it went into the saves) and `RE4CabbyCodes.itembox.unsaved.ini` / `.separate-ways.unsaved.ini` (what a save
  could not carry: rewritten as what it held before this box first came + the box, so a box failing twice is
  there once). `<id>:<count>` or `<id>:1:<rounds>:<ammo id>:<durability>:<condition>:<list.category.level/...>`.
  The panel's Move into this game reads both (one unreadable: neither), puts each item where Store would (the
  storage one at a time, a spray's rest by its count), and `moved_in` holds both back - no count, nothing read,
  so never moved in twice - until a save keeps the box: `saved(true)` then removes them (`files::remove`; one
  another program holds is offered no more and tried again at the next save). A load or the menus first (even
  with the box emptied by Take: the items may be in the case, unsaved) let that box go, and they are offered
  again. A save that cannot carry the box writes the unsaved file with the box alone (the files' items once) and
  removes the other. No copy is left: the user (2026-10-28), "we don't need backup in the name ... We don't need
  duplicates". Until 2026-10-27 the files were renamed `<name>.moved`, and the user deleted theirs (the 10-22
  box, moved in on 10-23): "I manually deleted it because I didn't expect the file to be called 'moved'"; for an
  hour they were renamed `.backup.ini`. Every save the user had was read offline then (the decryption tools of
  the save edits): each carries the mod's part (main game 122 entries, Separate Ways 57-61) - nothing was lost.

### The game's storage (`chainsaw.ArmouryManager`, the typewriter's Storage)
Read from code (2026-10-23) when the user asked whether it could replace the item box: not as it is - it
takes only some kinds, one of each.
- `ArmouryManager._ArmouryItemList` (`List<ArmouryItem>`: `_Item` a copy of the item (`Item.getCopy`, virtual
  slot 4), `_VirtualStackableCountMax`, `_CurrVirtualStackCount`). No limit: the static `ArmouryCapa` (10) is only
  the list's starting capacity. In the game save (`ArmouryManager.GameSaveData.ArmouryItems`, `ArmouryItemSaveData`
  {Item, both counts}), every entry whole; `loadGameSaveData` drops nothing by kind (an item whose DLC is missing,
  or a bonus weapon not bought, goes to `InventoryManager.backupExtraItem`). Taken over into a new game.
  `onInGameCleanup` empties the list.
- What it takes, `ItemExtension.isEnableMoveToArmoury(Item)`: never in the Mercenaries; `ItemIDDefine.
  isEnableMoveToArmoury(id)`: attachments (ItemType Parts), the First Aid Spray, and weapons by `WeaponCategory` -
  handguns, shotguns, SMGs, rifles, magnums; of the bows only the Bolt Thrower (4600) and Blast Crossbow (6102); of
  the rocket launchers the infinite ones (4902, 6111) or one still loaded; of the knives the Combat, Fighting,
  Primal, Tactical, Elite and wp6305 (not in some stages). Never ammo, grenades, eggs, herbs, food, resources, the
  Kitchen or Boot Knife, the Rocket Launcher (Special).
- One of each: `addArmouryItem(Item)` (the storage screen's `store`; the case's "Send to storage",
  `ItemCommandType.SendToArmoury`, offered instead of Discard for those items, anywhere) refuses the Special rocket
  launcher, tops up a stacking entry (same id and room: only the spray stacks, to 999 - a new entry starts at 1,
  a top-up adds the item's count) and drops an item whose id is stored already (`existsItem`); a new entry calls
  `GameRecordManager.unlockRecordItemToArmoury`. Out, at a typewriter only (`ArmouryGuiState_ScrollListDefault.
  addToInventory`): a stacking entry gives a new item of count 1 each time (`generateItem`), any other entry its
  stored item; `reduceArmouryItem(Item, n)` (by Guid) takes n off the stack and drops emptied entries
  (`<>c.<reduceArmouryItem>b__21_0`: count <= 0).
- The screen's rows (`ArmouryGuiBehavior.draw`): name, icon, a gun's rounds, a knife's durability, a stacking
  entry's count. The merchant's Sell list takes every stored entry at `getSellPrice` with no other check
  (`InGameShopGuiState_SellEnter.setupSellItemList`); a stored weapon shows sold out. So the mod never puts a kind
  the storage refuses into it.

### The challenges (`challenges.cpp`, `challenges_rules.h`; names `challenges_text.h` from `tools/gen_challenges.py`)
- `chainsaw.GameRecordManager` (AppSingleton): `GameRecordDataList` (`GameRecordData[]`, one per
  `GameRecordID`: `_RecordID`, `_IsUnlock` done, `_IsNotice`, `_IsHide`, `_Count`, `_ProgressBitFlag`),
  `ExShopDataList` (`ExShopData[]`, one per `ExShopBonusID`: `_BonusID`, `_IsUnlock` the shop sells it,
  `_IsBuy`, `_IsNewMark`), `RecordExPoint` (the CP), `requestCheckAllUnlock`, `SettingInfoData`
  (`GameRecordSettingUserData`), `_InitialReleaseBonus`. The system save keeps the records and the shop's
  entries whole, and the CP (`saveSystemSaveData`, virtual[4]; `SaveData.encode`).
- The table (a pak user file, found by its contents): `DataList` of `InfoData` {`RecordId`, `AddPoint` the
  CP, `Name`/`Detail` GUIDs, `DetailStringType` (Variable1: the detail's "{0}" is `AchievementNum`),
  `AchievementNum` the goal, `IsDetailHide`, `Category` (Story 1, Tutorials 2, Weapons 3, Battle 4,
  Collectibles 5, Cp1011 6 - the window's tabs), `Type` (ConditionType: Flag 0, Count 1, Progress 2,
  ProgressGauge 3, Time 4), `IsEnableBonus`, `BonusList`, `IsDisable`}, `UnlockBonusList` (more bonuses by
  `RecordId`), `UnlockBonusItemList` (bonuses an item found unlocks). 154 entries: the main story's 0-110,
  Separate Ways' 200-250. 093 and 250 are `IsDisable` with a bonus each (the Assisted S+ ranks' Wolf Tail and
  Wolf Set): the window's hidden challenges, named by the bonus and conditioned by the shop's lock text; 200 is
  rejected (no bonus) and left out.
- `unlockRecord(id)` (exe+0x1D9AEA0): main records outside Separate Ways and the Mercenaries only, 200+ only in
  Separate Ways (`CampaignDefinition`); under `_LockCollection`: `GameRecordData.setUnlock(true)` (`_IsUnlock` =
  `_IsNotice` = 1, `_IsHide` = 0, a `NewMarkRequest_GameRecord` - not for 093 and 250), the CP + `AddPoint`
  kept within 0..100,000,000 (`OnChangedExPoint` told), `setUnlockBonus` for the entry's `BonusList` (when
  `IsEnableBonus`) and its `UnlockBonusList` (`AdditionalBonusManager.canUnlockBonus`: only the running
  campaign's; `ExShopData._IsUnlock` = 1 and a shop NEW icon), `requestCheckAllUnlock` = 1; then, changed or
  not, its achievement: `GameRecordIdDefinition._Table` (static `Dictionary<GameRecordID,Int32>` its .cctor
  fills: 46 records, achievements 1-46) -> `NetworkManager._AchievementServiceController.unlock(n)` (adds n to
  `_UnlockRequests` while the service runs; its `update` sends it).
- `onLateUpdate` with `requestCheckAllUnlock`: every main record 2-103 but 067 and 093 done -> `unlockRecord(1)`;
  every Separate Ways record 202-249 -> `unlockRecord(201)`. Nothing in the game ever undoes a challenge.
- Progress: `getProgressNum(id)` (exe+0x1D0D680) is `_Count`, but 077-079, 095-097, 103 and 247-249 come from
  the game save (`RecordGameSaveData`, by `GameRecordGameSaveParamID`); `checkProgressNum` = progress >=
  `AchievementNum`. Things counted once each: `addProgressNum(id, add, checkSystem, bit)` sets the bit in
  `_ProgressBitFlag` and adds (a bit already set adds nothing) - 068, 076, 080-082, 235, 236
  (`GameRecordCheckBit.Progres_*`; the game save's `_Unattainable` keeps a save from counting one twice).
- `_IsNotice` is the game's "challenges completed" pop-up: `requestNotice` -> `checkRecordNotice` (the main
  menu, a game's start, the challenges screen opening, the chapter results) lists every one flagged, then
  clears them. The window sets no `_IsNotice` and no NEW mark.
- Bonuses the game gives outside the challenges: its first start (`firstUnlockBonus`: `_InitialReleaseBonus`),
  an item found (`checkReleaseBonusToItem`: `UnlockBonusItemList`), a new game (`ExShopData.get_isNewGameUnlock`:
  128, 153; 406 in Separate Ways), three requests (`checkReleaseBonusToQuest`: 209, 211, 212), the Mercenaries'
  results (7 the Handcannon, 424, 426-430 - `canUnlockBonus`'s set there), DLC
  (`AdditionalBonusManager.onIngameMainMenuSetupStart`). `isUnlockBonus`/`checkBuyBonus` answer true for every
  main-game bonus while `ExtraContentsManager.hasContents(0xA19A59D1)`. GameMode00 is Professional
  (`IsGameModeMostDifficult`, `DefficultySelectGuiBehavior.setup`), GameMode01 Separate Ways' Professional.
- The screen: `chainsaw.RecordListGuiBehavior` (`_OpenParam._OpenType` InGame 0, BonusMenu 1), GuiType
  RecordList (134) in the GUI table (`game::Run.challenges`); its rows read `isUnlock`, `getProgressNum` and the
  NEW mark as they update (a cursor move). Its tabs (`initTab`): ALL, INCOMPLETE (`!isUnlock`), then the categories
  MAIN STORY, BASICS, WEAPONS, COMBAT, FEATS (`CH_Mes_Main_Sys_Bonus_Record_Category01-05/07/09`) and SEPARATE WAYS
  (`AO_..._Category00`, only with that DLC). The window has the same tabs, in that order, labelled from those
  messages (`challenges_text.h` kTab*; the user, 2026-10-23: the tab names did not match the game's - the window
  had the enum's names and no INCOMPLETE).
- The window: on = `setUnlock(true)`'s writes but `_IsNotice`, the CP if it was not done, a counted one's count
  at its goal (not the game save's ones), its bonuses unlocked, `requestCheckAllUnlock`, the achievement (the
  call); off = `_IsUnlock`/`_IsNotice` 0, the CP back if it was done (never below 0), the count just short and
  the highest bits cleared down to it, each bonus locked unless bought, given otherwise (above) or another done
  challenge's. Saved by `savefiles::request_system_save` once the game's saves are free.

### The Extra Content Shop (`exshop.cpp`; names and lock texts `challenges_text.h` from `tools/gen_challenges.py`)
The user (2026-10-27): "add a new imgui cheat menu while on the extra content shop. Each entry in the show should be
unlockable (not purchased) via this menu".
- The screen: `chainsaw.gui.ExContentsShopGuiBehavior` (no subclasses; GuiType ExtraContentsShop 17, opened from
  the main menu's Bonuses). Its tabs: `setup` fills `CurrentCategoryTabList` with ExShopCategory None, Weapon,
  Costume, Figure, Art, then CP11 only while `ExtraContentsManager.hasContents(ContentsHash.DLC_0001)` (Separate
  Ways). That check looks for another hash (0x93C5815A, a literal of no name) when a runtime object says so on
  this PC build, so the mod takes the screen's own list rather than the DLC list. The labels: the static
  `TabElementNameTable` (.cctor) pairs None, Weapon, Costume, Figure, Art with `via.gui.MsgID`'s
  `CH_Mes_Main_Sys_Bonus_ContentsCategory_00/03/08/02/01` (ALL, WEAPONS, APPAREL, MODELS, CONCEPT ART - the
  static block's fields 16 bytes apart in that order); CP11's tab takes another path (`setupTab`), labelled here by
  `AO_Mes_Main_Sys_Bonus_ContentsCategory_00` (SEPARATE WAYS).
- `setupList(category)`: the table's items in order from `GameRecordManager.ExShopSettingInfoData`
  (`ExContentsShopUserData`, a via.UserData: `DataList`, InfoData[] - `BonusId`, `CPoint`, `Category`, the
  `Name`/`Detail`/`LockDetail` message GUIDs, `IsHide`, `IsHideMessageUntilGameClear`): every one under None, the
  Costume tab taking Accessory (7) too, CP11's only with that DLC. 257 items in this build: 3 weapons, 6 costumes,
  20 accessories, 135 models, 53 concept arts, 40 of Separate Ways'. No filter (PhotoStyle), game mode or CP12 item
  is sold: those bonuses come from the challenges and the Mercenaries' results only.
- An item's grid entry (`updateCurrentGrid` -> `ExShopGridItemGui.setupItem(data, unlock, buy, newMark,
  insufficient)`): unlock = `GameRecordManager.isUnlockBonus(id)`, buy = `checkBuyBonus(id)`, insufficient =
  `getExPoint() < CPoint`, newMark = `ExShopData._IsNewMark`; `_CurrentState` Purchased (1) when bought, else Lock
  (3) when not unlocked, else Insufficient (2) or Default (0). isUnlockBonus and checkBuyBonus are the item's
  `ExShopData._IsUnlock` / `_IsBuy` (`ExShopDataList`, one per ExShopBonusID) - but true for every id outside CP11
  (CP11_Weapon00-CP11End, 358-423), GameMode01 and CP12 (424-432) while `ExtraContentsManager.ExtraContentsList`
  holds `ContentsHash.DLC_3001` (0xA19A59D1; `GameRecordManager.hasDLCAllUnlockAndBuy`). checkBuyBonus also answers
  false in the Mercenaries' campaign, and in Separate Ways' outside 17-422 (the bonus weapons there).
- A purchase: `lateUpdateOnActive` (slot 40) starts the hold only on an entry whose `_CurrentState` is Default;
  `buyBonusItem` then pays (`addExPoint(-CPoint)`), `setBuyBonus(id, category)`, shows the page again
  (`updateCurrentGrid(CurrentListIndex, None)`) and sets `IsUpdateToSave`. Nothing else asks about the unlock: the
  entry's state is the gate. `buyBonusItem` has no direct caller: `lateUpdateOnActive` gives the selected entry's
  `setHold` an `Action` of it (kept on that entry), and fills the hold's gauge every frame while `isHoldItem` is
  set (`setHoldGuage`, which calls the Action once full) - so the purchase shows itself from the screen's frame.
- The page is set up from the items' state only by `setupList` (the tab list's `onTabSelectChanged`, the screen's
  opening - `draw` has no direct caller), a scroll (`onScrollEvent`), a move of the cursor (`onSelectionChanged`) and
  a purchase. `updateCurrentGrid(index, last)` reads neither argument: it takes the grid's cells
  (`_CurrentScrollGrid.get_Items()`, the SelectItems in view, each holding the index of the item it shows), empties
  `CurrentGridList` and `_DisableGridItemList` without a null check and makes each cell's `ExShopGridItemGui` anew -
  past the tab's items disabled, else set up from its item's state (above) with its icon and NEW mark
  (`checkNewIcon`). It calls nothing else: the cursor, the scroll and the selection stay where they are (`setupList`
  calls it with (0, None) after setting the grid's size and events afresh - the cursor back at the top, as a tab
  switch does). `setCaptionMessage(id)` sets the category's text, then asks the item's `getExContentsShopInfoData`,
  isUnlockBonus and checkBuyBonus: all 257 are `IsHide` - neither unlocked nor bought, the name reads "???" and the
  text its `LockDetail` ("???" too for the 7 `IsHideMessageUntilGameClear` ones until the game is cleared:
  `isUnlock` record 17, 208 for Separate Ways' and GameMode01) - else its `Name` and `Detail`. `onSelectionChanged`:
  `CurrentListIndex` from the grid (the cursor sound when it moved), the old entry's hold reset (`resetHold`,
  `CurrHoldRate` 0), `setCaptionMessage` for that index's item, `updateGuide`, `updateCurrentGrid`, then
  `notifyCheckedNewIcon` for the item under the cursor.
- The page shown again (2026-10-28, the user: "add the instant refresh call so the shop updates immediately";
  until then a change made beside the shop showed once its tab was switched): after a change the tick made (an
  unlock or a lock: a flag), the screen's next `lateUpdateOnActive` (kShopScreen, after the game's own frame,
  inside a `call::InHook`) has the shop's own code draw what a move of its cursor draws, less its hold reset and NEW
  mark: `setCaptionMessage` for `CurrentExShopList[CurrentListIndex]`'s item (none when the index is past the
  list), then `updateCurrentGrid(CurrentListIndex, None)`. Not while `isHoldItem` - the player holding to buy: the
  entries are made anew and the hold's Action is its entry's (the shop's own scroll ends a hold, `endHoldAll`, after
  its updateCurrentGrid) - the flag waits for the hold's end; nothing when the grid or its two lists are missing; the
  flag goes when the shop closes (it opens with its page set up). Without the calls (not found, or game calls off)
  the window says a change shows once the shop's tab is switched (`View.instant`).
- The game's own unlock, `setUnlockBonus(id)` (from unlockRecord, checkReleaseBonusToItem/ToQuest,
  unlockRecordItemToArmoury, the main menu's DLC bonuses, the Mercenaries' results): `AdditionalBonusManager.
  canUnlockBonus` (the running campaign's), then under `_LockExShopCollection`, the first time: `setUnlockNewIcon`
  (`NewMarkGuiManager.add`, a call) and `_IsUnlock` = 1. Nothing derives the unlocks again from the challenges done:
  `GameStateWakeUp` unlocks only Figure076 (the Killer7's model) when `hasAlreadyBonus076`, a new game
  `setUnlockNewGame`'s three, the main menu the Mercenaries' models (CP12) - a lock stays until the game unlocks the
  item by its own rule again.
- The window: a row per item of the shop's table in its order, under the shop's tabs (the screen's
  `CurrentCategoryTabList`, read by a vtable hook on its `lateUpdateOnActive`, kShopScreen - nothing written; without
  a frame of it for 2 s every tab with an item), a filter (name, whose, kind, lock text). Tick = `_IsUnlock` 1 - no
  NEW mark; untick = 0 unless purchased (or the DLC's); the shop's page shows either at once (above). Unlock all /
  Lock all take what is listed, one question each. Each row's state as the shop shows it (Locked, For sale, Not
  enough CP, Purchased - (DLC)) and its price; the CP shown, never written. Changes only while the shop is up (the
  GUI table); saved by `savefiles::request_system_save` (the challenges' path). The log: "shop: the Extra Content
  Shop is up", "shop: the game's table read: 257 items (...)", "shop: the shop's screen shows 6 tab(s), Separate
  Ways' too", "shop: Costume: Leon's Jacket unlocked", "shop: N item(s) unlocked: ...", "shop: the shop's page shown
  again by its own code (updateCurrentGrid, as after a purchase; the selected item's caption: ...) - each change
  shows at once (first time)", "shop: the game's system save asked for", "shop: saved". `test_exshop` checks the
  module and the game's code, `test_panel` the window.

### The save files (`savefiles.cpp`)
- One screen class for every list: `chainsaw.SaveLoadMenuGuiBehavior` (vtable hook `lateUpdateOnActive`
  slot 40, once a frame while up). `<CurrMode>` DefaultLoad 0 (the title), InGameLoad 2 (game over),
  InGameSave 3 (a typewriter), InGamePauseLoad 4 - the mod sits beside those, never 5/6 (chapter and ending
  saves) or 7/8. `<CurrCampaignIdentifier>` (main / Separate Ways), `<CurrAutoSaveSlotIndex>` (0 / 100),
  `<CurrItemMax>`; load lists start at the auto-save, save lists after it.
- Rows: `_CurrSaveFileDetails[i]` (`share.AppSaveFileDetail`: `<IsEmpty>`, `<SaveFileDetail>` - a native
  object: `get_Slot` +0x10, `get_LastUpdateTimeStamp` +0x30, decoded from the getters' code) and
  `_CurrSaveSlotDetails[i]` (`chainsaw.gui.SaveSlotInfoDetail` `<Info>`: `SlotIndex`, `ChapterID`,
  `Difficulty`, `SaveCount`, `MainCampaignClearCount`). A row shows a save when both match its slot. The
  chapter's number: `HighwayGuiManager.<ChapterMisc>[ChapterID]._ScenarioNo`.
- Busy: `share.SaveDataManager.<CurrentProcess>` not Idle, its `_RequestQueue`, `_SaveFlowController`
  (`<CurrPhase>` not Wait, its queue), `_RemoveFlowController` not Standby; the screen not Idle.
- Delete: `SaveService.removeSingle(slot)` (false while the service is busy); the game's own delete leaves
  the slot information too.
- Copy: never a byte copy - every file stamps its own slot in its SYSTEM_SAVE_FILE_DETAIL and the engine's
  table files it under that slot, so a byte copy shows as the source's twin and the target reads No Data.
  The engine writes it (`savefiles.cpp` service_op): the screen held still (`<CurrPhase>` DialogWait 5:
  `lateUpdateOnActive` does nothing there and `checkEnableClose` is false - `test_savefiles` checks);
  `SaveDataManager.<CurrentProcess>` = Processing (only with `_CurrentRequest` null: its update then waits);
  new `share.SaveDataManager.GameSaveData` and `GameSaveValidateData`, held by `entrySuspendGameSave`;
  `cleanData`, `addDataClass` under `getKeyString` of `Highway_GameSave_Key`/`_Validate_Key` (main, slots
  0-99) or `Chainsaw_...` (Separate Ways, 100-149), `loadSingle(source)`; at `SaveState` IDLE with
  `LastSaveResult` Success: the containers again, `set_SaveDataSize(ChainsawSaveDataDefinition.
  GAME_SAVE_DATA_SIZE)`, `saveSingle(target)`; at IDLE again: `exitSuspendGameSave` both, `cleanData`,
  Process Idle, the target's slot information made the source's (field copies by name, or
  `getSaveSlotInfo` + `addSaveSlotInfo`), `clearInfo(target)`; `updateSaveFileDetailTbl()` (mends earlier
  byte copies) and `requestSaveSystemData(-1, new SystemSaveRequestArgs)` once
  `isEnableAccessSaveFileDetailTbl`; then the screen Idle + WaitUpdateTbl. A failure lets everything go once
  the service is IDLE.
- Refresh: `<CurrSubPhase>` = WaitUpdateTbl (1), written by the hook after the screen's own update: its
  `lateUpdateOnActive` waits for the engine's table and the manager, then sets it back to None and calls its
  own `setup()` (`test_savefiles` checks that code).

## What is not done yet / unverified

The log of a first session (2026-10-22) shows the pause panel, the switches, Store into the old file box
and a save without counting at work; nothing else is confirmed in play. In the order the user should try it:

1. The panel appears beside the pause menu (Esc), F7 hides it, the mouse works on it and goes back to the
   game after; the log shows "the game's frame ticks the mod" and the events hooked.
2. God mode, One hit kills (a Ganado; the parasite out of a Ganado's head - the log: "one hit kills: a Ganado's
   parasite's hit raised to its health (first time)"; a boss), Infinite ammo (a handgun, a grenade, the bolt thrower), No
   durability loss (a parry), Ashley takes no damage (a hit on her). No armor durability loss (2026-10-24), with the Body
   Armor held (add it under Other items if need be): God mode off, take a Ganado's swing, an arrow, fire - the panel's
   (n / 1000) beside the switch stays, the HUD's armor gauge shows it full, no "armor's nearly gone" line, and Leon
   takes no damage from what the armor takes whole; the log: "events: UniqueInventoryController.onDurabilityChanged ...
   called by the game (first time)", "no armor durability loss: the body armor's wear taken back (first time)". Off, a
   hit wears it again. The Armor durability editor: 0 shows (broken), then 1000 mends it. God mode on the water
   scooter (2026-10-26; chapter 16's escape): crash into walls and debris - the HUD's vehicle gauge stays full, no
   crash sinks it, and the pause panel shows "(water scooter n / n)" beside God mode during the ride; the log: "god
   mode: the water scooter takes no damage either - its hull held at ...". The game then awards Smooth Escape at the
   goal. God mode for Luis (2026-10-27; Separate Ways chapter 3, the flanking tower where he goes into the flames): the
   gauge that drained stays full and never runs out, the pause panel shows "(time to save Luis held)" beside God mode
   (with it off: "(m:ss to save Luis)"); the log: "events: CustomLevelTimer.lateUpdate ... hooked" as the scene comes up,
   "god mode: the time limit to save Luis held - 180 s ...". Wherever he fights beside Ada: "(Luis n / max)" stays full and
   the log says "god mode (Luis): no damage, health held at its maximum".
3. Automatic QTEs: the first boulder/dodge prompt; a grab you mash out of; a knife-only grab with a knife;
   El Gigante's and Krauser's prompts; the laser corridor; the lever with Ashley; the swim after Del Lago.
4. Steady aim: no kick or jolt when firing (a handgun, the shotgun, a rifle's scope), no sway while aiming; the
   reticle focused at once; shots without spread; switching off puts the spread back. Since 2026-10-23 the log
   says "steady aim: the player's camera controller reached", the four camera hooks "hooked" and "called by the
   game (first time)", then "the camera's hand shake taken out (first time)" and "no camera shake while aiming
   (first time ...)" once you aim and fire. Since 2026-10-24 Leon's arms and the gun hold still while you aim a
   handgun, shotgun or SMG, standing still too (no breathing sway; the shot's own motion stays): the log says
   "PlayerEquipment.get_IsEnableOpticalScope ... hooked", then "steady aim: the arms' sway taken out with a gun aimed
   without a scope (first time)".
5. Infinite money at the merchant (a purchase, an upgrade; a case purchase cancelled). Infinite spinels (2026-10-23):
   a trade leaves the spinels and the screen's counter as they were (the log: "infinite spinels: the merchant took
   ... given back"); a trade whose item goes into a full case, its placement cancelled, leaves them as they were
   too ("a cancelled trade's refund ... taken back"); a spinel found still counts. Infinite upgrade tickets
   (2026-10-24): with one Exclusive Upgrade Ticket (traded for spinels, or added under Treasures), choose a
   weapon's exclusive upgrade, pay with the ticket and hold: the upgrade is made and the ticket is still there - the
   log: "events: InGameShopGuiState_CustomCommandDefault.onHoldGaugeCompleted ... called by the game (first time)",
   "infinite upgrade tickets: the merchant took an Exclusive Upgrade Ticket - given back (1)"; the merchant's ticket
   count on the next weapon still says 1. Paying in pesetas leaves the tickets as they were too. The tickets editor
   (under the switch, pause menu up): from 0 set 3 - the treasures list shows Exclusive Upgrade Ticket x3 (the log:
   "inventory: Exclusive Upgrade Ticket: 0 -> 3"); set 1, then 0 (taken out); the merchant counts what it set.
6. Save without counting (a typewriter save), Freeze play time (the pause menu's time; across a chapter end
   and a continue - fixed 2026-10-25: play a minute or two past a checkpoint, die, wait on the game over
   screen, Continue: the pause menu shows the time it had before the death, and the log says "freeze play
   time: the game kept its clock across this load (m:ss past the save it loaded ...) - the play time held
   stays h:mm:ss"; the same for the pause menu's Load of the newest save, while an older save brings its own
   time; the RESULTS screen's TOTAL PLAY TIME held too, and set with the play time editor - the
   pause menu's clock shows an edit after reopening the menu; Separate Ways' time across chapters 1-2), the
   countdown (a timed scene), the difficulty switch. The results (fixed 2026-10-23: the
   table never showed before): the table appears under Results; Deaths and Damage don't count (a death or a
   hit taken leaves the panel's count as it was; the log: "deaths don't count: a death taken back", "damage
   doesn't count: a hit taken back" once); the chapter's and the whole game's editors (deaths, times damaged,
   kills, hits, misses; the log "results: ... set to"), checked against the chapter-end results screen.
7. The attache case editor: a count, rounds, upgrades (Apply), the exclusive, Add (a weapon, ammo, a key
   item, a treasure, a charm), grow the case; the item box is 13. Add to the case's Weapons tab ends with
   Special knives - Combat, Fighting, Primal (in Separate Ways the Tactical and Elite too; 2026-10-27);
   Sub-weapons holds the Kitchen and Boot Knives and the grenades.
8. The save files: Delete and Copy beside the title's Load list and a typewriter's list: the copy shows at
   once, loads as its own slot, and is still listed after a restart; a slot an earlier byte copy broke
   reads right after copying into it again. The window holds steady beside the list (it flashed before
   2026-10-23's fix): the log has one "save files: the load list is up" and one "... closed" per visit.
9. The Mercenaries: Endless combo, Mercenary Mode always on, the clock frozen. Kill enemies as they appear
   (2026-10-26): on before or after the clock starts, every enemy falls as it appears (the big ones too; a Ganado's
   parasite once it is out), and the kills, the combo, the score and the clock rise as for your own shots; the panel
   says "(n enemies about)" beside it, "(waiting for the clock)" before. The log: "call: Kill enemies as they
   appear's calls: a hit from the player 1", "kill enemies as they appear: the first hit asked for - ...", "...: a hit
   of the mod's reached an enemy - raised to its health (first time)", and every 15 s "... hits asked for in 15 s, N
   reached an enemy; K kills (+k), E enemies about, up to C at once coming into play". "N reached" well below the hits
   asked for, or a "still stands after 25 hits" line (with the enemy's kind), says where the hits are turned away;
   kills that stop while enemies are about, or none about for long, say the stage stopped sending; "an enemy of kind
   N is left alone" names one the mod cannot tell is in play. The first build crashed the game at its first hit (an
   enemy not yet started, see the section); fixed and reinstalled 19:33 the same day. Played then (20:02-20:07): no
   crash, but spawns came only as the player walked around, and Ganados carrying a parasite never fell (two with it out
   "still stand after 25 hits"). Fixed the same evening (every area sends; a waiting parasite kept in, one out hit
   through its Ganado; see the section): standing still, enemies keep coming and falling; no Ganado or parasite left
   attacking; the log says "a Ganado carrying a parasite - the parasite kept in ... (first time)", "a spawn point's
   area (the player not in it) taken as holding ... (first time)", the three ConditionArea/FlagMer/Flag checks
   "hooked" once the stage is up, and the pace line's "spawn points opened K times (3 of 3 checks hooked)" with K
   above 0 and "N active (spawning stops at L, ...)" - N held at L says the game's own limit is what stops the
   spawns. Played 22:02-22:05: the parasites fixed (no survivors), but the kills still came in bursts as the player
   moved; then every enemy of the stage was down (the stage cleared - 150 is all a stage holds). Fixed 22:48 (the
   points' cooldowns cut to a second): standing still, the kills keep coming until the stage clears; the log's "cut
   short their cooldown (first time)", and no "the kills stalled" line - one names the spawners that could send and
   why they did not. Played 22:54-22:56: steady to 122 kills, then the throwers held back until the player moved
   (their points round the middle, out of view only, 10 m away). Fixed 23:04 (a held-up spawner's points send in view
   too, from 2 m): standing in the middle, the kills run on to the stage's clear; the log's "spawner ... held an enemy
   back for 2 s ... their own rules lifted" for the throwers, then "the held-up spawners' own rules put back". The user
   confirmed it worked (23:06). Since 23:12 it lasts one match: the panel's switch shows off in the next match's pause
   menu (and the ini says AutoKill = 0) after a clear, a game over, a quit or a retry - the log's "switched off - the
   match is over" - and a switch left on in the ini is off as the next match begins ("off as the match begins").
10. The shooting gallery plays itself (every game; 3-A's bonus wave needs the pool wait), and costumes change
    from the pause menu's EXTRAS (greyed in combat; the costume kept after a reload).

11. The challenges window beside the challenges screen (main menu > Bonuses > Challenges, and the pause
    menu's): switch one on (the game's list shows it once its cursor moves; the CP; its shop item buyable;
    an achievement), off (locked again), Complete all and Clear all on a tab; a restart keeps them. The log
    says how many challenges carry an achievement (the static table read at run time).
12. F7 and the windows: nothing at the title or the main menu; beside each screen its window, F7 hides it
    and it is back the next time; in play F7 opens the pause panel over the game; a window folded by its title
    bar's arrow comes back folded - the next visit and after a restart.
13. The item box in the saves (2026-10-23; the log: "events: ArmouryManager.saveGameSaveData/loadGameSaveData
    ... hooked", "this save carries the item box", "a save was loaded - its item box holds"): Store a weapon,
    an attachment and First Aid Sprays (the game's storage: marked "storage", listed at a typewriter's Storage,
    a second spray stacks) and ammo, herbs, gunpowder (the mod's part); Take each back (a weapon with its
    upgrades and rounds); save at a typewriter and load that save - both parts come back; another save has its
    own box; a new game starts empty; Continue after a death brings back the box of the save it loads; the
    merchant's Sell list shows only the storage's own kinds. Move into this game brings the old file's items in
    (the Combat Knife and the spray into the storage, the rest beside it); the file stays until the next save,
    then goes (since 2026-10-28; renamed `.moved` before).
14. Many at once (2026-10-25): Other items (and key items, treasures) sorted under headings (Recipes, Charms,
    Treasure maps, Attache cases, Armor); in Add other items, Tick all listed then Add - one question, every
    one added (the log: "inventory: N changes asked for together (panel)", then "inventory: N of N added");
    in Add to the case, tick ammo and resources with counts, Add with a case too small for them all - the note
    under the list says how many went in and what found no room; tick case rows (a heading's or All) and Store -
    no question, all in the box; tick box entries and Take; type two counts and Apply all; tick treasures and
    Remove - one question each.
15. The Extra Content Shop window (2026-10-27): main menu > Bonuses > Extra Content Shop - the window comes up
    beside it (F7 hides it), its tabs the shop's (SEPARATE WAYS with that DLC), each item's state as the shop shows
    it and its price. Tick a locked item (a costume): the shop's grid shows it for sale at once (or Not enough CP),
    the cursor and the scroll where they were - with the cursor on that item, its "???" and lock text become its
    name and text (2026-10-28) - and it buys with CP as usual; untick an unlocked one not bought - locked again at
    once; a purchased one's tick cannot be cleared. With a gamepad, hold its buy button on an item and tick another
    with the mouse: the purchase goes through, then the grid shows the tick. Unlock all under a tab asks once and unlocks all
    it lists; Lock all asks once. Restart the game: the unlocks stay (the system save). The log: "call: the shop
    window's calls: the shop's page shown again 1", "events: ExContentsShopGuiBehavior.lateUpdateOnActive ... hooked"
    once the shop has opened, "shop: the Extra Content Shop is up", "shop: the game's table read: 257 items", "shop:
    the shop's screen shows 6 tab(s), Separate Ways' too", "shop: ... unlocked", "shop: the shop's page shown again by
    its own code ... (first time)", "shop: the game's system save asked for", "shop: saved".

Open questions: the gallery's menu slot to stage file mapping (3-A/3-B and 4-A/4-B files swap by name); the
`GetTypeFromHandle`/`CreateInstance` route and the copy's T0-T4 timing are read from code only; the
challenges' achievement table is read from `GameRecordIdDefinition`'s static block once its .cctor has run.
The game's code reads it straight from a .data global (exe+0xD634730, `getAchievementId`), as it reads every
static - `AppSingleton.get_Instance` is `mov rax,[rip+X]` too, and those the static table finds - so it
should be the same block; unverified in play. The item box in the saves is read from code only: that the
engine writes the storage's `ArmourySaveData` items whole (its fields carry the serialized flag, as
`ArmouryItems`' do) and reads them back, and that Continue reloads through `applyLoadedGameSaveData` (its only
caller is `GameStateIngameSetup.update`).
