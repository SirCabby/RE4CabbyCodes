// No armor durability loss and the body armor's editor (game.cpp: body_armor,
// set_armor, keep_armor - the last read by events.cpp's hook on
// UniqueInventoryController.onDurabilityChanged) over objects shaped like the
// game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_armor.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_armor.exe
//   wine tests/build/test_armor.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then the player's unique items are built the way
// the VM lays out managed objects (as test_tickets does): a UniqueInventoryController,
// its UniqueInventory and entries, the Body Armor's item and its definition - every
// field placed by name from the real database. The armor is read as the panel
// shows it, set as its editor sets it, and a wear the game made is taken back as
// the hook takes it back. And the game's code the hook relies on (found by name,
// read in the mapped exe): every way the armor wears goes through the unique
// inventory's useDurability, which marks it broken (never removes it) and tells its
// controller after the change; the controller's handler does nothing for no change
// and otherwise only feeds the dynamic difficulty; a hit's "armor nearly gone"
// check reads the durability after that call; the merchant's repair is what the
// editor does.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "../src/game.h"
#include "../src/log.h"
#include "../src/mem.h"
#include "../src/re.h"

namespace {

using namespace re4cc;

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

HMODULE g_game = nullptr;
using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
GetModuleHandleAFn g_real_gmh = nullptr;
HMODULE WINAPI game_module(LPCSTR name) { return name ? g_real_gmh(name) : g_game; }

uintptr_t alloc(size_t n) {
  return reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

// One REObjectInfo per class: the fields start 0x10 into the object.
std::map<uint32_t, uintptr_t> g_infos;
uintptr_t info_of(uint32_t t) {
  auto it = g_infos.find(t);
  if (it != g_infos.end()) return it->second;
  const uintptr_t b = alloc(16);
  *reinterpret_cast<int32_t*>(b) = 0x10;
  *reinterpret_cast<uintptr_t*>(b + 8) = re::type_record(t);
  return g_infos[t] = b + 8;
}

uintptr_t make(const char* type_name) {
  const uint32_t t = re::find_type(type_name);
  if (!t || !re::type_record(t)) {
    std::printf("FAIL  no class %s\n", type_name);
    ++g_fail;
    return 0;
  }
  const uintptr_t o = alloc(0x400);
  *reinterpret_cast<uintptr_t*>(o) = info_of(t);
  return o;
}

uintptr_t at(uintptr_t obj, const char* field) {
  const re::Field f = re::find_field(re::type_of(obj), field);
  const uintptr_t a = re::field_addr(obj, f);
  if (!a) {
    std::printf("FAIL  no field %s on %s\n", field, re::full_name(re::type_of(obj)).c_str());
    ++g_fail;
  }
  return a;
}
template <typename T>
void set(uintptr_t obj, const char* field, T v) {
  if (const uintptr_t a = at(obj, field)) *reinterpret_cast<T*>(a) = v;
}
template <typename T>
T get(uintptr_t obj, const char* field) {
  const uintptr_t a = at(obj, field);
  return a ? *reinterpret_cast<T*>(a) : T{};
}

// A List`1 of references (its _items array, its _size).
uintptr_t list_of(const char* list_type, const char* elem_type, std::initializer_list<uintptr_t> items) {
  const uintptr_t list = make(list_type);
  const uintptr_t arr = alloc(0x20 + 8 * (items.size() + 4));
  *reinterpret_cast<uintptr_t*>(arr + 0x10) = re::type_record(re::find_type(elem_type));
  *reinterpret_cast<int32_t*>(arr + 0x1C) = static_cast<int32_t>(items.size() + 4);
  int i = 0;
  for (const uintptr_t it : items) reinterpret_cast<uintptr_t*>(arr + 0x20)[i++] = it;
  set<uintptr_t>(list, "_items", arr);
  set<int32_t>(list, "_size", static_cast<int32_t>(items.size()));
  return list;
}

int enum_of(const char* type, const char* literal) { return re::enum_value(re::find_type(type), literal, -1); }

// A unique item and its entry: the item (its id, durability, condition, definition).
uintptr_t unique(int32_t id, int durability, int condition, uintptr_t define) {
  const uintptr_t item = make("chainsaw.Item");
  set<int32_t>(item, "_ItemId", id);
  set<int32_t>(item, "_CurrentItemCount", 1);
  set<int32_t>(item, "_CurrentDurability", durability);
  set<int32_t>(item, "_CurrentCondition", condition);
  set<uint32_t>(item, "_ID", 0x2000u + static_cast<uint32_t>(id % 9973) + static_cast<uint32_t>(durability));
  set<uintptr_t>(item, "<_ItemDefine>k__BackingField", define);
  const uintptr_t entry = make("chainsaw.UniqueInventoryItem");
  set<uintptr_t>(entry, "<Item>k__BackingField", item);
  return entry;
}
uintptr_t item_of(uintptr_t entry) { return get<uintptr_t>(entry, "<Item>k__BackingField"); }
int durability_of(uintptr_t entry) { return get<int32_t>(item_of(entry), "_CurrentDurability"); }
int condition_of(uintptr_t entry) { return get<int32_t>(item_of(entry), "_CurrentCondition"); }
// What the game's useDurability leaves on the item before it tells the controller.
void wear(uintptr_t entry, int curr) {
  set<int32_t>(item_of(entry), "_CurrentDurability", curr);
  if (curr <= 0) set<int32_t>(item_of(entry), "_CurrentCondition", condition_of(entry) | game::kConditionBroken);
}

// A method's code in the mapped exe, up to the ret the compiler padded after it.
struct Code {
  uintptr_t at = 0;
  size_t n = 0;
  uint8_t u8(size_t i) const { return mem::read<uint8_t>(at + i); }
  int32_t i32(size_t i) const { return mem::read<int32_t>(at + i); }
  // The first direct call (E8) - or jump (E9: a tail call) - to `target` from `from` on: its offset, or -1.
  long branch_to(uint8_t op, uintptr_t target, size_t from) const {
    for (size_t i = from; i + 5 <= n; ++i)
      if (u8(i) == op && at + i + 5 + static_cast<intptr_t>(i32(i + 1)) == target) return static_cast<long>(i);
    return -1;
  }
  long call_to(uintptr_t target, size_t from = 0) const { return branch_to(0xE8, target, from); }
  long jump_to(uintptr_t target, size_t from = 0) const { return branch_to(0xE9, target, from); }
  // `bytes` from `from` on: their offset, or -1.
  long find(std::initializer_list<uint8_t> bytes, size_t from = 0) const {
    for (size_t i = from; i + bytes.size() <= n; ++i) {
      size_t k = 0;
      for (const uint8_t b : bytes)
        if (u8(i + k) == b) ++k;
        else break;
      if (k == bytes.size()) return static_cast<long>(i);
    }
    return -1;
  }
  // The first 32-bit load (mov r32,[reg+disp8], no REX.W) of `disp` from `from` on: its offset, or -1.
  long load32(int32_t disp, size_t from = 0) const {
    for (size_t i = from; i + 3 <= n; ++i)
      if (u8(i) == 0x8B && (u8(i + 1) >> 6) == 1 && (u8(i + 1) & 7) != 4 && static_cast<int8_t>(u8(i + 2)) == disp &&
          !(i > 0 && (u8(i - 1) & 0xF8) == 0x48))
        return static_cast<long>(i);
    return -1;
  }
};
Code code_at(uintptr_t a) {
  Code c;
  c.at = a;
  while (c.at && c.n < 0x1000 && !(c.u8(c.n) == 0xC3 && c.u8(c.n + 1) == 0xCC)) ++c.n;
  if (c.at) ++c.n;
  return c;
}
uintptr_t method_code(const char* type, const char* method, std::initializer_list<const char*> params,
                      bool is_static = false) {
  const uint32_t t = re::find_type(type);
  const uintptr_t a = t ? re::method_code_at(re::find_method(t, method, params, is_static)) : 0;
  if (!a) {
    std::printf("FAIL  no code for %s.%s\n", type, method);
    ++g_fail;
  }
  return a;
}
Code code_of(const char* type, const char* method, std::initializer_list<const char*> params) {
  return code_at(method_code(type, method, params));
}

// A field's place in its objects (a plain object's fields start after its header).
int32_t place(const char* type, const char* field) {
  const re::Field f = re::find_field(re::find_type(type), field);
  return f.valid() ? static_cast<int32_t>(f.offset) + 0x10 : -1;
}
uint8_t byte_of(uint32_t v, int i) { return static_cast<uint8_t>(v >> (8 * i)); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_armor.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_armor_" + std::to_string(GetCurrentProcessId()) + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  log_init(dir.c_str());

  // --- the game's image and database, as test_discover sets them up ---
  g_game = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!g_game) {
    std::printf("could not map %s (%lu)\n", argv[1], GetLastError());
    return 1;
  }
  const auto base = reinterpret_cast<uintptr_t>(g_game);
  const mem::Range data = mem::section(g_game, ".data");
  uintptr_t tdb = 0;
  for (uintptr_t a = (data.begin + 7) & ~static_cast<uintptr_t>(7); a + 8 < data.end; a += 8)
    if (!std::memcmp(reinterpret_cast<const void*>(a), "TDB\0", 4) && mem::read<uint32_t>(a + 4) == 71) {
      tdb = a;
      break;
    }
  if (!tdb) {
    std::printf("no TDB v71 in %s\n", argv[1]);
    return 1;
  }
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(data.begin), data.size(), PAGE_READWRITE, &old);
  for (int i = 0; i < 18; ++i) {
    auto* p = reinterpret_cast<uint64_t*>(tdb + 0x58 + 8 * i);
    if (*p) *p += tdb;
  }
  const uintptr_t methods = mem::read<uint64_t>(tdb + 0x70);
  const uint32_t n_methods = mem::read<uint32_t>(tdb + 0x10);
  const auto decoders = mem::find_all(mem::section(g_game, ".text"), "85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 03 C1 C3", 2);
  if (decoders.size() != 1) {
    std::printf("%u method-offset decoders in %s\n", static_cast<unsigned>(decoders.size()), argv[1]);
    return 1;
  }
  const uintptr_t code_base = mem::rip_target(decoders[0] + 10, 3, 7);
  FILE* f = std::fopen(argv[2], "rb");
  uint32_t header[4] = {};
  if (!f || std::fread(header, sizeof(header), 1, f) != 1 || header[0] != 0x4D344552 || header[2] != n_methods) {
    std::printf("%s is not a method map of this exe\n", argv[2]);
    return 1;
  }
  std::vector<uint32_t> rvas(n_methods);
  std::fread(rvas.data(), 4, rvas.size(), f);
  std::fclose(f);
  for (uint32_t i = 0; i < n_methods; ++i)
    if (rvas[i])
      *reinterpret_cast<int32_t*>(methods + static_cast<uintptr_t>(i) * 12 + 8) =
          static_cast<int32_t>(static_cast<intptr_t>(base + rvas[i]) - static_cast<intptr_t>(code_base));
  g_real_gmh = reinterpret_cast<GetModuleHandleAFn>(
      mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&game_module)));
  if (!g_real_gmh) {
    std::printf("could not redirect GetModuleHandleA\n");
    return 1;
  }
  // The stand-in VM: the database pointer and an empty static table.
  uintptr_t vm_slot = 0;
  {
    std::map<uintptr_t, int> seen;
    for (const uintptr_t s : mem::find_all(mem::section(g_game, ".text"), "48 8B 0D ?? ?? ?? ?? BA FF FF FF FF E8", 4096))
      if (++seen[mem::rip_target(s, 3, 7)] > 10) {
        vm_slot = mem::rip_target(s, 3, 7);
        break;
      }
  }
  if (!vm_slot) {
    std::printf("no VM global in %s\n", argv[1]);
    return 1;
  }
  const uint32_t n_types = mem::read<uint32_t>(tdb + 0x0C);
  auto* vm = reinterpret_cast<uint8_t*>(alloc(0x10000));
  auto* statics = reinterpret_cast<uintptr_t*>(alloc(static_cast<size_t>(n_types) * 8));
  *reinterpret_cast<uintptr_t*>(vm + 0x3000) = tdb;
  *reinterpret_cast<uintptr_t**>(vm + 0x3000 - 0x30) = statics;
  *reinterpret_cast<uint32_t*>(vm + 0x3000 - 0x28) = n_types;
  *reinterpret_cast<uint8_t**>(vm_slot) = vm;
  bool ready = re::init();
  for (int tries = 0; !ready && tries < 5 && !re::unsupported(); ++tries) ready = re::init();
  if (!ready) {
    std::printf("re::init: %s\n", re::status());
    return 1;
  }
  game::discover();
  check(game::parts().armor, "discovery: the body armor (the unique items' controller, its armor's id, the most durability)");

  const int32_t armor_id = enum_of("chainsaw.ItemID", "it_sm77_501_00_0");  // the Body Armor
  const int32_t map_id = enum_of("chainsaw.ItemID", "it_sm77_502_00_0");    // Treasure Map: Village
  check(armor_id > 0 && game::armor_id() == armor_id,
        "the armor's id: the controller's own constant (_ArmorItemId) is the catalog's Body Armor");

  // --- the player's unique items, as the head updater's controller holds them ---
  const uintptr_t controller = make("chainsaw.UniqueInventoryController");
  const uintptr_t inventory = make("chainsaw.UniqueInventory");
  set<uintptr_t>(controller, "<_Inventory>k__BackingField", inventory);
  const uintptr_t define = make("chainsaw.ItemDefiniition");
  set<int32_t>(define, "_DefaultDurabilityMax", 1000);
  const uintptr_t map_define = make("chainsaw.ItemDefiniition");
  set<int32_t>(map_define, "_DefaultDurabilityMax", 1000);
  const char* entries = "System.Collections.Generic.List`1<chainsaw.UniqueInventoryItem>";
  const char* entry_type = "chainsaw.UniqueInventoryItem";
  const uintptr_t map = unique(map_id, 1000, 0, map_define);
  const uintptr_t armor = unique(armor_id, 600, 0, define);
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {map, armor}));
  game::Inventories inv;
  inv.unique = controller;
  inv.unique_inventory = inventory;

  game::Armor a;
  check(game::body_armor(inv, &a) && a.item == item_of(armor) && a.durability == 600 && a.max == 1000 && !a.broken,
        "the armor read: its item, its durability, its most (the definition's), not broken");
  check(durability_of(armor) == 600 && condition_of(armor) == 0, "reading it writes nothing");
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {map}));
  check(game::body_armor(inv, &a) && a.item == 0, "no armor among the unique items: read, none held");
  check(!game::body_armor(game::Inventories{}, &a), "no unique items: not read");
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {map, armor}));

  // --- the editor (cheats.cpp: request_armor) ---
  check(game::body_armor(inv, &a) && game::set_armor(a, 1000) && durability_of(armor) == 1000 && condition_of(armor) == 0,
        "set to its most");
  check(!game::set_armor(a, 1001) && durability_of(armor) == 1000, "more than its most: refused, nothing written");
  check(game::set_armor(a, 0) && durability_of(armor) == 0 && (condition_of(armor) & game::kConditionBroken),
        "set to none: marked broken, as its wear marks it");
  check(game::body_armor(inv, &a) && a.broken, "read back: broken");
  set<int32_t>(item_of(armor), "_CurrentCondition", game::kConditionBroken | game::kConditionLimitBreak);
  check(game::set_armor(a, 250) && durability_of(armor) == 250 && condition_of(armor) == game::kConditionLimitBreak,
        "a broken armor given some: mended (the mark off, any other condition kept)");
  set<int32_t>(item_of(armor), "_CurrentCondition", 0);
  game::Armor none;
  check(!game::set_armor(none, 500), "no armor: nothing to set");

  // --- the hook (events.cpp kArmorWear): a wear taken back as the controller hears of it ---
  set<int32_t>(item_of(armor), "_CurrentDurability", 600);
  wear(armor, 550);
  check(game::keep_armor(controller, armor_id, 600, 550) && durability_of(armor) == 600 && condition_of(armor) == 0,
        "a hit's wear (600 -> 550): the armor back at 600");
  wear(armor, 0);
  check((condition_of(armor) & game::kConditionBroken) && game::keep_armor(controller, armor_id, 600, 0) &&
            durability_of(armor) == 600 && condition_of(armor) == 0,
        "a wear that would break it (600 -> 0, marked broken): back at 600, the mark taken off");
  set<int32_t>(item_of(armor), "_CurrentCondition", game::kConditionLimitBreak);
  wear(armor, 0);
  check(game::keep_armor(controller, armor_id, 40, 0) && durability_of(armor) == 40 &&
            condition_of(armor) == game::kConditionLimitBreak,
        "only the broken mark comes off");
  set<int32_t>(item_of(armor), "_CurrentCondition", 0);
  check(!game::keep_armor(controller, armor_id, 40, 1000) && durability_of(armor) == 40,
        "a rise (the merchant's repair): the game's, nothing written");
  check(!game::keep_armor(controller, armor_id, 40, 40), "no change: nothing to do");
  set<int32_t>(item_of(armor), "_CurrentDurability", 0);
  set<int32_t>(item_of(armor), "_CurrentCondition", game::kConditionBroken);
  check(!game::keep_armor(controller, armor_id, 0, 0) && durability_of(armor) == 0 &&
            condition_of(armor) == game::kConditionBroken,
        "an armor broken before: stays broken");
  set<int32_t>(item_of(armor), "_CurrentDurability", 550);
  set<int32_t>(item_of(armor), "_CurrentCondition", 0);
  check(!game::keep_armor(controller, map_id, 600, 550) && durability_of(armor) == 550,
        "another unique item's wear: left alone");
  check(!game::keep_armor(controller, armor_id, 600, 500) && durability_of(armor) == 550,
        "no armor holding what the game says it left: nothing written");
  const uintptr_t worn = unique(armor_id, 300, 0, define);
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {map, armor, worn}));
  check(game::keep_armor(controller, armor_id, 320, 300) && durability_of(worn) == 320 && durability_of(armor) == 550,
        "two of its id: the one holding what the game left is the one given back");
  check(!game::keep_armor(inventory, armor_id, 320, 300) && !game::keep_armor(0, armor_id, 320, 300),
        "not a unique items' controller: nothing");
  set<uintptr_t>(controller, "<_Inventory>k__BackingField", 0);
  check(!game::keep_armor(controller, armor_id, 600, 550), "a controller without its inventory: nothing");
  set<uintptr_t>(controller, "<_Inventory>k__BackingField", inventory);

  // --- the game's code the hook relies on ---
  const char* ui = "chainsaw.UniqueInventory";
  const uintptr_t use = method_code(ui, "useDurability", {"chainsaw.ItemID", "System.Int32"});
  const Code ud = code_at(use);
  const uintptr_t set_durability = method_code("chainsaw.Item", "setDurability", {"System.Int32"});
  const uintptr_t told = method_code("System.Action`3<chainsaw.ItemID,System.Int32,System.Int32>", "Invoke",
                                     {"chainsaw.ItemID", "System.Int32", "System.Int32"});
  const long sets = ud.call_to(set_durability);
  const long tells = ud.call_to(told);
  check(sets >= 0 && tells > sets, "useDurability: the durability set, then OnDurabilityChanged told (after the change)");
  const auto id = static_cast<uint32_t>(armor_id);
  const auto cond = static_cast<uint8_t>(place("chainsaw.Item", "_CurrentCondition"));
  const long is_armor = ud.find({byte_of(id, 0), byte_of(id, 1), byte_of(id, 2), byte_of(id, 3)}, static_cast<size_t>(sets));
  const long mark = ud.find({0x83, 0x48, cond, 0x01}, static_cast<size_t>(sets));
  check(is_armor > sets && mark > is_armor && tells > mark,
        "useDurability: one worn out is marked broken (_CurrentCondition |= Broken) when it is the armor, before it tells");
  check(ud.call_to(method_code(ui, "remove", {"System.Guid"}), static_cast<size_t>(sets)) > sets,
        "useDurability: any other item worn out is removed - the armor never is");
  // Every way the armor wears reaches it.
  const uintptr_t proportion = method_code("chainsaw.PlayerEquipment", "getArmorDurabilityProportion", {});
  const Code hit = code_of("chainsaw.PlayerBodyHitDriver", "onHitDamage", {"chainsaw.HitController.DamageInfo"});
  const long hit_use = hit.call_to(use);
  check(hit_use >= 0 && hit.call_to(proportion, static_cast<size_t>(hit_use)) > hit_use,
        "a hit (PlayerBodyHitDriver.onHitDamage): useDurability, then the \"armor nearly gone\" check reads it after");
  const uintptr_t use_armor = method_code("chainsaw.PlayerEquipment", "useArmor", {"System.Int32"});
  check(code_at(use_armor).jump_to(use) >= 0, "PlayerEquipment.useArmor: useDurability (its tail call)");
  check(code_at(method_code("chainsaw.UniqueInventoryController", "useArmor", {"System.Int32"})).jump_to(use) >= 0,
        "UniqueInventoryController.useArmor: useDurability (its tail call)");
  check(code_of("chainsaw.PlayerHeadUpdater", "calculateBurning", {"System.Boolean"}).call_to(use_armor) >= 0 &&
            code_of("chainsaw.PlayerHeadUpdater", "calculateFreezing", {"System.Boolean"}).call_to(use_armor) >= 0,
        "fire and frost (PlayerHeadUpdater.calculateBurning / calculateFreezing): PlayerEquipment.useArmor");
  const Code stone = code_of("chainsaw.GmRollingStone", "playDamageMot", {"chainsaw.ContextID"});
  const long stone_use = stone.call_to(use_armor);
  check(stone_use >= 0 && stone.call_to(proportion, static_cast<size_t>(stone_use)) > stone_use,
        "the boulder (GmRollingStone.playDamageMot): useArmor, then the same check after");
  // The controller's handler: nothing for no change (the hook tells it prev, prev),
  // else the armor's id only, and only the dynamic difficulty's points.
  const Code handler =
      code_of("chainsaw.UniqueInventoryController", "onDurabilityChanged", {"chainsaw.ItemID", "System.Int32", "System.Int32"});
  const long same = handler.find({0x44, 0x3B, 0xC8, 0x74});  // cmp r9d (prev), eax (curr); je
  bool quiet = same >= 0 && same < 0x20;
  if (quiet) {
    const size_t to = static_cast<size_t>(same + 5 + static_cast<int8_t>(handler.u8(static_cast<size_t>(same + 4))));
    for (size_t i = to; i < handler.n && quiet && handler.u8(i) != 0xC3; ++i)
      quiet = handler.u8(i) != 0xE8 && handler.u8(i) != 0xE9;
  }
  check(quiet, "onDurabilityChanged: prev == curr returns at once, calling nothing");
  check(handler.find({0x41, 0x81, 0xF8, byte_of(id, 0), byte_of(id, 1), byte_of(id, 2), byte_of(id, 3)}) > same,
        "onDurabilityChanged: then only the armor's id is heard");
  check(handler.call_to(method_code("chainsaw.GameRankSystem", "getRankPointItemDurabilityData",
                                    {"chainsaw.ItemID", "System.Int32"})) >= 0 &&
            handler.jump_to(method_code("chainsaw.GameRankSystem", "addRankPointDirect",
                                        {"System.Single", "chainsaw.GameRankSystem.RankPointType"})) >= 0 &&
            handler.jump_to(method_code("chainsaw.ChainsawGuiUtil", "addRankPointRecoverItemDurability",
                                        {"chainsaw.ItemID", "System.Int32"}, true)) >= 0,
        "onDurabilityChanged: a wear or a repair only moves the dynamic difficulty's points");
  // The armor's most is its definition's, and the merchant's repair is what the editor does.
  check(code_of("chainsaw.Item", "get_CurrentDurabilityMax", {}).load32(place("chainsaw.ItemDefiniition", "_DefaultDurabilityMax")) >= 0,
        "Item.get_CurrentDurabilityMax: the definition's _DefaultDurabilityMax (for what is not a knife)");
  const Code repair = code_of(ui, "repair", {"System.Guid"});
  const long repaired = repair.call_to(set_durability);
  check(repair.call_to(method_code("chainsaw.Item", "get_CurrentDurabilityMax", {})) >= 0 && repaired >= 0 &&
            repair.find({0x83, 0x66, cond, 0xFE}, static_cast<size_t>(repaired)) > repaired,
        "UniqueInventory.repair: its most set, then the broken mark taken off");

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
