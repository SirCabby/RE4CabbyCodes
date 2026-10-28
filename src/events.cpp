#include "events.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "call.h"
#include "config.h"
#include "exshop.h"
#include "game.h"
#include "inventory.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "savefiles.h"
#include "spinels_rules.h"
#include "stamp.h"

namespace re4cc::events {
namespace {

enum Hook : int {
  // Database entries: the handlers the game makes delegates of.
  kHitCalc = 0,     // CharacterBodyHitDriver.onCalculateDamage: every character's hit, worked out (One hit kills, Ashley)
  kParasiteCalc,    // Ch1c0ParasiteUpdaterCommon.onCalculateDamage: a hit on a Ganado's parasite, worked out (One hit kills)
  kShopMoney,       // InGameShopRootGui.onChangedPtas: the merchant's money changed (Infinite money)
  kShopSpinels,     // InGameShopRootGui.onSpinelCountChanged: the spinels changed at the merchant (Infinite spinels)
  kUpgradeHold,     // InGameShopGuiState_CustomCommandDefault.onHoldGaugeCompleted: an exclusive upgrade paid for (Infinite upgrade tickets)
  kArmorWear,       // UniqueInventoryController.onDurabilityChanged: the body armor worn down (No armor durability loss)
  kLaser,           // GmQTELaserTrap.onOverlappingLaserCollider: the laser corridor's prompt (Automatic QTEs)
  // Vtable slots: virtual methods.
  kUseWeapon,       // CsInventoryController.useWeapon(EquipType): a shot or a throw (Infinite ammo)
  kUseWeaponCost,   // CsInventoryController.useWeapon(EquipType, cost, ...): a knife worn down (No durability loss)
  kCampaignSave,    // CampaignManager.saveGameSaveData: a save's data collected (Save without counting)
  kOccurrence,      // ChapterStatsGameOccurrenceObserver.catchGameOccurrence: a results count (Misses/Deaths/Damage don't count)
  kSaveMenu,        // SaveLoadMenuGuiBehavior.lateUpdateOnActive: a save or load list's frame (the save files)
  kGrabEscape,      // ButtonMashingEscapeCondition.update: a grab's mash check (Automatic QTEs)
  kGallery,         // ShootingGallery2Controller.update: the shooting gallery's frame (it plays itself)
  kLaser3,          // GmQTELaserTrap3.updateGimmick: the second laser's prompt (Automatic QTEs)
  kHookShot,        // GmHookShot.updateGimmick: Ada's hookshot prompt in the Ch4fez0 fight (Automatic QTEs)
  kTentacle,        // GmTentacleAvoid.updateGimmick: the tentacle's dodge prompt (Automatic QTEs)
  kGrappleParry,    // Ch1f7z0 CheckGrappleParryEscape.update: mutated Krauser's grab, escaped by a parry (Automatic QTEs)
  kShutter,         // GmTimingShutterGauge.updateGimmick: the timed lever's gauge (Automatic QTEs)
  kKnifeEscape,     // KnifeEscapeCondition.update: a grab's knife escape (Automatic QTEs: knife-only grabs)
  kGanadoGrab,      // Ch1c0z0 CheckEscapeGrapple.update: a Ganado's hold (Automatic QTEs)
  kRegenGrab,       // Ch1d4z0 CheckEscapeGrapple.update: a Regenerador's grab (Automatic QTEs)
  kSwim,            // Ch1f1z0 SearchEvent_Start.update: swimming back to the boat at Del Lago (Automatic QTEs)
  kGunFire,         // PlayerBehaviorTreeAction_MFSM_GunFire.onStart: a shot fired (Steady aim: no recoil)
  kCameraUpdate,    // PlayerCameraController.onCameraUpdate: the player camera's frame (Steady aim: no recoil, no hand shake)
  kShakeRate,       // PlayerCameraController.get_CameraShakeApplyRate: how much of a camera shake it takes (Steady aim)
  kHoldShake,       // PlayerBehaviorTreeAction_MFSM_HoldShake.onUpdate: the hold's body shake layer (Steady aim)
  kScopeShake,      // PlayerBehaviorTreeAction_MFSM_ScopeDangerHoldShake.onUpdate: the same in a scope (Steady aim)
  kOpticScope,      // PlayerEquipment.get_IsEnableOpticalScope: the shake actions ask it (Steady aim: no sway with any gun)
  kArmourySave,     // ArmouryManager.saveGameSaveData: the storage's part of a save collected (the item box)
  kArmouryLoad,     // ArmouryManager.loadGameSaveData: the storage handed its part of a save (the item box)
  kSpawnArea,       // CharacterSpawnConditionMercenaries.ConditionArea.check: the player in a spawn point's areas (Kill enemies as they appear)
  kSpawnFlagMer,    // ... ConditionFlagMer.check: a spawn point's Mercenaries general flags (the same)
  kSpawnFlag,       // ... ConditionFlag.check: a spawn point's flags (the same)
  kLevelTimer,      // CustomLevelTimer.lateUpdate: Separate Ways' time limit to save Luis, its frame (God mode)
  kShopScreen,      // ExContentsShopGuiBehavior.lateUpdateOnActive: the Extra Content Shop's frame (the shop window)
  kHookCount
};
constexpr int kFirstSlotHook = kUseWeapon;

struct State {
  const char* name = "";
  uint32_t method = 0;
  uint32_t cls = 0;             // vtable hooks: the class whose vtable carries it
  bool interfaces = false;      // also the interface table slots holding the same code
  uintptr_t replacement = 0;
  std::atomic<uintptr_t> original{0};  // the game's code, forwarded to
  std::atomic<long> calls{0};
  uintptr_t record = 0;         // the database entry exchanged
  uintptr_t slot[8] = {};       // the vtable / interface slots exchanged
  int slots = 0;
  bool failed = false;          // not hookable this session (logged)
  bool first_logged = false;
  DWORD waiting_since = 0;      // a vtable hook still waiting for its class (the log)
};
State g_hook[kHookCount];
bool g_disabled = false;

inline void count(Hook h) { g_hook[h].calls.fetch_add(1, std::memory_order_relaxed); }
template <typename Fn>
inline Fn original(Hook h) {
  return reinterpret_cast<Fn>(g_hook[h].original.load(std::memory_order_relaxed));
}
inline bool on(cheats::Kind k) { return cheats::enabled(k); }
bool hooked(Hook h) {
  const State& st = g_hook[h];
  return st.original.load() && (st.record || st.slots > 0);
}

// --- family hooks: one virtual method in every class of a family ----------------------------------------------
// A method some subclasses override (each with code of its own) and others
// inherit: every class deriving from `base` that has a table of its own gets its
// slot exchanged, and keeps the code that slot held - the replacement calls the
// code of `this`'s own table.
struct FamilySlot {
  uintptr_t table = 0, at = 0, original = 0;
};
struct Family {
  const char* name = "";
  uint32_t base = 0, method = 0;
  int vt = -1;
  uintptr_t replacement = 0;
  uintptr_t fallback = 0;           // the base's own code (a table that is not found: cannot happen)
  std::vector<uint32_t> waiting;    // classes of the family whose table is not hooked yet
  FamilySlot slot[64];
  std::atomic<int> n{0};
  std::atomic<long> calls{0};
  bool failed = false, first_logged = false;
};
enum FamilyId : int {
  kFamActionSign = 0,  // PlayerHeadUpdater.onHitActionSign: a prompt's sign reaching the player (Automatic QTEs)
  kFamilyCount
};
Family g_family[kFamilyCount];

uintptr_t family_original(Family& f, uintptr_t self) {
  const uintptr_t table = re::vtable_of_object(self);
  const int n = f.n.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (f.slot[i].table == table) return f.slot[i].original;
  return f.fallback;
}

// Once per activation: true the first time `obj` is seen active since it was
// last seen inactive (a button pressed once per prompt - pressed again the next
// frame it would read as held).
struct Edge {
  std::atomic<uintptr_t> obj[16] = {};
};
bool rising(Edge& e, uintptr_t obj, bool active) {
  for (auto& a : e.obj)
    if (a.load(std::memory_order_relaxed) == obj) {
      if (!active) a.store(0, std::memory_order_relaxed);
      return false;
    }
  if (!active) return false;
  for (auto& a : e.obj) {
    uintptr_t none = 0;
    if (a.compare_exchange_strong(none, obj)) return true;
  }
  return false;
}

// --- names the hooks read (found in install) --------------------------------------------------------------
uint32_t t_body_driver, t_enemy_driver, t_partner_driver, t_cs_controller, t_cs_inventory, t_cs_item, t_item,
    t_weapon_item, t_occurrence_observer, t_save_menu, t_mash, t_gallery, t_shop_root, t_campaign, t_parasite;
re::Field f_driver_ctx, f_dv_damage, f_ctl_ctx, f_ctl_inventory, f_inv_items, f_inv_equips, f_equip_id, f_csi_item,
    f_csi_force, f_item_guid, f_item_count, f_wi_ammo, f_wi_tactical, f_mash_enabled, f_mash_required, f_mash_current,
    f_gallery_score, f_gallery_routine, f_gallery_stage;
// A Ganado's parasite: the host's head updater, and a head updater's context.
re::Field f_par_owner_head, f_head_context;
int k_gallery_game = 4;
// The shooting gallery's own play: its targets and medals.
uint32_t t_anim, t_medal;
re::Field f_gal_score_ud, f_gal_target_scores, f_gal_move, f_gal_stock, f_gal_medals, f_gal_wave, f_gal_timer, f_ts_target,
    f_ts_body, f_ts_head, f_stage_waves, f_wave_time, f_wave_units, f_unit_target, f_unit_life, f_anim_target,
    f_anim_ignore, f_anim_unit, f_anim_routine, f_anim_timer, f_anim_need_down, f_anim_head, f_anim_broken,
    f_anim_display, f_anim_disp_pos, f_medal_routine, f_medal_need_display, f_medal_broken, f_medal_display, f_medal_anim;
int k_anim_move = 1, k_anim_break = 2, k_anim_down = 3, k_medal_display = 1, k_target_fly = 4;
// The merchant's case purchase (Infinite money): the price a cancelled placement refunds.
re::Field f_shop_flow, f_flow_states, f_states_hub, f_hub_case, f_case_price;
uint32_t t_shopmgr;
re::Field i_shopmgr;
// The merchant's screen's state (Infinite spinels): a case trade's placement opening and closing.
re::Field f_states_curr;
uint32_t t_case_reward_enter, t_case_reward_exit;
// Automatic QTEs: the prompts' own objects.
uint32_t t_action_sign, t_player_head, t_laser, t_laser3, t_hookshot, t_tentacle, t_grapple_parry, t_shutter, t_gimmick_input;
re::Field f_hit_sign, f_hit_button, f_hit_act, f_hit_just, f_head_sign_driver, f_head_command, f_sign_avoid,
    f_sign_parry, f_sign_gui, f_cant_parry, f_info_accepting, f_info_reserve, f_info_request_enable;
re::Field f_laser_routine, f_laser_option, f_laser_head, f_laser_command, f_laser3_start, f_laser3_option,
    f_laser3_command;
re::Field f_hook_id, f_hook_can_use, f_hook_command, f_hook_disabled;
re::Field f_tentacle_time, f_tentacle_timer, f_tentacle_rno, f_tentacle_sensor, f_gimmick_keycheck, f_keycheck_ui;
re::Field f_gp_b_button, f_gp_just_enabled, f_gp_head, f_gp_gui_open, f_gp_success, f_gp_track_just;
re::Field f_shutter_gauge, f_shutter_area;
int k_button_avoid = 1, k_sign_parry_max = 5, k_sign_krauser_normal = 1, k_sign_krauser_special = 2;
int k_laser_approach = 3, k_laser_end = 4, k_hook_ch4fez0 = 5;
uint32_t k_cmd_avoid = 57363640u, k_cmd_melee_hold = 0, k_cmd_decide = 0;
Edge g_edge_laser3, g_edge_hookshot, g_edge_tentacle, g_edge_grapple;
// The timed levers' gauges the mod widened, and what they had (put back when the switch goes off).
struct Widened {
  std::atomic<uintptr_t> obj{0};
  float area = 0.0f;
};
Widened g_widened[8];
// Grabs and holds (the mash, hold and knife escapes).
uint32_t t_knife_cond, t_c0_grab, t_d4_grab, t_swim, t_gun_fire;
re::Field f_esc_enabled, f_esc_satisfied, f_cmd_triggers;
re::Field f_c0_knife, f_c0_button, f_c0_struggles, f_c0_delay, f_c0_head, f_c0_input, f_c0_counter;
re::Field f_d4_head, f_d4_settings, f_d4_push_count, f_d4_input, f_d4_finished, f_d4_disabled, f_gs_knife, f_gs_push,
    f_gs_count;
re::Field f_swim_max, f_swim_rno0, f_swim_rno1, f_swim_count;
uint32_t k_d4_disabled_knife = 1, k_d4_disabled_push = 2, k_cmd_escape_knife = 942818309u;
// The knife escape of the grab whose conditions are being updated (the grab's
// update runs its knife's, then its mash's, back to back on one thread): set by
// the knife's hook when the player could take it, taken by the mash's.
thread_local uintptr_t t_knife_ready = 0;

// --- One hit kills, and Ashley ------------------------------------------------------------------------------
// Every character's hit goes through its body hit driver's onCalculateDamage
// once the game has worked the damage out (a delegate each character makes of
// this method as it is configured), before the game decides whether it kills.
// An enemy's hit with damage is raised to the enemy's health, so the game's own
// code decides the kill - its death, drops and parasites as usual; an enemy the
// game keeps alive for now (NoDeath, a boss between phases) keeps its last point.
// Ashley's hits are zeroed (her driver is a PartnerBodyHitDriver): with no
// damage the game neither takes her health nor puts her down.
using CalcFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t damage_value, uintptr_t info);

// One hit kills: a hit's damage raised to the health the character has left.
// True when it was.
bool raise_to_health(uintptr_t dv, uintptr_t context, int damage, const char* who) {
  game::Health h;
  if (!context || !game::health(context, &h) || h.hp <= damage) return false;
  game::put(dv, f_dv_damage, h.hp);
  if (config::get().trace) logf("one hit kills: a hit of %d on %s with %d HP raised to %d", damage, who, h.hp, h.hp);
  return true;
}

// Kill enemies as they appear: the attacks the tick asked for, by the number the
// HitManager gave each (HitController.CalculateInfo carries its attack's). A hit
// of one of them is raised to the enemy's health whatever its damage came to and
// whatever One hit kills says.
constexpr int kKillRing = 128;
std::atomic<uint32_t> g_kill_ids[kKillRing];
std::atomic<uint32_t> g_kill_next{0};
std::atomic<long> g_kill_landed{0};
re::Field f_calc_attack_id;

bool kill_attack(uintptr_t info) {
  if (!on(cheats::kAutoKill) || !info || !f_calc_attack_id.valid()) return false;
  uint32_t id = 0;
  if (!game::get(info, f_calc_attack_id, &id) || !id) return false;
  for (const auto& k : g_kill_ids)
    if (k.load(std::memory_order_relaxed) == id) return true;
  return false;
}

void kill_landed(const char* who) {
  if (g_kill_landed.fetch_add(1, std::memory_order_relaxed) == 0)
    logf("kill enemies as they appear: a hit of the mod's reached %s - raised to its health (first time)", who);
}

void on_hit_calc(uintptr_t ctx, uintptr_t driver, uintptr_t dv, uintptr_t info) {
  count(kHitCalc);
  original<CalcFn>(kHitCalc)(ctx, driver, dv, info);
  if (call::exception_pending(ctx) || !dv) return;
  const bool ohk = on(cheats::kOneHitKills), ashley = on(cheats::kAshleyGod), ours = kill_attack(info);
  if (!ohk && !ashley && !ours) return;
  const uint32_t t = re::type_of(driver);
  if (!t) return;
  int damage = 0;
  if (!game::get(dv, f_dv_damage, &damage)) return;
  if ((ours || (ohk && damage > 0)) && re::derives(t, t_enemy_driver)) {
    raise_to_health(dv, game::ref(driver, f_driver_ctx), damage, "an enemy");
    if (ours) kill_landed("an enemy");
  } else if (ashley && damage > 0 && re::derives(t, t_partner_driver)) {
    const uintptr_t context = game::ref(driver, f_driver_ctx);
    if (context && game::kind_of(context) == game::kAshleyPartnerKind) {
      game::put(dv, f_dv_damage, 0);
      if (config::get().trace) logf("ashley: a hit of %d on Ashley taken to 0", damage);
    }
  }
}

// A Ganado's parasite (the plaga out of a broken head) is hit through a hit
// controller of its own, whose damage its updater works out - another method,
// of which each parasite makes a delegate as it starts - not through the body's
// driver. Its health is its host's: as it appears the game sets the parasite's
// own into the Ganado's HitPoint (Ch1c0HeadUpdaterCommon.setupParasiteParam),
// and its hit controller's context is the host's (the parasite's get_Context:
// its owner head updater's). A hit with damage is raised to that, as the body's is.
void on_parasite_calc(uintptr_t ctx, uintptr_t parasite, uintptr_t dv, uintptr_t info) {
  count(kParasiteCalc);
  original<CalcFn>(kParasiteCalc)(ctx, parasite, dv, info);
  if (call::exception_pending(ctx) || !dv) return;
  const bool ours = kill_attack(info);
  if (!ours && !on(cheats::kOneHitKills)) return;
  int damage = 0;
  if (!game::get(dv, f_dv_damage, &damage) || (damage <= 0 && !ours)) return;
  const uintptr_t head = game::ref(parasite, f_par_owner_head);
  if (raise_to_health(dv, head ? game::ref(head, f_head_context) : 0, damage, "a Ganado's parasite") && !ours) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true)) logf("one hit kills: a Ganado's parasite's hit raised to its health (first time)");
  }
  if (ours) kill_landed("a Ganado's parasite");
}

// Every area sends. A Mercenaries spawner sends its next enemy from the nearest of
// its points that is cool, out of the camera's view where the point asks it, past
// the spawner's distance from the player - and whose added conditions hold
// (CharacterSpawnPointControllerMercenaries.tryGetSpawnPoint, the point's
// _AddConditions): the player inside the point's areas (boxes over a part of the
// stage; a point serves the player in one part, its enemies coming from another),
// Mercenaries general flags, flags. Standing in one part, the few points serving it
// - in view, cooling down - left the spawns to a trickle until the player moved
// (the user, 2026-10-26: "I needed to walk around to trigger new spawns to come
// out"). While the cheat acts, each of those conditions holds, so every point of
// every area serves wherever the player stands, as the 2005 game's mod made every
// area send. The three condition classes are the spawn points' alone (nested in
// CharacterSpawnConditionMercenaries); CustomConditionBase.check asks each through
// its class's table (slot 7).
std::atomic<bool> g_spawn_everywhere{false};
std::atomic<long> g_spawn_opened{0};
std::atomic<bool> g_spawn_logged[3];
uint32_t t_spawn_condition[3];
using CheckFn = bool (*)(uintptr_t ctx, uintptr_t self);

bool spawn_condition(Hook h, int which, uintptr_t ctx, uintptr_t self) {
  count(h);
  const bool held = original<CheckFn>(h)(ctx, self);
  if (held || !g_spawn_everywhere.load(std::memory_order_relaxed) || !on(cheats::kAutoKill) ||
      call::exception_pending(ctx) || !re::is_a(self, t_spawn_condition[which]))
    return held;
  g_spawn_opened.fetch_add(1, std::memory_order_relaxed);
  static const char* const kWhat[3] = {"area (the player not in it)", "general flags", "flags"};
  if (!g_spawn_logged[which].exchange(true))
    logf("kill enemies as they appear: a spawn point's %s taken as holding - it sends wherever the player stands "
         "(first time)",
         kWhat[which]);
  return true;
}
bool on_spawn_area(uintptr_t ctx, uintptr_t self) { return spawn_condition(kSpawnArea, 0, ctx, self); }
bool on_spawn_flag_mer(uintptr_t ctx, uintptr_t self) { return spawn_condition(kSpawnFlagMer, 1, ctx, self); }
bool on_spawn_flag(uintptr_t ctx, uintptr_t self) { return spawn_condition(kSpawnFlag, 2, ctx, self); }

// --- Infinite money ----------------------------------------------------------------------------------------------
// The merchant's screen hears every change of the money through this handler (a
// delegate it subscribes as it sets up): every payment - a purchase, a tune-up,
// a repair, an exclusive upgrade, a case - lowers the money, and only selling
// raises it. A fall is given back at once, and the screen is told nothing
// changed, so its counter does not roll down. A case bought is placed in a
// screen of its own whose cancel refunds what the purchase cost: that price is
// cleared when it is what was given back, so a cancel refunds nothing.
using PtasFn = void (*)(uintptr_t ctx, uintptr_t self, int32_t prev, int32_t curr);
void on_shop_money(uintptr_t ctx, uintptr_t gui, int32_t prev, int32_t curr) {
  count(kShopMoney);
  if (on(cheats::kInfiniteMoney) && curr < prev && prev >= 0) {
    const int now = game::pesetas();
    const int spent = prev - curr;
    if (now >= 0 && now <= game::kPesetasMax - spent && game::set_pesetas(now + spent)) {
      // The case placement's refund (InGameShopManager -> flow -> states -> hub -> case purchase).
      const uintptr_t mgr = i_shopmgr.valid() ? mem::read_ptr(re::static_addr(i_shopmgr)) : 0;
      const uintptr_t flow = mgr ? game::ref(mgr, f_shop_flow) : 0;
      const uintptr_t states = flow ? game::ref(flow, f_flow_states) : 0;
      const uintptr_t hub = states ? game::ref(states, f_states_hub) : 0;
      const uintptr_t holder = hub ? game::ref(hub, f_hub_case) : 0;
      int price = 0;
      if (holder && game::get(holder, f_case_price, &price) && price == spent) game::put(holder, f_case_price, 0);
      logf("infinite money: the merchant took %d pesetas - given back (%d)", spent, now + spent);
      return original<PtasFn>(kShopMoney)(ctx, gui, prev, prev);
    }
  }
  original<PtasFn>(kShopMoney)(ctx, gui, prev, curr);
}

// --- Infinite spinels --------------------------------------------------------------------------------------------
// The merchant's screen hears every change of the spinels through this handler
// too (the manager's OnSpinelCountChanged, which it subscribes as it sets up),
// told only that they changed: the hook measures the change from the count the
// tick noted this frame, or the one it left them at itself. While the screen is
// up only a trade lowers them, and it is given back before the screen reads the
// count, so its counter does not roll down. A trade of an item to be placed in
// the case takes its spinels as the placement opens (the screen's state is then
// InGameShopGuiState_CaseRewardEnter) and a cancel refunds them as it closes
// (CaseRewardExit): a refund of spinels given back is taken out again
// (spinels_rules.h).
std::atomic<int> g_spinels_seen{-1};
std::atomic<int> g_case_given{0};  // what the last case trade's placement took and was given back (0: none)

using SpinelFn = void (*)(uintptr_t ctx, uintptr_t self);
// The merchant's screen's state (InGameShopManager -> flow -> states -> its current one).
uintptr_t shop_state() {
  const uintptr_t mgr = i_shopmgr.valid() ? mem::read_ptr(re::static_addr(i_shopmgr)) : 0;
  const uintptr_t flow = mgr ? game::ref(mgr, f_shop_flow) : 0;
  const uintptr_t states = flow ? game::ref(flow, f_flow_states) : 0;
  return states ? game::ref(states, f_states_curr) : 0;
}

void on_shop_spinels(uintptr_t ctx, uintptr_t gui) {
  count(kShopSpinels);
  const int seen = g_spinels_seen.load();
  int now = game::spinels();
  if (seen >= 0 && now >= 0 && now != seen) {
    const uintptr_t state = shop_state();
    const rules::Spinels r =
        rules::spinel_change(seen, now, on(cheats::kInfiniteSpinels), state && re::is_a(state, t_case_reward_enter),
                             state && re::is_a(state, t_case_reward_exit), g_case_given.load());
    int case_given = r.case_given;
    if (r.count != now) {
      if (game::set_spinels(r.count)) {
        if (r.given > 0) logf("infinite spinels: the merchant took %d spinel(s) - given back (%d)", r.given, r.count);
        else logf("infinite spinels: a cancelled trade's refund of %d spinel(s) taken back (%d)", -r.given, r.count);
        now = r.count;
      } else {
        case_given = 0;  // nothing was given back
      }
    }
    g_case_given.store(case_given);
  }
  if (now >= 0) g_spinels_seen.store(now);
  original<SpinelFn>(kShopSpinels)(ctx, gui);
}

// --- Infinite upgrade tickets ------------------------------------------------------------------------------------
// The merchant's exclusive upgrade is paid for when its hold gauge fills: this
// handler, a delegate its state makes each time it opens. Paid with the ticket,
// it takes one from the first of the treasures holding it (reduceTicket), and an
// entry emptied leaves the treasures - so that entry's count is raised by one
// first, and what the game takes leaves it as it was. When the game takes none
// (an upgrade it refused), the one added is taken off again. Pesetas and the
// ticket bought as DLC are left to the game (Infinite money keeps the pesetas).
using HoldFn = void (*)(uintptr_t ctx, uintptr_t self);
void on_upgrade_hold(uintptr_t ctx, uintptr_t state) {
  count(kUpgradeHold);
  const uintptr_t ticket = on(cheats::kInfiniteTickets) ? game::upgrade_ticket(state) : 0;
  int had = 0;
  const bool raised = ticket && game::get(ticket, f_item_count, &had) && had >= 1 && game::set_item_count(ticket, had + 1);
  original<HoldFn>(kUpgradeHold)(ctx, state);
  int now = 0;
  if (!raised || !game::get(ticket, f_item_count, &now)) return;
  if (now == had) logf("infinite upgrade tickets: the merchant took an Exclusive Upgrade Ticket - given back (%d)", had);
  else if (now == had + 1) game::set_item_count(ticket, had);
}

// --- Infinite ammo ----------------------------------------------------------------------------------------------------
// Every shot and throw of the player's: the head updater's useWeapon asks the
// case's controller to use the weapon in hand (this method - nothing else calls
// its slot), which takes the round (a gun's loaded rounds) or the piece (a
// grenade; the last one is removed inside the call). The player's own case only
// (Luis has one too). The equipped item's rounds, tactical rounds and count are
// noted first and its count raised by one, so the last grenade is not removed;
// after the call all three are put back. A scripted one-shot weapon the game
// takes back (IsForceGet) is left to it.
using UseFn = bool (*)(uintptr_t ctx, uintptr_t self, int32_t equip_type);
bool player_controller(uintptr_t controller) {
  const uintptr_t context = game::ref(controller, f_ctl_ctx);
  return context && game::is_player_kind(game::kind_of(context));
}
// The equipped item of the controller's case for an EquipType: its CsInventoryItem.
uintptr_t equipped_holder(uintptr_t controller, int equip_type) {
  const uintptr_t inv = game::ref(controller, f_ctl_inventory);
  if (!inv || !re::is_a(inv, t_cs_inventory)) return 0;
  re::Array eq;
  const uintptr_t arr = game::ref(inv, f_inv_equips);
  if (!arr || !re::read_array(arr, &eq) || equip_type < 0 || equip_type >= eq.count || eq.elem_size != 8) return 0;
  const uintptr_t info = mem::read_ptr(eq.data + static_cast<uintptr_t>(equip_type) * 8);
  uint8_t want[16] = {};
  const uintptr_t idat = info ? re::field_addr(info, f_equip_id) : 0;
  if (!idat || !mem::copy_from(want, idat, 16) || game::guid_empty(want)) return 0;
  re::List l;
  const uintptr_t list = game::ref(inv, f_inv_items);
  if (!list || !re::read_list(list, &l)) return 0;
  for (int i = 0; i < l.count && i < 256; ++i) {
    const uintptr_t holder = re::list_ref(l, i);
    const uintptr_t item = holder ? game::ref(holder, f_csi_item) : 0;
    uint8_t g[16] = {};
    const uintptr_t ga = item ? re::field_addr(item, f_item_guid) : 0;
    if (ga && mem::copy_from(g, ga, 16) && game::guid_equal(g, want)) return holder;
  }
  return 0;
}

bool on_use_weapon(uintptr_t ctx, uintptr_t controller, int32_t equip_type) {
  count(kUseWeapon);
  uintptr_t item = 0;
  int ammo = 0, tactical = 0, num = 0;
  bool weapon = false, held = false;
  if (on(cheats::kInfiniteAmmo) && re::is_a(controller, t_cs_controller) && player_controller(controller)) {
    const uintptr_t holder = equipped_holder(controller, equip_type);
    bool force = false;
    item = holder ? game::ref(holder, f_csi_item) : 0;
    if (item && re::is_a(item, t_item) && !(game::get(holder, f_csi_force, &force) && force) &&
        game::get(item, f_item_count, &num)) {
      weapon = re::is_a(item, t_weapon_item);
      if (weapon) {
        game::get(item, f_wi_ammo, &ammo);
        game::get(item, f_wi_tactical, &tactical);
      }
      held = game::put(item, f_item_count, num + 1);
    }
  }
  const bool r = original<UseFn>(kUseWeapon)(ctx, controller, equip_type);
  if (held) {
    game::put(item, f_item_count, num);
    if (weapon) {
      game::put(item, f_wi_ammo, ammo);
      game::put(item, f_wi_tactical, tactical);
    }
  }
  return r;
}

// --- No durability loss -------------------------------------------------------------------------------------------------
// Every way a knife wears - a slash, a parry, a stealth kill, breaking free of a
// grab - ends in the player's equipment asking its case to use the knife (the
// sub weapon) at a cost: this method. The cost goes in as 0, so the game's own
// code takes nothing off (and its rank and quest counts see no use either).
using UseCostFn = bool (*)(uintptr_t ctx, uintptr_t self, int32_t equip_type, int32_t cost, uintptr_t remaining);
bool on_use_weapon_cost(uintptr_t ctx, uintptr_t controller, int32_t equip_type, int32_t cost, uintptr_t remaining) {
  count(kUseWeaponCost);
  if (on(cheats::kNoDurability) && cost > 0 && equip_type == 1 /* EquipType.Sub: the knife */ &&
      re::is_a(controller, t_cs_controller) && player_controller(controller))
    cost = 0;
  return original<UseCostFn>(kUseWeaponCost)(ctx, controller, equip_type, cost, remaining);
}

// --- No armor durability loss -------------------------------------------------------------------------------------------------
// Every way the body armor wears - a hit it takes for the player, fire, frost,
// the boulder - ends in the unique inventory's useDurability: it takes the cost
// off (marking the armor broken once none is left) and then tells the inventory's
// controller through this handler, a delegate the controller makes as it sets up.
// The durability is put back here, still inside that call, so whatever the game
// reads after - the hit's "armor nearly gone" line, the HUD's armor gauge as it
// opens - finds it as it was; the controller is told nothing changed (it only
// feeds the dynamic difficulty). Repairs (a rise) are the game's.
using DurabilityFn = void (*)(uintptr_t ctx, uintptr_t self, int32_t item_id, int32_t prev, int32_t curr);
void on_armor_durability(uintptr_t ctx, uintptr_t controller, int32_t item_id, int32_t prev, int32_t curr) {
  count(kArmorWear);
  if (on(cheats::kNoArmorWear) && curr < prev && player_controller(controller) &&
      game::keep_armor(controller, item_id, prev, curr)) {
    static std::atomic<bool> s_logged{false};
    const bool first = !s_logged.exchange(true);
    if (first || config::get().trace)
      logf("no armor durability loss: the body armor's wear taken back (%d -> %d, kept at %d)%s", prev, curr, prev,
           first ? " - the next ones only with Trace" : "");
    return original<DurabilityFn>(kArmorWear)(ctx, controller, item_id, prev, prev);
  }
  original<DurabilityFn>(kArmorWear)(ctx, controller, item_id, prev, curr);
}

// --- Save without counting -------------------------------------------------------------------------------------------------
// A save at a typewriter adds one to the count as the save is asked for, and in
// the same call collects every manager's data - CampaignManager's, through this
// method, with the count. The count the tick took while no save list was open
// is put back here first, the chapter's own entry with it, so the save records
// the count as it was and the game goes on from it.
std::atomic<int> g_save_hold{-1};
std::atomic<DWORD> g_save_held_at{0};
using SaveFn = uintptr_t (*)(uintptr_t ctx, uintptr_t self);
uintptr_t on_campaign_save(uintptr_t ctx, uintptr_t mgr) {
  count(kCampaignSave);
  const int hold = g_save_hold.load();
  if (on(cheats::kNoSaveCount) && hold >= 0 && re::is_a(mgr, t_campaign)) {
    const int now = game::save_count();
    if (now == hold + 1 && game::set_save_count(hold)) {
      g_save_held_at.store(stamp::now());
      logf("save without counting: this save records %d (the game's count would have been %d)", hold, now);
    }
  }
  return original<SaveFn>(kCampaignSave)(ctx, mgr);
}

// --- The item box in the saves ------------------------------------------------------------------------------------------------
// The game's storage keeps its part of a save in ArmouryManager.GameSaveData, an
// object it makes at every save and reads at every load (IGameSaveData). The
// rest of the item box rides along in that object, in its item list the game
// reads back at a load but has written empty since an update (ArmourySaveData):
// every save carries the box of the game being played, and every load hands its
// own back to the mod - before the game reads the list, which would put those
// items in its storage, so the list is out of the game's sight while it does.
uint32_t t_armoury = 0;
using ArmourySaveFn = uintptr_t (*)(uintptr_t ctx, uintptr_t self);
uintptr_t on_armoury_save(uintptr_t ctx, uintptr_t mgr) {
  count(kArmourySave);
  const uintptr_t data = original<ArmourySaveFn>(kArmourySave)(ctx, mgr);
  if (call::exception_pending(ctx) || !re::is_a(mgr, t_armoury) || !game::is_storage_save(data)) return data;
  call::InHook calls;  // the box's items are made by the game's factory, inside the save's own call
  inventory::box_into_save(data);
  return data;
}

using ArmouryLoadFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t data);
void on_armoury_load(uintptr_t ctx, uintptr_t mgr, uintptr_t data) {
  count(kArmouryLoad);
  const uintptr_t list = re::is_a(mgr, t_armoury) ? inventory::box_from_save(data) : 0;
  if (list) game::set_storage_save_list(data, 0);
  original<ArmouryLoadFn>(kArmouryLoad)(ctx, mgr, data);
  if (list) game::set_storage_save_list(data, list);  // the save's object as it was (a load may read it again)
}

// --- Misses don't count, Deaths don't count, Damage doesn't count -------------------------------------------------------------
// Every count of the chapter's results - kills, deaths, shots, hits and times
// damaged (each hit that hurts the player) - comes to the chapter's occurrence
// observer through this method. The chapter's deaths, times damaged and shots
// are noted first; after the game's own count, a death or a hit taken is taken
// back, and only the shots that hit stay counted (the attempts move with the
// hits, never past them).
using OccurrenceFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t trigger, uintptr_t specifier, uintptr_t source,
                              uintptr_t target, uintptr_t value);
std::atomic<bool> g_damage_logged{false};  // hits come often: the first is logged, the rest with Trace
void on_occurrence(uintptr_t ctx, uintptr_t observer, uintptr_t trigger, uintptr_t specifier, uintptr_t source,
                   uintptr_t target, uintptr_t value) {
  count(kOccurrence);
  const bool deaths = on(cheats::kNoDeathCount), misses = on(cheats::kNoMissCount),
             damage = on(cheats::kNoDamageCount);
  game::StatsRefs r;
  int d0 = -1, g0 = -1, a0 = -1, h0 = -1;
  const bool have =
      (deaths || misses || damage) && re::is_a(observer, t_occurrence_observer) && game::stats_refs(&r);
  if (have) {
    d0 = game::element_count(r.death);
    if (r.damage) g0 = game::element_count(r.damage);
    game::accuracy(r.accuracy, &a0, &h0);
  }
  original<OccurrenceFn>(kOccurrence)(ctx, observer, trigger, specifier, source, target, value);
  if (!have || call::exception_pending(ctx)) return;
  if (deaths && d0 >= 0) {
    const int d1 = game::element_count(r.death);
    if (d1 > d0 && game::set_element_count(r.death, d0)) logf("deaths don't count: a death taken back (%d)", d0);
  }
  if (damage && g0 >= 0) {
    const int g1 = game::element_count(r.damage);
    if (g1 > g0 && game::set_element_count(r.damage, g0)) {
      const bool first = !g_damage_logged.exchange(true);
      if (first || config::get().trace)
        logf("damage doesn't count: a hit taken back (%d)%s", g0, first ? " - the next ones only with Trace" : "");
    }
  }
  if (misses && a0 >= 0) {
    int a1 = -1, h1 = -1;
    if (game::accuracy(r.accuracy, &a1, &h1) && a1 != a0) {
      const int want = a0 + (h1 > h0 ? h1 - h0 : 0);
      if (a1 != want) game::set_accuracy(r.accuracy, want, h1);
    }
  }
}

// --- Automatic QTEs: the prompts ---------------------------------------------------------------------------------------------------
// Every dodge and knife prompt of an enemy, a boss, a projectile or the boulder
// reaches the player through the head's onHitActionSign, each frame of its sign.
// After the game's own handling, a sign at its moment - the act or "just" timing
// - of a prompt the game draws as a QTE (a large sign, Krauser's knife fight, the
// normal signs it draws large) has its button "pressed": the action sign
// driver's own request, as the player's button would make it, with the game's
// checks (a knife, the parry cool-down). Ordinary parries (the small knife icon
// on every enemy's swing) are left alone: each costs durability and puts the
// knife in hand.
using SignFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t info);
void qte_sign(uintptr_t head, uintptr_t info) {
  int sign = -1, button = -1;
  bool act = false, just = false;
  if (!game::get(info, f_hit_sign, &sign) || !game::get(info, f_hit_button, &button)) return;
  game::get(info, f_hit_act, &act);
  game::get(info, f_hit_just, &just);
  if (!act && !just) return;
  const uintptr_t drv = game::ref(head, f_head_sign_driver);
  if (!drv || !re::is_a(drv, t_action_sign)) return;
  const bool avoid = button == k_button_avoid;
  const uintptr_t inf = game::ref(drv, avoid ? f_sign_avoid : f_sign_parry);
  bool accepting = true, reserve = true, cant = true, seen = false;
  if (!inf || !game::get(inf, f_info_accepting, &accepting) || !game::get(inf, f_info_reserve, &reserve) ||
      !game::get(drv, f_cant_parry, &cant) || !game::get(inf, f_info_request_enable, &seen))
    return;
  if (accepting || reserve || cant || !seen) return;  // (seen: the driver took this sign - Ashley's head filters some)
  bool qte = sign > k_sign_parry_max ||
             (!avoid && (sign == k_sign_krauser_normal || sign == k_sign_krauser_special));
  call::InHook in_hook;
  if (!qte && !avoid && sign < k_sign_parry_max) {
    const uintptr_t gui = game::ref(drv, f_sign_gui);
    qte = gui && call::large_normal_sign(gui) == 1;
  }
  if (!qte) return;
  const bool done = avoid ? call::reserve_avoid(drv) : call::reserve_parry(drv);
  if (config::get().trace)
    logf("automatic QTEs: %s prompt (sign %d, %s timing) %s", avoid ? "a dodge" : "a knife", sign,
         just ? "just" : "act", done ? "pressed" : "NOT pressed (the call failed)");
}
void on_action_sign(uintptr_t ctx, uintptr_t head, uintptr_t info) {
  Family& f = g_family[kFamActionSign];
  f.calls.fetch_add(1, std::memory_order_relaxed);
  reinterpret_cast<SignFn>(family_original(f, head))(ctx, head, info);
  if (!on(cheats::kAutoQte) || !info || call::exception_pending(ctx)) return;
  qte_sign(head, info);
}

// The laser corridor: in its approach, the frame the player is in the laser's
// avoid zone the trap reads the avoid button's trigger (its own hash: Separate
// Ways remaps it to the hookshot button). The button is pressed for that frame,
// before the trap looks - the trap still checks the player and the zone itself.
using LaserFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t collision);
void on_laser(uintptr_t ctx, uintptr_t trap, uintptr_t collision) {
  count(kLaser);
  if (on(cheats::kAutoQte) && re::is_a(trap, t_laser)) {
    int routine = -1, command_type = 0;
    const uintptr_t head = game::ref(trap, f_laser_head), opt = game::ref(trap, f_laser_option);
    if (game::get(trap, f_laser_routine, &routine) && routine == k_laser_approach && head && opt &&
        game::get(opt, f_laser_command, &command_type)) {
      call::InHook in_hook;
      const uint32_t hash = call::laser_avoid_hash(trap, command_type);
      const uintptr_t command = game::ref(head, f_head_command);
      if (hash && command) call::press_command(command, hash);
    }
  }
  original<LaserFn>(kLaser)(ctx, trap, collision);
}

// The second laser: its prompt, once its timing track starts it.
using GimmickFn = void (*)(uintptr_t ctx, uintptr_t self);
using ActionFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t arg);
void on_laser3(uintptr_t ctx, uintptr_t trap) {
  count(kLaser3);
  if (re::is_a(trap, t_laser3)) {
    bool started = false;
    int routine = -1, command_type = 0;
    const uintptr_t head = game::ref(trap, f_laser_head), opt = game::ref(trap, f_laser3_option);
    const bool active = game::get(trap, f_laser3_start, &started) && started &&
                        game::get(trap, f_laser_routine, &routine) && routine == k_laser_end && head && opt;
    if (rising(g_edge_laser3, trap, active) && on(cheats::kAutoQte) && game::get(opt, f_laser3_command, &command_type)) {
      call::InHook in_hook;
      const uint32_t hash = call::laser_avoid_hash(trap, command_type);
      const uintptr_t command = game::ref(head, f_head_command);
      if (hash && command) call::press_command(command, hash);
    }
  }
  original<GimmickFn>(kLaser3)(ctx, trap);
}

// Ada's hookshot prompt in the Ch4fez0 fight (Separate Ways).
void on_hookshot(uintptr_t ctx, uintptr_t hook) {
  count(kHookShot);
  if (re::is_a(hook, t_hookshot)) {
    int id = -1;
    bool can = false, disabled = true;
    const uintptr_t command = game::ref(hook, f_hook_command);
    const bool active = game::get(hook, f_hook_id, &id) && id == k_hook_ch4fez0 && game::get(hook, f_hook_can_use, &can) &&
                        can && game::get(hook, f_hook_disabled, &disabled) && !disabled && command;
    if (rising(g_edge_hookshot, hook, active) && on(cheats::kAutoQte)) {
      call::InHook in_hook;
      if (const uint32_t hash = call::hookshot_hash()) call::press_command(command, hash);
    }
  }
  original<GimmickFn>(kHookShot)(ctx, hook);
}

// The tentacle's dodge: the decide button within its time.
void on_tentacle(uintptr_t ctx, uintptr_t gm) {
  count(kTentacle);
  if (re::is_a(gm, t_tentacle)) {
    bool sensor = false;
    int rno = -1;
    float time = 0.0f, timer = 0.0f;
    const bool active = game::get(gm, f_tentacle_sensor, &sensor) && sensor && game::get(gm, f_tentacle_rno, &rno) &&
                        rno == 0 && game::get(gm, f_tentacle_time, &time) && game::get(gm, f_tentacle_timer, &timer) &&
                        timer <= time;
    if (rising(g_edge_tentacle, gm, active) && on(cheats::kAutoQte) && k_cmd_decide) {
      const uintptr_t gim = t_gimmick_input ? re::singleton(t_gimmick_input) : 0;
      const uintptr_t kc = gim ? game::ref(gim, f_gimmick_keycheck) : 0;
      const uintptr_t ui = kc ? game::ref(kc, f_keycheck_ui) : 0;
      call::InHook in_hook;
      if (ui) call::press_command(ui, k_cmd_decide);
    }
  }
  original<GimmickFn>(kTentacle)(ctx, gm);
}

// Mutated Krauser's grab, escaped by a parry: the button his large prompt asks
// for, at its "just" moment when it has one.
void on_grapple_parry(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kGrappleParry);
  if (re::is_a(node, t_grapple_parry)) {
    bool open = false, success = true, just_enabled = false, track_just = false, b_button = false;
    const uintptr_t head = game::ref(node, f_gp_head);
    game::get(node, f_gp_just_enabled, &just_enabled);
    game::get(node, f_gp_track_just, &track_just);
    game::get(node, f_gp_b_button, &b_button);
    const bool active = game::get(node, f_gp_gui_open, &open) && open && game::get(node, f_gp_success, &success) &&
                        !success && (!just_enabled || track_just) && head;
    if (rising(g_edge_grapple, node, active) && on(cheats::kAutoQte)) {
      const uintptr_t command = game::ref(head, f_head_command);
      const uint32_t hash = b_button ? k_cmd_avoid : k_cmd_melee_hold;
      call::InHook in_hook;
      if (command && hash) call::press_command(command, hash);
    }
  }
  original<ActionFn>(kGrappleParry)(ctx, node, arg);
}

// The timed lever (with Ashley): any release succeeds - the gauge's correct
// area made as wide as the gauge; put back as it was when the switch goes off.
void on_shutter(uintptr_t ctx, uintptr_t gauge) {
  count(kShutter);
  if (re::is_a(gauge, t_shutter)) {
    float size = 0.0f, area = 0.0f;
    if (game::get(gauge, f_shutter_gauge, &size) && game::get(gauge, f_shutter_area, &area)) {
      Widened* w = nullptr;
      for (Widened& x : g_widened)
        if (x.obj.load(std::memory_order_relaxed) == gauge) w = &x;
      if (on(cheats::kAutoQte)) {
        if (area < size) {
          if (!w)
            for (Widened& x : g_widened) {
              uintptr_t none = 0;
              if (x.obj.compare_exchange_strong(none, gauge)) {
                x.area = area;
                w = &x;
                break;
              }
            }
          if (w) game::put(gauge, f_shutter_area, size);
        }
      } else if (w) {
        game::put(gauge, f_shutter_area, w->area);
        w->obj.store(0, std::memory_order_relaxed);
      }
    }
  }
  original<GimmickFn>(kShutter)(ctx, gauge);
}

// --- The save files ------------------------------------------------------------------------------------------------------------
// The save and load lists' own frame: one class serves the title's load list,
// the pause menu's, the game over screen's and a typewriter's save list, and
// this runs once a frame while one is up (savefiles.h).
using UpdateFn = void (*)(uintptr_t ctx, uintptr_t self);
void on_save_menu(uintptr_t ctx, uintptr_t screen) {
  count(kSaveMenu);
  original<UpdateFn>(kSaveMenu)(ctx, screen);
  if (!call::exception_pending(ctx)) savefiles::screen_frame(ctx, screen);
}

// --- The Extra Content Shop ------------------------------------------------------------------------------------------------------
// The shop's own frame, once a frame while it is up: the tabs it shows (Separate
// Ways' only with that DLC) for the shop window (exshop.h), and - after the window
// changed an item - the shop's page shown again by its own code, as its purchase
// shows itself from this frame (its hold's callback). The mod writes no field here.
void on_shop_screen(uintptr_t ctx, uintptr_t screen) {
  count(kShopScreen);
  original<UpdateFn>(kShopScreen)(ctx, screen);
  if (call::exception_pending(ctx)) return;
  call::InHook in_hook;
  exshop::screen_frame(screen);
}

// --- Automatic QTEs: a grab's button mashing --------------------------------------------------------------------------------------
// Every enemy's grab the player can shake off checks its mash condition here,
// each frame of the grab: the count the presses have reached so far is put at
// the count the grab needs first, so the game's own check finds it met and the
// player breaks free at once.
using MashFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t player_head);
void on_grab_escape(uintptr_t ctx, uintptr_t cond, uintptr_t head) {
  count(kGrabEscape);
  const uintptr_t knife = t_knife_ready;
  t_knife_ready = 0;
  if (on(cheats::kAutoQte) && re::is_a(cond, t_mash)) {
    bool enabled = false;
    int required = 0;
    float current = 0.0f;
    if (game::get(cond, f_mash_enabled, &enabled) && enabled) {
      if (game::get(cond, f_mash_required, &required) && required > 0 && game::get(cond, f_mash_current, &current) &&
          current < static_cast<float>(required))
        game::put(cond, f_mash_current, static_cast<float>(required));
    } else if (knife && re::is_a(knife, t_knife_cond)) {
      // A grab only the knife breaks (El Gigante, some holds): taken, as the
      // knife's button would - never where mashing is offered, which costs nothing.
      game::put(knife, f_esc_satisfied, true);
      if (config::get().trace) logf("automatic QTEs: a knife-only grab broken with the knife");
    }
  }
  original<MashFn>(kGrabEscape)(ctx, cond, head);
}

// A grab's knife escape: after the game's own update, noted for the mash's hook
// when the player could take it (a knife to hand: the game's own test).
void on_knife_escape(uintptr_t ctx, uintptr_t cond, uintptr_t head) {
  count(kKnifeEscape);
  original<MashFn>(kKnifeEscape)(ctx, cond, head);
  t_knife_ready = 0;
  if (!on(cheats::kAutoQte) || call::exception_pending(ctx) || !head || !re::is_a(cond, t_knife_cond)) return;
  bool enabled = false, satisfied = true;
  if (game::get(cond, f_esc_enabled, &enabled) && enabled && game::get(cond, f_esc_satisfied, &satisfied) && !satisfied) {
    call::InHook in_hook;
    if (call::knife_can_escape(cond, head) == 1) t_knife_ready = cond;
  }
}

// A trigger of the knife's escape button for the player whose head holds `command`.
void knife_trigger(uintptr_t player_head) {
  const uintptr_t command = player_head ? game::ref(player_head, f_head_command) : 0;
  const uintptr_t triggers = command ? game::ref(command, f_cmd_triggers) : 0;
  if (!triggers) return;
  call::InHook in_hook;
  call::trigger_command(triggers, k_cmd_escape_knife);
}

// A Ganado's hold (villagers, zealots, soldiers): the presses it counts made
// enough at once; a hold only the knife breaks gets the knife's button once the
// hold lets it be pressed (the game then checks the knife and takes it in hand).
void on_ganado_grab(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kGanadoGrab);
  if (on(cheats::kAutoQte) && re::is_a(node, t_c0_grab)) {
    bool knife = false, button = false;
    int struggles = 0;
    float delay = 0.0f, counter = 0.0f, input = 0.0f;
    const uintptr_t head = game::ref(node, f_c0_head);
    if (head && game::get(node, f_c0_button, &button) && game::get(node, f_c0_knife, &knife)) {
      if (button && game::get(node, f_c0_struggles, &struggles) && struggles > 0 && game::get(node, f_c0_input, &input) &&
          input < static_cast<float>(struggles))
        game::put(node, f_c0_input, static_cast<float>(struggles));
      else if (!button && knife && game::get(node, f_c0_delay, &delay) && game::get(node, f_c0_counter, &counter) &&
               counter >= delay)
        knife_trigger(head);
    }
  }
  original<ActionFn>(kGanadoGrab)(ctx, node, arg);
}

// A Regenerador's grab: the pushes it counts made enough at once, or the knife's
// button where only the knife breaks it.
void on_regen_grab(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kRegenGrab);
  if (on(cheats::kAutoQte) && re::is_a(node, t_d4_grab)) {
    bool input = false, finished = true, push = false, knife = false;
    uint32_t disabled = 0;
    int needed = 0;
    float pushes = 0.0f;
    const uintptr_t settings = game::ref(node, f_d4_settings);
    if (settings && game::get(node, f_d4_input, &input) && input && game::get(node, f_d4_finished, &finished) &&
        !finished) {
      game::get(node, f_d4_disabled, &disabled);
      game::get(settings, f_gs_push, &push);
      game::get(settings, f_gs_knife, &knife);
      if (push && !(disabled & k_d4_disabled_push) && game::get(settings, f_gs_count, &needed) && needed > 0) {
        if (game::get(node, f_d4_push_count, &pushes) && pushes < static_cast<float>(needed))
          game::put(node, f_d4_push_count, static_cast<float>(needed));
      } else if (knife && !(disabled & k_d4_disabled_knife)) {
        knife_trigger(game::ref(node, f_d4_head));
      }
    }
  }
  original<ActionFn>(kRegenGrab)(ctx, node, arg);
}

// Swimming back to the boat (Del Lago): the strokes it counts made enough at
// once - the game then keeps its own failure timer full and calls it a success.
void on_swim(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kSwim);
  if (on(cheats::kAutoQte) && re::is_a(node, t_swim)) {
    int r0 = -1, r1 = -1, max = 0;
    float done = 0.0f;
    if (game::get(node, f_swim_rno0, &r0) && r0 == 0 && game::get(node, f_swim_rno1, &r1) && r1 == 1 &&
        game::get(node, f_swim_max, &max) && max > 0 && game::get(node, f_swim_count, &done) &&
        done < static_cast<float>(max))
      game::put(node, f_swim_count, static_cast<float>(max));
  }
  original<ActionFn>(kSwim)(ctx, node, arg);
}

// A shot fired (Steady aim): the recoil's copy is made inside this call, before
// the camera's update applies any of it - held at 0 at once, not a frame later.
void on_gun_fire(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kGunFire);
  original<ActionFn>(kGunFire)(ctx, node, arg);
  if (on(cheats::kSteadyAim) && !call::exception_pending(ctx) && re::is_a(node, t_gun_fire))
    game::steady_camera(game::player_context());
}

// The player camera's frame (MainCameraController.lateUpdate -> advancedTime ->
// this slot): what turns the view by its recoil (updateRecoil: _RecoilParam's
// _Yaw and _Pitch) and its hand shake (updateHandShake: the body's Null_Offset
// angle times getRStickOffset's lerp) is held at 0 just before it, and the hand
// shake it hands over (MainCameraController._IsHandShake, which the camera's
// interpolation right after turns the view by and clears) taken back after it -
// every frame, whatever the tick's timing.
std::atomic<bool> g_logged_hand_shake{false}, g_logged_shake_rate{false};
void on_camera_update(uintptr_t ctx, uintptr_t pcc) {
  count(kCameraUpdate);
  const bool steady = on(cheats::kSteadyAim);
  if (steady) game::steady_camera_controller(pcc);
  original<UpdateFn>(kCameraUpdate)(ctx, pcc);
  if (steady && !call::exception_pending(ctx) && game::drop_hand_shake(pcc) && !g_logged_hand_shake.exchange(true))
    logf("steady aim: the camera's hand shake taken out (first time)");
}

// How much of a camera shake the player camera takes: the camera's interpolation
// applies its CameraShakeController's shakes - a shot's own among them
// (PlayerEquipment.execFire requests the weapon's) - at the busy controller's
// rate, 1 while aiming (0.05 in a sight or scope). None while it aims.
using RateFn = float (*)(uintptr_t ctx, uintptr_t self);
float on_shake_rate(uintptr_t ctx, uintptr_t pcc) {
  count(kShakeRate);
  const float rate = original<RateFn>(kShakeRate)(ctx, pcc);
  if (!on(cheats::kSteadyAim) || call::exception_pending(ctx) || !game::camera_aiming(pcc)) return rate;
  if (rate != 0.0f && !g_logged_shake_rate.exchange(true))
    logf("steady aim: no camera shake while aiming (first time; the game's rate was %.2f)", static_cast<double>(rate));
  return 0.0f;
}

// The hold's body shake layer (and the scope's near danger): its blend is the
// camera's getHandShakeRate - the breathing curve times getRStickOffset's lerp,
// which each read moves on - read here before anything else in the frame: the
// lerp held at 0 just before, so the layer is flat.
//   The game sets that blend only for a gun aimed through a sight or a scope
// (PlayerEquipment's virtuals 20-23: a rifle without its scope, the scopes); with
// any other gun the layer - the player's shoot layer, playing the weapon's
// additive breath loop (or its danger loop at low health) over the aiming pose -
// stays at full weight, and the arms and the gun sway in view. While one of the
// two actions runs, the equipment's get_IsEnableOpticalScope (which both ask)
// answers yes: the game sets the blend for every gun, to the flat rate.
thread_local bool t_steady_hold = false;
void on_hold_shake(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kHoldShake);
  const uintptr_t pcc = on(cheats::kSteadyAim) ? game::player_camera(game::player_context()) : 0;
  if (pcc) game::steady_camera_controller(pcc);
  t_steady_hold = pcc != 0;
  original<ActionFn>(kHoldShake)(ctx, node, arg);
  t_steady_hold = false;
}
void on_scope_shake(uintptr_t ctx, uintptr_t node, uintptr_t arg) {
  count(kScopeShake);
  const uintptr_t pcc = on(cheats::kSteadyAim) ? game::player_camera(game::player_context()) : 0;
  if (pcc) game::steady_camera_controller(pcc);
  t_steady_hold = pcc != 0;
  original<ActionFn>(kScopeShake)(ctx, node, arg);
  t_steady_hold = false;
}

// Whether the player's equipment aims through an optical scope: the game's
// answer - but yes while one of the hold's shake actions runs for Steady aim (above).
using BoolFn = bool (*)(uintptr_t ctx, uintptr_t self);
std::atomic<bool> g_logged_sway{false};
bool on_optic_scope(uintptr_t ctx, uintptr_t equipment) {
  count(kOpticScope);
  const bool scope = original<BoolFn>(kOpticScope)(ctx, equipment);
  if (!t_steady_hold || scope || call::exception_pending(ctx)) return scope;
  if (!g_logged_sway.exchange(true))
    logf("steady aim: the arms' sway taken out with a gun aimed without a scope (first time)");
  return true;
}

// --- The shooting gallery ---------------------------------------------------------------------------------------------------------------
// The gallery plays itself. After its own frame, while a game runs, every target
// up that scores (never a sailor) is hit in the head as a shot would hit it: the
// record a head shot leaves on the target - where its score shows, the head, the
// break, last - which the target turns into its break at its next update, the
// game's scoring, popup and counts with it. Every medal up is taken the same way.
// No shot is fired, so every "shots fewer than" and "no miss" bonus condition
// holds; the time bonus is what the fast waves leave.
//   The pools: a wave ends 0.1 s after its last target breaks, and a broken target
// goes back to its kind's pool a second later. The hit that would end a wave is
// held back a moment (at most 1.2 s, never past the target's own time or the
// wave's) while the next wave needs more of a kind than its pool holds yet - else
// that target would never appear.
std::atomic<uintptr_t> g_gallery{0};
std::atomic<DWORD> g_gallery_at{0};
std::atomic<int> g_gallery_score{-1};
std::atomic<DWORD> g_gallery_hold_since{0};

uintptr_t array_ref(const re::Array& a, int i) {
  return i >= 0 && i < a.count && a.elem_size == 8 ? mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8) : 0;
}

void gallery_play(uintptr_t ctrl) {
  // What each kind scores (its best of head and body), and whether the head is that best.
  constexpr int kKinds = 16;
  int score_of[kKinds] = {};
  bool head_best[kKinds] = {};
  re::Array sa;
  const uintptr_t sud = game::ref(ctrl, f_gal_score_ud);
  const uintptr_t sarr = sud ? game::ref(sud, f_gal_target_scores) : 0;
  if (!sarr || !re::read_array(sarr, &sa)) return;
  for (int i = 0; i < sa.count && i < 32; ++i) {
    const uintptr_t ts = array_ref(sa, i);
    int kind = -1, body = 0, head = 0;
    if (!ts || !game::get(ts, f_ts_target, &kind) || kind < 0 || kind >= kKinds) continue;
    game::get(ts, f_ts_body, &body);
    game::get(ts, f_ts_head, &head);
    score_of[kind] = body > head ? body : head;
    head_best[kind] = head >= body;
  }
  auto kind_of = [](uintptr_t anim) {
    int k = -1;
    return anim && game::get(anim, f_anim_target, &k) ? k : -1;
  };
  auto scores = [&](int k) { return k >= 0 && k < kKinds && score_of[k] > 0; };
  // The targets up: those to hit, and those that keep the wave going.
  re::List move;
  const uintptr_t ml = game::ref(ctrl, f_gal_move);
  if (!ml || !re::read_list(ml, &move)) return;
  uintptr_t hit[64];
  bool hit_keeps[64];
  int n_hit = 0, keeping = 0, keeping_hit = 0;
  for (int i = 0; i < move.count && i < 96; ++i) {
    const uintptr_t a = re::list_ref(move, i);
    int routine = -1;
    bool broken = true, shown = false, down = false, ignore = false;
    if (!a || !re::is_a(a, t_anim) || !game::get(a, f_anim_routine, &routine) || routine != k_anim_move) continue;
    if (!game::get(a, f_anim_broken, &broken) || broken) continue;
    game::get(a, f_anim_ignore, &ignore);
    if (!ignore) ++keeping;
    game::get(a, f_anim_display, &shown);
    game::get(a, f_anim_need_down, &down);
    if (shown && !down && scores(kind_of(a)) && n_hit < 64) {
      hit_keeps[n_hit] = !ignore;
      if (!ignore) ++keeping_hit;
      hit[n_hit++] = a;
    }
  }
  // The pool guard, for the hit that would end the wave.
  int held = -1;
  if (keeping > 0 && keeping_hit == keeping) {
    int wave_no = -1;
    float timer = 0.0f;
    re::Array wa, ua;
    const uintptr_t stage = game::ref(ctrl, f_gallery_stage);
    const uintptr_t warr = stage ? game::ref(stage, f_stage_waves) : 0;
    if (warr && re::read_array(warr, &wa) && game::get(ctrl, f_gal_wave, &wave_no) && wave_no >= 0 &&
        wave_no + 1 < wa.count && game::get(ctrl, f_gal_timer, &timer)) {
      const uintptr_t cur = array_ref(wa, wave_no), next = array_ref(wa, wave_no + 1);
      const uintptr_t uarr = next ? game::ref(next, f_wave_units) : 0;
      int need[kKinds] = {}, pooled[kKinds] = {}, returning[kKinds] = {};
      if (uarr && re::read_array(uarr, &ua))
        for (int i = 0; i < ua.count && i < 64; ++i) {
          int k = -1;
          const uintptr_t u = array_ref(ua, i);
          if (u && game::get(u, f_unit_target, &k) && scores(k)) ++need[k];
        }
      re::List stock;
      const uintptr_t sl = game::ref(ctrl, f_gal_stock);
      if (sl && re::read_list(sl, &stock))
        for (int i = 0; i < stock.count && i < 256; ++i) {
          const int k = kind_of(re::list_ref(stock, i));
          if (k >= 0 && k < kKinds) ++pooled[k];
        }
      for (int i = 0; i < move.count && i < 96; ++i) {
        const uintptr_t a = re::list_ref(move, i);
        const int k = kind_of(a);
        int routine = -1;
        if (k >= 0 && k < kKinds && game::get(a, f_anim_routine, &routine) &&
            (routine == k_anim_break || routine == k_anim_down))
          ++returning[k];
      }
      bool short_of = false;
      for (int k = 0; k < kKinds; ++k) short_of = short_of || (need[k] > pooled[k] && returning[k] > 0);
      int last = -1;
      for (int i = 0; i < n_hit; ++i)
        if (hit_keeps[i]) last = i;
      if (short_of && last >= 0) {
        const uintptr_t unit = game::ref(hit[last], f_anim_unit);
        float shown_for = 0.0f, life = -1.0f, wave_time = 0.0f;
        game::get(hit[last], f_anim_timer, &shown_for);
        if (unit) game::get(unit, f_unit_life, &life);
        if (cur) game::get(cur, f_wave_time, &wave_time);
        DWORD since = g_gallery_hold_since.load();
        if (!since) g_gallery_hold_since.store(since = stamp::now());
        if (stamp::within(since, 1200) && (life < 0.0f || shown_for < life - 0.2f) && timer < wave_time - 0.2f) held = last;
      }
    }
  }
  if (held < 0) g_gallery_hold_since.store(0);
  if (n_hit > (held >= 0 ? 1 : 0)) {
    call::InHook in_hook;
    for (int i = 0; i < n_hit; ++i) {
      if (i == held) continue;
      const uintptr_t a = hit[i];
      const int k = kind_of(a);
      float p[3];
      if (call::component_position(a, p)) {
        p[1] += k == k_target_fly ? -1.5f : 1.5f;  // the score shows above the board (below a hanging one)
        if (const uintptr_t at = re::field_addr(a, f_anim_disp_pos)) mem::copy_to(at, p, sizeof(p));
      }
      game::put(a, f_anim_head, head_best[k]);
      game::put(a, f_anim_broken, true);  // last: the target breaks at its next update
    }
  }
  // The medals up (a scoring target's: no sailor carries one).
  re::List medals;
  const uintptr_t mdl = game::ref(ctrl, f_gal_medals);
  if (mdl && re::read_list(mdl, &medals))
    for (int i = 0; i < medals.count && i < 32; ++i) {
      const uintptr_t m = re::list_ref(medals, i);
      int routine = -1;
      bool shown = false, broken = true;
      if (!m || !re::is_a(m, t_medal) || !game::get(m, f_medal_routine, &routine) || routine != k_medal_display) continue;
      if (!game::get(m, f_medal_display, &shown) || !shown || !game::get(m, f_medal_broken, &broken) || broken) continue;
      const uintptr_t owner = game::ref(m, f_medal_anim);
      if (owner && !scores(kind_of(owner))) continue;
      game::put(m, f_medal_need_display, false);
      game::put(m, f_medal_broken, true);
    }
}

void on_gallery(uintptr_t ctx, uintptr_t gallery) {
  count(kGallery);
  original<UpdateFn>(kGallery)(ctx, gallery);
  if (call::exception_pending(ctx) || !re::is_a(gallery, t_gallery)) return;
  g_gallery.store(gallery);
  g_gallery_at.store(stamp::now());
  int routine = -1, score = 0;
  if (game::get(gallery, f_gallery_score, &score)) g_gallery_score.store(score);
  if (!on(cheats::kAutoGallery)) return;
  // (Read after the game's own frame: the results begin inside it.)
  if (!game::get(gallery, f_gallery_routine, &routine) || routine != k_gallery_game) return;
  gallery_play(gallery);
}

// --- God mode: Separate Ways' time limit to save Luis --------------------------------------------------------------------------
// Chapter 3's flanking tower: Luis goes into the flames and Ada has a time limit to
// save him - the scene's level timer, whose own frame runs its timer, shows it as a
// gauge that reads as Luis's health draining, and once it has run out sets the flag
// the game over follows (game.h LevelTimer). With God mode on, the time gone is put
// back to none before each of its frames: the gauge shows full and the time never
// runs out. Its frame does nothing (and its time stands) before its start flag and
// after its stop or end flag: a frame that moved the time on says it is under way.
std::atomic<DWORD> g_luis_time_at{0};  // the last frame its time ran (stamp.h; 0 none)
std::atomic<float> g_luis_time_left{0.0f};
std::atomic<uintptr_t> g_luis_timer_logged{0};

void on_level_timer(uintptr_t ctx, uintptr_t self) {
  count(kLevelTimer);
  game::LevelTimer was;
  const bool known = game::level_timer(self, &was);
  const bool wrote = known && on(cheats::kGodMode) && game::hold_level_timer(self);  // (not once it has run out)
  original<UpdateFn>(kLevelTimer)(ctx, self);
  game::LevelTimer now;
  if (call::exception_pending(ctx) || !known || !game::level_timer(self, &now) || now.done ||
      !(now.elapsed > (wrote ? 0.0f : was.elapsed)))
    return;
  g_luis_time_left.store(now.limit > now.elapsed ? now.limit - now.elapsed : 0.0f, std::memory_order_relaxed);
  g_luis_time_at.store(stamp::now());
  if (wrote && g_luis_timer_logged.exchange(self) != self)
    logf("god mode: the time limit to save Luis held - %.0f s, %.1f gone when it took hold: its gauge stays full and "
         "the time never runs out", now.limit, was.elapsed);
}

// --- installing ---------------------------------------------------------------------------------------------------------------------
uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
unsigned long long rva(uintptr_t a) { return a > exe_base() ? static_cast<unsigned long long>(a - exe_base()) : 0; }

void define(Hook h, const char* name, uint32_t method, uintptr_t replacement, uint32_t cls = 0, bool interfaces = false) {
  State& st = g_hook[h];
  st.name = name;
  st.method = method;
  st.replacement = replacement;
  st.cls = cls;
  st.interfaces = interfaces;
}

void fail(State& st, const char* why) {
  st.failed = true;
  logf("events: %s - not hooked: %s", st.name, why);
}

void hook_entry(Hook h) {
  State& st = g_hook[h];
  if (!st.method) return fail(st, "its method is not in the database");
  const uintptr_t record = re::method_record_at(st.method);
  const uintptr_t code = re::method_code_at(st.method);
  if (!record || !code) return fail(st, "its database entry holds no code");
  // Delegates already made call the replacement, which forwards to `original`:
  // only ever the game's own code.
  if (!mem::section(GetModuleHandleA(nullptr), ".text").contains(code))
    return fail(st, "its database entry holds something other than the game's code (another mod?)");
  st.original = code;  // before the exchange: the game may call it at once
  unsigned long protect = 0;
  const uintptr_t was = re::hook_method(record, st.replacement, &protect);
  if (!was) {
    char why[160];
    std::snprintf(why, sizeof(why), "its database entry %p could not be exchanged (page protection 0x%lX)",
                  reinterpret_cast<void*>(record), protect);
    return fail(st, why);
  }
  if (was != code) st.original = was;
  st.record = record;
  logf("events: %s hooked in its database entry %p (code exe+0x%llX; %d global(s) of the game point at the entry)",
       st.name, reinterpret_cast<void*>(record), rva(was), re::globals_pointing_at(record));
}

// A virtual method: its slot in the class's vtable - and any interface table
// slot holding the same code (a save goes through IGameSaveData) - once the
// class has its REObjectInfo.
void hook_slots(Hook h) {
  State& st = g_hook[h];
  if (st.failed || st.slots > 0) return;
  if (!st.method || !st.cls) return fail(st, "its class or method is not in the database");
  const uintptr_t code = re::method_code_at(st.method);
  if (!code) return fail(st, "its database entry holds no code");
  if (!re::vtable_of(st.cls)) {
    if (!st.waiting_since) {
      st.waiting_since = GetTickCount();
      logf("events: %s - waiting for %s to come up", st.name, re::full_name(st.cls).c_str());
    }
    return;
  }
  uintptr_t found[9];
  int n = 0;
  if (const uintptr_t slot = re::vtable_slot(st.cls, st.method, code)) found[n++] = slot;
  if (st.interfaces) {
    uintptr_t more[8];
    const int m = re::interface_slots(st.cls, code, more, 8);
    for (int i = 0; i < m; ++i) {
      bool dup = false;
      for (int k = 0; k < n; ++k) dup = dup || found[k] == more[i];
      if (!dup && n < 9) found[n++] = more[i];
    }
  }
  if (n == 0) {
    const int vt = re::method_vt_index(st.method);
    const uintptr_t table = re::vtable_of(st.cls);
    if (vt >= 0 && table && mem::read_ptr(table + static_cast<uintptr_t>(vt) * 8) == 0) return;  // not filled in yet
    return fail(st, "no slot of the class's vtable holds the method's code");
  }
  st.original = code;
  for (int i = 0; i < n && st.slots < 8; ++i)
    if (re::swap_slot(found[i], code, st.replacement)) st.slot[st.slots++] = found[i];
  if (st.slots == 0) return fail(st, "its vtable slot could not be exchanged");
  logf("events: %s hooked in %d slot(s) of %s's tables (first %p, code exe+0x%llX; %d other class(es) share the vtable)",
       st.name, st.slots, re::full_name(st.cls).c_str(), reinterpret_cast<void*>(st.slot[0]), rva(code),
       re::classes_sharing_vtable(st.cls));
}

// A family: its classes found once (every class deriving from the base).
void define_family(FamilyId id, const char* name, uint32_t base, uint32_t method_index, uintptr_t replacement) {
  Family& f = g_family[id];
  f.name = name;
  f.base = base;
  f.method = method_index;
  f.replacement = replacement;
  f.vt = method_index ? re::method_vt_index(method_index) : -1;
  if (!base || !method_index || f.vt < 0) {
    f.failed = true;
    logf("events: %s - not hooked: its class or virtual method is not in the database", name);
    return;
  }
  f.fallback = re::method_code_at(method_index);
  for (uint32_t t = 1; t < re::num_types(); ++t)
    if (re::derives(t, base)) f.waiting.push_back(t);
  logf("events: %s - %u class(es) of the family, hooked as each comes up", name, static_cast<unsigned>(f.waiting.size()));
}

// Each class of a family whose table is up: its slot exchanged (the code it held kept first).
void hook_family(Family& f) {
  if (f.failed) return;
  const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
  for (size_t i = 0; i < f.waiting.size();) {
    const uint32_t cls = f.waiting[i];
    const uintptr_t table = re::vtable_of(cls);
    if (!table) {
      ++i;
      continue;
    }
    const uintptr_t at = table + static_cast<uintptr_t>(f.vt) * 8;
    const uintptr_t code = mem::read_ptr(at);
    bool shared = false;
    const int n = f.n.load(std::memory_order_acquire);
    for (int k = 0; k < n; ++k) shared = shared || f.slot[k].table == table;
    if (shared || code == f.replacement) {
      // (another class of the family shares this table: hooked already)
    } else if (!text.contains(code)) {
      logf("events: %s - %s's slot holds something other than the game's code (another tool?) - left alone",
           f.name, re::full_name(cls).c_str());
    } else if (n < 64) {
      FamilySlot& sl = f.slot[n];
      sl.table = table;
      sl.at = at;
      sl.original = code;
      f.n.store(n + 1, std::memory_order_release);  // the original is known before the slot calls the mod
      if (re::swap_slot(at, code, f.replacement)) {
        logf("events: %s hooked in %s's table (slot %p, code exe+0x%llX)", f.name, re::full_name(cls).c_str(),
             reinterpret_cast<void*>(at), rva(code));
      } else {
        f.n.store(n, std::memory_order_release);
      }
    }
    f.waiting.erase(f.waiting.begin() + static_cast<std::ptrdiff_t>(i));
  }
  if (!f.first_logged && f.calls.load() > 0) {
    f.first_logged = true;
    logf("events: %s called by the game (first time)", f.name);
  }
}

uint32_t method(uint32_t t, const char* name, std::initializer_list<const char*> params) {
  return t ? re::find_method(t, name, params) : 0;
}

}  // namespace

void install() {
  t_body_driver = re::find_type("chainsaw.CharacterBodyHitDriver");
  t_enemy_driver = re::find_type("chainsaw.EnemyBodyHitDriver");
  t_partner_driver = re::find_type("chainsaw.PartnerBodyHitDriver");
  f_driver_ctx = re::find_field(re::find_type("chainsaw.CharacterDriver"), "<Context>k__BackingField");
  f_dv_damage = re::find_field(re::find_type("chainsaw.HitController.DamageValue"), "<Damage>k__BackingField");
  t_parasite = re::find_type("chainsaw.Ch1c0ParasiteUpdaterCommon");
  f_par_owner_head = re::find_field(t_parasite, "_OwnerHeadUpdater");
  f_head_context = re::find_field(re::find_type("chainsaw.HeadUpdater"), "<Context>k__BackingField");
  f_calc_attack_id = re::find_field(re::find_type("chainsaw.HitController.CalculateInfo"), "<AttackID>k__BackingField");
  if (!f_calc_attack_id.valid()) logf("events: HitController.CalculateInfo.<AttackID> was not found - enemies are not killed as they appear");
  t_cs_controller = re::find_type("chainsaw.CsInventoryController");
  t_cs_inventory = re::find_type("chainsaw.gui.inventory.CsInventory");
  t_cs_item = re::find_type("chainsaw.CsInventoryItem");
  t_item = re::find_type("chainsaw.Item");
  t_weapon_item = re::find_type("chainsaw.WeaponItem");
  f_ctl_ctx = re::find_field(re::find_type("chainsaw.InventoryControllerBase"), "<_CharacterContext>k__BackingField");
  f_ctl_inventory = re::find_field(t_cs_controller, "<_CsInventory>k__BackingField");
  f_inv_items = re::find_field(t_cs_inventory, "_InventoryItems");
  f_inv_equips = re::find_field(t_cs_inventory, "_EquipInfos");
  f_equip_id = re::find_field(re::find_type("chainsaw.InventoryEquipInfo"), "<ID>k__BackingField");
  f_csi_item = re::find_field(re::find_type("chainsaw.InventoryItemBase"), "<Item>k__BackingField");
  f_csi_force = re::find_field(t_cs_item, "<IsForceGet>k__BackingField");
  f_item_guid = re::find_field(t_item, "_ID");
  f_item_count = re::find_field(t_item, "_CurrentItemCount");
  f_wi_ammo = re::find_field(t_weapon_item, "_CurrentAmmoCount");
  f_wi_tactical = re::find_field(t_weapon_item, "_CurrentTacticalAmmoCount");
  t_occurrence_observer = re::find_type("chainsaw.ChapterStatsGameOccurrenceObserver");
  t_save_menu = re::find_type("chainsaw.SaveLoadMenuGuiBehavior");
  t_mash = re::find_type("chainsaw.EnemyBehaviorTreeAction_MFSM_CheckEscapeGrapple.ButtonMashingEscapeCondition");
  f_mash_enabled = re::find_field(t_mash, "_IsEnabled");
  f_mash_required = re::find_field(t_mash, "<RequiredCount>k__BackingField");
  f_mash_current = re::find_field(t_mash, "<CurrentCount>k__BackingField");
  t_gallery = re::find_type("chainsaw.ShootingGallery2Controller");
  f_gallery_score = re::find_field(t_gallery, "_Score");
  f_gallery_routine = re::find_field(t_gallery, "_Routine");
  f_gallery_stage = re::find_field(t_gallery, "_Stage");
  {
    const uint32_t rt = re::find_type("chainsaw.ShootingGallery2Controller.RoutineType");
    k_gallery_game = rt ? re::enum_value(rt, "Game", 4) : 4;
  }
  f_gal_score_ud = re::find_field(t_gallery, "UserdataScore");
  f_gal_target_scores = re::find_field(re::find_type("chainsaw.ShootingGallery2ScoreUserdata"), "TargetScores");
  f_gal_move = re::find_field(t_gallery, "_TargetMove");
  f_gal_stock = re::find_field(t_gallery, "_TargetStock");
  f_gal_medals = re::find_field(t_gallery, "_MedalMove");
  f_gal_wave = re::find_field(t_gallery, "_CntWave");
  f_gal_timer = re::find_field(t_gallery, "_Timer");
  {
    const uint32_t ts = re::find_type("chainsaw.ShootingGallery2ScoreUserdata.TargetScore");
    f_ts_target = re::find_field(ts, "Target");
    f_ts_body = re::find_field(ts, "ScoreBody");
    f_ts_head = re::find_field(ts, "ScoreHead");
    f_stage_waves = re::find_field(re::find_type("chainsaw.ShootingGallery2StageUserdata"), "Waves");
    const uint32_t wave = re::find_type("chainsaw.ShootingGallery2StageUserdata.Wave");
    f_wave_time = re::find_field(wave, "Time");
    f_wave_units = re::find_field(wave, "Units");
    const uint32_t unit = re::find_type("chainsaw.ShootingGallery2Unit");
    f_unit_target = re::find_field(unit, "Target");
    f_unit_life = re::find_field(unit, "TimeLife");
  }
  t_anim = re::find_type("chainsaw.ShootingGallery2Animator");
  f_anim_target = re::find_field(t_anim, "_Target");
  f_anim_ignore = re::find_field(t_anim, "_IgnoreWave");
  f_anim_unit = re::find_field(t_anim, "_Unit");
  f_anim_routine = re::find_field(t_anim, "_Routine");
  f_anim_timer = re::find_field(t_anim, "<RnoTimer>k__BackingField");
  f_anim_need_down = re::find_field(t_anim, "<NeedDown>k__BackingField");
  f_anim_head = re::find_field(t_anim, "<HitHead>k__BackingField");
  f_anim_broken = re::find_field(t_anim, "<IsBroken>k__BackingField");
  f_anim_display = re::find_field(t_anim, "<IsDisplay>k__BackingField");
  f_anim_disp_pos = re::find_field(t_anim, "<DispPos>k__BackingField");
  t_medal = re::find_type("chainsaw.ShootingGallery2Medal");
  f_medal_routine = re::find_field(t_medal, "_Routine");
  f_medal_need_display = re::find_field(t_medal, "<NeedDisplay>k__BackingField");
  f_medal_broken = re::find_field(t_medal, "<IsBroken>k__BackingField");
  f_medal_display = re::find_field(t_medal, "<IsDisplay>k__BackingField");
  f_medal_anim = re::find_field(t_medal, "_Anim");
  if (const uint32_t e = re::find_type("chainsaw.ShootingGallery2Animator.RoutineType")) {
    k_anim_move = re::enum_value(e, "Move", 1);
    k_anim_break = re::enum_value(e, "Break", 2);
    k_anim_down = re::enum_value(e, "Down", 3);
  }
  if (const uint32_t e = re::find_type("chainsaw.ShootingGallery2Medal.RoutineType"))
    k_medal_display = re::enum_value(e, "Display", 1);
  if (const uint32_t e = re::find_type("chainsaw.ShootingGallery2Target")) k_target_fly = re::enum_value(e, "Fly", 4);
  t_shop_root = re::find_type("chainsaw.gui.shop.InGameShopRootGui");
  t_shopmgr = re::find_type("chainsaw.InGameShopManager");
  i_shopmgr = re::find_field(t_shopmgr, "_Instance");
  f_shop_flow = re::find_field(t_shopmgr, "<FlowController>k__BackingField");
  // (_StateController is typed as the states' base class; the hub is the subclass's.)
  f_flow_states = re::find_field(re::find_type("chainsaw.InGameShopFlowController"), "_StateController");
  f_states_hub = re::find_field(re::find_type("chainsaw.InGameShopStateController"), "<InGameShopGuiParamHub>k__BackingField");
  if (f_states_hub.valid()) {
    f_hub_case = re::find_field(f_states_hub.type, "<CasePurchaseParamHolder>k__BackingField");
    if (f_hub_case.valid()) f_case_price = re::find_field(f_hub_case.type, "<PurchasePrice>k__BackingField");
  }
  if (!(f_shop_flow.valid() && f_flow_states.valid() && f_states_hub.valid() && f_hub_case.valid() && f_case_price.valid()))
    logf("events: the merchant's case purchase price was not found - with Infinite money, cancelling the placement of "
         "a case bought refunds its price");
  f_states_curr = re::find_field(re::find_type("chainsaw.InGameShopStateController"), "_CurrState");
  t_case_reward_enter = re::find_type("chainsaw.gui.shop.InGameShopGuiState_CaseRewardEnter");
  t_case_reward_exit = re::find_type("chainsaw.gui.shop.InGameShopGuiState_CaseRewardExit");
  if (!(f_shop_flow.valid() && f_flow_states.valid() && f_states_curr.valid() && t_case_reward_enter && t_case_reward_exit))
    logf("events: the merchant's screen's states were not found - with Infinite spinels, cancelling the placement of "
         "an item traded refunds its spinels on top");
  t_campaign = re::find_type("chainsaw.CampaignManager");
  // Automatic QTEs.
  t_action_sign = re::find_type("chainsaw.PlayerHeadActionSign");
  t_player_head = re::find_type("chainsaw.PlayerHeadUpdater");
  {
    const uint32_t hit = re::find_type("chainsaw.ActionSignHitInfo");
    f_hit_sign = re::find_field(hit, "SignType");
    f_hit_button = re::find_field(hit, "ButtonType");
    f_hit_act = re::find_field(hit, "ActTiming");
    f_hit_just = re::find_field(hit, "JustTiming");
    f_head_sign_driver = re::find_field(t_player_head, "<ActionSignDriver>k__BackingField");
    f_head_command = re::find_field(re::find_type("chainsaw.CharacterHeadUpdater"), "<Command>k__BackingField");
    f_sign_avoid = re::find_field(t_action_sign, "<AvoidInfo>k__BackingField");
    f_sign_parry = re::find_field(t_action_sign, "<ParryInfo>k__BackingField");
    f_sign_gui = re::find_field(t_action_sign, "<GuiUnit>k__BackingField");
    f_cant_parry = re::find_field(t_action_sign, "_CantParry");
    const uint32_t info = re::find_type("chainsaw.PlayerHeadActionSign.Info");
    f_info_accepting = re::find_field(info, "<Accepting>k__BackingField");
    f_info_reserve = re::find_field(info, "<RequestReserve>k__BackingField");
    f_info_request_enable = re::find_field(info, "<RequestEnable>k__BackingField");
    if (const uint32_t e = re::find_type("chainsaw.ActionSignButton")) k_button_avoid = re::enum_value(e, "Avoid", 1);
    if (const uint32_t e = re::find_type("chainsaw.ActionSignType")) {
      k_sign_parry_max = re::enum_value(e, "ParrySignTypeMax", 5);
      k_sign_krauser_normal = re::enum_value(e, "Ch1b7z0NormalParry", 1);
      k_sign_krauser_special = re::enum_value(e, "Ch1b7z0SpecialParry", 2);
    }
    if (const uint32_t e = re::find_type("chainsaw.PlayerCommandType")) {
      k_cmd_avoid = static_cast<uint32_t>(re::enum_value(e, "Avoid", 0));
      k_cmd_melee_hold = static_cast<uint32_t>(re::enum_value(e, "MeleeHold", 0));
    }
    if (const uint32_t e = re::find_type("chainsaw.GuiCommandType")) k_cmd_decide = static_cast<uint32_t>(re::enum_value(e, "Decide", 0));
  }
  t_laser = re::find_type("chainsaw.GmQTELaserTrap");
  f_laser_routine = re::find_field(t_laser, "_CurrRoutine");
  f_laser_option = re::find_field(t_laser, "_Option");
  f_laser_head = re::find_field(t_laser, "_UserHead");
  f_laser_command = re::find_field(re::find_type("chainsaw.GmOptionQTELaserTrap"), "_AvoidCommand");
  if (const uint32_t e = re::find_type("chainsaw.GmQTELaserTrap.RoutineType")) {
    k_laser_approach = re::enum_value(e, "Approach", 3);
    k_laser_end = re::enum_value(e, "End", 4);
  }
  t_laser3 = re::find_type("chainsaw.GmQTELaserTrap3");
  f_laser3_start = re::find_field(t_laser3, "_IsStartActionSign");
  f_laser3_option = re::find_field(t_laser3, "_Option3");
  f_laser3_command = re::find_field(re::find_type("chainsaw.GmOptionQTELaserTrap3"), "_AvoidCommand2nd");
  t_hookshot = re::find_type("chainsaw.GmHookShot");
  f_hook_id = re::find_field(t_hookshot, "_HookShotId");
  f_hook_can_use = re::find_field(t_hookshot, "_CanUseForCh4fez0");
  f_hook_command = re::find_field(t_hookshot, "_PlayerCommand");
  f_hook_disabled = re::find_field(t_hookshot, "_DisableCh4fez0Button");
  if (const uint32_t e = re::find_type("chainsaw.GmHookShot.eHookShotId")) k_hook_ch4fez0 = re::enum_value(e, "Ch5_Ch4fez0", 5);
  t_tentacle = re::find_type("chainsaw.GmTentacleAvoid");
  f_tentacle_time = re::find_field(t_tentacle, "_AvoidTime");
  f_tentacle_timer = re::find_field(t_tentacle, "_AvoidTimer");
  f_tentacle_rno = re::find_field(t_tentacle, "Rno1");
  f_tentacle_sensor = re::find_field(t_tentacle, "_AvoidSensor");
  t_gimmick_input = re::find_type("chainsaw.GimmickInputManager");
  f_gimmick_keycheck = re::find_field(t_gimmick_input, "_KeyCheck");
  f_keycheck_ui = re::find_field(re::find_type("chainsaw.GimmickInputManager.KeyCheck"), "_CommandUI");
  t_grapple_parry = re::find_type("chainsaw.Ch1f7z0BehaviorTreeAction_CheckGrappleParryEscape");
  f_gp_b_button = re::find_field(t_grapple_parry, "EnableBButton");
  f_gp_just_enabled = re::find_field(t_grapple_parry, "EnableJastParry");
  f_gp_head = re::find_field(t_grapple_parry, "_PlHeadUpdater");
  f_gp_gui_open = re::find_field(t_grapple_parry, "IsOpenedLargeGui");
  f_gp_success = re::find_field(t_grapple_parry, "_NextSuccess");
  f_gp_track_just = re::find_field(t_grapple_parry, "_trackJustParry");
  t_knife_cond = re::find_type("chainsaw.EnemyBehaviorTreeAction_MFSM_CheckEscapeGrapple.KnifeEscapeCondition");
  {
    const uint32_t base = re::find_type("chainsaw.EnemyBehaviorTreeAction_MFSM_CheckEscapeGrapple.EscapeConditionBase");
    f_esc_enabled = re::find_field(base, "_IsEnabled");
    f_esc_satisfied = re::find_field(base, "<IsSatisfied>k__BackingField");
  }
  f_cmd_triggers = re::find_field(re::find_type("share.hid.Command"), "_Triggers");
  if (const uint32_t e = re::find_type("chainsaw.PlayerCommandType"))
    k_cmd_escape_knife = static_cast<uint32_t>(re::enum_value(e, "Escape_Knife", static_cast<int>(k_cmd_escape_knife)));
  t_c0_grab = re::find_type("chainsaw.Ch1c0z0BehaviorTreeAction_CheckEscapeGrapple");
  f_c0_knife = re::find_field(t_c0_grab, "isEnableKnifeEscape");
  f_c0_button = re::find_field(t_c0_grab, "isEnableButtonEscape");
  f_c0_struggles = re::find_field(t_c0_grab, "NumberOfStruggles");
  f_c0_delay = re::find_field(t_c0_grab, "DelaySecond");
  f_c0_head = re::find_field(t_c0_grab, "_PlHeadUpdater");
  f_c0_input = re::find_field(t_c0_grab, "inputCount");
  f_c0_counter = re::find_field(t_c0_grab, "delaySecCounter");
  t_d4_grab = re::find_type("chainsaw.Ch1d4z0BehaviorTreeAction_CheckEscapeGrapple");
  f_d4_head = re::find_field(t_d4_grab, "<PlayerHeadUpdater>k__BackingField");
  f_d4_settings = re::find_field(t_d4_grab, "<Settings>k__BackingField");
  f_d4_push_count = re::find_field(t_d4_grab, "<PushCount>k__BackingField");
  f_d4_input = re::find_field(t_d4_grab, "<EnabledInput>k__BackingField");
  f_d4_finished = re::find_field(t_d4_grab, "<Finished>k__BackingField");
  f_d4_disabled = re::find_field(t_d4_grab, "Disabled");
  {
    const uint32_t gs = re::find_type("chainsaw.character.ch1d4z0.GrappleSettings");
    f_gs_knife = re::find_field(gs, "_EnabledKnife");
    f_gs_push = re::find_field(gs, "_EnabledPush");
    f_gs_count = re::find_field(gs, "_PushCount");
    if (const uint32_t e = re::find_type("chainsaw.Ch1d4z0BehaviorTreeAction_CheckEscapeGrapple.DisabledBit")) {
      k_d4_disabled_knife = static_cast<uint32_t>(re::enum_value(e, "Knife", 1));
      k_d4_disabled_push = static_cast<uint32_t>(re::enum_value(e, "Push", 2));
    }
  }
  t_swim = re::find_type("chainsaw.Ch1f1z0BehaviorTreeAction_MFSM_SearchEvent_Start");
  f_swim_max = re::find_field(t_swim, "_CountMax");
  f_swim_rno0 = re::find_field(t_swim, "_Rno0");
  f_swim_rno1 = re::find_field(t_swim, "_Rno1");
  f_swim_count = re::find_field(t_swim, "_Count");
  t_gun_fire = re::find_type("chainsaw.PlayerBehaviorTreeAction_MFSM_GunFire");
  t_shutter = re::find_type("chainsaw.GmTimingShutterGauge");
  f_shutter_gauge = re::find_field(t_shutter, "_GaugeSize");
  f_shutter_area = re::find_field(t_shutter, "_CorrectAreaSize");

  define(kHitCalc, "CharacterBodyHitDriver.onCalculateDamage (One hit kills, Ashley takes no damage)",
         f_driver_ctx.valid() && f_dv_damage.valid()
             ? method(t_body_driver, "onCalculateDamage",
                      {"chainsaw.HitController.DamageValue", "chainsaw.HitController.CalculateInfo"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_hit_calc));
  define(kParasiteCalc, "Ch1c0ParasiteUpdaterCommon.onCalculateDamage (One hit kills: a Ganado's parasite)",
         f_dv_damage.valid() && f_par_owner_head.valid() && f_head_context.valid()
             ? method(t_parasite, "onCalculateDamage",
                      {"chainsaw.HitController.DamageValue", "chainsaw.HitController.CalculateInfo"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_parasite_calc));
  define(kShopMoney, "InGameShopRootGui.onChangedPtas (Infinite money)",
         method(t_shop_root, "onChangedPtas", {"System.Int32", "System.Int32"}), reinterpret_cast<uintptr_t>(&on_shop_money));
  define(kShopSpinels, "InGameShopRootGui.onSpinelCountChanged (Infinite spinels)",
         game::parts().spinels ? method(t_shop_root, "onSpinelCountChanged", {}) : 0,
         reinterpret_cast<uintptr_t>(&on_shop_spinels));
  define(kUpgradeHold, "InGameShopGuiState_CustomCommandDefault.onHoldGaugeCompleted (Infinite upgrade tickets)",
         game::parts().tickets
             ? method(re::find_type("chainsaw.gui.shop.InGameShopGuiState_CustomCommandDefault"), "onHoldGaugeCompleted", {})
             : 0,
         reinterpret_cast<uintptr_t>(&on_upgrade_hold));
  define(kArmorWear, "UniqueInventoryController.onDurabilityChanged (No armor durability loss)",
         game::parts().armor && f_ctl_ctx.valid()
             ? method(re::find_type("chainsaw.UniqueInventoryController"), "onDurabilityChanged",
                      {"chainsaw.ItemID", "System.Int32", "System.Int32"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_armor_durability));
  define(kUseWeapon, "CsInventoryController.useWeapon (Infinite ammo)",
         f_inv_equips.valid() && f_equip_id.valid() ? method(t_cs_controller, "useWeapon", {"chainsaw.EquipType"}) : 0,
         reinterpret_cast<uintptr_t>(&on_use_weapon), t_cs_controller);
  define(kUseWeaponCost, "CsInventoryController.useWeapon with a cost (No durability loss)",
         method(t_cs_controller, "useWeapon", {"chainsaw.EquipType", "System.Int32", "System.Int32"}),
         reinterpret_cast<uintptr_t>(&on_use_weapon_cost), t_cs_controller);
  define(kCampaignSave, "CampaignManager.saveGameSaveData (Save without counting)", method(t_campaign, "saveGameSaveData", {}),
         reinterpret_cast<uintptr_t>(&on_campaign_save), t_campaign, true);
  define(kOccurrence, "ChapterStatsGameOccurrenceObserver.catchGameOccurrence (Misses/Deaths/Damage don't count)",
         method(t_occurrence_observer, "catchGameOccurrence",
                {"chainsaw.GameOccurrenceEventTrigger", "chainsaw.GameOccurrenceObserverSpecifier",
                 "chainsaw.IGameOccurrenceIdentifier", "chainsaw.IGameOccurrenceIdentifier", "System.Int32"}),
         reinterpret_cast<uintptr_t>(&on_occurrence), t_occurrence_observer, true);
  define(kSaveMenu, "SaveLoadMenuGuiBehavior.lateUpdateOnActive (the save files)",
         method(t_save_menu, "lateUpdateOnActive", {}), reinterpret_cast<uintptr_t>(&on_save_menu), t_save_menu);
  define(kGrabEscape, "ButtonMashingEscapeCondition.update (Automatic QTEs: grabs)",
         f_mash_required.valid() && f_mash_current.valid() ? method(t_mash, "update", {"chainsaw.PlayerHeadUpdater"}) : 0,
         reinterpret_cast<uintptr_t>(&on_grab_escape), t_mash);
  const bool gallery_names =
      f_gallery_score.valid() && f_gallery_routine.valid() && f_gallery_stage.valid() && f_gal_score_ud.valid() &&
      f_gal_target_scores.valid() && f_gal_move.valid() && f_gal_stock.valid() && f_gal_medals.valid() &&
      f_gal_wave.valid() && f_gal_timer.valid() && f_ts_target.valid() && f_ts_body.valid() && f_ts_head.valid() &&
      f_stage_waves.valid() && f_wave_time.valid() && f_wave_units.valid() && f_unit_target.valid() &&
      f_unit_life.valid() && f_anim_target.valid() && f_anim_ignore.valid() && f_anim_unit.valid() &&
      f_anim_routine.valid() && f_anim_timer.valid() && f_anim_need_down.valid() && f_anim_head.valid() &&
      f_anim_broken.valid() && f_anim_display.valid() && f_anim_disp_pos.valid() && f_medal_routine.valid() &&
      f_medal_need_display.valid() && f_medal_broken.valid() && f_medal_display.valid() && f_medal_anim.valid();
  if (!gallery_names) logf("events: the shooting gallery's targets were not all found - it does not play itself");
  define(kGallery, "ShootingGallery2Controller.update (the shooting gallery plays itself)",
         gallery_names ? method(t_gallery, "update", {}) : 0, reinterpret_cast<uintptr_t>(&on_gallery), t_gallery);
  const bool sign_names = f_hit_sign.valid() && f_hit_button.valid() && f_hit_act.valid() && f_hit_just.valid() &&
                          f_head_sign_driver.valid() && f_sign_avoid.valid() && f_sign_parry.valid() && f_sign_gui.valid() &&
                          f_cant_parry.valid() && f_info_accepting.valid() && f_info_reserve.valid() &&
                          f_info_request_enable.valid();
  const bool command = f_head_command.valid();
  define(kLaser, "GmQTELaserTrap.onOverlappingLaserCollider (Automatic QTEs: the laser corridor)",
         command && f_laser_routine.valid() && f_laser_option.valid() && f_laser_head.valid() && f_laser_command.valid()
             ? method(t_laser, "onOverlappingLaserCollider", {"via.physics.CollisionInfo"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_laser));
  define(kLaser3, "GmQTELaserTrap3.updateGimmick (Automatic QTEs: the second laser)",
         command && f_laser3_start.valid() && f_laser3_option.valid() && f_laser3_command.valid()
             ? method(t_laser3, "updateGimmick", {})
             : 0,
         reinterpret_cast<uintptr_t>(&on_laser3), t_laser3);
  define(kHookShot, "GmHookShot.updateGimmick (Automatic QTEs: Ada's hookshot prompt)",
         f_hook_id.valid() && f_hook_can_use.valid() && f_hook_command.valid() && f_hook_disabled.valid()
             ? method(t_hookshot, "updateGimmick", {})
             : 0,
         reinterpret_cast<uintptr_t>(&on_hookshot), t_hookshot);
  define(kTentacle, "GmTentacleAvoid.updateGimmick (Automatic QTEs: the tentacle)",
         f_tentacle_time.valid() && f_tentacle_timer.valid() && f_tentacle_rno.valid() && f_tentacle_sensor.valid() &&
                 f_gimmick_keycheck.valid() && f_keycheck_ui.valid()
             ? method(t_tentacle, "updateGimmick", {})
             : 0,
         reinterpret_cast<uintptr_t>(&on_tentacle), t_tentacle);
  define(kGrappleParry, "Ch1f7z0 CheckGrappleParryEscape.update (Automatic QTEs: mutated Krauser's grab)",
         command && f_gp_b_button.valid() && f_gp_just_enabled.valid() && f_gp_head.valid() && f_gp_gui_open.valid() &&
                 f_gp_success.valid() && f_gp_track_just.valid()
             ? method(t_grapple_parry, "update", {"via.behaviortree.ActionArg"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_grapple_parry), t_grapple_parry);
  define(kShutter, "GmTimingShutterGauge.updateGimmick (Automatic QTEs: the timed lever)",
         f_shutter_gauge.valid() && f_shutter_area.valid() ? method(t_shutter, "updateGimmick", {}) : 0,
         reinterpret_cast<uintptr_t>(&on_shutter), t_shutter);
  define(kKnifeEscape, "KnifeEscapeCondition.update (Automatic QTEs: knife-only grabs)",
         f_esc_enabled.valid() && f_esc_satisfied.valid() && f_mash_enabled.valid()
             ? method(t_knife_cond, "update", {"chainsaw.PlayerHeadUpdater"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_knife_escape), t_knife_cond);
  define(kGanadoGrab, "Ch1c0z0 CheckEscapeGrapple.update (Automatic QTEs: Ganados' holds)",
         f_c0_knife.valid() && f_c0_button.valid() && f_c0_struggles.valid() && f_c0_delay.valid() && f_c0_head.valid() &&
                 f_c0_input.valid() && f_c0_counter.valid() && f_cmd_triggers.valid() && command
             ? method(t_c0_grab, "update", {"via.behaviortree.ActionArg"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_ganado_grab), t_c0_grab);
  define(kRegenGrab, "Ch1d4z0 CheckEscapeGrapple.update (Automatic QTEs: Regeneradors' grabs)",
         f_d4_head.valid() && f_d4_settings.valid() && f_d4_push_count.valid() && f_d4_input.valid() &&
                 f_d4_finished.valid() && f_d4_disabled.valid() && f_gs_knife.valid() && f_gs_push.valid() &&
                 f_gs_count.valid() && f_cmd_triggers.valid() && command
             ? method(t_d4_grab, "update", {"via.behaviortree.ActionArg"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_regen_grab), t_d4_grab);
  define(kSwim, "Ch1f1z0 SearchEvent_Start.update (Automatic QTEs: swimming back to the boat)",
         f_swim_max.valid() && f_swim_rno0.valid() && f_swim_rno1.valid() && f_swim_count.valid()
             ? method(t_swim, "update", {"via.behaviortree.ActionArg"})
             : 0,
         reinterpret_cast<uintptr_t>(&on_swim), t_swim);
  define(kGunFire, "PlayerBehaviorTreeAction_MFSM_GunFire.onStart (Steady aim: no recoil)",
         method(t_gun_fire, "onStart", {"via.behaviortree.ActionArg"}), reinterpret_cast<uintptr_t>(&on_gun_fire),
         t_gun_fire);
  {
    const bool camera = game::parts().aim_camera;
    const uint32_t pcc = re::find_type("chainsaw.PlayerCameraController");
    define(kCameraUpdate, "PlayerCameraController.onCameraUpdate (Steady aim: no recoil, no hand shake)",
           camera ? method(pcc, "onCameraUpdate", {}) : 0, reinterpret_cast<uintptr_t>(&on_camera_update), pcc);
    define(kShakeRate, "PlayerCameraController.get_CameraShakeApplyRate (Steady aim: no camera shake while aiming)",
           camera ? method(pcc, "get_CameraShakeApplyRate", {}) : 0, reinterpret_cast<uintptr_t>(&on_shake_rate), pcc);
    const uint32_t hold = re::find_type("chainsaw.PlayerBehaviorTreeAction_MFSM_HoldShake");
    define(kHoldShake, "PlayerBehaviorTreeAction_MFSM_HoldShake.onUpdate (Steady aim: no body shake)",
           camera ? method(hold, "onUpdate", {"via.behaviortree.ActionArg"}) : 0,
           reinterpret_cast<uintptr_t>(&on_hold_shake), hold);
    const uint32_t scope = re::find_type("chainsaw.PlayerBehaviorTreeAction_MFSM_ScopeDangerHoldShake");
    define(kScopeShake, "PlayerBehaviorTreeAction_MFSM_ScopeDangerHoldShake.onUpdate (Steady aim: no body shake)",
           camera ? method(scope, "onUpdate", {"via.behaviortree.ActionArg"}) : 0,
           reinterpret_cast<uintptr_t>(&on_scope_shake), scope);
    const uint32_t equipment = re::find_type("chainsaw.PlayerEquipment");
    define(kOpticScope, "PlayerEquipment.get_IsEnableOpticalScope (Steady aim: no body shake with any gun)",
           camera ? method(equipment, "get_IsEnableOpticalScope", {}) : 0,
           reinterpret_cast<uintptr_t>(&on_optic_scope), equipment);
  }
  t_armoury = re::find_type("chainsaw.ArmouryManager");
  define(kArmourySave, "ArmouryManager.saveGameSaveData (the item box in the saves)",
         game::parts().storage ? method(t_armoury, "saveGameSaveData", {}) : 0,
         reinterpret_cast<uintptr_t>(&on_armoury_save), t_armoury, true);
  define(kArmouryLoad, "ArmouryManager.loadGameSaveData (the item box in the saves)",
         game::parts().storage ? method(t_armoury, "loadGameSaveData", {"share.SaveDataBase"}) : 0,
         reinterpret_cast<uintptr_t>(&on_armoury_load), t_armoury, true);
  {
    // The spawn points' conditions: ConditionArea and ConditionFlag inherit their check
    // (from the generic bases, whose code it is), ConditionFlagMer has its own.
    const bool kill = game::parts().auto_kill;
    const char* const kCond = "<chainsaw.CharacterSpawnConditionMercenaries.ConditionType>";
    t_spawn_condition[0] = re::find_type("chainsaw.CharacterSpawnConditionMercenaries.ConditionArea");
    t_spawn_condition[1] = re::find_type("chainsaw.CharacterSpawnConditionMercenaries.ConditionFlagMer");
    t_spawn_condition[2] = re::find_type("chainsaw.CharacterSpawnConditionMercenaries.ConditionFlag");
    const uint32_t area_base = re::find_type((std::string("chainsaw.CustomConditionBase`1.ConditionAreaBase") + kCond).c_str());
    const uint32_t flag_base = re::find_type((std::string("chainsaw.CustomConditionBase`1.ConditionFlagBase") + kCond).c_str());
    define(kSpawnArea, "CharacterSpawnConditionMercenaries.ConditionArea.check (Kill enemies as they appear: every area sends)",
           kill ? method(area_base, "check", {}) : 0, reinterpret_cast<uintptr_t>(&on_spawn_area), t_spawn_condition[0]);
    define(kSpawnFlagMer, "CharacterSpawnConditionMercenaries.ConditionFlagMer.check (Kill enemies as they appear: every area sends)",
           kill ? method(t_spawn_condition[1], "check", {}) : 0, reinterpret_cast<uintptr_t>(&on_spawn_flag_mer),
           t_spawn_condition[1]);
    define(kSpawnFlag, "CharacterSpawnConditionMercenaries.ConditionFlag.check (Kill enemies as they appear: every area sends)",
           kill ? method(flag_base, "check", {}) : 0, reinterpret_cast<uintptr_t>(&on_spawn_flag), t_spawn_condition[2]);
  }
  {
    // Separate Ways' time limit to save Luis: the level timer's own frame (it has no subclasses).
    const uint32_t lt = re::find_type("chainsaw.CustomLevelTimer");
    define(kLevelTimer, "CustomLevelTimer.lateUpdate (God mode: Separate Ways' time limit to save Luis)",
           game::parts().luis_timer ? method(lt, "lateUpdate", {}) : 0, reinterpret_cast<uintptr_t>(&on_level_timer), lt);
  }
  {
    // The Extra Content Shop's screen (it has no subclasses): the tabs it shows, for the shop window, and its page shown again.
    const uint32_t ss = re::find_type("chainsaw.gui.ExContentsShopGuiBehavior");
    define(kShopScreen, "ExContentsShopGuiBehavior.lateUpdateOnActive (the shop window: the shop's tabs, its page shown again)",
           exshop::ready() ? method(ss, "lateUpdateOnActive", {}) : 0, reinterpret_cast<uintptr_t>(&on_shop_screen), ss);
  }
  if (sign_names)
    define_family(kFamActionSign, "PlayerHeadUpdater.onHitActionSign (Automatic QTEs: dodge and knife prompts)",
                  t_player_head, method(t_player_head, "onHitActionSign", {"chainsaw.ActionSignHitInfo"}),
                  reinterpret_cast<uintptr_t>(&on_action_sign));
  else
    logf("events: the action signs' names were not all found - the dodge and knife prompts are left to the tick's dodge");
  if (config::get().disable_events) {
    g_disabled = true;
    logf("events: disabled by config - the cheats that need them do nothing this session");
    return;
  }
  for (int h = 0; h < kFirstSlotHook; ++h)
    if (!g_hook[h].failed) hook_entry(static_cast<Hook>(h));
  service();
}

void service() {
  if (g_disabled) return;
  for (int h = kFirstSlotHook; h < kHookCount; ++h) hook_slots(static_cast<Hook>(h));
  for (Family& f : g_family) hook_family(f);
  for (State& st : g_hook) {
    // An entry the runtime filled again (it links a class's code into its
    // entries as it starts): the game's code taken as the original, hooked again.
    if (st.record && re::method_target_at(st.method) != st.replacement) {
      const uintptr_t code = re::method_code_at(st.method);
      logf("events: %s - its database entry holds exe+0x%llX again; hooking it again", st.name, rva(code));
      st.record = 0;
      hook_entry(static_cast<Hook>(&st - g_hook));
    }
    // A slot someone else put back (or the class's tables rebuilt): logged once, hooked again.
    for (int i = 0; i < st.slots; ++i)
      if (mem::read_ptr(st.slot[i]) != st.replacement) {
        logf("events: %s - slot %p no longer holds the hook; looking again", st.name, reinterpret_cast<void*>(st.slot[i]));
        st.slots = 0;
        break;
      }
    if (!st.first_logged && st.calls.load() > 0) {
      st.first_logged = true;
      logf("events: %s called by the game (first time)", st.name);
    }
  }
}

bool ready(cheats::Kind k) {
  if (g_disabled) return false;
  switch (k) {
    case cheats::kOneHitKills: return hooked(kHitCalc);
    case cheats::kAshleyGod: return game::parts().partner;  // the hold works without the hook; the hook keeps her up
    case cheats::kInfiniteAmmo: return hooked(kUseWeapon) || !g_hook[kUseWeapon].failed;
    case cheats::kNoDurability: return hooked(kUseWeaponCost) || !g_hook[kUseWeaponCost].failed;
    case cheats::kNoArmorWear: return game::parts().armor && hooked(kArmorWear);
    case cheats::kNoSaveCount: return game::parts().saves && (hooked(kCampaignSave) || !g_hook[kCampaignSave].failed);
    case cheats::kInfiniteMoney: return game::parts().money && hooked(kShopMoney);
    case cheats::kInfiniteSpinels: return game::parts().spinels && hooked(kShopSpinels);
    case cheats::kInfiniteTickets: return game::parts().tickets && hooked(kUpgradeHold);
    case cheats::kNoMissCount:
    case cheats::kNoDeathCount: return game::parts().stats && (hooked(kOccurrence) || !g_hook[kOccurrence].failed);
    case cheats::kNoDamageCount: return game::parts().damage && (hooked(kOccurrence) || !g_hook[kOccurrence].failed);
    case cheats::kAutoQte:
      return game::parts().action_sign || !g_family[kFamActionSign].failed || hooked(kGrabEscape) ||
             !g_hook[kGrabEscape].failed;
    case cheats::kAutoGallery: return hooked(kGallery) || !g_hook[kGallery].failed;
    case cheats::kAutoKill: return hooked(kHitCalc) && f_calc_attack_id.valid();
    default: return true;
  }
}

const char* why_not(cheats::Kind k) {
  if (g_disabled) return "events disabled in the ini";
  auto state = [](Hook h) { return g_hook[h].failed ? "not hookable (see the log)" : "waiting for the game"; };
  switch (k) {
    case cheats::kOneHitKills: return state(kHitCalc);
    case cheats::kInfiniteAmmo: return state(kUseWeapon);
    case cheats::kNoDurability: return state(kUseWeaponCost);
    case cheats::kNoArmorWear: return game::parts().armor ? state(kArmorWear) : "the body armor was not found (see the log)";
    case cheats::kNoSaveCount: return state(kCampaignSave);
    case cheats::kInfiniteMoney: return state(kShopMoney);
    case cheats::kInfiniteSpinels:
      return game::parts().spinels ? state(kShopSpinels) : "the spinels were not found (see the log)";
    case cheats::kInfiniteTickets:
      return game::parts().tickets ? state(kUpgradeHold) : "the merchant's upgrade tickets were not found (see the log)";
    case cheats::kNoMissCount:
    case cheats::kNoDeathCount: return state(kOccurrence);
    case cheats::kNoDamageCount:
      return game::parts().damage ? state(kOccurrence) : "the times damaged were not found (see the log)";
    case cheats::kAutoQte: return state(kGrabEscape);
    case cheats::kAutoGallery: return state(kGallery);
    case cheats::kAutoKill: return f_calc_attack_id.valid() ? state(kHitCalc) : "a hit's attack was not found (see the log)";
    default: return "not found";
  }
}

void note_kill_attack(uint32_t attack_id) {
  if (!attack_id) return;
  g_kill_ids[g_kill_next.fetch_add(1, std::memory_order_relaxed) % kKillRing].store(attack_id, std::memory_order_relaxed);
}

long kill_hits() { return g_kill_landed.load(std::memory_order_relaxed); }

void spawn_everywhere(bool on) { g_spawn_everywhere.store(on, std::memory_order_relaxed); }
long spawn_opened() { return g_spawn_opened.load(std::memory_order_relaxed); }
int spawn_hooked() { return hooked(kSpawnArea) + hooked(kSpawnFlagMer) + hooked(kSpawnFlag); }

bool save_list_hooked() { return !g_disabled && hooked(kSaveMenu); }

bool box_hooked() { return !g_disabled && hooked(kArmourySave) && hooked(kArmouryLoad); }
bool box_hook_failed() { return g_disabled || g_hook[kArmourySave].failed || g_hook[kArmouryLoad].failed; }

void set_save_hold(int c) { g_save_hold.store(c); }
unsigned long last_save_held() { return g_save_held_at.load(); }

void note_spinels(int count) { g_spinels_seen.store(count); }

uintptr_t gallery_controller() {
  return stamp::within(g_gallery_at.load(), 2000) ? g_gallery.load() : 0;
}

int gallery_score() { return gallery_controller() ? g_gallery_score.load() : -1; }

bool luis_time(float* left) {
  if (!stamp::within(g_luis_time_at.load(), 1000)) return false;
  *left = g_luis_time_left.load(std::memory_order_relaxed);
  return true;
}

void uninstall() {
  for (Family& f : g_family) {
    const int n = f.n.load();
    for (int i = 0; i < n; ++i) re::swap_slot(f.slot[i].at, f.replacement, f.slot[i].original);
  }
  // Delegates the game made from an exchanged entry keep the mod's jump: only a
  // DLL that stays loaded (it is pinned) can be called through them.
  for (State& st : g_hook) {
    const uintptr_t orig = st.original.load();
    if (st.record && re::unhook_method(st.record, st.replacement, orig)) st.record = 0;
    for (int i = 0; i < st.slots; ++i) re::swap_slot(st.slot[i], st.replacement, orig);
    st.slots = 0;
  }
}

}  // namespace re4cc::events
