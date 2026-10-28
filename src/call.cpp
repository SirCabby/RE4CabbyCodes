#include "call.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "dispatch.h"
#include "log.h"
#include "mem.h"
#include "game.h"
#include "re.h"

namespace re4cc::call {
namespace {

// The VM's helpers (build 22377325: get_thread_context exe+0x39D9E30, begin
// frame exe+0x3A1C640, exception handler exe+0x3A222C0, local frame gc
// exe+0x3A20100, end frame exe+0x3A1D600) and the thread context's fields they
// use (reference count +0x78, frame +0x50, the frame's pending exception +0x18 -
// RE2's and RE3's offsets).
struct Vm {
  uintptr_t get_thread_context = 0, begin_frame = 0, unhandled = 0, local_gc = 0, end_frame = 0;
  uint32_t refcount_off = 0, frame_off = 0, exception_off = 0;
} V;

// The methods, each found by name and parameter types (0: not found - the
// feature that needs it says so).
struct Methods {
  // The attache case and the lists off it.
  uintptr_t generate = 0, case_add = 0, case_remove = 0, change_count = 0, update_reload = 0, change_size = 0;
  uintptr_t key_add = 0, key_remove = 0, treasure_add = 0, treasure_remove = 0, unique_add = 0, unique_remove = 0;
  // A weapon's upgrades.
  uintptr_t set_common = 0, set_individual = 0, set_limit = 0, max_common = 0, max_individual = 0, max_limit = 0,
            customized = 0;
  // The difficulty.
  uintptr_t set_difficulty = 0, recalc_rank = 0;
  // The save files.
  uintptr_t remove_single = 0, refresh_table = 0, slot_info = 0, add_slot_info = 0, clear_info = 0;
  // The engine's copy of a save.
  uintptr_t type_from_handle = 0, create_instance = 0, access_ok = 0, suspend_enter = 0, suspend_exit = 0,
            key_string = 0, clean_data = 0, add_data = 0, load_single = 0, save_single = 0, save_state = 0,
            last_result = 0, set_size = 0, system_save = 0;
  // Automatic QTEs.
  uintptr_t set_value = 0, laser_hash = 0, hookshot_hash = 0, reserve_avoid = 0, large_normal = 0, try_add = 0,
            knife_gui = 0;
  // The shooting gallery.
  uintptr_t get_game_object = 0, get_transform = 0, get_position = 0;
  // The challenges.
  uintptr_t achievement = 0;
  // The item box: the game's storage, and the save's item list.
  uintptr_t storage_add = 0, storage_reduce = 0, storage_accepts = 0, item_list_add = 0;
  // Kill enemies as they appear: a hit from the player.
  uintptr_t attack_id = 0, request_attack = 0;
  // The Extra Content Shop: its page shown again.
  uintptr_t shop_page = 0, shop_caption = 0;
  uint32_t reserve_parry = 0;  // a method index: virtual, called through the object's own table
} M;
int32_t g_scroll_none = 0;  // via.gui.GridScrollDirection.None
uint32_t t_additional = 0;
re::Field f_option_manager;
uint32_t t_weapon_custom = 0, t_rank_system = 0, t_add_result = 0, t_system_save_args = 0;
re::Field f_add_count;

std::atomic<bool> g_vm{false}, g_failed{false};
char g_status[240] = "not searched yet";  // the VM's helpers, or a call that failed
mem::Range g_text;

void set_status(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void set_status(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(g_status, sizeof(g_status), fmt, a);
  va_end(a);
  logf("call: %s", g_status);
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
unsigned long long rva(uintptr_t a) { return static_cast<unsigned long long>(a - exe_base()); }

uintptr_t rel_target(uintptr_t rel32_at) { return rel32_at + 4 + static_cast<intptr_t>(mem::read<int32_t>(rel32_at)); }

// The most common key and its share, or 0.
template <typename K>
K winner(const std::unordered_map<K, int>& votes, int* count, int* total) {
  K best{};
  *count = 0;
  *total = 0;
  for (const auto& [k, n] : votes) {
    *total += n;
    if (n > *count) {
      *count = n;
      best = k;
    }
  }
  return best;
}
template <typename K>
bool clear_winner(const std::unordered_map<K, int>& votes, int min_count, K* out, int* count, int* total) {
  *out = winner(votes, count, total);
  return *count >= min_count && *count * 10 >= *total * 9;
}

// Each E8 call in .text whose target is `target`, handed to fn(call site).
template <typename F>
void each_call_to(uintptr_t target, F fn) {
  const auto* const end = reinterpret_cast<const uint8_t*>(g_text.end) - 5;
  for (auto* c = reinterpret_cast<const uint8_t*>(g_text.begin); c < end; ++c) {
    c = static_cast<const uint8_t*>(std::memchr(c, 0xE8, static_cast<size_t>(end - c)));
    if (!c) break;
    const uintptr_t site = reinterpret_cast<uintptr_t>(c);
    if (rel_target(site + 1) == target) fn(site);
  }
}

bool find_vm_helpers() {
  // get_thread_context: `mov rcx,[vm]; mov edx,-1; call X`, every one the same X.
  std::unordered_map<uintptr_t, int> gtc_votes;
  const auto vm_sites = mem::find_all(g_text, "48 8B 0D ?? ?? ?? ?? BA FF FF FF FF E8", 1024);
  for (const uintptr_t s : vm_sites) ++gtc_votes[rel_target(s + 13)];
  int n = 0, total = 0;
  if (!clear_winner(gtc_votes, 20, &V.get_thread_context, &n, &total) || !g_text.contains(V.get_thread_context)) {
    set_status("the VM's get_thread_context was not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  // begin frame: right after it, `jne +8; mov rcx,rax; call X` (the first reference).
  std::unordered_map<uintptr_t, int> begin_votes;
  for (const uintptr_t s : vm_sites) {
    if (rel_target(s + 13) != V.get_thread_context) continue;
    for (uintptr_t p = s + 17; p < s + 17 + 24; ++p)
      if (mem::matches(p, "75 08 48 8B C8 E8")) {
        ++begin_votes[rel_target(p + 6)];
        break;
      }
  }
  if (!clear_winner(begin_votes, 20, &V.begin_frame, &n, &total) || !g_text.contains(V.begin_frame)) {
    set_status("the VM's frame begin was not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  // The reference count: the field every caller of begin compares with 0 just
  // before it - `cmp dword [reg+d8],0` or `cmp [reg+d8],reg32`, then `jne +8`.
  std::unordered_map<uint32_t, int> rc_votes;
  each_call_to(V.begin_frame, [&](uintptr_t site) {
    if (!mem::matches(site - 5, "75 08 48 8B C8")) return;
    const auto* b = reinterpret_cast<const uint8_t*>(site - 9);  // the 4 bytes before `jne`
    if (b[0] == 0x83 && (b[1] & 0xF8) == 0x78 && b[3] == 0x00) ++rc_votes[b[2]];          // 83 /7 d8 00
    else if (b[1] == 0x39 && (b[2] & 0xC0) == 0x40) ++rc_votes[b[3]];                      // [REX] 39 modrm d8
  });
  if (!clear_winner(rc_votes, 20, &V.refcount_off, &n, &total)) {
    set_status("the thread context's reference count was not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  // The exception handler hands the frame's pending exception to the VM and clears
  // it. Build 22377325: `mov rdx,rcx; mov rcx,[rcx+V]; mov rax,[rdx+F]; mov r8,[rax+E];
  // mov qword [rax+E],0; jmp` (RE2's and RE3's had the same instructions in another
  // order: `mov rax,[rcx+F]; mov rdx,rcx; mov r8,[rax+E]; mov qword [rax+E],0;
  // mov rcx,[rcx+V]; jmp`).
  auto unh = mem::find_all(g_text, "48 8B D1 48 8B 49 ?? 48 8B 42 ?? 4C 8B 40 ?? 48 C7 40 ?? 00 00 00 00 E9", 4);
  if (unh.size() == 1 && mem::read<uint8_t>(unh[0] + 14) == mem::read<uint8_t>(unh[0] + 18)) {
    V.unhandled = unh[0];
    V.frame_off = mem::read<uint8_t>(unh[0] + 10);
    V.exception_off = mem::read<uint8_t>(unh[0] + 14);
  } else {
    unh = mem::find_all(g_text, "48 8B 41 ?? 48 8B D1 4C 8B 40 ?? 48 C7 40 ?? 00 00 00 00 48 8B 49 ?? E9", 4);
    if (unh.size() != 1 || mem::read<uint8_t>(unh[0] + 10) != mem::read<uint8_t>(unh[0] + 14)) {
      set_status("the VM's exception handler was not found (%u matches) - no game calls", static_cast<unsigned>(unh.size()));
      return false;
    }
    V.unhandled = unh[0];
    V.frame_off = mem::read<uint8_t>(unh[0] + 3);
    V.exception_off = mem::read<uint8_t>(unh[0] + 10);
  }
  // Local frame gc and end frame: the last reference - `call handler; mov rcx,reg; call gc; mov rcx,reg; call end`.
  std::unordered_map<uint64_t, int> tail_votes;
  auto mov_rcx = [](uintptr_t p) {  // [REX.W] 8B modrm(11 001 rrr)
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    return (b[0] == 0x48 || b[0] == 0x49) && b[1] == 0x8B && (b[2] & 0xF8) == 0xC8;
  };
  each_call_to(V.unhandled, [&](uintptr_t site) {
    if (!mov_rcx(site + 5) || mem::read<uint8_t>(site + 8) != 0xE8 || !mov_rcx(site + 13) ||
        mem::read<uint8_t>(site + 16) != 0xE8)
      return;
    const uint64_t gc = rel_target(site + 9) - exe_base(), end = rel_target(site + 17) - exe_base();
    ++tail_votes[(gc << 32) | end];
  });
  uint64_t tail = 0;
  if (!clear_winner(tail_votes, 20, &tail, &n, &total)) {
    set_status("the VM's local frame gc and frame end were not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  V.local_gc = exe_base() + static_cast<uintptr_t>(tail >> 32);
  V.end_frame = exe_base() + static_cast<uintptr_t>(tail & 0xFFFFFFFF);
  if (!g_text.contains(V.local_gc) || !g_text.contains(V.end_frame)) {
    set_status("the VM's frame helpers are not in the game's code - no game calls");
    return false;
  }
  logf("call: VM helpers: get_thread_context exe+0x%llX, begin frame exe+0x%llX, exception handler exe+0x%llX, "
       "local frame gc exe+0x%llX, end frame exe+0x%llX; thread context: reference count +0x%X, frame +0x%X "
       "(its exception +0x%X)",
       rva(V.get_thread_context), rva(V.begin_frame), rva(V.unhandled), rva(V.local_gc), rva(V.end_frame),
       V.refcount_off, V.frame_off, V.exception_off);
  return true;
}

// A method's code, by name and parameter types - 0 when the database has no such
// method or it has no code in the game's image (logged).
uintptr_t find_code(const char* type_name, const char* name, std::initializer_list<const char*> params,
                    bool is_static = false) {
  const uint32_t t = re::find_type(type_name);
  const uintptr_t code = t ? re::method_code_typed(t, name, params, is_static) : 0;
  if (!code || !g_text.contains(code)) {
    logf("call: %s.%s was not found in the type database, or has no code", type_name, name);
    return 0;
  }
  return code;
}

void find_methods() {
  // The attache case and the lists off it.
  M.generate = find_code("chainsaw.ChainsawGuiUtil", "generateItem",
                         {"chainsaw.ItemID", "System.Int32", "System.Int32", "chainsaw.ItemID", "System.Int32",
                          "System.Boolean"},
                         true);
  M.case_add = find_code("chainsaw.CsInventoryController", "add", {"chainsaw.Item", "chainsaw.InventorySlotType"});
  M.case_remove = find_code("chainsaw.CsInventoryController", "remove", {"System.Guid"});
  M.change_count = find_code("chainsaw.gui.inventory.CsInventory", "changeCount", {"System.Guid", "System.Int32"});
  M.update_reload = find_code("chainsaw.gui.inventory.CsInventory", "updateReloadInfos", {});
  M.change_size = find_code("chainsaw.CsInventoryController", "changeSize", {"chainsaw.AttacheCaseSize", "System.Boolean"});
  M.key_add = find_code("chainsaw.KeyItemInventoryController", "add", {"chainsaw.Item"});
  M.key_remove = find_code("chainsaw.KeyItemInventoryController", "remove", {"System.Guid"});
  M.treasure_add = find_code("chainsaw.TreasureInventoryController", "add", {"chainsaw.Item"});
  M.treasure_remove = find_code("chainsaw.TreasureInventoryController", "remove", {"System.Guid"});
  M.unique_add = find_code("chainsaw.UniqueInventoryController", "add", {"chainsaw.Item"});
  M.unique_remove = find_code("chainsaw.UniqueInventory", "remove", {"System.Guid"});
  t_add_result = re::find_type("chainsaw.AddItemResult");
  f_add_count = re::find_field(t_add_result, "AddCount");
  // A weapon's upgrades.
  t_weapon_custom = re::find_type("chainsaw.WeaponCustomManager");
  M.set_common = find_code("chainsaw.WeaponCustomManager", "setCommomCustomLevel",
                           {"chainsaw.CommonCustomCategory", "System.Int32", "chainsaw.CustomLevelInWeapon", "chainsaw.WeaponID"});
  M.set_individual = find_code("chainsaw.WeaponCustomManager", "setIndividualCustomLevel",
                               {"chainsaw.IndividualCustomCategory", "System.Int32", "chainsaw.CustomLevelInWeapon",
                                "chainsaw.WeaponID"});
  M.set_limit = find_code("chainsaw.WeaponCustomManager", "setLimitBreakCustomLevel",
                          {"chainsaw.LimitBreakCustomCategory", "System.Int32", "chainsaw.CustomLevelInWeapon"});
  M.max_common = find_code("chainsaw.WeaponCustomManager", "getMaxCustomLevel", {"chainsaw.WeaponID", "chainsaw.CommonCustomCategory"});
  M.max_individual =
      find_code("chainsaw.WeaponCustomManager", "getMaxCustomLevel", {"chainsaw.WeaponID", "chainsaw.IndividualCustomCategory"});
  M.max_limit =
      find_code("chainsaw.WeaponCustomManager", "getMaxCustomLevel", {"chainsaw.WeaponID", "chainsaw.LimitBreakCustomCategory"});
  M.customized = find_code("chainsaw.WeaponItem", "onCustomized", {});
  // The difficulty.
  M.set_difficulty = find_code("chainsaw.CampaignManager", "setDifficulty", {"chainsaw.CampaignManager.Difficulty"});
  t_rank_system = re::find_type("chainsaw.GameRankSystem");
  M.recalc_rank = find_code("chainsaw.GameRankSystem", "recalcRank", {});
  // The save files.
  M.remove_single = find_code("via.storage.saveService.SaveService", "removeSingle", {"System.Int32"}, true);
  M.refresh_table = find_code("via.storage.saveService.SaveService", "updateSaveFileDetailTbl", {}, true);
  M.slot_info = find_code("chainsaw.SaveLoadMenuGuiManager", "getSaveSlotInfo", {"System.Int32"});
  M.add_slot_info = find_code("chainsaw.SaveLoadMenuGuiManager", "addSaveSlotInfo", {"chainsaw.gui.SaveSlotInfo"});
  M.clear_info = find_code("chainsaw.SaveLoadMenuGuiManager", "clearInfo", {"System.Int32"});
  // The engine's copy of a save.
  const char* const kService = "via.storage.saveService.SaveService";
  M.type_from_handle = find_code("System.Type", "GetTypeFromHandle", {"System.RuntimeTypeHandle"}, true);
  M.create_instance = find_code("System.Activator", "CreateInstance", {"System.Type"}, true);
  M.access_ok = find_code("share.SaveDataUtil", "isEnableAccessSaveFileDetailTbl", {}, true);
  M.suspend_enter = find_code("share.SaveDataManager", "entrySuspendGameSave", {"System.Object"});
  M.suspend_exit = find_code("share.SaveDataManager", "exitSuspendGameSave", {"System.Object"});
  M.key_string = find_code("share.SaveDataManagerEnumExtension", "getKeyString", {"share.SaveLoadKey"}, true);
  M.clean_data = find_code(kService, "cleanData", {}, true);
  M.add_data = find_code(kService, "addDataClass", {"System.String", "System.Object"}, true);
  M.load_single = find_code(kService, "loadSingle", {"System.Int32"}, true);
  M.save_single = find_code(kService, "saveSingle", {"System.Int32"}, true);
  M.save_state = find_code(kService, "get_SaveState", {}, true);
  M.last_result = find_code(kService, "get_LastSaveResult", {}, true);
  M.set_size = find_code(kService, "set_SaveDataSize", {"System.UInt64"}, true);
  M.system_save = find_code("share.SaveDataManager", "requestSaveSystemData",
                            {"share.AppSaveSlot", "share.SystemSaveRequestArgs"});
  t_system_save_args = re::find_type("share.SystemSaveRequestArgs");
  // Automatic QTEs.
  M.set_value = find_code("share.hid.Command", "setValue", {"System.UInt32"});
  M.laser_hash = find_code("chainsaw.GmQTELaserTrap", "calcAvoidCommandHash", {"chainsaw.PlayerCommandType"});
  M.hookshot_hash = find_code("chainsaw.AdditionalOptionManager", "getCp11HSCommandHash", {});
  M.reserve_avoid = find_code("chainsaw.PlayerHeadActionSign", "requestReserveAvoid", {});
  M.large_normal = find_code("chainsaw.PlayerHeadActionSign.ActionSignGuiUnit", "checkNormalActionSignDispLargeUI", {});
  M.try_add = find_code("System.Collections.Concurrent.ConcurrentDictionary`2<System.UInt32,System.Boolean>", "TryAdd",
                        {"System.UInt32", "System.Boolean"});
  M.knife_gui = find_code("chainsaw.EnemyBehaviorTreeAction_MFSM_CheckEscapeGrapple.KnifeEscapeCondition", "canOpenGui",
                          {"chainsaw.PlayerHeadUpdater"});
  // The shooting gallery.
  M.get_game_object = find_code("via.Component", "get_GameObject", {});
  M.get_transform = find_code("via.GameObject", "get_Transform", {});
  M.get_position = find_code("via.Transform", "get_Position", {});
  {
    const uint32_t t = re::find_type("chainsaw.PlayerHeadActionSign");
    M.reserve_parry = t ? re::find_method(t, "requestReserveParry", {}) : 0;
    if (!M.reserve_parry || re::method_vt_index(M.reserve_parry) < 0) {
      logf("call: chainsaw.PlayerHeadActionSign.requestReserveParry was not found as a virtual method");
      M.reserve_parry = 0;
    }
  }
  t_additional = re::find_type("chainsaw.AdditionalManager");
  f_option_manager = re::find_field(t_additional, "_OptionManager");
  logf("call: Automatic QTEs' calls: press %d, laser hash %d, hookshot hash %d, reserve avoid %d, reserve parry %d, "
       "large normal sign %d, trigger %d, knife escape %d",
       M.set_value != 0, M.laser_hash != 0, M.hookshot_hash != 0, M.reserve_avoid != 0, M.reserve_parry != 0,
       M.large_normal != 0, M.try_add != 0, M.knife_gui != 0);
  logf("call: the shooting gallery's calls: a target's position %d",
       M.get_game_object && M.get_transform && M.get_position);
  M.achievement = find_code("chainsaw.AchievementServiceController", "unlock", {"System.Int32"});
  logf("call: the challenges' call: Steam achievements %d", M.achievement != 0);
  M.storage_add = find_code("chainsaw.ArmouryManager", "addArmouryItem", {"chainsaw.Item"});
  M.storage_reduce = find_code("chainsaw.ArmouryManager", "reduceArmouryItem", {"chainsaw.Item", "System.Int32"});
  M.storage_accepts = find_code("chainsaw.ItemExtension", "isEnableMoveToArmoury", {"chainsaw.Item"}, true);
  M.item_list_add = find_code("System.Collections.Generic.List`1<chainsaw.Item>", "Add", {"chainsaw.Item"});
  logf("call: the item box's calls: the game's storage %d (store exe+0x%llX, take exe+0x%llX, its rule exe+0x%llX), "
       "the save's item list %d (Add exe+0x%llX)",
       M.storage_add && M.storage_reduce && M.storage_accepts, rva(M.storage_add), rva(M.storage_reduce),
       rva(M.storage_accepts), M.item_list_add != 0, rva(M.item_list_add));
  M.attack_id = find_code("chainsaw.HitManager", "generateAttackID", {});
  M.request_attack = find_code("chainsaw.HitController", "requestAttack",
                               {"via.GameObject", "chainsaw.collision.AttackUserData", "chainsaw.collision.DamageUserData"});
  logf("call: Kill enemies as they appear's calls: a hit from the player %d (requestAttack exe+0x%llX, generateAttackID "
       "exe+0x%llX)",
       kill_ready(), rva(M.request_attack), rva(M.attack_id));
  const char* const kShop = "chainsaw.gui.ExContentsShopGuiBehavior";
  M.shop_page = find_code(kShop, "updateCurrentGrid", {"System.Int32", "via.gui.GridScrollDirection"});
  M.shop_caption = find_code(kShop, "setCaptionMessage", {"chainsaw.ExShopBonusID"});
  g_scroll_none = re::enum_value(re::find_type("via.gui.GridScrollDirection"), "None", 0);
  logf("call: the shop window's calls: the shop's page shown again %d (updateCurrentGrid exe+0x%llX, setCaptionMessage "
       "exe+0x%llX)",
       shop_ready(), rva(M.shop_page), rva(M.shop_caption));
  logf("call: methods: items %d (generate exe+0x%llX, case add exe+0x%llX, remove exe+0x%llX), upgrades %d, "
       "difficulty %d, save files %d (removeSingle exe+0x%llX), the engine's copy of a save %d",
       items_ready(), rva(M.generate), rva(M.case_add), rva(M.case_remove), levels_ready(), difficulty_ready(),
       saves_ready(), rva(M.remove_single), copy_ready());
}

// The VM's frame, as the engine's native-to-managed bridges keep it: begun on
// the thread context's first reference, and on its last one the pending
// exception handled, the local frame collected (what the calls made and nothing
// else holds is let go) and the frame ended.
void call_helper(uintptr_t fn, uintptr_t ctx) { reinterpret_cast<void (*)(uintptr_t)>(fn)(ctx); }

bool exception_in(uintptr_t ctx) {
  const uintptr_t frame = mem::read_ptr(ctx + V.frame_off);
  return frame && mem::readable(frame + V.exception_off, 8) && mem::read_ptr(frame + V.exception_off) != 0;
}

thread_local int t_in_hook = 0;
// The VM's context for this thread as last checked: the VM keeps one per
// thread, so it is checked again only when the VM hands this thread another.
// The check is a VirtualQuery, which under Wine scans the page table (5-15 us in
// the game's heap) - and calls come in bursts: a prompt's every frame, an item
// box's save.
thread_local uintptr_t t_ctx_checked = 0;

uintptr_t enter_frame(const char* what) {
  if (!g_vm || g_failed || !(dispatch::in_tick() || t_in_hook > 0)) return 0;
  const uintptr_t vm = re::vm();
  if (!vm) return 0;
  using GetCtx = uintptr_t (*)(uintptr_t vm, int32_t unk);
  const uintptr_t ctx = reinterpret_cast<GetCtx>(V.get_thread_context)(vm, -1);
  if (!ctx ||
      (ctx != t_ctx_checked && (!mem::writable(ctx + V.refcount_off, 4) || !mem::readable(ctx + V.frame_off, 8)))) {
    logf("call: %s: no VM thread context on thread %lu", what, GetCurrentThreadId());
    return 0;
  }
  t_ctx_checked = ctx;
  const int32_t refs = mem::read<int32_t>(ctx + V.refcount_off);
  if (refs == 0) call_helper(V.begin_frame, ctx);
  mem::store<int32_t>(ctx + V.refcount_off, refs + 1);
  if (exception_in(ctx)) call_helper(V.unhandled, ctx);  // one left over from before: the engine reports it here too
  return ctx;
}

void leave_frame(uintptr_t ctx) {
  if (!ctx) return;
  const int32_t refs = mem::read<int32_t>(ctx + V.refcount_off) - 1;
  mem::store<int32_t>(ctx + V.refcount_off, refs);
  if (refs == 0) {
    if (exception_in(ctx)) call_helper(V.unhandled, ctx);
    call_helper(V.local_gc, ctx);
    call_helper(V.end_frame, ctx);
  }
}

// A call's frame. Opened only for a method that was found (`code`); inside a
// Frame it is that frame.
class Scope {
 public:
  Scope(const char* what, uintptr_t code) : what_(what) {
    if (code) ctx_ = enter_frame(what);
  }
  ~Scope() { leave_frame(ctx_); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  uintptr_t ctx() const { return ctx_; }
  // After the call: true when it returned normally. An exception it left is
  // named in the log, handed to the VM's handler, and ends the mod's calls.
  bool returned() {
    if (!exception_in(ctx_)) return true;
    const uintptr_t ex = mem::read_ptr(mem::read_ptr(ctx_ + V.frame_off) + V.exception_off);
    const uint32_t t = re::type_of(ex);
    g_failed = true;
    std::snprintf(g_status, sizeof(g_status), "%s threw %s - game calls are off until the game restarts", what_,
                  t ? re::full_name(t).c_str() : "an exception");
    logf("ERROR: call: %s", g_status);
    call_helper(V.unhandled, ctx_);
    return false;
  }

 private:
  const char* what_;
  uintptr_t ctx_ = 0;
};

}  // namespace

bool init() {
  if (g_vm) return true;
  if (!re::ready() || !re::vm()) {
    set_status("waiting for the VM");
    return false;
  }
  g_text = mem::section(GetModuleHandleA(nullptr), ".text");
  if (!find_vm_helpers()) return false;
  g_vm = true;
  find_methods();
  set_status("ready");
  return true;
}

InHook::InHook() { ++t_in_hook; }
InHook::~InHook() { --t_in_hook; }

Frame::Frame() : ctx_(enter_frame("a change")) {}
Frame::~Frame() { leave_frame(ctx_); }

bool ready() { return g_vm && !g_failed; }
const char* status() { return g_status; }
bool items_ready() {
  return ready() && M.generate && M.case_add && M.case_remove && M.change_count && M.update_reload && f_add_count.valid();
}
bool levels_ready() { return ready() && M.set_common && M.set_individual && M.set_limit && M.customized && t_weapon_custom; }
bool difficulty_ready() { return ready() && M.set_difficulty && M.recalc_rank; }
bool saves_ready() { return ready() && M.remove_single && M.refresh_table && M.slot_info && M.add_slot_info; }
bool copy_ready() {
  return saves_ready() && M.clear_info && M.type_from_handle && M.create_instance && M.access_ok && M.suspend_enter &&
         M.suspend_exit && M.key_string && M.clean_data && M.add_data && M.load_single && M.save_single &&
         M.save_state && M.last_result && M.set_size && M.system_save && t_system_save_args;
}
bool system_save_ready() {
  return ready() && M.system_save && M.type_from_handle && M.create_instance && t_system_save_args;
}
bool achievements_ready() { return ready() && M.achievement; }
bool storage_ready() { return items_ready() && M.storage_add && M.storage_reduce && M.storage_accepts; }
bool box_save_ready() { return ready() && M.generate && M.item_list_add && levels_ready(); }
bool kill_ready() {
  return ready() && M.attack_id && M.request_attack && M.get_game_object && M.type_from_handle && M.create_instance;
}
bool shop_ready() { return ready() && M.shop_page && M.shop_caption; }

bool exception_pending(uintptr_t ctx) {
  if (!ctx || !V.frame_off) return false;  // the offsets come with the VM's helpers (init)
  const uintptr_t frame = mem::read_ptr(ctx + V.frame_off);
  return frame && mem::read_ptr(frame + V.exception_off) != 0;
}

namespace {
int add_count(uintptr_t result) {
  int n = 0;
  return result && re::is_a(result, t_add_result) && game::get(result, f_add_count, &n) ? n : 0;
}
}  // namespace

uintptr_t generate_item(int32_t id, int count, int durability, int32_t ammo_id, int ammo_count) {
  Scope s("ChainsawGuiUtil.generateItem", M.generate);
  if (!s.ctx() || count <= 0) return 0;
  const uintptr_t item = reinterpret_cast<uintptr_t (*)(uintptr_t, int32_t, int32_t, int32_t, int32_t, int32_t, bool)>(
      M.generate)(s.ctx(), id, count, durability, ammo_id, ammo_count, false);
  return s.returned() ? item : 0;
}

int case_add(uintptr_t cs_controller, uintptr_t item) {
  Scope s("CsInventoryController.add", M.case_add);
  if (!s.ctx() || !cs_controller || !item) return -1;
  const uintptr_t r = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, int32_t)>(M.case_add)(
      s.ctx(), cs_controller, item, 0 /* InventorySlotType.Default */);
  return s.returned() ? add_count(r) : -1;
}

// A Guid (16 bytes) goes by reference, as the x64 convention passes any struct
// wider than 8 bytes: a copy of the mod's own.
bool case_remove(uintptr_t cs_controller, const uint8_t guid[16]) {
  Scope s("CsInventoryController.remove", M.case_remove);
  if (!s.ctx() || !cs_controller) return false;
  alignas(16) uint8_t g[16];
  std::memcpy(g, guid, 16);
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, const uint8_t*)>(M.case_remove)(s.ctx(), cs_controller, g);
  return s.returned();
}

bool case_change_count(uintptr_t cs_inventory, const uint8_t guid[16], int count) {
  Scope s("CsInventory.changeCount", M.change_count);
  if (!s.ctx() || !cs_inventory) return false;
  alignas(16) uint8_t g[16];
  std::memcpy(g, guid, 16);
  const bool done = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, const uint8_t*, int32_t)>(M.change_count)(
      s.ctx(), cs_inventory, g, count);
  return s.returned() && done;
}

bool case_update_reload(uintptr_t cs_inventory) {
  Scope s("CsInventory.updateReloadInfos", M.update_reload);
  if (!s.ctx() || !cs_inventory) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(M.update_reload)(s.ctx(), cs_inventory);
  return s.returned();
}

bool case_change_size(uintptr_t cs_controller, int size) {
  Scope s("CsInventoryController.changeSize", M.change_size);
  if (!s.ctx() || !cs_controller || size < 0) return false;
  const bool done = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, int32_t, bool)>(M.change_size)(s.ctx(), cs_controller,
                                                                                                   size, true);
  return s.returned() && done;
}

int list_add(int kind, uintptr_t controller, uintptr_t item) {
  const uintptr_t code = kind == 1 ? M.key_add : kind == 2 ? M.treasure_add : kind == 3 ? M.unique_add : 0;
  Scope s("an item list's add", code);
  if (!s.ctx() || !controller || !item) return -1;
  const uintptr_t r = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t)>(code)(s.ctx(), controller, item);
  return s.returned() ? add_count(r) : -1;
}

bool list_remove(int kind, uintptr_t owner, const uint8_t guid[16]) {
  const uintptr_t code = kind == 1 ? M.key_remove : kind == 2 ? M.treasure_remove : kind == 3 ? M.unique_remove : 0;
  Scope s("an item list's remove", code);
  if (!s.ctx() || !owner) return false;
  alignas(16) uint8_t g[16];
  std::memcpy(g, guid, 16);
  // KeyItemInventoryController.remove and UniqueInventory.remove answer a bool,
  // TreasureInventoryController.remove nothing: al is read only where it means something.
  const bool r = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, const uint8_t*)>(code)(s.ctx(), owner, g);
  return s.returned() && (kind == 2 || r);
}

int max_level(int list, int category, int32_t weapon_id) {
  const uintptr_t code = list == 0 ? M.max_common : list == 1 ? M.max_individual : M.max_limit;
  const uintptr_t mgr = t_weapon_custom ? re::singleton(t_weapon_custom) : 0;
  Scope s("WeaponCustomManager.getMaxCustomLevel", code);
  if (!s.ctx() || !mgr) return -1;
  const int v = reinterpret_cast<int32_t (*)(uintptr_t, uintptr_t, int32_t, int32_t)>(code)(s.ctx(), mgr, weapon_id, category);
  return s.returned() ? v : -1;
}

bool set_level(int list, int category, int level, uintptr_t custom_levels, int32_t weapon_id) {
  const uintptr_t code = list == 0 ? M.set_common : list == 1 ? M.set_individual : M.set_limit;
  const uintptr_t mgr = t_weapon_custom ? re::singleton(t_weapon_custom) : 0;
  Scope s("WeaponCustomManager.set...CustomLevel", code);
  if (!s.ctx() || !mgr || !custom_levels || level < 0) return false;
  if (list == 2)
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t, int32_t, uintptr_t)>(code)(s.ctx(), mgr, category, level,
                                                                                     custom_levels);
  else
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t, int32_t, uintptr_t, int32_t)>(code)(
        s.ctx(), mgr, category, level, custom_levels, weapon_id);
  return s.returned();
}

bool weapon_customized(uintptr_t weapon_item) {
  Scope s("WeaponItem.onCustomized", M.customized);
  if (!s.ctx() || !weapon_item) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(M.customized)(s.ctx(), weapon_item);
  return s.returned();
}

bool set_difficulty(uintptr_t campaign_manager, int value) {
  const uintptr_t rank = t_rank_system ? re::singleton(t_rank_system) : 0;
  {
    Scope s("CampaignManager.setDifficulty", M.set_difficulty);
    if (!s.ctx() || !campaign_manager) return false;
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(M.set_difficulty)(s.ctx(), campaign_manager, value);
    if (!s.returned()) return false;
  }
  if (!rank) return true;  // nothing to recompute yet
  Scope s("GameRankSystem.recalcRank", M.recalc_rank);
  if (!s.ctx()) return true;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(M.recalc_rank)(s.ctx(), rank);
  return s.returned();
}

bool press_command(uintptr_t command, uint32_t hash) {
  Scope s("share.hid.Command.setValue", M.set_value);
  if (!s.ctx() || !command || !hash) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uint32_t)>(M.set_value)(s.ctx(), command, hash);
  return s.returned();
}

uint32_t laser_avoid_hash(uintptr_t trap, int32_t command_type) {
  Scope s("GmQTELaserTrap.calcAvoidCommandHash", M.laser_hash);
  if (!s.ctx() || !trap) return 0;
  const uint32_t h =
      reinterpret_cast<uint32_t (*)(uintptr_t, uintptr_t, int32_t)>(M.laser_hash)(s.ctx(), trap, command_type);
  return s.returned() ? h : 0;
}

uint32_t hookshot_hash() {
  const uintptr_t am = t_additional ? re::singleton(t_additional) : 0;
  const uintptr_t om = am ? game::ref(am, f_option_manager) : 0;
  Scope s("AdditionalOptionManager.getCp11HSCommandHash", M.hookshot_hash);
  if (!s.ctx() || !om) return 0;
  const uint32_t h = reinterpret_cast<uint32_t (*)(uintptr_t, uintptr_t)>(M.hookshot_hash)(s.ctx(), om);
  return s.returned() ? h : 0;
}

bool reserve_avoid(uintptr_t action_sign) {
  Scope s("PlayerHeadActionSign.requestReserveAvoid", M.reserve_avoid);
  if (!s.ctx() || !action_sign) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(M.reserve_avoid)(s.ctx(), action_sign);
  return s.returned();
}

bool reserve_parry(uintptr_t action_sign) {
  // Virtual: the driver's own (the Mercenaries' bullet parry overrides it).
  const int vt = M.reserve_parry ? re::method_vt_index(M.reserve_parry) : -1;
  const uintptr_t table = vt >= 0 ? re::vtable_of_object(action_sign) : 0;
  const uintptr_t code = table ? mem::read_ptr(table + static_cast<uintptr_t>(vt) * 8) : 0;
  Scope s("PlayerHeadActionSign.requestReserveParry", code && g_text.contains(code) ? code : 0);
  if (!s.ctx()) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(code)(s.ctx(), action_sign);
  return s.returned();
}

int large_normal_sign(uintptr_t gui_unit) {
  Scope s("ActionSignGuiUnit.checkNormalActionSignDispLargeUI", M.large_normal);
  if (!s.ctx() || !gui_unit) return -1;
  const bool r = reinterpret_cast<bool (*)(uintptr_t, uintptr_t)>(M.large_normal)(s.ctx(), gui_unit);
  return s.returned() ? (r ? 1 : 0) : -1;
}

bool trigger_command(uintptr_t triggers, uint32_t hash) {
  Scope s("ConcurrentDictionary<UInt32,Boolean>.TryAdd", M.try_add);
  if (!s.ctx() || !triggers || !hash) return false;
  reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uint32_t, bool)>(M.try_add)(s.ctx(), triggers, hash, true);
  return s.returned();
}

int knife_can_escape(uintptr_t knife_condition, uintptr_t player_head) {
  Scope s("KnifeEscapeCondition.canOpenGui", M.knife_gui);
  if (!s.ctx() || !knife_condition || !player_head) return -1;
  const bool r =
      reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uintptr_t)>(M.knife_gui)(s.ctx(), knife_condition, player_head);
  return s.returned() ? (r ? 1 : 0) : -1;
}

bool component_position(uintptr_t component, float out[3]) {
  if (!M.get_game_object || !M.get_transform || !M.get_position || !component) return false;
  Scope s("a target's position", M.get_position);
  if (!s.ctx()) return false;
  const uintptr_t go = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(M.get_game_object)(s.ctx(), component);
  if (!s.returned() || !go) return false;
  const uintptr_t tr = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(M.get_transform)(s.ctx(), go);
  if (!s.returned() || !tr) return false;
  // via.vec3 comes back through a buffer the caller passes first (16 bytes: the vector's own alignment).
  alignas(16) float v[4] = {};
  reinterpret_cast<void* (*)(float*, uintptr_t, uintptr_t)>(M.get_position)(v, s.ctx(), tr);
  if (!s.returned()) return false;
  out[0] = v[0];
  out[1] = v[1];
  out[2] = v[2];
  return true;
}

uintptr_t component_game_object(uintptr_t component) {
  Scope s("via.Component.get_GameObject", M.get_game_object);
  if (!s.ctx() || !component) return 0;
  const uintptr_t go = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(M.get_game_object)(s.ctx(), component);
  return s.returned() ? go : 0;
}

uint32_t new_attack_id(uintptr_t hit_manager) {
  Scope s("HitManager.generateAttackID", M.attack_id);
  if (!s.ctx() || !hit_manager) return 0;
  const uint32_t id = reinterpret_cast<uint32_t (*)(uintptr_t, uintptr_t)>(M.attack_id)(s.ctx(), hit_manager);
  return s.returned() ? id : 0;
}

bool request_attack(uintptr_t attacker, uintptr_t target, uintptr_t attack_user_data, uintptr_t damage_user_data) {
  Scope s("HitController.requestAttack", M.request_attack);
  if (!s.ctx() || !attacker || !target || !attack_user_data || !damage_user_data) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(M.request_attack)(
      s.ctx(), attacker, target, attack_user_data, damage_user_data);
  return s.returned();
}

bool remove_slot(int slot) {
  Scope s("SaveService.removeSingle", M.remove_single);
  if (!s.ctx()) return false;
  const bool started = reinterpret_cast<bool (*)(uintptr_t, int32_t)>(M.remove_single)(s.ctx(), slot);
  return s.returned() && started;
}

bool refresh_save_table() {
  Scope s("SaveService.updateSaveFileDetailTbl", M.refresh_table);
  if (!s.ctx()) return false;
  reinterpret_cast<void (*)(uintptr_t)>(M.refresh_table)(s.ctx());
  return s.returned();
}

uintptr_t slot_info_copy(uintptr_t mgr, int slot) {
  Scope s("SaveLoadMenuGuiManager.getSaveSlotInfo", M.slot_info);
  if (!s.ctx() || !mgr) return 0;
  const uintptr_t info = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, int32_t)>(M.slot_info)(s.ctx(), mgr, slot);
  return s.returned() ? info : 0;
}

bool add_slot_info(uintptr_t mgr, uintptr_t info) {
  Scope s("SaveLoadMenuGuiManager.addSaveSlotInfo", M.add_slot_info);
  if (!s.ctx() || !mgr || !info) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t)>(M.add_slot_info)(s.ctx(), mgr, info);
  return s.returned();
}

bool clear_load_info(uintptr_t mgr, int slot) {
  Scope s("SaveLoadMenuGuiManager.clearInfo", M.clear_info);
  if (!s.ctx() || !mgr) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(M.clear_info)(s.ctx(), mgr, slot);
  return s.returned();
}

uintptr_t new_object(uint32_t t) {
  // A RuntimeTypeHandle (8 bytes) goes by reference: the address of a copy that holds the type's record.
  alignas(16) uintptr_t handle = re::type_record(t);
  Scope s("System.Activator.CreateInstance", M.type_from_handle && handle ? M.create_instance : 0);
  if (!s.ctx()) return 0;
  const uintptr_t type =
      reinterpret_cast<uintptr_t (*)(uintptr_t, const uintptr_t*)>(M.type_from_handle)(s.ctx(), &handle);
  if (!s.returned() || !type) return 0;
  const uintptr_t obj = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(M.create_instance)(s.ctx(), type);
  return s.returned() && obj && re::type_of(obj) == t ? obj : 0;
}

bool save_access_ok() {
  Scope s("SaveDataUtil.isEnableAccessSaveFileDetailTbl", M.access_ok);
  if (!s.ctx()) return false;
  const bool ok = reinterpret_cast<bool (*)(uintptr_t)>(M.access_ok)(s.ctx());
  return s.returned() && ok;
}

bool suspend_game_saves(uintptr_t sdm, uintptr_t caller, bool enter) {
  const uintptr_t code = enter ? M.suspend_enter : M.suspend_exit;
  Scope s(enter ? "SaveDataManager.entrySuspendGameSave" : "SaveDataManager.exitSuspendGameSave", code);
  if (!s.ctx() || !sdm || !caller) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t)>(code)(s.ctx(), sdm, caller);
  return s.returned();
}

uintptr_t save_key(int save_load_key) {
  Scope s("SaveDataManagerEnumExtension.getKeyString", M.key_string);
  if (!s.ctx()) return 0;
  const uintptr_t str = reinterpret_cast<uintptr_t (*)(uintptr_t, int32_t)>(M.key_string)(s.ctx(), save_load_key);
  return s.returned() ? str : 0;
}

bool service_clean() {
  Scope s("SaveService.cleanData", M.clean_data);
  if (!s.ctx()) return false;
  reinterpret_cast<void (*)(uintptr_t)>(M.clean_data)(s.ctx());
  return s.returned();
}

bool service_add(uintptr_t key, uintptr_t container) {
  Scope s("SaveService.addDataClass", M.add_data);
  if (!s.ctx() || !key || !container) return false;
  const bool ok = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uintptr_t)>(M.add_data)(s.ctx(), key, container);
  return s.returned() && ok;
}

bool service_load(int slot) {
  Scope s("SaveService.loadSingle", M.load_single);
  if (!s.ctx()) return false;
  const bool started = reinterpret_cast<bool (*)(uintptr_t, int32_t)>(M.load_single)(s.ctx(), slot);
  return s.returned() && started;
}

bool service_save(int slot, uint64_t size) {
  {
    Scope s("SaveService.set_SaveDataSize", M.set_size);
    if (!s.ctx() || !size) return false;
    reinterpret_cast<void (*)(uintptr_t, uint64_t)>(M.set_size)(s.ctx(), size);
    if (!s.returned()) return false;
  }
  Scope s("SaveService.saveSingle", M.save_single);
  if (!s.ctx()) return false;
  const bool started = reinterpret_cast<bool (*)(uintptr_t, int32_t)>(M.save_single)(s.ctx(), slot);
  return s.returned() && started;
}

int service_state() {
  Scope s("SaveService.get_SaveState", M.save_state);
  if (!s.ctx()) return -1;
  const int32_t v = reinterpret_cast<int32_t (*)(uintptr_t)>(M.save_state)(s.ctx());
  return s.returned() ? v : -1;
}

int service_result() {
  Scope s("SaveService.get_LastSaveResult", M.last_result);
  if (!s.ctx()) return -1;
  const int32_t v = reinterpret_cast<int32_t (*)(uintptr_t)>(M.last_result)(s.ctx());
  return s.returned() ? v : -1;
}

bool request_system_save(uintptr_t sdm, int32_t system_slot) {
  Scope s("SaveDataManager.requestSaveSystemData", M.system_save);
  if (!s.ctx() || !sdm) return false;
  // The request keeps a copy of the arguments (getCopy), so the frame may let them go.
  const uintptr_t args = new_object(t_system_save_args);
  if (!args) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t, uintptr_t)>(M.system_save)(s.ctx(), sdm, system_slot, args);
  return s.returned();
}

bool storage_add(uintptr_t armoury_manager, uintptr_t item) {
  Scope s("ArmouryManager.addArmouryItem", M.storage_add);
  if (!s.ctx() || !armoury_manager || !item) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t)>(M.storage_add)(s.ctx(), armoury_manager, item);
  return s.returned();
}

bool storage_reduce(uintptr_t armoury_manager, uintptr_t item, int n) {
  Scope s("ArmouryManager.reduceArmouryItem", M.storage_reduce);
  if (!s.ctx() || !armoury_manager || !item || n <= 0) return false;
  const bool done = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uintptr_t, int32_t)>(M.storage_reduce)(
      s.ctx(), armoury_manager, item, n);
  return s.returned() && done;
}

int storage_accepts(uintptr_t item) {
  Scope s("ItemExtension.isEnableMoveToArmoury", M.storage_accepts);
  if (!s.ctx() || !item) return -1;
  const bool r = reinterpret_cast<bool (*)(uintptr_t, uintptr_t)>(M.storage_accepts)(s.ctx(), item);
  return s.returned() ? (r ? 1 : 0) : -1;
}

bool item_list_add(uintptr_t list, uintptr_t item) {
  Scope s("List<chainsaw.Item>.Add", M.item_list_add);
  if (!s.ctx() || !list || !item) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t)>(M.item_list_add)(s.ctx(), list, item);
  return s.returned();
}

bool unlock_achievement(uintptr_t achievement_controller, int32_t number) {
  Scope s("AchievementServiceController.unlock", M.achievement);
  if (!s.ctx() || !achievement_controller) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(M.achievement)(s.ctx(), achievement_controller, number);
  return s.returned();
}

bool shop_page(uintptr_t screen, int32_t index) {
  Scope s("ExContentsShopGuiBehavior.updateCurrentGrid", M.shop_page);
  if (!s.ctx() || !screen) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t, int32_t)>(M.shop_page)(s.ctx(), screen, index, g_scroll_none);
  return s.returned();
}

bool shop_caption(uintptr_t screen, int32_t bonus_id) {
  Scope s("ExContentsShopGuiBehavior.setCaptionMessage", M.shop_caption);
  if (!s.ctx() || !screen) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(M.shop_caption)(s.ctx(), screen, bonus_id);
  return s.returned();
}

}  // namespace re4cc::call
