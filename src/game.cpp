#include "game.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.h"
#include "mem.h"
#include "results_rules.h"

namespace re4cc::game {
namespace {

std::atomic<bool> g_ready{false};  // set last by discover() on the mod thread
char g_status[240] = "not started";
Parts g_parts;

// --- names, found once ---------------------------------------------------------------------------------
// A feature's names: the first one missing is logged, and the feature is greyed
// out (Parts) - one missing name never takes another feature with it.
struct Need {
  const char* feature;
  int missing = 0;
  explicit Need(const char* f) : feature(f) {}
};
uint32_t type(const char* name, Need& n) {
  const uint32_t t = re::find_type(name);
  if (!t && n.missing++ < 3) logf("game: %s: no class %s", n.feature, name);
  return t;
}
re::Field field(uint32_t t, const char* name, Need& n) {
  if (!t) {
    ++n.missing;
    return re::Field{};
  }
  const re::Field f = re::find_field(t, name);
  if (!f.valid() && n.missing++ < 3) logf("game: %s: no field %s.%s", n.feature, re::full_name(t).c_str(), name);
  return f;
}
int enum_of(const char* type_name, const char* literal, int fallback, Need& n) {
  const uint32_t t = re::find_type(type_name);
  const int v = t ? re::enum_value(t, literal, INT32_MIN) : INT32_MIN;
  if (v == INT32_MIN) {
    if (n.missing++ < 3) logf("game: %s: no enum value %s.%s", n.feature, type_name, literal);
    return fallback;
  }
  return v;
}

// A manager: its class and the `_Instance` field of its AppSingleton`1<T> base.
struct Single {
  uint32_t t = 0;
  re::Field inst;
};
Single single(const char* name, Need& n) {
  Single s;
  s.t = type(name, n);
  if (s.t) {
    s.inst = re::find_field(s.t, "_Instance");
    if (!s.inst.valid() && n.missing++ < 3) logf("game: %s: %s has no _Instance", n.feature, name);
  }
  return s;
}
uintptr_t instance(const Single& s) {
  const uintptr_t at = re::static_addr(s.inst);
  const uintptr_t obj = at ? mem::read_ptr(at) : 0;
  return obj && re::is_a(obj, s.t) ? obj : 0;
}

// --- where the game is ---
Single s_mode, s_contents, s_gui, s_casemgr;
re::Field f_cur_state, f_state_obj, f_cur_campaign, f_camp_id, f_camp_phase, f_gui_data, f_gui_types, f_gtd_type,
    f_gtd_num, f_ac_setup, f_ac_phase;
uint32_t t_state_ingame, t_state_chapter, t_state_interlude, t_gtd;
int k_campaign_main = 1, k_campaign_sw = 2, k_campaign_mc = 3, k_campaign_active = 3;
int k_gui_menu = 53, k_gui_saveload = 8, k_gui_records = -1, k_gui_shop = -1;
int k_ac_wait_open = 0;

// --- characters ---
Single s_chars;
re::Field f_player_list, f_partner_list, f_enemy_list, f_ctx_kind, f_ctx_hp, f_ctx_head;
re::Field f_hp_def, f_hp_cur, f_hp_nodamage;
uint32_t t_context, t_hitpoint, t_player_head;

// --- the water scooter ---
Single s_jet_skis;
re::Field f_jsm_main, f_js_hp, f_js_setting, f_js_state, f_js_next, f_js_last_damage, f_js_grace, f_jsu_max_hp;
uint32_t t_jet_ski;
int k_js_run = 1, k_js_crash = 2;

// --- Luis's time limit (Separate Ways) ---
re::Field f_lt_timer, f_lt_transit, f_lt_done, f_lt_limit;
uint32_t t_level_timer, t_lt_timer;

// --- inventories ---
re::Field f_head_cs, f_head_key, f_head_treasure, f_head_unique, f_head_action_sign;
re::Field f_cs_inventory, f_key_inventory, f_treasure_inventory, f_unique_inventory;
re::Field f_cs_items, f_key_items, f_treasure_items, f_unique_items;
re::Field f_cs_size, f_cs_rows, f_cs_cols, f_cs_equips, f_equip_id;
re::Field f_csi_item, f_csi_slot, f_csi_dir, f_csi_force, f_slot_row, f_slot_col;
re::Field f_item_id, f_item_guid, f_item_cond, f_item_dur, f_item_count, f_item_define, f_define_size;
re::Field f_wi_ammo_id, f_wi_ammo, f_wi_tactical, f_wi_levels;
re::Field f_cl_common, f_cl_individual, f_cl_limit, f_lv_now, f_lv_common_cat, f_lv_individual_cat, f_lv_limit_cat;
uint32_t t_item, t_weapon_item, t_cs_inv_item, t_lv_common, t_lv_individual, t_lv_limit;
int k_rot_090 = 1;

// --- the body armor ---
re::Field f_define_dur_max;
uint32_t t_unique_ctl;
int k_armor_id = 124001600;  // ItemID it_sm77_501_00_0, the Body Armor

// --- the game's storage ---
Single s_armoury;
re::Field f_arm_list, f_ai_item, f_ai_count, f_ai_max, f_asd_list;
uint32_t t_armoury_item, t_armoury_save, t_state_menus;

// --- money, saves, difficulty, rank ---
Single s_invmgr, s_shop, s_campaign, s_rank;
re::Field f_ptas, f_spinels, f_save_count, f_chapter_saves, f_cur_chapter, f_difficulty, f_rank_value, f_rank_action,
    f_rank_fixed, f_fixed_guid;
int k_difficulty[kDifficulties] = {10, 20, 30, 40};
// The merchant's exclusive upgrade: the way to pay chosen in its list, and the customer's treasures.
re::Field f_shop_state_ctl, f_ssc_behaviors, f_ssc_params, f_sbh_popup, f_popup_limit_break, f_lb_options, f_lb_selected,
    f_lbo_command, f_lbo_selectable, f_sph_treasure;
uint32_t t_custom_command;
int k_cmd_ticket = 25;        // ItemCommandType.TicketLimitBreak
int k_ticket_id = 120867200;  // ItemID it_sm75_542_00_0, the Exclusive Upgrade Ticket

// --- stats and the clock ---
Single s_stats, s_clock;
re::Field f_ongoing, f_history, f_gameover, f_os_start, f_os_kill, f_os_death, f_os_acc, f_os_damage, f_rec_lap,
    f_rec_kill, f_rec_death, f_rec_acc, f_rec_damage, f_lap_time, f_kill_count, f_death_count, f_damage_count, f_attempt,
    f_success, f_go_need, f_go_death;
re::Field f_clock_data, f_elapsed, f_demo, f_pause;
re::Field f_clock_reset, f_inventory;  // its second copy, which tells a load that kept the run's clock
re::Field f_rec_chapter;
// Separate Ways' chapters 1 and 2 are recorded in two parts; the second part's
// lap holds the first's (ChapterID cp11_chp1_1, cp11_chp2_1: the first parts).
int k_first_parts[2] = {31100, 32100};
// The RESULTS screen's total play time: GameClock's system count, and the
// campaign clock that takes its growth.
Single s_cgc;
re::Field f_clock_system, f_system_elapsed, f_cgc_prev;

// --- countdowns, the Mercenaries ---
Single s_timer, s_mercmgr;
re::Field f_t_second, f_t_state, f_t_setting;
int k_t_counting = 1, k_t_stop = 2;
re::Field f_mm_bonus_get, f_mc_routine, f_mc_combo, f_mc_timer, f_mc_score, f_mc_rush, f_mc_kills;
re::Field f_mt_busy, f_mt_timeout, f_mt_remain, f_cc_state, f_cc_timer, f_cc_count, f_cc_max;
re::Field f_sc_score, f_sc_normal, f_sc_unique, f_ai_value;
re::Field f_rb_busy, f_rb_gauges, f_rb_timer, f_rb_max, f_rb_limit;
uint32_t t_merc_ctl, t_score_counter;
int k_routine_game = 3, k_combo_accepting = 1;

// --- Kill enemies as they appear: the enemies, where a hit goes, a shot's attack data ---
re::Field f_ctx_setupped, f_ctx_body, f_body_hc, f_hp_invincible, f_hp_nodeath, f_hc_effect, f_effect_players;
uint32_t t_hit_controller, t_c0_head, t_damage_effect;
re::Field f_c0_parasite_state, f_c0_body, f_c0b_parasite, f_par_appeared, f_c0_parasite_chance, f_c0_parasite_force;
int k_parasite_wait = 1, k_parasite_start = 2, k_parasite_appeared = 3;
re::Field f_cm_enemy_mgr, f_em_active, f_em_limit, f_mc_spawn_limit;
// the spawners and their points' cooldowns
uint32_t t_spawner, t_timer;
re::Field f_mc_spawners, f_sd_setting, f_sd_controller, f_ss_name, f_nh_hash;
re::Field f_sp_enabled, f_sp_spent, f_sp_requests, f_sp_active, f_sp_limit, f_sp_spawned, f_sp_targets, f_sp_points,
    f_sp_cooldowns, f_timer_done, f_timer_transit, f_sp_distance, f_point_camera;
uint32_t t_spawn_point;
Single s_hitmgr;
re::Field f_hm_catalog, f_cat_merged, f_rc_item, f_ahud_list, f_ad_key, f_ad_type;
uint32_t t_attack_data, t_weapon_id;
int k_attack_bullet = 2, k_attack_shot = 1;
uint32_t t_attack_ud, t_damage_ud;
re::Field f_aud_data_id, f_aud_key, f_aud_attack_id, f_dud_group, f_dud_priority, f_dud_parts, f_dud_side;
int k_group_enemy = 2, k_parts_chest = 1, k_side_center = 0;

// --- the dodge prompts ---
re::Field f_as_avoid, f_as_cant_parry, f_info_enable, f_info_reserve, f_info_accepting, f_info_large;

// --- costumes ---
Single s_costume_gui;
re::Field f_gcm_dict, f_csm_situation, f_csm_reason, f_pbc_state, f_ctx_costume_changing;
uint32_t t_csm, t_player_base_ctx;
int k_sit_main_only = 2, k_sit_except = 3;
int k_costume_kinds[5] = {0, 2, 6, 8, 10};
int k_accessory_kinds[2] = {1, 7};  // Leon's, Ada's: the reason text the costumes borrow
int64_t k_unsafe_states = 0;
// The entries the mod opened, and what the game had in them.
struct CostumeOpened {
  uintptr_t info = 0;
  int kind = -1;
  int situation = 0;
  uint8_t reason[16] = {};
};
CostumeOpened g_costume_opened[5];
int g_costumes_opened = 0;

// --- steady aim ---
re::Field f_head_equipment, f_eq_weapons, f_eq_weapon_id, f_gun_fit, f_gun_shellgen, f_arms_fit_param, f_fit_range,
    f_range_s, f_range_r, f_bud_info, f_sud_center, f_sud_around, f_info_move, f_move_radius, f_move_radius_fit,
    f_rud_info;
re::Field f_wl_entries, f_wl_count;  // the weapon list's Dictionary`2: its class's own
uint32_t t_gun, t_equipment, t_bullet_gen_ud, t_shotgun_gen_ud, t_rocket_gen_ud, t_bullet_info, t_move_info;
re::Field f_ctx_camera, f_mcc_array, f_pcc_recoil, f_pcc_old_rs, f_pcc_rs_lerp, f_recoil_yaw, f_recoil_pitch;
uint32_t t_mcc, t_pcc;
// The camera's own shakes (Steady aim's hooks on the player camera): the hand
// shake it hands its MainCameraController, and the state it is aiming in.
re::Field f_root_mcc, f_mcc_hand_shake, f_pcc_state, f_csp_state, f_csp_gimmick;
bool g_shake_parts = false;
int k_cam_hold = 6, k_cam_hold_last = 10, k_cam_gimmick = 15, k_cam_hold_variation = 33, k_cam_pump = 21,
    k_gimmick_machine_gun = 6;
bool g_camera_parts = false;

// --- enemies ---

uintptr_t list_at(uintptr_t obj, const re::Field& f, int i) {
  re::List l;
  const uintptr_t list = ref(obj, f);
  return list && re::read_list(list, &l) ? re::list_ref(l, i) : 0;
}

int list_count(uintptr_t obj, const re::Field& f) {
  re::List l;
  const uintptr_t list = ref(obj, f);
  return list && re::read_list(list, &l) ? l.count : -1;
}

}  // namespace

// --- helpers -----------------------------------------------------------------------------------------------
uintptr_t ref(uintptr_t obj, const re::Field& f) {
  uintptr_t v = 0;
  return get(obj, f, &v) && mem::plausible(v) ? v : 0;
}

// Dictionary`2's entries {hashCode, next, key, value}: an int key at +8, a
// reference value in the entry's last 8 bytes; a free entry's hashCode is -1.
uintptr_t dict_ref(uintptr_t dict, int32_t key) {
  const uint32_t dt = dict ? re::type_of(dict) : 0;
  if (!dt) return 0;
  const re::Field fe = re::find_field(dt, "_entries"), fc = re::find_field(dt, "_count");
  re::Array a;
  int count = 0;
  const uintptr_t entries = ref(dict, fe);
  if (!entries || !get(dict, fc, &count) || !re::read_array(entries, &a) || a.elem_size < 24) return 0;
  for (int i = 0; i < count && i < a.count && i < 4096; ++i) {
    const uintptr_t e = a.data + static_cast<uintptr_t>(i) * a.elem_size;
    int hash = -1, k = 0;
    if (!mem::read_safe(e, &hash) || hash < 0 || !mem::read_safe(e + 8, &k) || k != key) continue;
    const uintptr_t v = mem::read_ptr(e + a.elem_size - 8);
    return v && mem::plausible(v) ? v : 0;
  }
  return 0;
}

int list_size(uintptr_t list) {
  re::List l;
  return list && re::read_list(list, &l) ? l.count : -1;
}

bool truncate_list(uintptr_t list, int count) {
  const uint32_t t = list ? re::type_of(list) : 0;
  const re::Field f = t ? re::find_field(t, "_size") : re::Field{};
  const int now = list_size(list);
  return f.valid() && now >= 0 && count >= 0 && count <= now && put(list, f, count);
}

bool guid_equal(const uint8_t a[16], const uint8_t b[16]) { return std::memcmp(a, b, 16) == 0; }
bool guid_empty(const uint8_t g[16]) {
  static const uint8_t z[16] = {};
  return std::memcmp(g, z, 16) == 0;
}

uintptr_t singleton(const char* type_name) {
  const uint32_t t = re::find_type(type_name);
  return t ? re::singleton(t) : 0;
}

// --- discovery -------------------------------------------------------------------------------------------------
bool discover() {
  if (g_ready) return true;
  if (!re::ready()) return false;
  {
    Need n("where the game is");
    s_mode = single("share.MainModeManager", n);
    f_cur_state = field(s_mode.t, "CurrentGameState", n);
    f_state_obj = field(type("share.MainModeManager.GameStateControl", n), "GameState", n);
    t_state_ingame = type("chainsaw.GameStateInGame", n);
    t_state_chapter = re::find_type("chainsaw.GameStateInGameChapterChange");
    t_state_interlude = re::find_type("chainsaw.GameStateInGameInterlude");
    s_contents = single("share.ContentsManager", n);
    f_cur_campaign = field(s_contents.t, "_CurrentCampaign", n);
    const uint32_t cc = type("share.CampaignController", n);
    f_camp_id = field(cc, "<CampaignID>k__BackingField", n);
    f_camp_phase = field(cc, "<CurrentPhase>k__BackingField", n);
    k_campaign_main = enum_of("share.CampaignIdentifier", "Campaign1001", 1, n);
    k_campaign_sw = enum_of("share.CampaignIdentifier", "Campaign1011", 2, n);
    k_campaign_mc = enum_of("share.CampaignIdentifier", "Campaign1021", 3, n);
    k_campaign_active = enum_of("share.CampaignController.Phase", "Active", 3, n);
    s_gui = single("chainsaw.GuiManager", n);
    f_gui_data = field(s_gui.t, "<GuiOpenCloseData>k__BackingField", n);
    f_gui_types = field(type("chainsaw.PlayerGuiOpenCloseData", n), "<ActiveGuiTypeDatas>k__BackingField", n);
    t_gtd = type("chainsaw.PlayerGuiOpenCloseData.ActiveGuiTypeData", n);
    f_gtd_type = field(t_gtd, "<GuiType>k__BackingField", n);
    f_gtd_num = field(t_gtd, "<ActiveNum>k__BackingField", n);
    k_gui_menu = enum_of("chainsaw.GuiType", "InGameMenu", 53, n);
    k_gui_saveload = enum_of("chainsaw.GuiType", "SaveLoad", 8, n);
    // The challenges screen and the Extra Content Shop: their windows' own discovery says what they lack.
    if (const uint32_t gt = re::find_type("chainsaw.GuiType")) {
      k_gui_records = re::enum_value(gt, "RecordList", -1);
      k_gui_shop = re::enum_value(gt, "ExtraContentsShop", -1);
    }
    g_parts.state = !n.missing;
  }
  {
    Need n("the attache case's screen");
    s_casemgr = single("chainsaw.AttacheCaseManager", n);
    f_ac_setup = field(s_casemgr.t, "<AcSetupController>k__BackingField", n);
    f_ac_phase = field(type("chainsaw.AttacheCaseSetupController", n), "<CurrPhase>k__BackingField", n);
    k_ac_wait_open = enum_of("chainsaw.AttacheCaseSetupController.Phase", "WaitOpen", 0, n);
    g_parts.case_scene = !n.missing;
  }
  {
    Need n("characters");
    s_chars = single("chainsaw.CharacterManager", n);
    f_player_list = field(s_chars.t, "<PlayerContextList>k__BackingField", n);
    f_enemy_list = field(s_chars.t, "<EnemyContextList>k__BackingField", n);
    t_context = type("chainsaw.CharacterContext", n);
    f_ctx_kind = field(t_context, "<KindID>k__BackingField", n);
    f_ctx_hp = field(t_context, "<HitPoint>k__BackingField", n);
    f_ctx_head = field(t_context, "_HeadUpdater", n);
    t_hitpoint = type("chainsaw.HitPoint", n);
    f_hp_def = field(t_hitpoint, "<DefaultHitPoint>k__BackingField", n);
    f_hp_cur = field(t_hitpoint, "<CurrentHitPoint>k__BackingField", n);
    f_hp_nodamage = field(t_hitpoint, "<NoDamage>k__BackingField", n);
    g_parts.players = !n.missing;
    Need p("Ashley");
    f_partner_list = field(s_chars.t, "<PartnerContextList>k__BackingField", p);
    g_parts.partner = g_parts.players && !p.missing;
  }
  {
    // The water scooter of the main game's escape: its hull's health, its state
    // (a crash is its game over), and the grace after a hit its damage code reads.
    Need n("the water scooter");
    s_jet_skis = single("chainsaw.JetSkiManager", n);
    f_jsm_main = field(s_jet_skis.t, "<MainJetSki>k__BackingField", n);
    t_jet_ski = type("chainsaw.GmJetSki", n);
    f_js_hp = field(t_jet_ski, "_CurrentHP", n);
    f_js_setting = field(t_jet_ski, "_JetSkiSetting", n);
    f_js_state = field(t_jet_ski, "<CurrentGameState>k__BackingField", n);
    f_js_next = field(t_jet_ski, "_NextGameState", n);
    f_js_last_damage = field(t_jet_ski, "_LastDamage", n);
    f_js_grace = field(t_jet_ski, "_IgnoreDamageTimer", n);
    f_jsu_max_hp = field(type("chainsaw.GmJetSkiUserdata", n), "_MaxHP", n);
    k_js_run = enum_of("chainsaw.GmJetSki.GameState", "Run", k_js_run, n);
    k_js_crash = enum_of("chainsaw.GmJetSki.GameState", "Crash", k_js_crash, n);
    g_parts.jet_ski = !n.missing;
  }
  {
    // Separate Ways' time limit to save Luis from the flames: the scene's level
    // timer and the timer it runs.
    Need n("Luis's time limit");
    t_level_timer = type("chainsaw.CustomLevelTimer", n);
    f_lt_timer = field(t_level_timer, "_Timer", n);
    t_lt_timer = type("chainsaw.Timer", n);
    f_lt_transit = field(t_lt_timer, "_TransitTime", n);
    f_lt_done = field(t_lt_timer, "_Completed", n);
    f_lt_limit = field(type("chainsaw.TimerBase", n), "_TimeLimit", n);
    g_parts.luis_timer = !n.missing;
  }
  {
    Need n("the inventories");
    t_player_head = type("chainsaw.PlayerHeadUpdater", n);
    f_head_cs = field(t_player_head, "<InventoryController>k__BackingField", n);
    f_head_key = field(t_player_head, "<KeyItemInventoryController>k__BackingField", n);
    f_head_treasure = field(t_player_head, "<TreasureInventoryController>k__BackingField", n);
    f_head_unique = field(t_player_head, "<UniqueInventoryController>k__BackingField", n);
    f_cs_inventory = field(type("chainsaw.CsInventoryController", n), "<_CsInventory>k__BackingField", n);
    f_key_inventory = field(type("chainsaw.KeyItemInventoryController", n), "<_Inventory>k__BackingField", n);
    f_treasure_inventory = field(type("chainsaw.TreasureInventoryController", n), "<_Inventory>k__BackingField", n);
    f_unique_inventory = field(type("chainsaw.UniqueInventoryController", n), "<_Inventory>k__BackingField", n);
    const uint32_t csi = type("chainsaw.gui.inventory.CsInventory", n);
    f_cs_items = field(csi, "_InventoryItems", n);
    f_cs_size = field(csi, "<CurrInventorySize>k__BackingField", n);
    f_cs_rows = field(csi, "<CurrRowSize>k__BackingField", n);
    f_cs_cols = field(csi, "<CurrColumnSize>k__BackingField", n);
    f_cs_equips = field(csi, "_EquipInfos", n);
    f_equip_id = field(type("chainsaw.InventoryEquipInfo", n), "<ID>k__BackingField", n);
    f_key_items = field(type("chainsaw.gui.keyitem.KeyItemInventory", n), "_Items", n);
    f_treasure_items = field(type("chainsaw.gui.treasure.TreasureInventory", n), "_Items", n);
    f_unique_items = field(type("chainsaw.UniqueInventory", n), "_Items", n);
    t_cs_inv_item = type("chainsaw.CsInventoryItem", n);
    f_csi_item = field(type("chainsaw.InventoryItemBase", n), "<Item>k__BackingField", n);
    f_csi_slot = field(t_cs_inv_item, "<SlotIndex>k__BackingField", n);
    f_csi_dir = field(t_cs_inv_item, "<CurrDirection>k__BackingField", n);
    f_csi_force = field(t_cs_inv_item, "<IsForceGet>k__BackingField", n);
    const uint32_t slot = type("chainsaw.CsSlotIndex", n);
    f_slot_row = field(slot, "Row", n);
    f_slot_col = field(slot, "Column", n);
    k_rot_090 = enum_of("chainsaw.ItemDirection", "Rot_090", 1, n);
    t_item = type("chainsaw.Item", n);
    f_item_id = field(t_item, "_ItemId", n);
    f_item_guid = field(t_item, "_ID", n);
    f_item_cond = field(t_item, "_CurrentCondition", n);
    f_item_dur = field(t_item, "_CurrentDurability", n);
    f_item_count = field(t_item, "_CurrentItemCount", n);
    f_item_define = field(t_item, "<_ItemDefine>k__BackingField", n);
    f_define_size = field(type("chainsaw.ItemDefiniition", n), "_ItemSize", n);
    t_weapon_item = type("chainsaw.WeaponItem", n);
    f_wi_ammo_id = field(t_weapon_item, "_CurrentAmmo", n);
    f_wi_ammo = field(t_weapon_item, "_CurrentAmmoCount", n);
    f_wi_tactical = field(t_weapon_item, "_CurrentTacticalAmmoCount", n);
    f_wi_levels = field(t_weapon_item, "_CustomLevelInWeapon", n);
    const uint32_t cl = type("chainsaw.CustomLevelInWeapon", n);
    f_cl_common = field(cl, "_CommonLevelInWeapon", n);
    f_cl_individual = field(cl, "_IndividualLevelInWeapon", n);
    f_cl_limit = field(cl, "_LimitBreakLevelInWeapon", n);
    f_lv_now = field(type("chainsaw.LevelInWeapon", n), "_NowLevel", n);
    t_lv_common = type("chainsaw.CommonLevelInWeapon", n);
    t_lv_individual = type("chainsaw.IndividualLevelInWeapon", n);
    t_lv_limit = type("chainsaw.LimitBreakLevelInWeapon", n);
    f_lv_common_cat = field(t_lv_common, "_CommonCustomCategory", n);
    f_lv_individual_cat = field(t_lv_individual, "_IndividualCustomCategory", n);
    f_lv_limit_cat = field(t_lv_limit, "_LimitBreakCustomCategory", n);
    g_parts.inventory = g_parts.players && !n.missing;
  }
  {
    // The armor's id is the controller's own constant, the one its armor code names.
    Need n("the body armor");
    t_unique_ctl = type("chainsaw.UniqueInventoryController", n);
    k_armor_id = enum_of("chainsaw.UniqueInventoryController", "_ArmorItemId", k_armor_id, n);
    f_define_dur_max = field(type("chainsaw.ItemDefiniition", n), "_DefaultDurabilityMax", n);
    g_parts.armor = g_parts.inventory && !n.missing;
  }
  {
    Need n("the game's storage");
    s_armoury = single("chainsaw.ArmouryManager", n);
    f_arm_list = field(s_armoury.t, "_ArmouryItemList", n);
    t_armoury_item = type("chainsaw.ArmouryItem", n);
    f_ai_item = field(t_armoury_item, "_Item", n);
    f_ai_count = field(t_armoury_item, "_CurrVirtualStackCount", n);
    f_ai_max = field(t_armoury_item, "_VirtualStackableCountMax", n);
    t_armoury_save = type("chainsaw.ArmouryManager.GameSaveData", n);
    f_asd_list = field(t_armoury_save, "ArmourySaveData", n);
    // The title and the main menus (the main game's, Separate Ways', the
    // Mercenaries'): between games, where the mod's part of the box is let go.
    t_state_menus = type("chainsaw.GameStateMainMenuBase", n);
    g_parts.storage = g_parts.inventory && !n.missing;
  }
  {
    Need n("pesetas");
    s_invmgr = single("chainsaw.InventoryManager", n);
    f_ptas = field(s_invmgr.t, "<CurrPTAS>k__BackingField", n);
    g_parts.money = !n.missing;
    Need m("spinels");
    s_shop = single("chainsaw.InGameShopManager", m);
    f_spinels = field(s_shop.t, "<CurrSpinelCount>k__BackingField", m);
    g_parts.spinels = !m.missing;
    // The exclusive upgrade's state reaches both through the shop's state
    // controller: its GUI behaviours (the popup's list of ways to pay) and its
    // parameters (the customer's inventories).
    Need t("the exclusive upgrade's tickets");
    t_custom_command = type("chainsaw.gui.shop.InGameShopGuiState_CustomCommandDefault", t);
    f_shop_state_ctl = field(type("chainsaw.gui.shop.InGameShopGuiStateBase", t), "<_Controller>k__BackingField", t);
    const uint32_t sc = type("chainsaw.InGameShopStateController", t);
    f_ssc_behaviors = field(sc, "<BehaviorHub>k__BackingField", t);
    f_ssc_params = field(sc, "<InGameShopGuiParamHub>k__BackingField", t);
    f_sbh_popup = field(type("chainsaw.gui.shop.InGameShopGuiBehaviorHub", t), "_InventoryPopupGuiBehavior", t);
    f_popup_limit_break = field(type("chainsaw.InventoryPopupGuiBehavior", t), "_LimitBreakSelectListGui", t);
    const uint32_t lb = type("chainsaw.gui.commandmenu.LimitBreakSelectListGui", t);
    f_lb_options = field(lb, "_AppSelectItems", t);
    f_lb_selected = field(lb, "<CurrSelectedSlotIndex>k__BackingField", t);
    const uint32_t opt = type("chainsaw.gui.commandmenu.LimitBreakSelectListGui.AppSelectItem", t);
    f_lbo_command = field(opt, "<CommandType>k__BackingField", t);
    f_lbo_selectable = field(opt, "<Selectable>k__BackingField", t);
    f_sph_treasure =
        field(type("chainsaw.gui.shop.InGameShopGuiParamHub", t), "<TreasureInventoryController>k__BackingField", t);
    k_cmd_ticket = enum_of("chainsaw.ItemCommandType", "TicketLimitBreak", k_cmd_ticket, t);
    k_ticket_id = enum_of("chainsaw.ItemID", "it_sm75_542_00_0", k_ticket_id, t);
    g_parts.tickets = g_parts.inventory && !t.missing;
  }
  {
    Need n("the save count");
    s_campaign = single("chainsaw.CampaignManager", n);
    f_save_count = field(s_campaign.t, "<SaveCount>k__BackingField", n);
    f_chapter_saves = field(s_campaign.t, "<ChapterSaveCount>k__BackingField", n);
    f_cur_chapter = field(s_campaign.t, "<CurrentChapter>k__BackingField", n);
    g_parts.saves = !n.missing;
    Need d("the difficulty");
    f_difficulty = field(s_campaign.t, "<CurrentDifficulty>k__BackingField", d);
    static const char* const kLit[kDifficulties] = {"Rank_10", "Rank_20", "Rank_30", "Rank_40"};
    for (int i = 0; i < kDifficulties; ++i) k_difficulty[i] = enum_of("chainsaw.CampaignManager.Difficulty", kLit[i], k_difficulty[i], d);
    g_parts.difficulty = !d.missing;
    Need r("the dynamic difficulty");
    s_rank = single("chainsaw.GameRankSystem", r);
    f_rank_value = field(s_rank.t, "_GameRank", r);
    f_rank_action = field(s_rank.t, "_ActionPoint", r);
    f_rank_fixed = field(s_rank.t, "_RankFixedData", r);
    f_fixed_guid = field(type("chainsaw.GameRankSystem.RankFixedData", r), "Guid", r);
    g_parts.rank = !r.missing;
  }
  {
    Need n("the results");
    s_stats = single("chainsaw.GameStatsManager", n);
    f_ongoing = field(s_stats.t, "<OngoingStats>k__BackingField", n);
    f_history = field(s_stats.t, "<OngoingChapterStatsHistory>k__BackingField", n);
    f_gameover = field(s_stats.t, "<GameOverOngoingStats>k__BackingField", n);
    const uint32_t os = type("chainsaw.OngoingStats", n);
    f_os_start = field(os, "<ChapterStartTime>k__BackingField", n);
    f_os_kill = field(os, "<Kill>k__BackingField", n);
    f_os_death = field(os, "<Death>k__BackingField", n);
    f_os_acc = field(os, "<Accuracy>k__BackingField", n);
    const uint32_t rec = type("chainsaw.ChapterStatsRecord", n);
    f_rec_lap = field(rec, "<ChapterLapTime>k__BackingField", n);
    f_rec_kill = field(rec, "<Kill>k__BackingField", n);
    f_rec_death = field(rec, "<Death>k__BackingField", n);
    f_rec_acc = field(rec, "<Accuracy>k__BackingField", n);
    f_rec_chapter = field(rec, "_Chapter", n);
    f_lap_time = field(type("chainsaw.GameStatsChapterLapTimeElement", n), "_Time", n);
    f_kill_count = field(type("chainsaw.GameStatsKillCountElement", n), "_Count", n);
    f_death_count = field(type("chainsaw.GameStatsDeathElement", n), "_Count", n);
    const uint32_t acc = type("chainsaw.GameStatsAccuracyElement", n);
    f_attempt = field(acc, "_HitAttempt", n);
    f_success = field(acc, "_HitSuccess", n);
    const uint32_t go = type("chainsaw.GameStatsGameOverOngoingStats", n);
    f_go_need = field(go, "<NeedTakeOver>k__BackingField", n);
    f_go_death = field(go, "<Death>k__BackingField", n);
    g_parts.stats = !n.missing;
    Need dm("the times damaged");
    f_os_damage = field(os, "<Damage>k__BackingField", dm);
    f_rec_damage = field(rec, "<Damage>k__BackingField", dm);
    f_damage_count = field(type("chainsaw.GameStatsDamageCountElement", dm), "_Count", dm);
    g_parts.damage = g_parts.stats && !dm.missing;
    Need c("the play time");
    s_clock = single("share.GameClock", c);
    f_clock_data = field(s_clock.t, "_GameSaveData", c);
    const uint32_t gsd = type("share.GameClock.GameSaveData", c);
    f_elapsed = field(gsd, "_GameElapsedTime", c);
    f_demo = field(gsd, "_DemoSpendingTime", c);
    f_pause = field(gsd, "_PauseSpendingTime", c);
    k_first_parts[0] = enum_of("chainsaw.ChapterID", "cp11_chp1_1", k_first_parts[0], c);
    k_first_parts[1] = enum_of("chainsaw.ChapterID", "cp11_chp2_1", k_first_parts[1], c);
    g_parts.clock = g_parts.stats && !c.missing;
    Need k("the play time across a Continue");
    f_clock_reset = field(s_clock.t, "_GameSaveData_Reset", k);
    f_inventory = field(gsd, "_InventorySpendingTime", k);
    g_parts.clock_copy = g_parts.clock && !k.missing;
    Need st("the results screen's total play time");
    f_clock_system = field(s_clock.t, "_SystemSaveData", st);
    f_system_elapsed = field(type("share.GameClock.SystemSaveData", st), "_SystemElapsedTime", st);
    s_cgc = single("chainsaw.ChainsawGameClock", st);
    f_cgc_prev = field(s_cgc.t, "PrevTime", st);
    g_parts.system_time = !st.missing;
  }
  {
    Need n("the countdown");
    s_timer = single("chainsaw.TimerManager", n);
    f_t_second = field(s_timer.t, "<CurrentSecond>k__BackingField", n);
    f_t_state = field(s_timer.t, "_CurrentState", n);
    f_t_setting = field(s_timer.t, "_CurrentFuncParamSetting", n);
    k_t_counting = enum_of("chainsaw.TimerManager.TimerState", "Counting", 1, n);
    k_t_stop = enum_of("chainsaw.TimerManager.TimerState", "Stop", 2, n);
    g_parts.countdown = !n.missing;
  }
  {
    Need n("the Mercenaries");
    s_mercmgr = single("chainsaw.MercenariesManager", n);
    f_mm_bonus_get = field(s_mercmgr.t, "_OnTimeBonusGet", n);
    t_merc_ctl = type("chainsaw.MercenariesModeController", n);
    f_mc_routine = field(t_merc_ctl, "_Routine", n);
    f_mc_combo = field(t_merc_ctl, "_ComboCounter", n);
    f_mc_timer = field(t_merc_ctl, "_PlayTimer", n);
    f_mc_score = field(t_merc_ctl, "_ScoreCounter", n);
    f_mc_rush = field(t_merc_ctl, "_BulletRushMultiCounter", n);
    f_mc_kills = field(t_merc_ctl, "_DeadEnemyCount", n);
    k_routine_game = enum_of("chainsaw.MercenariesModeController.RoutineType", "Game", 3, n);
    const uint32_t mt = type("chainsaw.MercenariesTimer", n);
    f_mt_busy = field(mt, "_IsBusy", n);
    f_mt_timeout = field(mt, "_IsTimeOut", n);
    f_mt_remain = field(mt, "_RemainTimer", n);
    const uint32_t cc = type("chainsaw.MercenariesComboCounter", n);
    f_cc_state = field(cc, "_AcceptionState", n);
    f_cc_timer = field(cc, "_Timer", n);
    f_cc_count = field(cc, "_ComboCnt", n);
    f_cc_max = field(cc, "_MaxComboCnt", n);
    k_combo_accepting = enum_of("chainsaw.MercenariesComboCounter.AcceptionState", "Accepting", 1, n);
    t_score_counter = type("chainsaw.MercenariesScoreCounter", n);
    f_sc_score = field(t_score_counter, "_Score", n);
    f_sc_normal = field(t_score_counter, "_AddEntryComboBonusScoreNormal", n);
    f_sc_unique = field(t_score_counter, "_AddEntryComboBonusScoreUnique", n);
    f_ai_value = field(type("chainsaw.SimpleAntiMemoryCheatInteger", n), "_Value", n);
    const uint32_t rb = type("chainsaw.MercenariesBulletRushMultiCounter", n);
    f_rb_busy = field(rb, "_IsRushTimeBusy", n);
    f_rb_gauges = field(rb, "_FullGaugeNum", n);
    f_rb_timer = field(rb, "_RushTimer", n);
    f_rb_max = field(rb, "_MaxGaugeNum", n);
    f_rb_limit = field(rb, "_RushLimitTime", n);
    g_parts.mercs = !n.missing;
  }
  {
    // Kill enemies as they appear: the enemies and the hit controller a hit goes to,
    // a Ganado's parasite (its state, whether it is out, its chance), the
    // HitManager's attack data, and the two objects of a hit request
    // (HitController.requestAttack's).
    Need n("kill enemies as they appear");
    f_ctx_setupped = field(t_context, "<Setupped>k__BackingField", n);
    f_ctx_body = field(t_context, "_BodyUpdater", n);
    f_body_hc = field(type("chainsaw.CharacterBodyUpdater", n), "<HitController>k__BackingField", n);
    t_hit_controller = type("chainsaw.HitController", n);
    f_hc_effect = field(t_hit_controller, "<DamageEffect>k__BackingField", n);
    t_damage_effect = type("chainsaw.EPVExpertDamageEffect", n);
    f_effect_players = field(t_damage_effect, "_Players", n);
    f_hp_invincible = field(t_hitpoint, "<Invincible>k__BackingField", n);
    f_hp_nodeath = field(t_hitpoint, "<NoDeath>k__BackingField", n);
    t_c0_head = type("chainsaw.Ch1c0HeadUpdaterCommon", n);
    f_c0_parasite_state = field(t_c0_head, "parasiteState", n);
    f_c0_body = field(t_c0_head, "_BodyUpdater", n);
    f_c0b_parasite = field(type("chainsaw.Ch1c0BodyUpdaterCommon", n), "parasiteUpdater", n);
    f_par_appeared = field(type("chainsaw.Ch1c0ParasiteUpdaterCommon", n), "<IsFinishParasiteAppearance>k__BackingField", n);
    f_c0_parasite_chance = field(t_c0_head, "parasiteAppearanceProbability", n);
    f_c0_parasite_force = field(t_c0_head, "forceParasiteAppearance", n);
    k_parasite_wait = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Wait", k_parasite_wait, n);
    k_parasite_start = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Start", k_parasite_start, n);
    k_parasite_appeared = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Appeared", k_parasite_appeared, n);
    s_hitmgr = single("chainsaw.HitManager", n);
    f_hm_catalog = field(s_hitmgr.t, "AttackDataCatalog", n);
    f_cat_merged = field(f_hm_catalog.type, "_MergedCatalog", n);
    f_rc_item = field(type("share.RefCounter`1<chainsaw.collision.AttackHitUserData>", n), "_Item", n);
    f_ahud_list = field(type("chainsaw.collision.AttackHitUserData", n), "_AttackDataList", n);
    t_attack_data = type("chainsaw.collision.AttackHitUserData.AttackData", n);
    f_ad_key = field(t_attack_data, "_KeyNameHash", n);
    f_ad_type = field(t_attack_data, "_AttackType", n);
    k_attack_bullet = enum_of("chainsaw.collision.AttackDataID", "Bullet", k_attack_bullet, n);
    k_attack_shot = enum_of("chainsaw.character.AttackType", "Shot", k_attack_shot, n);
    t_weapon_id = type("chainsaw.WeaponID", n);
    t_attack_ud = type("chainsaw.collision.AttackUserData", n);
    f_aud_data_id = field(t_attack_ud, "_AttackHitDataID", n);
    f_aud_key = field(t_attack_ud, "_KeyNameHash", n);
    f_aud_attack_id = field(t_attack_ud, "<AttackID>k__BackingField", n);
    t_damage_ud = type("chainsaw.collision.DamageUserData", n);
    f_dud_group = field(t_damage_ud, "HitGroup", n);
    f_dud_priority = field(t_damage_ud, "Priority", n);
    f_dud_parts = field(t_damage_ud, "Parts", n);
    f_dud_side = field(t_damage_ud, "PartsSide", n);
    k_group_enemy = enum_of("chainsaw.HitManager.HitGroup", "Enemy", k_group_enemy, n);
    k_parts_chest = enum_of("chainsaw.character.BodyParts", "Chest", k_parts_chest, n);
    k_side_center = enum_of("chainsaw.character.BodyPartsSide", "Center", k_side_center, n);
    g_parts.auto_kill = g_parts.players && g_parts.mercs && !n.missing;
  }
  {
    // What that cheat's log tells of the spawns (the enemies counted active, the limits).
    Need n("kill enemies as they appear: the spawn counts");
    f_cm_enemy_mgr = field(s_chars.t, "<EnemyManager>k__BackingField", n);
    f_em_active = field(f_cm_enemy_mgr.type, "<EnemyActiveContextCount>k__BackingField", n);
    f_em_limit = field(f_cm_enemy_mgr.type, "EnemySpawnNumLimit", n);
    f_mc_spawn_limit = field(t_merc_ctl, "_SpawnLimitActiveCount", n);
    g_parts.spawn_counts = g_parts.auto_kill && !n.missing;
  }
  {
    // Its spawners: what each holds and has sent, and its points' cooldown timers.
    Need n("kill enemies as they appear: the spawners");
    f_mc_spawners = field(t_merc_ctl, "_SpawnerList", n);
    const uint32_t data = type("chainsaw.MercenariesModeController.SpawnerData", n);
    f_sd_setting = field(data, "<Setting>k__BackingField", n);
    f_sd_controller = field(data, "<SpawnController>k__BackingField", n);
    f_ss_name = field(type("chainsaw.MercenariesModeController.SpawnerSetting", n), "_SpawnControllerKeyNameHash", n);
    f_nh_hash = field(type("chainsaw.NameHash", n), "_Hash", n);
    t_spawner = type("chainsaw.CharacterSpawnPointControllerMercenaries", n);
    f_sp_enabled = field(t_spawner, "<HasSpawnEnabled>k__BackingField", n);
    f_sp_spent = field(t_spawner, "<HasAllSpawned>k__BackingField", n);
    f_sp_requests = field(t_spawner, "<RequestCount>k__BackingField", n);
    f_sp_active = field(t_spawner, "<ActiveCount>k__BackingField", n);
    f_sp_limit = field(t_spawner, "_ActiveCountLimit", n);
    f_sp_spawned = field(t_spawner, "SpawnedDataList", n);
    f_sp_targets = field(t_spawner, "<SpawnTargets>k__BackingField", n);
    f_sp_points = field(t_spawner, "_PracticalSpawnPoints", n);
    f_sp_cooldowns = field(t_spawner, "_CoolDownTimerList", n);
    t_timer = type("chainsaw.Timer", n);
    f_timer_done = field(t_timer, "_Completed", n);
    f_timer_transit = field(t_timer, "_TransitTime", n);
    f_sp_distance = field(t_spawner, "_SpawnDistanceMin", n);
    t_spawn_point = type("chainsaw.CharacterSpawnPoint", n);
    f_point_camera = field(t_spawn_point, "_IsOutOfCameraOnly", n);
    g_parts.spawners = g_parts.auto_kill && !n.missing;
  }
  {
    Need n("the dodge prompts");
    f_head_action_sign = field(t_player_head, "<ActionSignDriver>k__BackingField", n);
    const uint32_t as = type("chainsaw.PlayerHeadActionSign", n);
    f_as_avoid = field(as, "<AvoidInfo>k__BackingField", n);
    f_as_cant_parry = field(as, "_CantParry", n);
    const uint32_t info = type("chainsaw.PlayerHeadActionSign.Info", n);
    f_info_enable = field(info, "<IsEnable>k__BackingField", n);
    f_info_reserve = field(info, "<RequestReserve>k__BackingField", n);
    f_info_accepting = field(info, "<Accepting>k__BackingField", n);
    f_info_large = field(info, "<IsLargeActionSign>k__BackingField", n);
    g_parts.action_sign = g_parts.players && !n.missing;
  }
  {
    Need n("steady aim");
    f_head_equipment = field(t_player_head, "<Equipment>k__BackingField", n);
    t_equipment = type("chainsaw.Equipment", n);
    f_eq_weapons = field(t_equipment, "WeaponList", n);
    f_wl_entries = re::find_field(f_eq_weapons.type, "_entries");
    f_wl_count = re::find_field(f_eq_weapons.type, "_count");
    f_eq_weapon_id = field(t_equipment, "_EquipWeaponID", n);
    t_gun = type("chainsaw.Gun", n);
    f_gun_fit = field(t_gun, "_CurrentReticleFitPoint", n);
    f_gun_shellgen = field(t_gun, "<ShellGenerator>k__BackingField", n);
    f_arms_fit_param = field(type("chainsaw.Arms", n), "<ReticleFitParam>k__BackingField", n);
    f_fit_range = field(type("chainsaw.WeaponReticleFitParam", n), "_PointRange", n);
    const uint32_t range = type("via.Range", n);
    f_range_s = field(range, "s", n);
    f_range_r = field(range, "r", n);
    t_bullet_gen_ud = type("chainsaw.BulletShellGeneratorUserData", n);
    f_bud_info = field(t_bullet_gen_ud, "_ShellInfoUserData", n);
    t_shotgun_gen_ud = type("chainsaw.ShotgunShellGeneratorUserData", n);
    f_sud_center = field(t_shotgun_gen_ud, "_CenterShellInfoUserData", n);
    f_sud_around = field(t_shotgun_gen_ud, "_AroundShellInfoUserData", n);
    t_bullet_info = type("chainsaw.BulletShellInfoUserData", n);
    f_info_move = field(t_bullet_info, "_MoveInfo", n);
    t_move_info = type("chainsaw.BulletShellMoveInfo", n);
    f_move_radius = field(t_move_info, "_RandomRadius", n);
    f_move_radius_fit = field(t_move_info, "_RandomRadius_Fit", n);
    g_parts.aim = g_parts.players && !n.missing;
    // The rocket launcher's rocket, and the camera's recoil and hand shake: parts of
    // Steady aim the rest of it works without (logged when missing).
    Need r("steady aim (the rocket, the camera)");
    t_rocket_gen_ud = type("chainsaw.RocketLauncherShellGeneratorUserData", r);
    f_rud_info = field(t_rocket_gen_ud, "_RocketLauncherShellInfoUserData", r);
    f_ctx_camera = field(t_context, "<CameraController>k__BackingField", r);
    t_mcc = type("chainsaw.MainCameraController", r);
    f_mcc_array = field(t_mcc, "_CameraControllerArray", r);
    t_pcc = type("chainsaw.PlayerCameraController", r);
    f_pcc_recoil = field(t_pcc, "_RecoilParam", r);
    f_pcc_old_rs = field(t_pcc, "_OldRSOffset", r);
    f_pcc_rs_lerp = field(t_pcc, "_RSOffsetLerp", r);
    const uint32_t rp = type("chainsaw.CameraRecoilParam", r);
    f_recoil_yaw = field(rp, "_Yaw", r);
    f_recoil_pitch = field(rp, "_Pitch", r);
    g_camera_parts = g_parts.aim && !r.missing;
    // The camera's hand shake as it hands it over (MainCameraController._IsHandShake:
    // interpolateCamera turns the view by _HandShakeRotation only while it is set,
    // and clears it), and the states the camera aims in (checkHandShake's).
    Need s("steady aim (the camera's shakes)");
    f_root_mcc = field(type("chainsaw.CameraControllerRoot", s), "_MainCameraController", s);
    f_mcc_hand_shake = field(t_mcc, "_IsHandShake", s);
    f_pcc_state = field(t_pcc, "_CurrentStateParam", s);
    const uint32_t csp = type("chainsaw.PlayerCameraStateParam", s);
    f_csp_state = field(csp, "<State>k__BackingField", s);
    f_csp_gimmick = field(csp, "<GimmickType>k__BackingField", s);
    const char* states = "chainsaw.CameraDefine.PlayerCameraState";
    k_cam_hold = enum_of(states, "Hold", k_cam_hold, s);
    k_cam_hold_last = enum_of(states, "HoldExtraScope", k_cam_hold_last, s);
    k_cam_gimmick = enum_of(states, "Gimmick", k_cam_gimmick, s);
    k_cam_hold_variation = enum_of(states, "HoldVariation", k_cam_hold_variation, s);
    k_cam_pump = enum_of(states, "PumpAction", k_cam_pump, s);
    k_gimmick_machine_gun = enum_of("chainsaw.CameraDefine.GimmickType", "InstalledMachineGun", k_gimmick_machine_gun, s);
    g_shake_parts = g_camera_parts && !s.missing;
    g_parts.aim_camera = g_shake_parts;
  }
  {
    Need n("costumes in game");
    s_costume_gui = single("chainsaw.GuiCostumeManager", n);
    f_gcm_dict = field(s_costume_gui.t, "CostumeSettingDict", n);
    t_csm = type("chainsaw.GuiCostumeManager.CostumeSelectMenuInfo", n);
    f_csm_situation = field(t_csm, "_SelectableSituation", n);
    f_csm_reason = field(t_csm, "_UnselectableReason", n);
    k_sit_main_only = enum_of("chainsaw.CostumeSelectableSituation", "OnlyMainMenu", 2, n);
    k_sit_except = enum_of("chainsaw.CostumeSelectableSituation", "ExceptEventOrBattle", 3, n);
    static const char* const kCostume[5] = {"Chara00_Costume", "Chara01_Costume", "AO_Chara00_Costume",
                                            "AO_Chara01_Costume", "AO_Chara02_Costume"};
    for (int i = 0; i < 5; ++i) k_costume_kinds[i] = enum_of("chainsaw.CostumeMenuKind", kCostume[i], k_costume_kinds[i], n);
    k_accessory_kinds[0] = enum_of("chainsaw.CostumeMenuKind", "Chara00_Accessory", 1, n);
    k_accessory_kinds[1] = enum_of("chainsaw.CostumeMenuKind", "AO_Chara00_Accessory", 7, n);
    t_player_base_ctx = type("chainsaw.PlayerBaseContext", n);
    f_pbc_state = field(t_player_base_ctx, "<State>k__BackingField", n);
    f_ctx_costume_changing = field(t_context, "<IsCostumeChanging>k__BackingField", n);
    const uint32_t st = type("chainsaw.PlayerDefine.State", n);
    static const char* const kUnsafe[] = {"Jacked", "Dead", "Ladder", "Boat", "BodyDrawOff", "ParentGimmick", "HookShot"};
    for (const char* name : kUnsafe) {
      const int64_t v = st ? re::enum_value64(st, name, 0) : 0;
      if (!v && n.missing++ < 3) logf("game: costumes in game: no enum value chainsaw.PlayerDefine.State.%s", name);
      k_unsafe_states |= v;
    }
    g_parts.costumes = g_parts.players && !n.missing;
  }
  const Parts& p = g_parts;
  logf("game: discovery: state %d, case screen %d, characters %d, partners %d, water scooter %d, Luis's time limit %d, "
       "inventories %d, body armor %d, storage %d, "
       "pesetas %d, spinels %d, upgrade tickets %d, save count %d, difficulty %d, dynamic difficulty %d, results %d, "
       "times damaged %d, play time %d (across a Continue %d), the RESULTS screen's total %d, countdown %d, "
       "Mercenaries %d (killing enemies as they appear %d), dodge prompts %d, steady aim %d (the camera %d), costumes %d "
       "(difficulty values %d/%d/%d/%d, "
       "GUI InGameMenu %d, SaveLoad %d, RecordList %d, ExtraContentsShop %d; Separate Ways' first parts %d/%d; the body "
       "armor's item %d)",
       p.state, p.case_scene, p.players, p.partner, p.jet_ski, p.luis_timer, p.inventory, p.armor, p.storage, p.money,
       p.spinels, p.tickets,
       p.saves, p.difficulty, p.rank, p.stats, p.damage, p.clock, p.clock_copy, p.system_time, p.countdown, p.mercs,
       p.auto_kill, p.action_sign, p.aim, p.aim_camera, p.costumes,
       k_difficulty[0], k_difficulty[1], k_difficulty[2], k_difficulty[3], k_gui_menu, k_gui_saveload, k_gui_records,
       k_gui_shop,
       k_first_parts[0], k_first_parts[1], k_armor_id);
  if (!p.state) {
    std::snprintf(g_status, sizeof(g_status), "where the game is could not be read (see the log)");
    return false;
  }
  std::snprintf(g_status, sizeof(g_status), "ready");
  g_ready = true;
  return true;
}

bool ready() { return g_ready; }
const char* status_text() { return g_status; }
const Parts& parts() { return g_parts; }

// --- where the game is ---------------------------------------------------------------------------------------
const char* mode_name(Mode m) {
  switch (m) {
    case Mode::kMain: return "the main game";
    case Mode::kSeparateWays: return "Separate Ways";
    case Mode::kMercenaries: return "The Mercenaries";
    default: return "no game";
  }
}

namespace {
// The open count of one GUI type: ActiveGuiTypeDatas is indexed by GuiType,
// each entry naming its type (checked; else looked for).
int gui_active(uintptr_t types_array, int gui_type) {
  re::Array a;
  if (!types_array || !re::read_array(types_array, &a) || a.elem_size != 8) return -1;
  auto at = [&](int i) -> uintptr_t { return i >= 0 && i < a.count ? mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8) : 0; };
  uintptr_t e = at(gui_type);
  int t = -1;
  if (!e || !get(e, f_gtd_type, &t) || t != gui_type) {
    e = 0;
    for (int i = 0; i < a.count && !e; ++i) {
      const uintptr_t c = at(i);
      if (c && get(c, f_gtd_type, &t) && t == gui_type) e = c;
    }
  }
  int n = 0;
  return e && get(e, f_gtd_num, &n) ? n : 0;
}
}  // namespace

Run run() {
  Run r;
  if (!g_ready) return r;
  if (const uintptr_t mm = instance(s_mode)) {
    const uintptr_t ctl = ref(mm, f_cur_state);
    const uintptr_t st = ctl ? ref(ctl, f_state_obj) : 0;
    const uint32_t t = st ? re::type_of(st) : 0;
    r.state = static_cast<int>(t);
    r.in_game = t && (t == t_state_ingame || (t_state_chapter && t == t_state_chapter) ||
                      (t_state_interlude && t == t_state_interlude));
    r.menus = t && t_state_menus && re::derives(t, t_state_menus);
  }
  if (const uintptr_t cm = instance(s_contents)) {
    const uintptr_t camp = ref(cm, f_cur_campaign);
    int phase = -1, id = 0;
    if (camp && get(camp, f_camp_phase, &phase) && phase == k_campaign_active && get(camp, f_camp_id, &id))
      r.mode = id == k_campaign_main ? Mode::kMain
               : id == k_campaign_sw ? Mode::kSeparateWays
               : id == k_campaign_mc ? Mode::kMercenaries
                                     : Mode::kNone;
  }
  if (const uintptr_t gm = instance(s_gui)) {
    const uintptr_t data = ref(gm, f_gui_data);
    const uintptr_t types = data ? ref(data, f_gui_types) : 0;
    r.paused = r.in_game && gui_active(types, k_gui_menu) > 0;
    r.save_load = gui_active(types, k_gui_saveload) > 0;
    r.challenges = k_gui_records >= 0 && gui_active(types, k_gui_records) > 0;
    r.shop = k_gui_shop >= 0 && gui_active(types, k_gui_shop) > 0;
  }
  r.case_scene = case_scene_up();
  return r;
}

const char* state_name(int state) {
  if (state <= 0) return "none";
  const std::string& n = re::full_name(static_cast<uint32_t>(state));
  const size_t dot = n.rfind('.');
  static thread_local char buf[96];
  std::snprintf(buf, sizeof(buf), "%s", dot == std::string::npos ? n.c_str() : n.c_str() + dot + 1);
  return buf;
}

bool case_scene_up() {
  if (!g_parts.case_scene) return false;
  const uintptr_t m = instance(s_casemgr);
  const uintptr_t setup = m ? ref(m, f_ac_setup) : 0;
  int phase = k_ac_wait_open;
  return setup && get(setup, f_ac_phase, &phase) && phase != k_ac_wait_open && phase > 0;
}

// --- characters ----------------------------------------------------------------------------------------------------
uintptr_t player_context() {
  if (!g_parts.players) return 0;
  const uintptr_t cm = instance(s_chars);
  const uintptr_t ctx = cm ? list_at(cm, f_player_list, 0) : 0;
  return ctx && re::is_a(ctx, t_context) ? ctx : 0;
}

namespace {
// A partner of the kind (the game rebuilds the list every frame from the contexts of
// its partner class: Ashley's, Luis's).
uintptr_t partner_context(int kind) {
  if (!g_parts.partner) return 0;
  const uintptr_t cm = instance(s_chars);
  if (!cm) return 0;
  re::List l;
  const uintptr_t list = ref(cm, f_partner_list);
  if (!list || !re::read_list(list, &l)) return 0;
  for (int i = 0; i < l.count && i < 16; ++i) {
    const uintptr_t ctx = re::list_ref(l, i);
    if (ctx && re::is_a(ctx, t_context) && kind_of(ctx) == kind) return ctx;
  }
  return 0;
}
}  // namespace

uintptr_t ashley_context() { return partner_context(kAshleyPartnerKind); }
uintptr_t luis_context() { return partner_context(kLuisPartnerKind); }

int kind_of(uintptr_t context) {
  int k = -1;
  return context && get(context, f_ctx_kind, &k) ? k : -1;
}

uintptr_t hitpoint_of(uintptr_t context) {
  const uintptr_t hp = context ? ref(context, f_ctx_hp) : 0;
  return hp && re::is_a(hp, t_hitpoint) ? hp : 0;
}

uintptr_t head_updater_of(uintptr_t context) { return context ? ref(context, f_ctx_head) : 0; }

bool health(uintptr_t context, Health* out) {
  const uintptr_t hp = hitpoint_of(context);
  Health h;
  if (!hp || !get(hp, f_hp_cur, &h.hp) || !get(hp, f_hp_def, &h.max)) return false;
  get(hp, f_hp_nodamage, &h.no_damage);
  *out = h;
  return true;
}

bool hold_health(uintptr_t context, bool on) {
  const uintptr_t hp = hitpoint_of(context);
  if (!hp) return false;
  bool wrote = false;
  bool nd = false;
  if (get(hp, f_hp_nodamage, &nd) && nd != on) wrote |= put(hp, f_hp_nodamage, on);
  if (on) {
    int cur = 0, max = 0;
    if (get(hp, f_hp_cur, &cur) && get(hp, f_hp_def, &max) && max > 0 && cur > 0 && cur < max)
      wrote |= put(hp, f_hp_cur, max);  // (0 is a death under way: the game's)
  }
  return wrote;
}

bool is_player_kind(int kind) {
  return kind == 100000 || kind == 110000 || kind == 380000 || (kind >= 600000 && kind <= 600005);
}

// --- the water scooter ----------------------------------------------------------------------------------------------
uintptr_t jet_ski() {
  if (!g_parts.jet_ski) return 0;
  const uintptr_t m = instance(s_jet_skis);
  const uintptr_t js = m ? ref(m, f_jsm_main) : 0;
  return js && re::is_a(js, t_jet_ski) ? js : 0;
}

bool jet_ski_health(uintptr_t js, JetSki* out) {
  const uintptr_t setting = js && re::is_a(js, t_jet_ski) ? ref(js, f_js_setting) : 0;
  JetSki j;
  int state = -1, next = -1;
  if (!setting || !get(js, f_js_hp, &j.hp) || !get(setting, f_jsu_max_hp, &j.max) || !get(js, f_js_state, &state) ||
      !get(js, f_js_next, &next))
    return false;
  j.running = state == k_js_run && next == k_js_run;
  j.crashed = state == k_js_crash || next == k_js_crash;
  *out = j;
  return true;
}

bool hold_jet_ski(uintptr_t js, bool on) {
  bool wrote = false;
  int last = 0;
  float grace = 0.0f;
  if (on) {
    JetSki j;
    if (!jet_ski_health(js, &j) || !j.running) return false;  // waiting for the ride, or a crash under way: the game's
    if (j.max > 0 && j.hp < j.max) wrote |= put(js, f_js_hp, j.max);
    if (get(js, f_js_last_damage, &last) && last != kJetSkiNoWorse) wrote |= put(js, f_js_last_damage, kJetSkiNoWorse);
    if (get(js, f_js_grace, &grace) && !(grace >= kJetSkiGrace / 2)) wrote |= put(js, f_js_grace, kJetSkiGrace);
  } else if (js && re::is_a(js, t_jet_ski) && get(js, f_js_last_damage, &last) && last == kJetSkiNoWorse) {
    // The game's own resting values: no hit before, the grace run out - the next hit counts.
    wrote |= put(js, f_js_last_damage, 0);
    if (get(js, f_js_grace, &grace) && grace > 0.0f) wrote |= put(js, f_js_grace, 0.0f);
  }
  return wrote;
}

// --- Luis's time limit (Separate Ways) -----------------------------------------------------------------------------
namespace {
uintptr_t level_timer_timer(uintptr_t lt) {
  const uintptr_t t = g_parts.luis_timer && lt && re::is_a(lt, t_level_timer) ? ref(lt, f_lt_timer) : 0;
  return t && re::is_a(t, t_lt_timer) ? t : 0;  // (none before the timer's start)
}
}  // namespace

bool level_timer(uintptr_t lt, LevelTimer* out) {
  const uintptr_t t = level_timer_timer(lt);
  LevelTimer v;
  if (!t || !get(t, f_lt_limit, &v.limit) || !get(t, f_lt_transit, &v.elapsed) || !get(t, f_lt_done, &v.done))
    return false;
  *out = v;
  return true;
}

bool hold_level_timer(uintptr_t lt) {
  LevelTimer v;
  if (!level_timer(lt, &v) || v.done || !(v.elapsed > 0.0f)) return false;
  return put(level_timer_timer(lt), f_lt_transit, 0.0f);
}

int enemy_count() {
  const uintptr_t cm = g_parts.players ? instance(s_chars) : 0;
  return cm ? list_count(cm, f_enemy_list) : -1;
}

// --- the inventories ---------------------------------------------------------------------------------------------------
bool inventories(Inventories* out) {
  Inventories v;
  if (!g_parts.inventory) return false;
  v.context = player_context();
  const uintptr_t head = head_updater_of(v.context);
  if (!head || !re::is_a(head, t_player_head)) return false;
  v.cs = ref(head, f_head_cs);
  v.key = ref(head, f_head_key);
  v.treasure = ref(head, f_head_treasure);
  v.unique = ref(head, f_head_unique);
  v.cs_inventory = v.cs ? ref(v.cs, f_cs_inventory) : 0;
  v.key_inventory = v.key ? ref(v.key, f_key_inventory) : 0;
  v.treasure_inventory = v.treasure ? ref(v.treasure, f_treasure_inventory) : 0;
  v.unique_inventory = v.unique ? ref(v.unique, f_unique_inventory) : 0;
  if (!v.cs || !v.cs_inventory) return false;
  *out = v;
  return true;
}

bool case_size(uintptr_t cs_inventory, CaseSize* out) {
  CaseSize c;
  if (!cs_inventory || !get(cs_inventory, f_cs_size, &c.size) || !get(cs_inventory, f_cs_rows, &c.rows) ||
      !get(cs_inventory, f_cs_cols, &c.columns))
    return false;
  *out = c;
  return true;
}

const char* case_size_name(int size) {
  static const char* const kNames[] = {"S (7x10)", "M (7x12)", "L (8x12)", "XL (8x13)", "XXL (9x13)"};
  return size >= 0 && size < 5 ? kNames[size] : "?";
}

bool equipped_guids(uintptr_t cs_inventory, uint8_t main[16], uint8_t sub[16]) {
  std::memset(main, 0, 16);
  std::memset(sub, 0, 16);
  re::Array a;
  const uintptr_t arr = cs_inventory ? ref(cs_inventory, f_cs_equips) : 0;
  if (!arr || !re::read_array(arr, &a) || a.elem_size != 8) return false;
  for (int i = 0; i < a.count && i < 2; ++i) {
    const uintptr_t e = mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8);
    const uintptr_t at = e ? re::field_addr(e, f_equip_id) : 0;
    if (at) mem::copy_from(i == 0 ? main : sub, at, 16);
  }
  return true;
}

bool item_type(uintptr_t obj, uint32_t* t) {
  const uint32_t ty = obj ? re::type_of(obj) : 0;
  if (!ty || !re::derives(ty, t_item)) return false;
  *t = ty;
  return true;
}

namespace {
bool read_item(uintptr_t holder, uintptr_t item, ItemView* v) {
  uint32_t t = 0;
  if (!item_type(item, &t)) return false;
  v->item = item;
  v->holder = holder;
  const uintptr_t g = re::field_addr(item, f_item_guid);
  if (!g || !mem::copy_from(v->guid, g, 16)) return false;
  if (!get(item, f_item_id, &v->id) || !get(item, f_item_count, &v->count)) return false;
  get(item, f_item_cond, &v->condition);
  int dur = -1;
  if (get(item, f_item_dur, &dur)) v->durability = dur;
  v->weapon = re::derives(t, t_weapon_item);
  if (v->weapon) {
    get(item, f_wi_ammo, &v->ammo);
    get(item, f_wi_tactical, &v->tactical);
    get(item, f_wi_ammo_id, &v->ammo_id);
  }
  return true;
}
}  // namespace

bool read_item_view(uintptr_t item, ItemView* out) {
  ItemView v;
  if (!read_item(0, item, &v)) return false;
  *out = v;
  return true;
}

int read_items(const Inventories& inv, int kind, ItemView* out, int max) {
  uintptr_t owner = 0;
  const re::Field* f = nullptr;
  switch (kind) {
    case 0: owner = inv.cs_inventory; f = &f_cs_items; break;
    case 1: owner = inv.key_inventory; f = &f_key_items; break;
    case 2: owner = inv.treasure_inventory; f = &f_treasure_items; break;
    case 3: owner = inv.unique_inventory; f = &f_unique_items; break;
    case kStorageKind: {
      // The game's storage: ArmouryItems, each holding its copy of the item.
      re::List l;
      const uintptr_t mgr = armoury_manager();
      const uintptr_t list = mgr ? ref(mgr, f_arm_list) : 0;
      if (!list || !re::read_list(list, &l)) return -1;
      int n = 0;
      for (int i = 0; i < l.count && n < max; ++i) {
        const uintptr_t entry = re::list_ref(l, i);
        if (!entry || !re::is_a(entry, t_armoury_item)) continue;
        ItemView v;
        if (!read_item(entry, ref(entry, f_ai_item), &v) || !get(entry, f_ai_count, &v.stored) ||
            !get(entry, f_ai_max, &v.stored_max))
          continue;
        out[n++] = v;
      }
      return n;
    }
    default: return -1;
  }
  re::List l;
  const uintptr_t list = owner ? ref(owner, *f) : 0;
  if (!list || !re::read_list(list, &l)) return -1;
  uint8_t main[16] = {}, sub[16] = {};
  if (kind == 0) equipped_guids(inv.cs_inventory, main, sub);
  int n = 0;
  for (int i = 0; i < l.count && n < max; ++i) {
    const uintptr_t holder = re::list_ref(l, i);
    if (!holder) continue;
    // Every list's entry is an InventoryItemBase, its item first; the unique
    // list's entries may be the items themselves.
    uintptr_t item = 0;
    if (re::is_a(holder, t_item)) item = holder;
    else item = ref(holder, f_csi_item);
    ItemView v;
    if (!read_item(holder, item, &v)) continue;
    if (kind == 0 && re::is_a(holder, t_cs_inv_item)) {
      const uintptr_t slot = re::field_addr(holder, f_csi_slot);
      if (slot) {
        mem::read_safe(re::value_field_addr(slot, f_slot_row), &v.row);
        mem::read_safe(re::value_field_addr(slot, f_slot_col), &v.column);
      }
      int dir = 0;
      if (get(holder, f_csi_dir, &dir)) v.rotated = dir == k_rot_090;
      get(holder, f_csi_force, &v.force_get);
      v.equipped = !guid_empty(v.guid) && (guid_equal(v.guid, main) || guid_equal(v.guid, sub));
    }
    out[n++] = v;
  }
  return n;
}

bool weapon_levels(uintptr_t weapon_item, Levels* out) {
  Levels lv;
  const uintptr_t cl = weapon_item && re::is_a(weapon_item, t_weapon_item) ? ref(weapon_item, f_wi_levels) : 0;
  if (!cl) return false;
  const re::Field* lists[3] = {&f_cl_common, &f_cl_individual, &f_cl_limit};
  const re::Field* cats[3] = {&f_lv_common_cat, &f_lv_individual_cat, &f_lv_limit_cat};
  const uint32_t types[3] = {t_lv_common, t_lv_individual, t_lv_limit};
  for (int k = 0; k < 3; ++k) {
    re::List l;
    const uintptr_t list = ref(cl, *lists[k]);
    if (!list || !re::read_list(list, &l)) continue;
    for (int i = 0; i < l.count && lv.n < 16; ++i) {
      const uintptr_t e = re::list_ref(l, i);
      if (!e || !re::is_a(e, types[k])) continue;
      Levels::Entry& en = lv.e[lv.n];
      en.list = k;
      if (!get(e, *cats[k], &en.category) || !get(e, f_lv_now, &en.level)) continue;
      ++lv.n;
    }
  }
  lv.ok = true;
  *out = lv;
  return true;
}

const char* level_name(int list, int category) {
  // CommonCustomCategory: AttackUp 0, Stabilization 1, AmmoMaxUp 2, ShotGunAroundAttackUp 3.
  // IndividualCustomCategory: ... Strength 6, ReloadSpeed 7, Rapid 8.
  // LimitBreakCustomCategory: the exclusive upgrade, whatever it is.
  if (list == 0) return category == 0 ? "Power" : category == 2 ? "Capacity" : nullptr;
  if (list == 1)
    return category == 7 ? "Reload speed" : category == 8 ? "Rate of fire" : category == 6 ? "Durability" : nullptr;
  if (list == 2) return "Exclusive";
  return nullptr;
}

bool set_loaded(uintptr_t weapon_item, int ammo) {
  return weapon_item && re::is_a(weapon_item, t_weapon_item) && ammo >= 0 && put(weapon_item, f_wi_ammo, ammo);
}

bool set_durability(uintptr_t item, int durability) {
  return item && re::is_a(item, t_item) && durability >= 0 && put(item, f_item_dur, durability);
}

bool set_condition(uintptr_t item, int condition) {
  return item && re::is_a(item, t_item) && condition >= 0 && put(item, f_item_cond, condition);
}

bool item_defined(uintptr_t item) {
  const uintptr_t def = item && re::is_a(item, t_item) ? ref(item, f_item_define) : 0;
  int size = -1;
  return def && get(def, f_define_size, &size) && size >= 0;
}

int condition_of(uintptr_t item) {
  int c = -1;
  return item && re::is_a(item, t_item) && get(item, f_item_cond, &c) ? c : -1;
}

bool set_item_count(uintptr_t item, int count) {
  return item && re::is_a(item, t_item) && count >= 1 && put(item, f_item_count, count);
}

uintptr_t weapon_custom_levels(uintptr_t weapon_item) {
  return weapon_item && re::is_a(weapon_item, t_weapon_item) ? ref(weapon_item, f_wi_levels) : 0;
}

bool set_item_guid(uintptr_t item, const uint8_t guid[16]) {
  const uintptr_t at = item && re::is_a(item, t_item) ? re::field_addr(item, f_item_guid) : 0;
  return at && mem::copy_to(at, guid, 16);
}

// --- the body armor ----------------------------------------------------------------------------------------------------
namespace {
// The armor's item among a unique inventory's entries: the first of its id
// (holding `durability`, when that is >= 0), as the game's own useDurability
// takes the first. False when the list cannot be read.
bool armor_item(uintptr_t unique_inventory, int durability, uintptr_t* out) {
  *out = 0;
  re::List l;
  const uintptr_t list = unique_inventory ? ref(unique_inventory, f_unique_items) : 0;
  if (!list || !re::read_list(list, &l)) return false;
  for (int i = 0; i < l.count && i < 64; ++i) {
    const uintptr_t entry = re::list_ref(l, i);
    // (an entry is an InventoryItemBase, its item first - or the item itself, as read_items allows)
    const uintptr_t item = !entry ? 0 : re::is_a(entry, t_item) ? entry : ref(entry, f_csi_item);
    int32_t id = 0;
    int now = -1;
    if (!item || !re::is_a(item, t_item) || !get(item, f_item_id, &id) || id != k_armor_id) continue;
    if (durability >= 0 && (!get(item, f_item_dur, &now) || now != durability)) continue;
    *out = item;
    return true;
  }
  return true;
}
}  // namespace

bool body_armor(const Inventories& inv, Armor* out) {
  uintptr_t item = 0;
  if (!g_parts.armor || !armor_item(inv.unique_inventory, -1, &item)) return false;
  Armor a;
  const uintptr_t def = item ? ref(item, f_item_define) : 0;
  int condition = 0;
  if (item && (!get(item, f_item_dur, &a.durability) || !def || !get(def, f_define_dur_max, &a.max) ||
               !get(item, f_item_cond, &condition)))
    return false;
  a.item = item;
  a.broken = (condition & kConditionBroken) != 0;
  *out = a;
  return true;
}

int32_t armor_id() { return g_parts.armor ? k_armor_id : 0; }

bool set_armor(const Armor& a, int durability) {
  int condition = 0;
  if (!g_parts.armor || !a.item || !re::is_a(a.item, t_item) || a.max <= 0 || durability < 0 || durability > a.max ||
      !get(a.item, f_item_cond, &condition))
    return false;
  const int want = durability > 0 ? condition & ~kConditionBroken : condition | kConditionBroken;
  return put(a.item, f_item_dur, durability) && (want == condition || put(a.item, f_item_cond, want));
}

bool keep_armor(uintptr_t unique_controller, int32_t item_id, int prev, int curr) {
  if (!g_parts.armor || item_id != k_armor_id || curr >= prev || prev <= 0 || !unique_controller ||
      !re::is_a(unique_controller, t_unique_ctl))
    return false;
  uintptr_t item = 0;
  int condition = 0;
  if (!armor_item(ref(unique_controller, f_unique_inventory), curr, &item) || !item ||
      !get(item, f_item_cond, &condition) || !put(item, f_item_dur, prev))
    return false;
  // It had some left (prev), so it was not broken before: a mark is this wear's.
  if (condition & kConditionBroken) put(item, f_item_cond, condition & ~kConditionBroken);
  return true;
}

// --- the game's storage ------------------------------------------------------------------------------------------------
uintptr_t armoury_manager() { return g_parts.storage ? instance(s_armoury) : 0; }

bool set_storage_stack(uintptr_t armoury_item, int count) {
  int most = 0;
  if (!armoury_item || !re::is_a(armoury_item, t_armoury_item) || !get(armoury_item, f_ai_max, &most) || count < 1 ||
      count > most)
    return false;
  return put(armoury_item, f_ai_count, count);
}

bool is_storage_save(uintptr_t data) { return g_parts.storage && data && re::is_a(data, t_armoury_save); }

uintptr_t storage_save_list(uintptr_t data) { return is_storage_save(data) ? ref(data, f_asd_list) : 0; }

bool set_storage_save_list(uintptr_t data, uintptr_t list) {
  return is_storage_save(data) && put(data, f_asd_list, list);
}

// --- money -----------------------------------------------------------------------------------------------------------------
int pesetas() {
  const uintptr_t m = g_parts.money ? instance(s_invmgr) : 0;
  int v = -1;
  return m && get(m, f_ptas, &v) ? v : -1;
}
bool set_pesetas(int v) {
  const uintptr_t m = g_parts.money ? instance(s_invmgr) : 0;
  return m && v >= 0 && v <= kPesetasMax && put(m, f_ptas, v);
}
int spinels() {
  const uintptr_t m = g_parts.spinels ? instance(s_shop) : 0;
  int v = -1;
  return m && get(m, f_spinels, &v) ? v : -1;
}
bool set_spinels(int v) {
  const uintptr_t m = g_parts.spinels ? instance(s_shop) : 0;
  return m && v >= 0 && v <= kSpinelsMax && put(m, f_spinels, v);
}

uintptr_t upgrade_ticket(uintptr_t state) {
  if (!g_parts.tickets || !state || !re::is_a(state, t_custom_command)) return 0;
  const uintptr_t ctl = ref(state, f_shop_state_ctl);
  // The way chosen: the ticket's, and one the list lets be taken (a ticket to pay with).
  const uintptr_t behaviors = ctl ? ref(ctl, f_ssc_behaviors) : 0;
  const uintptr_t popup = behaviors ? ref(behaviors, f_sbh_popup) : 0;
  const uintptr_t list = popup ? ref(popup, f_popup_limit_break) : 0;
  int at = -1, command = -1;
  bool selectable = false;
  const uintptr_t option = list && get(list, f_lb_selected, &at) ? list_at(list, f_lb_options, at) : 0;
  if (!option || !get(option, f_lbo_command, &command) || command != k_cmd_ticket ||
      !get(option, f_lbo_selectable, &selectable) || !selectable)
    return 0;
  // The ticket the game's reduceTicket takes: the first of the treasures it counts
  // valid (TreasureInventoryItem.get_IsValid: an item with a Guid, in a slot) with its id.
  const uintptr_t params = ref(ctl, f_ssc_params);
  const uintptr_t treasures = params ? ref(params, f_sph_treasure) : 0;
  const uintptr_t inventory = treasures ? ref(treasures, f_treasure_inventory) : 0;
  re::List l;
  const uintptr_t items = inventory ? ref(inventory, f_treasure_items) : 0;
  if (!items || !re::read_list(items, &l)) return 0;
  for (int i = 0; i < l.count && i < 256; ++i) {
    const uintptr_t entry = re::list_ref(l, i);
    const uintptr_t item = entry ? ref(entry, f_csi_item) : 0;
    int32_t id = 0;
    if (!item || !re::is_a(item, t_item) || !get(item, f_item_id, &id) || id != k_ticket_id) continue;
    uint8_t guid[16] = {};
    const uintptr_t g = re::field_addr(item, f_item_guid);
    const uintptr_t slot = re::field_addr(entry, f_csi_slot);
    int row = -1, column = -1;
    if (g && mem::copy_from(guid, g, 16) && !guid_empty(guid) && slot &&
        mem::read_safe(re::value_field_addr(slot, f_slot_row), &row) &&
        mem::read_safe(re::value_field_addr(slot, f_slot_col), &column) && row >= 0 && column >= 0)
      return item;
  }
  return 0;
}

int32_t upgrade_ticket_id() { return g_parts.tickets ? k_ticket_id : 0; }

int upgrade_tickets(const Inventories& inv) {
  if (!g_parts.tickets || !inv.treasure_inventory) return -1;
  re::List l;
  const uintptr_t items = ref(inv.treasure_inventory, f_treasure_items);
  if (!items || !re::read_list(items, &l)) return -1;
  int sum = 0;
  for (int i = 0; i < l.count && i < 256; ++i) {
    const uintptr_t entry = re::list_ref(l, i);
    const uintptr_t item = entry ? ref(entry, f_csi_item) : 0;
    int32_t id = 0;
    int count = 0;
    if (item && re::is_a(item, t_item) && get(item, f_item_id, &id) && id == k_ticket_id && get(item, f_item_count, &count))
      sum += count;
  }
  return sum;
}

// --- the save count -----------------------------------------------------------------------------------------------------------
uintptr_t campaign_manager() { return g_parts.saves || g_parts.difficulty ? instance(s_campaign) : 0; }

int save_count() {
  const uintptr_t m = g_parts.saves ? instance(s_campaign) : 0;
  int v = -1;
  return m && get(m, f_save_count, &v) ? v : -1;
}

bool set_save_count(int v) {
  const uintptr_t m = g_parts.saves ? instance(s_campaign) : 0;
  int old = 0, chapter = 0;
  if (!m || v < 0 || v > kSavesMax || !get(m, f_save_count, &old)) return false;
  if (!put(m, f_save_count, v)) return false;
  // The chapter's own entry moves with it (the chapter results show it), within
  // what it holds: Dictionary<ChapterID, Int32>, entries {hash, next, key, value}.
  const uintptr_t dict = ref(m, f_chapter_saves);
  if (dict && get(m, f_cur_chapter, &chapter)) {
    const uint32_t dt = re::type_of(dict);
    const re::Field fe = re::find_field(dt, "_entries"), fc = re::find_field(dt, "_count");
    re::Array a;
    int count = 0;
    const uintptr_t entries = ref(dict, fe);
    if (entries && get(dict, fc, &count) && re::read_array(entries, &a) && a.elem_size >= 16) {
      for (int i = 0; i < count && i < a.count; ++i) {
        const uintptr_t e = a.data + static_cast<uintptr_t>(i) * a.elem_size;
        int hash = -1, key = 0, val = 0;
        if (!mem::read_safe(e, &hash) || hash < 0 || !mem::read_safe(e + 8, &key) || key != chapter) continue;
        if (mem::read_safe(e + 12, &val)) {
          const int nv = val + (v - old);
          mem::store(e + 12, nv < 0 ? 0 : nv);
        }
        break;
      }
    }
  }
  return true;
}

// --- difficulty and the dynamic rank ------------------------------------------------------------------------------------------
const char* difficulty_name(int index) {
  static const char* const kNames[kDifficulties] = {"Assisted", "Standard", "Hardcore", "Professional"};
  return index >= 0 && index < kDifficulties ? kNames[index] : "?";
}
int difficulty_value(int index) { return index >= 0 && index < kDifficulties ? k_difficulty[index] : -1; }
int difficulty_index() {
  const uintptr_t m = g_parts.difficulty ? instance(s_campaign) : 0;
  int v = -1;
  if (!m || !get(m, f_difficulty, &v)) return -1;
  for (int i = 0; i < kDifficulties; ++i)
    if (k_difficulty[i] == v) return i;
  return -1;
}

bool rank(Rank* out) {
  const uintptr_t m = g_parts.rank ? instance(s_rank) : 0;
  Rank r;
  if (!m || !get(m, f_rank_value, &r.rank)) return false;
  get(m, f_rank_action, &r.action);
  const uintptr_t fixed = re::field_addr(m, f_rank_fixed);
  if (fixed) {
    uint8_t g[16] = {};
    if (mem::copy_from(g, re::value_field_addr(fixed, f_fixed_guid), 16)) r.fixed = !guid_empty(g);
  }
  r.ok = true;
  *out = r;
  return true;
}

// --- the results ----------------------------------------------------------------------------------------------------------------
namespace {
uintptr_t ongoing() {
  const uintptr_t m = g_parts.stats ? instance(s_stats) : 0;
  return m ? ref(m, f_ongoing) : 0;
}

// Each element class declares its own _Count, and a field is read only on its
// own class (re::field_addr): the element's class picks the field.
const re::Field* count_field(uintptr_t element) {
  const uint32_t t = element ? re::type_of(element) : 0;
  if (!t) return nullptr;
  for (const re::Field* f : {&f_kill_count, &f_death_count, &f_damage_count})
    if (f->valid() && re::derives(t, f->declaring)) return f;
  return nullptr;
}
}  // namespace

int element_count(uintptr_t element) {
  const re::Field* f = count_field(element);
  int v = -1;
  return f && get(element, *f, &v) ? v : -1;
}
bool set_element_count(uintptr_t element, int v) {
  const re::Field* f = count_field(element);
  return f && v >= 0 && put(element, *f, v);
}
bool accuracy(uintptr_t element, int* attempts, int* hits) {
  return element && get(element, f_attempt, attempts) && get(element, f_success, hits);
}
bool set_accuracy(uintptr_t element, int attempts, int hits) {
  return element && attempts >= 0 && hits >= 0 && put(element, f_attempt, attempts) && put(element, f_success, hits);
}

int counter_max(Counter c) { return c == Counter::kHits || c == Counter::kMisses ? 999999 : 9999; }

namespace {
// A counter's element in this chapter's stats (OngoingStats) or a finished
// chapter's record (ChapterStatsRecord): the hits and the misses share the
// accuracy element.
uintptr_t counter_element(uintptr_t stats, Counter c, bool record) {
  switch (c) {
    case Counter::kDeaths: return ref(stats, record ? f_rec_death : f_os_death);
    case Counter::kDamage: return g_parts.damage ? ref(stats, record ? f_rec_damage : f_os_damage) : 0;
    case Counter::kKills: return ref(stats, record ? f_rec_kill : f_os_kill);
    case Counter::kHits:
    case Counter::kMisses: return ref(stats, record ? f_rec_acc : f_os_acc);
  }
  return 0;
}

// Its count (-1 unknown): the misses are the shots that did not hit.
int counter_value(uintptr_t element, Counter c) {
  if (c != Counter::kHits && c != Counter::kMisses) return element_count(element);
  int at = 0, hi = 0;
  if (!accuracy(element, &at, &hi)) return -1;
  return c == Counter::kHits ? hi : at > hi ? at - hi : 0;
}

// How much more it holds: the shots fired (hits and misses) together at most 999,999.
int counter_room(uintptr_t element, Counter c) {
  if (c != Counter::kHits && c != Counter::kMisses) {
    const int v = element_count(element);
    return v >= 0 && v < counter_max(c) ? counter_max(c) - v : 0;
  }
  int at = 0, hi = 0;
  if (!accuracy(element, &at, &hi)) return 0;
  const int shots = at > hi ? at : hi;
  return shots < counter_max(c) ? counter_max(c) - shots : 0;
}

// The hits keep the misses and the misses the hits: the shots fired follow.
bool set_counter_value(uintptr_t element, Counter c, int v) {
  if (v < 0) return false;
  if (c != Counter::kHits && c != Counter::kMisses) return set_element_count(element, v > counter_max(c) ? counter_max(c) : v);
  int at = 0, hi = 0;
  if (!accuracy(element, &at, &hi)) return false;
  rules::set_shots(at, hi, c == Counter::kMisses, v, counter_max(c), &at, &hi);
  return set_accuracy(element, at, hi);
}

// The finished chapters' records, oldest first.
int finished_records(uintptr_t mgr, uintptr_t* out, int max) {
  re::List l;
  const uintptr_t hist = ref(mgr, f_history);
  int n = 0;
  if (hist && re::read_list(hist, &l))
    for (int i = 0; i < l.count && n < max; ++i)
      if (const uintptr_t r = re::list_ref(l, i)) out[n++] = r;
  return n;
}
constexpr int kMaxRecords = 64;
}  // namespace

bool stats_refs(StatsRefs* out) {
  const uintptr_t os = ongoing();
  if (!os) return false;
  StatsRefs r;
  r.kill = ref(os, f_os_kill);
  r.death = ref(os, f_os_death);
  r.accuracy = ref(os, f_os_acc);
  if (!r.kill || !r.death || !r.accuracy) return false;
  r.damage = g_parts.damage ? ref(os, f_os_damage) : 0;
  *out = r;
  return true;
}

bool stats(Stats* out) {
  const uintptr_t m = g_parts.stats ? instance(s_stats) : 0;
  const uintptr_t os = m ? ref(m, f_ongoing) : 0;
  if (!os) return false;
  Stats s;
  for (int i = 0; i < kCounters; ++i) {
    const auto c = static_cast<Counter>(i);
    s.chapter[i] = s.total[i] = counter_value(counter_element(os, c, false), c);
  }
  // The times damaged may be missing; the rest are the results.
  for (const Counter c : {Counter::kDeaths, Counter::kKills, Counter::kHits})
    if (s.chapter[static_cast<int>(c)] < 0) return false;
  uintptr_t rec[kMaxRecords];
  s.finished = finished_records(m, rec, kMaxRecords);
  for (int r = 0; r < s.finished; ++r)
    for (int i = 0; i < kCounters; ++i) {
      if (s.chapter[i] < 0) continue;
      const auto c = static_cast<Counter>(i);
      const int v = counter_value(counter_element(rec[r], c, true), c);
      if (v > 0) s.total[i] += v;
    }
  s.ok = true;
  *out = s;
  return true;
}

bool set_counter(Counter c, int v) {
  const uintptr_t os = ongoing();
  return os && set_counter_value(counter_element(os, c, false), c, v);
}

bool set_total(Counter c, int v) {
  const uintptr_t m = g_parts.stats ? instance(s_stats) : 0;
  const uintptr_t os = m ? ref(m, f_ongoing) : 0;
  if (!os || v < 0) return false;
  // The finished chapters' elements, oldest first, and this chapter's last.
  uintptr_t rec[kMaxRecords], el[kMaxRecords + 1];
  int have[kMaxRecords + 1], room[kMaxRecords + 1], want[kMaxRecords + 1];
  const int n = finished_records(m, rec, kMaxRecords);
  for (int i = 0; i <= n; ++i) {
    el[i] = i < n ? counter_element(rec[i], c, true) : counter_element(os, c, false);
    have[i] = counter_value(el[i], c);
    room[i] = counter_room(el[i], c);
  }
  if (have[n] < 0) return false;
  bool ok = rules::spread_total(have, room, n, v, want);
  for (int i = 0; i <= n; ++i)
    if (want[i] != have[i] && !set_counter_value(el[i], c, want[i])) ok = false;
  return ok;
}

uintptr_t game_over_stats() {
  const uintptr_t m = g_parts.stats ? instance(s_stats) : 0;
  return m ? ref(m, f_gameover) : 0;
}

bool game_over_death(uintptr_t gos, bool* need_take_over, uintptr_t* death_element) {
  if (!gos || !get(gos, f_go_need, need_take_over)) return false;
  *death_element = ref(gos, f_go_death);
  return true;
}

// --- the play time ----------------------------------------------------------------------------------------------------------------
namespace {
// The finished chapters' laps as the game adds them up
// (calcPrevTotalChapterStatsClearTime): the elements, oldest first. A first part
// of a Separate Ways chapter counts only while it is the latest record - once a
// later one is there, its second part's lap holds it.
int counted_laps(uintptr_t mgr, uintptr_t* laps, int max) {
  re::List l;
  const uintptr_t hist = mgr ? ref(mgr, f_history) : 0;
  if (!hist || !re::read_list(hist, &l) || l.count <= 0) return 0;
  int last = 0;
  if (const uintptr_t r = re::list_ref(l, l.count - 1)) get(r, f_rec_chapter, &last);
  int n = 0;
  for (int i = 0; i < l.count && i < 64 && n < max; ++i) {
    const uintptr_t r = re::list_ref(l, i);
    int chapter = 0;
    if (!r || !get(r, f_rec_chapter, &chapter)) continue;
    if ((chapter == k_first_parts[0] || chapter == k_first_parts[1]) && last > chapter) continue;
    if (const uintptr_t lap = ref(r, f_rec_lap)) laps[n++] = lap;
  }
  return n;
}
}  // namespace

bool clock(Clock* out) {
  if (!g_parts.clock) return false;
  const uintptr_t gc = instance(s_clock);
  const uintptr_t data = gc ? ref(gc, f_clock_data) : 0;
  const uintptr_t m = instance(s_stats);
  const uintptr_t os = m ? ref(m, f_ongoing) : 0;
  if (!data || !os) return false;
  Clock c;
  uint64_t el = 0, demo = 0, pause = 0;
  if (!get(data, f_elapsed, &el) || !get(data, f_demo, &demo) || !get(data, f_pause, &pause)) return false;
  c.now = el > demo + pause ? el - demo - pause : 0;
  c.start_element = ref(os, f_os_start);
  if (!c.start_element || !get(c.start_element, f_lap_time, &c.start)) return false;
  uintptr_t laps[64];
  const int n = counted_laps(m, laps, 64);
  for (int i = 0; i < n; ++i) {
    uint64_t t = 0;
    if (get(laps[i], f_lap_time, &t)) c.laps += t;
  }
  c.ok = true;
  *out = c;
  return true;
}

uint64_t play_time(const Clock& c) {
  const uint64_t lap = c.now > c.start ? c.now - c.start : 0;
  const uint64_t t = c.laps + lap;
  return t > kPlayTimeMax ? kPlayTimeMax : t;
}

bool set_chapter_start(uintptr_t start_element, uint64_t start) {
  return start_element && put(start_element, f_lap_time, start);
}

bool cap_laps(uint64_t sum) {
  const uintptr_t m = g_parts.clock ? instance(s_stats) : 0;
  uintptr_t laps[64];
  const int n = counted_laps(m, laps, 64);
  uint64_t total = 0;
  for (int i = 0; i < n; ++i) {
    uint64_t t = 0;
    if (get(laps[i], f_lap_time, &t)) total += t;
  }
  for (int i = n - 1; i >= 0 && total > sum; --i) {
    uint64_t t = 0;
    if (!get(laps[i], f_lap_time, &t)) continue;
    const uint64_t cut = total - sum < t ? total - sum : t;
    if (put(laps[i], f_lap_time, t - cut)) total -= cut;
  }
  return total <= sum;
}

bool raise_last_lap(uint64_t extra) {
  const uintptr_t m = g_parts.clock ? instance(s_stats) : 0;
  uintptr_t laps[64];
  const int n = counted_laps(m, laps, 64);
  for (int i = n - 1; i >= 0; --i) {
    uint64_t t = 0;
    if (get(laps[i], f_lap_time, &t)) return put(laps[i], f_lap_time, t + extra);
  }
  return false;
}

bool clock_kept(uint64_t* ahead) {
  const uintptr_t gc = g_parts.clock_copy ? instance(s_clock) : 0;
  const uintptr_t live = gc ? ref(gc, f_clock_data) : 0;
  const uintptr_t copy = gc ? ref(gc, f_clock_reset) : 0;
  if (!live || !copy || live == copy) return false;
  const re::Field* const times[4] = {&f_elapsed, &f_demo, &f_pause, &f_inventory};
  uint64_t a[4] = {}, b[4] = {};
  bool differ = false;
  for (int i = 0; i < 4; ++i) {
    if (!get(live, *times[i], &a[i]) || !get(copy, *times[i], &b[i])) return false;
    differ |= a[i] != b[i];
  }
  if (ahead) {
    // The record times: elapsed less cutscenes and pauses (clock()).
    const uint64_t run = a[0] > a[1] + a[2] ? a[0] - a[1] - a[2] : 0;
    const uint64_t save = b[0] > b[1] + b[2] ? b[0] - b[1] - b[2] : 0;
    *ahead = run > save ? run - save : 0;
  }
  return differ;
}

bool system_time(uint64_t* out) {
  const uintptr_t gc = g_parts.system_time ? instance(s_clock) : 0;
  const uintptr_t sys = gc ? ref(gc, f_clock_system) : 0;
  return sys && get(sys, f_system_elapsed, out);
}

bool set_system_time(uint64_t t) {
  const uintptr_t gc = g_parts.system_time ? instance(s_clock) : 0;
  const uintptr_t sys = gc ? ref(gc, f_clock_system) : 0;
  const uintptr_t cgc = sys ? instance(s_cgc) : 0;
  return cgc && put(sys, f_system_elapsed, t) && put(cgc, f_cgc_prev, t);
}

// --- countdowns -------------------------------------------------------------------------------------------------------------------
bool countdown(Countdown* out) {
  const uintptr_t m = g_parts.countdown ? instance(s_timer) : 0;
  Countdown c;
  int st = -1;
  if (!m || !get(m, f_t_state, &st) || !get(m, f_t_second, &c.seconds)) return false;
  c.running = st == k_t_counting || st == k_t_stop;
  c.setting = ref(m, f_t_setting);
  c.ok = true;
  *out = c;
  return true;
}

bool set_countdown(float seconds) {
  const uintptr_t m = g_parts.countdown ? instance(s_timer) : 0;
  return m && seconds >= 0.0f && put(m, f_t_second, seconds);
}

// --- the Mercenaries -----------------------------------------------------------------------------------------------------------------
namespace {
// The mode controller: a target of the delegate it puts into the manager's
// _OnTimeBonusGet as it wakes. A delegate is {.., count +0x10, then entries of
// 0x18 bytes from +0x18, each {target, code, method}} (a multicast one's
// entries); the controller is the target whose class it is.
uintptr_t merc_controller() {
  const uintptr_t m = g_parts.mercs ? instance(s_mercmgr) : 0;
  const uintptr_t d = m ? ref(m, f_mm_bonus_get) : 0;
  int32_t count = 0;
  if (!d || !mem::read_safe(d + 0x10, &count) || count < 0 || count > 64) return 0;
  for (int i = 0; i < count; ++i) {
    const uintptr_t target = mem::read_ptr(d + 0x18 + static_cast<uintptr_t>(i) * 0x18);
    if (target && re::is_a(target, t_merc_ctl)) return target;
  }
  return 0;
}
}  // namespace

namespace {
const re::Field& merc_score_field(MercScore which) {
  return which == MercScore::kScore ? f_sc_score : which == MercScore::kStockNormal ? f_sc_normal : f_sc_unique;
}

// The key of an anti-cheat integer: the low 32 bits of the object's own address, signed.
int64_t anti_cheat_key(uintptr_t obj) { return static_cast<int32_t>(static_cast<uint32_t>(obj & 0xFFFFFFFFu)); }

bool anti_cheat_get(uintptr_t holder, const re::Field& f, int* value) {
  const uintptr_t obj = ref(holder, f);
  int64_t raw = 0;
  if (!obj || !get(obj, f_ai_value, &raw)) return false;
  const int64_t key = anti_cheat_key(obj);
  if (!key) return false;
  *value = static_cast<int>(raw / key);
  return true;
}
}  // namespace

bool set_merc_score(uintptr_t score_counter, MercScore which, int value) {
  const uintptr_t obj = score_counter ? ref(score_counter, merc_score_field(which)) : 0;
  const int64_t key = obj ? anti_cheat_key(obj) : 0;
  return key && put(obj, f_ai_value, key * static_cast<int64_t>(value));
}

bool mercs(Mercs* out) {
  Mercs m;
  m.controller = merc_controller();
  if (!m.controller) return false;
  int routine = -1;
  get(m.controller, f_mc_routine, &routine);
  m.active = routine == k_routine_game;
  m.timer = ref(m.controller, f_mc_timer);
  m.combo = ref(m.controller, f_mc_combo);
  m.score = ref(m.controller, f_mc_score);
  m.rush = ref(m.controller, f_mc_rush);
  get(m.controller, f_mc_kills, &m.kills);
  if (m.timer) {
    get(m.timer, f_mt_remain, &m.remaining);
    get(m.timer, f_mt_busy, &m.timer_busy);
    get(m.timer, f_mt_timeout, &m.timed_out);
  }
  if (m.combo) {
    int st = -1;
    get(m.combo, f_cc_state, &st);
    m.combo_on = st == k_combo_accepting;
    get(m.combo, f_cc_timer, &m.combo_timer);
    get(m.combo, f_cc_count, &m.combo_count);
    get(m.combo, f_cc_max, &m.max_combo);
  }
  if (m.score && re::is_a(m.score, t_score_counter)) {
    anti_cheat_get(m.score, f_sc_score, &m.score_value);
    anti_cheat_get(m.score, f_sc_normal, &m.stock_normal);
    anti_cheat_get(m.score, f_sc_unique, &m.stock_unique);
  }
  if (m.rush) {
    get(m.rush, f_rb_busy, &m.rush_busy);
    get(m.rush, f_rb_timer, &m.rush_timer);
    get(m.rush, f_rb_limit, &m.rush_limit);
    get(m.rush, f_rb_gauges, &m.gauges);
    get(m.rush, f_rb_max, &m.gauges_max);
  }
  m.ok = true;
  *out = m;
  return true;
}

bool set_merc_remaining(uintptr_t timer, float seconds) { return timer && seconds >= 0.0f && put(timer, f_mt_remain, seconds); }
bool set_merc_combo_timer(uintptr_t combo, float t) { return combo && put(combo, f_cc_timer, t); }
bool set_rush(uintptr_t rush, float timer, int gauges) {
  if (!rush) return false;
  bool ok = true;
  if (timer >= 0.0f) ok &= put(rush, f_rb_timer, timer);
  if (gauges >= 0) ok &= put(rush, f_rb_gauges, gauges);
  return ok;
}

// --- Kill enemies as they appear -------------------------------------------------------------------------------------------
uintptr_t body_hit_controller(uintptr_t context) {
  const uintptr_t body = context && g_parts.auto_kill ? ref(context, f_ctx_body) : 0;
  const uintptr_t hc = body ? ref(body, f_body_hc) : 0;
  return hc && re::is_a(hc, t_hit_controller) ? hc : 0;
}

int hit_started(uintptr_t hc) {
  if (!hc || !g_parts.auto_kill || !re::is_a(hc, t_hit_controller)) return 0;
  const uintptr_t effect = ref(hc, f_hc_effect);
  if (!effect) return -1;
  re::Array a;
  const uintptr_t players = re::is_a(effect, t_damage_effect) ? ref(effect, f_effect_players) : 0;
  if (!players || !re::read_array(players, &a) || a.count <= 0) return 0;
  // Its start makes every one of them, in one go.
  const uintptr_t last = a.data + static_cast<uintptr_t>(a.count - 1) * a.elem_size;
  return mem::read_ptr(a.data) && mem::read_ptr(last) ? 1 : 0;
}

int kill_targets(KillTarget* out, int max, KillScan* scan) {
  *scan = KillScan{};
  int n = 0;
  const uintptr_t cm = g_parts.auto_kill ? instance(s_chars) : 0;
  re::List l;
  const uintptr_t list = cm ? ref(cm, f_enemy_list) : 0;
  if (!list || !re::read_list(list, &l)) return 0;
  scan->listed = l.count;
  for (int i = 0; i < l.count && i < 256 && n < max; ++i) {
    const uintptr_t ctx = re::list_ref(l, i);
    if (!ctx || !re::is_a(ctx, t_context)) continue;
    bool setupped = false, invincible = false, no_death = false;
    int hp = 0;
    const uintptr_t point = hitpoint_of(ctx);
    if (!get(ctx, f_ctx_setupped, &setupped) || !setupped || !point || !get(point, f_hp_cur, &hp) || hp <= 0) continue;
    // A Ganado's parasite: waiting for the Ganado's death (Ch1c0HeadUpdaterCommon.onStart
    // holds such a Ganado alive - NoDeath - until its lethal hit draws the parasite's lot),
    // on its way out (left to come), or out.
    const uintptr_t head = head_updater_of(ctx);
    int parasite = -1;
    if (!head || !re::is_a(head, t_c0_head) || !get(head, f_c0_parasite_state, &parasite)) parasite = -1;
    if (parasite == k_parasite_start) {
      ++scan->coming;
      continue;
    }
    // A hit the HitManager would turn away (HitController.get_Invincible reads this - a
    // Ganado's while its parasite comes out), or one that could not kill.
    if (get(point, f_hp_invincible, &invincible) && invincible) continue;
    if (get(point, f_hp_nodeath, &no_death) && no_death && parasite != k_parasite_wait) continue;
    KillTarget t;
    t.context = ctx;
    t.kind = kind_of(ctx);
    t.hp = hp;
    if (parasite == k_parasite_wait) t.parasite_waits = head;
    if (parasite == k_parasite_appeared) {
      // Out: hit through the Ganado's body - the game hands a hit on the parasite to the
      // Ganado's hit controller (HitController.poolDamage, its DamageToParent), the two
      // with one health (the parasite's) - once its appearance has played (onAppearance;
      // the Ganado stays invincible until its body updater's onAppearedParasite).
      // With no parasite updater to tell, the Ganado no longer invincible says it (above).
      const uintptr_t body = ref(head, f_c0_body);
      const uintptr_t updater = body ? ref(body, f_c0b_parasite) : 0;
      bool appeared = false;
      if (updater && (!get(updater, f_par_appeared, &appeared) || !appeared)) {
        ++scan->coming;
        continue;
      }
      t.parasite = true;
    }
    t.hit_controller = body_hit_controller(ctx);
    if (!t.hit_controller) continue;
    const int started = hit_started(t.hit_controller);
    if (started == 0) {
      ++scan->coming;
      continue;
    }
    if (started < 0) {
      scan->unknown_kind = t.kind;
      continue;
    }
    out[n++] = t;
  }
  return n;
}

bool keep_parasite_in(uintptr_t head) {
  int state = -1;
  if (!head || !g_parts.auto_kill || !re::is_a(head, t_c0_head) || !get(head, f_c0_parasite_state, &state) ||
      state != k_parasite_wait)
    return false;
  return put(head, f_c0_parasite_chance, 0.0f) && put(head, f_c0_parasite_force, false);
}

bool spawn_counts(uintptr_t merc_controller, SpawnCounts* out) {
  *out = SpawnCounts{};
  const uintptr_t cm = g_parts.spawn_counts ? instance(s_chars) : 0;
  const uintptr_t em = cm ? ref(cm, f_cm_enemy_mgr) : 0;
  if (!em) return false;
  get(em, f_em_active, &out->active);
  get(em, f_em_limit, &out->game_limit);
  if (merc_controller) get(merc_controller, f_mc_spawn_limit, &out->mode_limit);
  return out->active >= 0;
}

namespace {
// The mode's spawners: the controller of each SpawnerData, its setting's name hash.
template <typename Fn>
void each_spawner(uintptr_t merc_controller, Fn&& fn) {
  re::List l;
  const uintptr_t list = merc_controller && g_parts.spawners ? ref(merc_controller, f_mc_spawners) : 0;
  if (!list || !re::read_list(list, &l)) return;
  for (int i = 0; i < l.count && i < 128; ++i) {
    const uintptr_t data = re::list_ref(l, i);
    const uintptr_t spawner = data ? ref(data, f_sd_controller) : 0;
    if (!spawner || !re::is_a(spawner, t_spawner)) continue;
    const uintptr_t setting = ref(data, f_sd_setting);
    const uintptr_t name = setting ? ref(setting, f_ss_name) : 0;
    uint32_t hash = 0;
    if (name) get(name, f_nh_hash, &hash);
    fn(spawner, hash);
  }
}
// Its points' cooldown timers: {not finished, all}; with `ready_after` >= 0 one that
// has run that long is marked finished.
void cooldowns(uintptr_t spawner, float ready_after, int* cooling, int* all, int* marked) {
  re::List l;
  const uintptr_t list = ref(spawner, f_sp_cooldowns);
  if (!list || !re::read_list(list, &l)) return;
  *all = l.count;
  for (int i = 0; i < l.count && i < 512; ++i) {
    const uintptr_t timer = re::list_ref(l, i);
    bool done = true;
    float ran = 0.0f;
    if (!timer || !get(timer, f_timer_done, &done) || done) continue;
    ++*cooling;
    if (ready_after >= 0.0f && get(timer, f_timer_transit, &ran) && ran >= ready_after && re::is_a(timer, t_timer) &&
        put(timer, f_timer_done, true))
      ++*marked;
  }
}
}  // namespace

int ready_spawn_points(uintptr_t merc_controller, float cooldown) {
  int marked = 0;
  each_spawner(merc_controller, [&](uintptr_t spawner, uint32_t) {
    bool enabled = false, spent = true;
    if (!get(spawner, f_sp_enabled, &enabled) || !enabled || !get(spawner, f_sp_spent, &spent) || spent) return;
    int cooling = 0, all = 0;
    cooldowns(spawner, cooldown < 0.0f ? 0.0f : cooldown, &cooling, &all, &marked);
  });
  return marked;
}

namespace {
// The rules lift_spawn_rules changed, with the game's values, and the controller
// whose spawners they belong to.
struct LiftedPoint {
  uintptr_t point = 0;
  bool camera_only = false;
};
struct LiftedSpawner {
  uintptr_t spawner = 0;
  float distance = 0.0f;
};
LiftedPoint g_lifted_points[512];
LiftedSpawner g_lifted_spawners[96];
int g_lifted_np = 0, g_lifted_ns = 0;
uintptr_t g_lifted_controller = 0;

// A spawner's points, by its _PracticalSpawnPoints.
template <typename Fn>
void each_point(uintptr_t spawner, Fn&& fn) {
  re::Array a;
  const uintptr_t points = ref(spawner, f_sp_points);
  if (!points || !re::read_array(points, &a) || a.elem_size != 8) return;
  for (int i = 0; i < a.count && i < 512; ++i) {
    const uintptr_t point = mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8);
    if (point && re::is_a(point, t_spawn_point)) fn(point);
  }
}
}  // namespace

int lift_spawn_rules(uintptr_t merc_controller, uintptr_t spawner, float distance) {
  if (!g_parts.spawners || !merc_controller || !spawner || !re::is_a(spawner, t_spawner)) return -1;
  if (g_lifted_controller != merc_controller) {  // another stage: the old values went with it
    g_lifted_np = g_lifted_ns = 0;
    g_lifted_controller = merc_controller;
  }
  int changed = 0;
  bool known = false;
  for (int i = 0; i < g_lifted_ns; ++i) known = known || g_lifted_spawners[i].spawner == spawner;
  float d = 0.0f;
  if (!known && g_lifted_ns < 96 && get(spawner, f_sp_distance, &d) && d > distance && put(spawner, f_sp_distance, distance)) {
    g_lifted_spawners[g_lifted_ns++] = {spawner, d};
    ++changed;
  }
  each_point(spawner, [&](uintptr_t point) {
    bool camera_only = false;
    if (g_lifted_np < 512 && get(point, f_point_camera, &camera_only) && camera_only && put(point, f_point_camera, false)) {
      g_lifted_points[g_lifted_np++] = {point, true};
      ++changed;
    }
  });
  return changed;
}

int restore_spawn_rules(uintptr_t controller) {
  int put_back = 0;
  if (controller && controller == g_lifted_controller) {
    each_spawner(controller, [&](uintptr_t spawner, uint32_t) {
      for (int i = 0; i < g_lifted_ns; ++i)
        if (g_lifted_spawners[i].spawner == spawner) {
          if (put(spawner, f_sp_distance, g_lifted_spawners[i].distance)) ++put_back;
          g_lifted_spawners[i].spawner = 0;
        }
      each_point(spawner, [&](uintptr_t point) {
        for (int i = 0; i < g_lifted_np; ++i)
          if (g_lifted_points[i].point == point) {
            if (put(point, f_point_camera, g_lifted_points[i].camera_only)) ++put_back;
            g_lifted_points[i].point = 0;
          }
      });
    });
  }
  g_lifted_np = g_lifted_ns = 0;
  g_lifted_controller = 0;
  return put_back;
}

bool spawn_rules_lifted() { return g_lifted_np > 0 || g_lifted_ns > 0; }

int spawner_states(uintptr_t merc_controller, SpawnerState* out, int max) {
  int n = 0;
  each_spawner(merc_controller, [&](uintptr_t spawner, uint32_t hash) {
    if (n >= max) return;
    SpawnerState s;
    s.spawner = spawner;
    s.name_hash = hash;
    get(spawner, f_sp_enabled, &s.enabled);
    get(spawner, f_sp_spent, &s.spent);
    get(spawner, f_sp_requests, &s.requests);
    get(spawner, f_sp_active, &s.active);
    get(spawner, f_sp_limit, &s.limit);
    s.sent = list_size(ref(spawner, f_sp_spawned));
    re::Array a;
    const uintptr_t targets = ref(spawner, f_sp_targets);
    if (targets && re::read_array(targets, &a)) s.holds = a.count;
    const uintptr_t points = ref(spawner, f_sp_points);
    if (points && re::read_array(points, &a)) s.points = a.count;
    int cooling = 0, all = 0, marked = 0;
    cooldowns(spawner, -1.0f, &cooling, &all, &marked);
    s.cooling = cooling;
    out[n++] = s;
  });
  return n;
}

uintptr_t hit_manager() { return g_parts.auto_kill ? instance(s_hitmgr) : 0; }

uint32_t key_hash(const char* name) {
  // murmur3 (x86, 32 bits) over the name's UTF-16 code units (the names are ASCII).
  const size_t n = name ? std::strlen(name) : 0;
  uint32_t h = 0xFFFFFFFFu;
  auto mix = [](uint32_t k) {
    k *= 0xCC9E2D51u;
    k = (k << 15) | (k >> 17);
    return k * 0x1B873593u;
  };
  size_t i = 0;
  for (; i + 2 <= n; i += 2) {  // two characters: one 4-byte block
    h ^= mix(static_cast<uint8_t>(name[i]) | (static_cast<uint32_t>(static_cast<uint8_t>(name[i + 1])) << 16));
    h = (h << 13) | (h >> 19);
    h = h * 5 + 0xE6546B64u;
  }
  if (i < n) h ^= mix(static_cast<uint8_t>(name[i]));  // the tail: one character, two bytes
  h ^= static_cast<uint32_t>(n * 2);
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  h *= 0xC2B2AE35u;
  h ^= h >> 16;
  return h;
}

uint32_t shot_attack_key(int32_t weapon_id, bool* weapons_own) {
  if (weapons_own) *weapons_own = false;
  // HitManager.AttackDataCatalog: a DictionaryCatalog`2 whose merged dictionary
  // holds a RefCounter`1 per AttackDataID, its _Item the AttackHitUserData.
  const uintptr_t mgr = hit_manager();
  const uintptr_t catalog = mgr ? ref(mgr, f_hm_catalog) : 0;
  const uintptr_t merged = catalog ? ref(catalog, f_cat_merged) : 0;
  const uintptr_t counter = merged ? dict_ref(merged, k_attack_bullet) : 0;
  const uintptr_t data = counter ? ref(counter, f_rc_item) : 0;
  const uintptr_t list = data ? ref(data, f_ahud_list) : 0;
  re::Array a;
  if (!list || !re::read_array(list, &a)) return 0;
  const std::string name = weapon_id >= 0 && t_weapon_id ? re::enum_name(t_weapon_id, weapon_id) : std::string();
  const uint32_t want = name.empty() ? 0 : key_hash(name.c_str());
  uint32_t first_shot = 0;
  for (int i = 0; i < a.count && i < 4096; ++i) {
    const uintptr_t ad = mem::read_ptr(a.data + static_cast<uintptr_t>(i) * a.elem_size);
    uint32_t key = 0;
    int type = -1;
    if (!ad || !re::is_a(ad, t_attack_data) || !get(ad, f_ad_key, &key)) continue;
    if (want && key == want) {
      if (weapons_own) *weapons_own = true;
      return key;
    }
    if (!first_shot && key && get(ad, f_ad_type, &type) && type == k_attack_shot) first_shot = key;
  }
  return first_shot;
}

uint32_t attack_user_data_type() { return g_parts.auto_kill ? t_attack_ud : 0; }
uint32_t damage_user_data_type() { return g_parts.auto_kill ? t_damage_ud : 0; }

bool fill_kill_attack(uintptr_t attack, uint32_t key, uint32_t attack_id) {
  return attack && re::is_a(attack, t_attack_ud) && key && put(attack, f_aud_data_id, k_attack_bullet) &&
         put(attack, f_aud_key, key) && put(attack, f_aud_attack_id, attack_id);
}

bool fill_kill_damage(uintptr_t damage) {
  return damage && re::is_a(damage, t_damage_ud) && put(damage, f_dud_group, k_group_enemy) &&
         put(damage, f_dud_priority, 0) && put(damage, f_dud_parts, k_parts_chest) && put(damage, f_dud_side, k_side_center);
}

// --- steady aim ------------------------------------------------------------------------------------------------------------------------
namespace {
uintptr_t player_equipment(uintptr_t player_context) {
  const uintptr_t head = g_parts.aim ? head_updater_of(player_context) : 0;
  if (!head || !re::is_a(head, t_player_head)) return 0;
  const uintptr_t eq = ref(head, f_head_equipment);
  return eq && re::is_a(eq, t_equipment) ? eq : 0;
}

// Dictionary<WeaponID, Arms>: entries {hash, next, key, value} - an int key and a
// reference value (the value the entry's last 8 bytes). Its fields are the
// field's own class's (discover), or found by name on the dictionary's own
// class should it be another.
template <typename Fn>
void each_weapon(uintptr_t equipment, Fn fn) {
  const uintptr_t dict = ref(equipment, f_eq_weapons);
  if (!dict) return;
  re::Field fe = f_wl_entries, fc = f_wl_count;
  if (!re::field_addr(dict, fe)) {
    const uint32_t dt = re::type_of(dict);
    if (!dt) return;
    fe = re::find_field(dt, "_entries");
    fc = re::find_field(dt, "_count");
  }
  re::Array a;
  int count = 0;
  const uintptr_t entries = ref(dict, fe);
  if (!entries || !get(dict, fc, &count) || !re::read_array(entries, &a) || a.elem_size < 24) return;
  for (int i = 0; i < count && i < a.count && i < 128; ++i) {
    const uintptr_t e = a.data + static_cast<uintptr_t>(i) * a.elem_size;
    int hash = -1, key = 0;
    if (!mem::read_safe(e, &hash) || hash < 0 || !mem::read_safe(e + 8, &key)) continue;
    const uintptr_t v = mem::read_ptr(e + a.elem_size - 8);
    if (v && mem::plausible(v) && !fn(key, v)) return;
  }
}

// A field that each class of a family declares through a generic base of its
// own (the shell generators' _UserData, the shell infos' _MoveInfo): found by
// name per class, the last few kept (the tick's alone).
struct FieldPerClass {
  explicit FieldPerClass(const char* field_name) : name(field_name) {}
  const char* name;
  uint32_t cls[4] = {};
  re::Field f[4];
  int next = 0;
  re::Field of(uint32_t t) {
    for (int i = 0; i < 4; ++i)
      if (cls[i] == t) return f[i];
    const re::Field found = re::find_field(t, name);
    cls[next] = t;
    f[next] = found;
    next = (next + 1) % 4;
    return found;
  }
};
FieldPerClass g_user_data("_UserData"), g_move_info("_MoveInfo");
}  // namespace

uintptr_t equipped_gun(uintptr_t player_context, int32_t* weapon_id) {
  const uintptr_t eq = player_equipment(player_context);
  int32_t id = -1;
  if (!eq || !get(eq, f_eq_weapon_id, &id) || id < 0) return 0;
  uintptr_t gun = 0;
  each_weapon(eq, [&](int key, uintptr_t arms) {
    if (key != id) return true;
    if (re::is_a(arms, t_gun)) gun = arms;
    return false;
  });
  if (gun && weapon_id) *weapon_id = id;
  return gun;
}

int player_guns(uintptr_t player_context, uintptr_t* guns, int32_t* ids, int max) {
  const uintptr_t eq = player_equipment(player_context);
  int n = 0;
  if (!eq) return 0;
  each_weapon(eq, [&](int key, uintptr_t arms) {
    if (re::is_a(arms, t_gun)) {
      guns[n] = arms;
      ids[n] = key;
      ++n;
    }
    return n < max;
  });
  return n;
}

bool focus_reticle(uintptr_t gun) {
  if (!gun || !re::is_a(gun, t_gun)) return false;
  const uintptr_t param = ref(gun, f_arms_fit_param);
  const uintptr_t range = param ? re::field_addr(param, f_fit_range) : 0;
  float s = 0.0f, r = 0.0f, now = 0.0f;
  if (!range || !mem::read_safe(re::value_field_addr(range, f_range_s), &s) ||
      !mem::read_safe(re::value_field_addr(range, f_range_r), &r) || !get(gun, f_gun_fit, &now))
    return false;
  const float max = s + r;  // via.Range: its start, and how far it reaches
  if (!(max > 0.0f && max < 100000.0f) || now >= max) return false;
  return put(gun, f_gun_fit, max);
}

int shell_move_infos(uintptr_t gun, uintptr_t* out, int max) {
  if (!gun || !re::is_a(gun, t_gun) || max <= 0) return 0;
  const uintptr_t gen = ref(gun, f_gun_shellgen);
  const uint32_t gt = gen ? re::type_of(gen) : 0;
  if (!gt) return 0;
  const re::Field fud = g_user_data.of(gt);
  const uintptr_t ud = fud.valid() ? ref(gen, fud) : 0;
  if (!ud) return 0;
  uintptr_t infos[2] = {};
  if (re::is_a(ud, t_bullet_gen_ud)) {
    infos[0] = ref(ud, f_bud_info);
  } else if (re::is_a(ud, t_shotgun_gen_ud)) {
    infos[0] = ref(ud, f_sud_center);
    infos[1] = ref(ud, f_sud_around);
  } else if (t_rocket_gen_ud && re::is_a(ud, t_rocket_gen_ud)) {
    infos[0] = ref(ud, f_rud_info);
  }
  int n = 0;
  for (const uintptr_t info : infos) {
    // The info's own _MoveInfo (the rocket's info is another instantiation of the
    // same generic base): kept only when it is a BulletShellMoveInfo.
    const uint32_t it = info ? re::type_of(info) : 0;
    const re::Field fm = !it ? re::Field{} : re::is_a(info, t_bullet_info) ? f_info_move : g_move_info.of(it);
    const uintptr_t mi = fm.valid() ? ref(info, fm) : 0;
    if (mi && re::is_a(mi, t_move_info) && n < max) out[n++] = mi;
  }
  return n;
}

uintptr_t player_camera(uintptr_t player_context) {
  if (!g_camera_parts || !player_context) return 0;
  // The log says where the way to it failed the first time it did, and when it
  // was first reached (in play the panel cannot show either) - once each.
  static std::atomic<int> s_logged{0};  // 1: a failure logged, 2: reached logged
  auto lost = [](const char* where) -> uintptr_t {
    int none = 0;
    if (s_logged.compare_exchange_strong(none, 1))
      logf("steady aim: the player's camera controller was not reached (%s)", where);
    return 0;
  };
  const uintptr_t mcc = ref(player_context, f_ctx_camera);
  if (!mcc || !re::is_a(mcc, t_mcc)) return lost("the context's CameraController");
  const uintptr_t arr = ref(mcc, f_mcc_array);
  re::Array a;
  if (!arr || !re::read_array(arr, &a) || a.count < 1 || a.elem_size != 8) return lost("its _CameraControllerArray");
  const uintptr_t pcc = mem::read_ptr(a.data);
  if (!pcc || !re::is_a(pcc, t_pcc)) return lost("the array's first controller");
  if (s_logged.exchange(2) != 2) logf("steady aim: the player's camera controller reached");
  return pcc;
}

bool steady_camera(uintptr_t player_context) { return steady_camera_controller(player_camera(player_context)); }

bool steady_camera_controller(uintptr_t pcc) {
  if (!g_camera_parts || !pcc || !re::is_a(pcc, t_pcc)) return false;
  bool wrote = false;
  float v = 0.0f;
  if (const uintptr_t recoil = ref(pcc, f_pcc_recoil)) {
    if (get(recoil, f_recoil_yaw, &v) && v != 0.0f) wrote |= put(recoil, f_recoil_yaw, 0.0f);
    if (get(recoil, f_recoil_pitch, &v) && v != 0.0f) wrote |= put(recoil, f_recoil_pitch, 0.0f);
  }
  if (get(pcc, f_pcc_old_rs, &v) && v != 0.0f) wrote |= put(pcc, f_pcc_old_rs, 0.0f);
  if (get(pcc, f_pcc_rs_lerp, &v) && v != 0.0f) wrote |= put(pcc, f_pcc_rs_lerp, 0.0f);
  return wrote;
}

bool drop_hand_shake(uintptr_t pcc) {
  if (!g_shake_parts || !pcc || !re::is_a(pcc, t_pcc)) return false;
  const uintptr_t mcc = ref(pcc, f_root_mcc);
  bool shake = false;
  return mcc && re::is_a(mcc, t_mcc) && get(mcc, f_mcc_hand_shake, &shake) && shake &&
         put(mcc, f_mcc_hand_shake, false);
}

bool camera_aiming(uintptr_t pcc) {
  if (!g_shake_parts || !pcc || !re::is_a(pcc, t_pcc)) return false;
  const uintptr_t param = ref(pcc, f_pcc_state);
  int state = -1, gimmick = -1;
  if (!param || !get(param, f_csp_state, &state)) return false;
  if (state >= k_cam_hold && state <= k_cam_hold_last) return true;
  if (state == k_cam_hold_variation || state == k_cam_pump) return true;
  return state == k_cam_gimmick && get(param, f_csp_gimmick, &gimmick) && gimmick == k_gimmick_machine_gun;
}

bool spread(uintptr_t move_info, float* radius, float* radius_fit) {
  return move_info && get(move_info, f_move_radius, radius) && get(move_info, f_move_radius_fit, radius_fit);
}

bool set_spread(uintptr_t move_info, float radius, float radius_fit) {
  return move_info && re::is_a(move_info, t_move_info) && put(move_info, f_move_radius, radius) &&
         put(move_info, f_move_radius_fit, radius_fit);
}

// --- costumes ------------------------------------------------------------------------------------------------------------------------
bool costume_change_unsafe() {
  for (const uintptr_t ctx : {player_context(), ashley_context()}) {
    if (!ctx) continue;
    bool changing = false;
    if (get(ctx, f_ctx_costume_changing, &changing) && changing) return true;
    int64_t state = 0;
    if (re::is_a(ctx, t_player_base_ctx) && get(ctx, f_pbc_state, &state) && (state & k_unsafe_states)) return true;
  }
  return false;
}

bool costume_menu(bool open, bool held) {
  if (!g_parts.costumes) return false;
  const uintptr_t gcm = instance(s_costume_gui);
  const uintptr_t dict = gcm ? ref(gcm, f_gcm_dict) : 0;
  bool wrote = false;
  // An entry the dictionary no longer holds is the game's to drop: forgotten, never written.
  int kept = 0;
  for (int i = 0; i < g_costumes_opened; ++i) {
    const CostumeOpened& o = g_costume_opened[i];
    const bool live = dict && dict_ref(dict, o.kind) == o.info && re::is_a(o.info, t_csm);
    if (!live) continue;
    if (!open) {
      wrote |= put(o.info, f_csm_situation, o.situation);
      if (const uintptr_t r = re::field_addr(o.info, f_csm_reason)) wrote |= mem::copy_to(r, o.reason, 16);
      continue;
    }
    g_costume_opened[kept++] = o;
  }
  g_costumes_opened = kept;
  if (!open || !dict) return wrote;
  // The text a greyed entry shows: the accessories' ("unavailable in certain situations").
  uint8_t reason[16] = {};
  bool have_reason = false;
  for (const int kind : k_accessory_kinds) {
    const uintptr_t acc = have_reason ? 0 : dict_ref(dict, kind);
    const uintptr_t r = acc && re::is_a(acc, t_csm) ? re::field_addr(acc, f_csm_reason) : 0;
    have_reason = r && mem::copy_from(reason, r, 16);
  }
  const int want = held ? k_sit_main_only : k_sit_except;
  for (const int kind : k_costume_kinds) {
    const uintptr_t info = dict_ref(dict, kind);
    if (!info || !re::is_a(info, t_csm)) continue;
    CostumeOpened* o = nullptr;
    for (int i = 0; i < g_costumes_opened && !o; ++i)
      if (g_costume_opened[i].info == info) o = &g_costume_opened[i];
    int now = -1;
    if (!get(info, f_csm_situation, &now)) continue;
    if (!o) {
      // Only an entry the game keeps to the main menu; any other is the game's choice.
      if (now != k_sit_main_only || g_costumes_opened >= 5) continue;
      o = &g_costume_opened[g_costumes_opened++];
      o->info = info;
      o->kind = kind;
      o->situation = now;
      if (const uintptr_t r = re::field_addr(info, f_csm_reason)) mem::copy_from(o->reason, r, 16);
    }
    if (now != want) wrote |= put(info, f_csm_situation, want);
    if (have_reason) {
      uint8_t cur[16] = {};
      const uintptr_t r = re::field_addr(info, f_csm_reason);
      if (r && mem::copy_from(cur, r, 16) && std::memcmp(cur, reason, 16) != 0) wrote |= mem::copy_to(r, reason, 16);
    }
  }
  return wrote;
}

// --- the dodge prompts ------------------------------------------------------------------------------------------------------------------
bool press_dodge(uintptr_t player_context) {
  if (!g_parts.action_sign) return false;
  const uintptr_t head = head_updater_of(player_context);
  if (!head || !re::is_a(head, t_player_head)) return false;
  const uintptr_t as = ref(head, f_head_action_sign);
  const uintptr_t avoid = as ? ref(as, f_as_avoid) : 0;
  if (!avoid) return false;
  bool enable = false, large = false, accepting = false, reserve = false, cant = false;
  if (!get(avoid, f_info_enable, &enable) || !enable) return false;
  if (!get(avoid, f_info_large, &large) || !large) return false;
  get(as, f_as_cant_parry, &cant);
  if (cant) return false;
  if (!get(avoid, f_info_accepting, &accepting) || accepting) return false;
  if (!get(avoid, f_info_reserve, &reserve) || reserve) return false;
  return put(avoid, f_info_reserve, true);
}

}  // namespace re4cc::game
