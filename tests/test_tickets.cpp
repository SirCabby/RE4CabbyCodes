// Infinite upgrade tickets (game.cpp: upgrade_ticket, which events.cpp's hook on the
// merchant's InGameShopGuiState_CustomCommandDefault.onHoldGaugeCompleted reads) over
// objects shaped like the game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_tickets.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_tickets.exe
//   wine tests/build/test_tickets.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then the exclusive upgrade's state is built the way
// the VM lays out managed objects (as test_stats does): the state, its shop state
// controller, the GUI behaviours' hub and the item popup holding the list of ways
// to pay (LimitBreakSelectListGui and its options), the parameters' hub and the
// customer's treasures (TreasureInventoryController, its TreasureInventory and
// entries) - every field placed by name from the real database. The ticket is the
// one the game's reduceTicket would take: only when the way chosen is the ticket's.
// And the game's code the hook relies on (found by name, read in the mapped exe):
// the state's getters read the same fields the mod does; the ticket's way takes one
// ticket by its id; the reduce takes from the first valid entry holding it, and an
// entry it empties leaves the treasures (so the hook raises the count first).
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

// A treasure and its entry: the item (its id, Guid and count) in a slot of the treasures' grid.
uintptr_t treasure(int32_t id, int count, int row, int column, bool guid = true) {
  const uintptr_t item = make("chainsaw.Item");
  set<int32_t>(item, "_ItemId", id);
  set<int32_t>(item, "_CurrentItemCount", count);
  if (guid) set<uint32_t>(item, "_ID", 0x1000u + static_cast<uint32_t>(id % 9973) + static_cast<uint32_t>(row));
  const uintptr_t entry = make("chainsaw.gui.treasure.TreasureInventoryItem");
  set<uintptr_t>(entry, "<Item>k__BackingField", item);
  const uint32_t slot = re::find_type("chainsaw.CsSlotIndex");
  if (const uintptr_t s = at(entry, "<SlotIndex>k__BackingField")) {
    *reinterpret_cast<int32_t*>(re::value_field_addr(s, re::find_field(slot, "Row"))) = row;
    *reinterpret_cast<int32_t*>(re::value_field_addr(s, re::find_field(slot, "Column"))) = column;
  }
  return entry;
}
uintptr_t item_of(uintptr_t entry) { return get<uintptr_t>(entry, "<Item>k__BackingField"); }

// A method's code in the mapped exe, up to the ret the compiler padded after it.
struct Code {
  uintptr_t at = 0;
  size_t n = 0;
  uint8_t u8(size_t i) const { return mem::read<uint8_t>(at + i); }
  int32_t i32(size_t i) const { return mem::read<int32_t>(at + i); }
  // The first direct call (E8) to `target` from `from` on: its offset, or -1.
  long call_to(uintptr_t target, size_t from = 0) const {
    for (size_t i = from; i + 5 <= n; ++i)
      if (u8(i) == 0xE8 && at + i + 5 + static_cast<intptr_t>(i32(i + 1)) == target) return static_cast<long>(i);
    return -1;
  }
  // `bytes` within the `window` bytes before offset `end`.
  bool before(long end, std::initializer_list<uint8_t> bytes, long window = 24) const {
    const long size = static_cast<long>(bytes.size());
    for (long i = end > window ? end - window : 0; end >= 0 && i + size <= end; ++i) {
      long k = 0;
      for (const uint8_t b : bytes)
        if (u8(static_cast<size_t>(i + k)) == b) ++k;
        else break;
      if (k == size) return true;
    }
    return false;
  }
  // The first virtual call through `slot` of a table from `from` on - the slot read
  // (mov r64,[reg+slot*8], a one- or four-byte displacement) then a call through a
  // register: its offset, or -1.
  long virtual_call(int slot, size_t from = 0) const {
    for (size_t i = from; i + 4 <= n; ++i) {
      if ((u8(i) & 0xF8) != 0x48 || u8(i + 1) != 0x8B || (u8(i + 2) & 7) == 4) continue;
      const uint8_t mod = u8(i + 2) >> 6;
      size_t len = 0;
      if (mod == 1 && static_cast<int8_t>(u8(i + 3)) == slot * 8) len = 4;
      else if (mod == 2 && i + 7 <= n && i32(i + 3) == slot * 8) len = 7;
      if (!len) continue;
      for (size_t k = i + len; k + 2 <= n && k < i + len + 12; ++k)
        if (u8(k) == 0xFF && (u8(k + 1) & 0xF8) == 0xD0) return static_cast<long>(i);
    }
    return -1;
  }
  // The displacements of its 64-bit loads from memory (mov r64,[reg+disp]), in order.
  std::vector<int32_t> loads() const {
    std::vector<int32_t> out;
    for (size_t i = 0; i + 4 <= n; ++i) {
      if ((u8(i) & 0xF8) != 0x48 || u8(i + 1) != 0x8B || (u8(i + 2) & 7) == 4) continue;
      const uint8_t mod = u8(i + 2) >> 6;
      if (mod == 1) out.push_back(static_cast<int8_t>(u8(i + 3)));
      else if (mod == 2 && i + 7 <= n) out.push_back(i32(i + 3));
    }
    return out;
  }
};
Code code_at(uintptr_t a) {
  Code c;
  c.at = a;
  while (c.at && c.n < 0x1000 && !(c.u8(c.n) == 0xC3 && c.u8(c.n + 1) == 0xCC)) ++c.n;
  if (c.at) ++c.n;
  return c;
}
uintptr_t method_code(const char* type, const char* method, std::initializer_list<const char*> params) {
  const uint32_t t = re::find_type(type);
  const uintptr_t a = t ? re::method_code_at(re::find_method(t, method, params)) : 0;
  if (!a) {
    std::printf("FAIL  no code for %s.%s\n", type, method);
    ++g_fail;
  }
  return a;
}
Code code_of(const char* type, const char* method, std::initializer_list<const char*> params) {
  return code_at(method_code(type, method, params));
}

// Where a class's managed fields start in its objects (the game's REObjectInfo
// says, at run time): after a behaviour's native part, else after the header.
int32_t start_of(uint32_t t) { return re::derives(t, re::find_type("via.Behavior")) ? 0x50 : 0x10; }
// A field's place in its objects, by its declaring class.
int32_t place(const char* type, const char* field) {
  const re::Field f = re::find_field(re::find_type(type), field);
  return f.valid() ? static_cast<int32_t>(f.offset) + start_of(f.declaring) : -1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_tickets.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_tickets_" + std::to_string(GetCurrentProcessId()) + "\\";
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
  check(game::parts().tickets, "discovery: the exclusive upgrade's ways to pay and the customer's treasures");

  const int32_t ticket_id = enum_of("chainsaw.ItemID", "it_sm75_542_00_0");  // the Exclusive Upgrade Ticket
  const int32_t dlc_ticket_id = enum_of("chainsaw.ItemID", "it_sm75_600_00_0");
  const int32_t gold_id = enum_of("chainsaw.ItemID", "it_sm75_526_00_0");  // Gold Bar (L)
  const int money = enum_of("chainsaw.ItemCommandType", "MoneyLimitBreak");
  const int ticket = enum_of("chainsaw.ItemCommandType", "TicketLimitBreak");
  const int dlc_ticket = enum_of("chainsaw.ItemCommandType", "TicketLimitBreak01");
  check(ticket_id > 0 && dlc_ticket_id > 0 && gold_id > 0 && money >= 0 && ticket > money && dlc_ticket > ticket,
        "the ticket's item ids and the ways to pay are enum values");

  // --- the merchant's exclusive upgrade, as its state reaches it ---
  const uintptr_t state = make("chainsaw.gui.shop.InGameShopGuiState_CustomCommandDefault");
  const uintptr_t ctl = make("chainsaw.InGameShopStateController");
  const uintptr_t behaviors = make("chainsaw.gui.shop.InGameShopGuiBehaviorHub");
  const uintptr_t popup = make("chainsaw.InventoryPopupGuiBehavior");
  const uintptr_t ways = make("chainsaw.gui.commandmenu.LimitBreakSelectListGui");
  const char* option_type = "chainsaw.gui.commandmenu.LimitBreakSelectListGui.AppSelectItem";
  uintptr_t option[3] = {make(option_type), make(option_type), make(option_type)};
  const int command[3] = {money, ticket, dlc_ticket};
  for (int i = 0; i < 3; ++i) {
    set<int32_t>(option[i], "<CommandType>k__BackingField", command[i]);
    set<bool>(option[i], "<Selectable>k__BackingField", true);
  }
  set<uintptr_t>(state, "<_Controller>k__BackingField", ctl);
  set<uintptr_t>(ctl, "<BehaviorHub>k__BackingField", behaviors);
  set<uintptr_t>(behaviors, "_InventoryPopupGuiBehavior", popup);
  set<uintptr_t>(popup, "_LimitBreakSelectListGui", ways);
  set<uintptr_t>(ways, "_AppSelectItems",
                 list_of("System.Collections.Generic.List`1<chainsaw.gui.commandmenu.LimitBreakSelectListGui.AppSelectItem>",
                         option_type, {option[0], option[1], option[2]}));
  const uintptr_t params = make("chainsaw.gui.shop.InGameShopGuiParamHub");
  const uintptr_t treasures = make("chainsaw.TreasureInventoryController");
  const uintptr_t inventory = make("chainsaw.gui.treasure.TreasureInventory");
  set<uintptr_t>(ctl, "<InGameShopGuiParamHub>k__BackingField", params);
  set<uintptr_t>(params, "<TreasureInventoryController>k__BackingField", treasures);
  set<uintptr_t>(treasures, "<_Inventory>k__BackingField", inventory);
  const char* entries = "System.Collections.Generic.List`1<chainsaw.gui.treasure.TreasureInventoryItem>";
  const char* entry_type = "chainsaw.gui.treasure.TreasureInventoryItem";
  const uintptr_t gold = treasure(gold_id, 1, 0, 0), held = treasure(ticket_id, 2, 0, 2);
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {gold, held}));
  auto choose = [&](int i) { set<int32_t>(ways, "<CurrSelectedSlotIndex>k__BackingField", i); };

  choose(1);
  check(game::upgrade_ticket(state) == item_of(held), "the ticket's way chosen: the ticket held is the one taken");
  check(get<int32_t>(item_of(held), "_CurrentItemCount") == 2, "reading it writes nothing");
  choose(0);
  check(game::upgrade_ticket(state) == 0, "pesetas chosen: no ticket");
  choose(2);
  check(game::upgrade_ticket(state) == 0, "the DLC's ticket chosen: left to the game");
  choose(-1);
  check(game::upgrade_ticket(state) == 0, "nothing chosen: no ticket");
  choose(3);
  check(game::upgrade_ticket(state) == 0, "a way past the list's end: no ticket");
  choose(1);
  set<bool>(option[1], "<Selectable>k__BackingField", false);
  check(game::upgrade_ticket(state) == 0, "the ticket's way greyed (no ticket to pay with): no ticket");
  set<bool>(option[1], "<Selectable>k__BackingField", true);

  // The entry the game's reduceTicket takes from: the first valid one with the id.
  const uintptr_t loose = treasure(ticket_id, 5, -1, -1), blank = treasure(ticket_id, 4, 1, 0, false);
  const uintptr_t second = treasure(ticket_id, 3, 1, 1);
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {gold, loose, blank, second, held}));
  check(game::upgrade_ticket(state) == item_of(second),
        "an entry in no slot or an item without a Guid is passed over, as the game's get_IsValid does");
  // The panel's count (the editor's): every entry of the ticket's id added up, as
  // the game's getItemCountSum adds them - valid or not.
  game::Inventories inv;
  inv.treasure = treasures;
  inv.treasure_inventory = inventory;
  check(game::upgrade_tickets(inv) == 5 + 4 + 3 + 2, "the tickets held: every entry of the ticket added up");
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {gold, treasure(dlc_ticket_id, 1, 2, 0)}));
  check(game::upgrade_ticket(state) == 0, "no Exclusive Upgrade Ticket held (another treasure, the DLC's): none");
  check(game::upgrade_tickets(inv) == 0, "the tickets held: none (the DLC's is not counted)");
  set<uintptr_t>(inventory, "_Items", list_of(entries, entry_type, {held}));
  check(game::upgrade_ticket(state) == item_of(held), "the ticket alone in the treasures");
  check(game::upgrade_tickets(inv) == 2, "the tickets held: the one entry's count");
  check(game::upgrade_tickets(game::Inventories{}) == -1, "no treasures: the count unknown");

  // Not the exclusive upgrade's state, or one cut off from its parts.
  const uintptr_t other = make("chainsaw.gui.shop.InGameShopGuiState_CustomDefault");
  set<uintptr_t>(other, "<_Controller>k__BackingField", ctl);
  check(game::upgrade_ticket(other) == 0, "another of the shop's states: left alone");
  check(game::upgrade_ticket(ctl) == 0 && game::upgrade_ticket(0) == 0, "no state: nothing");
  set<uintptr_t>(params, "<TreasureInventoryController>k__BackingField", 0);
  check(game::upgrade_ticket(state) == 0, "no treasures: nothing");
  set<uintptr_t>(params, "<TreasureInventoryController>k__BackingField", treasures);
  set<uintptr_t>(behaviors, "_InventoryPopupGuiBehavior", 0);
  check(game::upgrade_ticket(state) == 0, "no list of ways to pay: nothing");
  set<uintptr_t>(behaviors, "_InventoryPopupGuiBehavior", popup);
  check(game::upgrade_ticket(state) == item_of(held), "all back: the ticket again");

  // --- the game's code the hook relies on (events.cpp kUpgradeHold) ---
  // The state's own getters read the fields the mod follows: the list of ways to
  // pay through the GUI behaviours' hub and the item popup, the treasures through
  // the parameters' hub.
  {
    const std::vector<int32_t> lb = code_of("chainsaw.gui.shop.InGameShopGuiState_CustomCommandBase",
                                            "get__LimitBreakSelectListGui", {}).loads();
    const std::vector<int32_t> want = {place("chainsaw.gui.shop.InGameShopGuiStateBase", "<_Controller>k__BackingField"),
                                       place("chainsaw.InGameShopStateController", "<BehaviorHub>k__BackingField"),
                                       place("chainsaw.gui.shop.InGameShopGuiBehaviorHub", "_InventoryPopupGuiBehavior"),
                                       place("chainsaw.InventoryPopupGuiBehavior", "_LimitBreakSelectListGui")};
    check(lb == want, "the state's get__LimitBreakSelectListGui: controller, GUI behaviours, item popup, its list");
    const std::vector<int32_t> ph = code_of("chainsaw.gui.shop.InGameShopGuiStateBase", "get__ParamHolder", {}).loads();
    const std::vector<int32_t> want_ph = {
        place("chainsaw.gui.shop.InGameShopGuiStateBase", "<_Controller>k__BackingField"),
        place("chainsaw.InGameShopStateController", "<InGameShopGuiParamHub>k__BackingField")};
    check(ph == want_ph, "the state's get__ParamHolder: controller, parameters' hub");
    const Code tc = code_of("chainsaw.gui.shop.InGameShopGuiStateBase", "get__TreasureInventoryController", {});
    const std::vector<int32_t> tl = tc.loads();
    check(tc.call_to(method_code("chainsaw.gui.shop.InGameShopGuiStateBase", "get__ParamHolder", {})) >= 0 &&
              tl.size() == 1 &&
              tl[0] == place("chainsaw.gui.shop.InGameShopGuiParamHub", "<TreasureInventoryController>k__BackingField"),
          "the state's get__TreasureInventoryController: the parameters' hub's");
  }
  // The handler: the ticket's way takes one ticket by its id; the DLC's ticket has a way of its own.
  const char* state_type = "chainsaw.gui.shop.InGameShopGuiState_CustomCommandDefault";
  const Code hold = code_of(state_type, "onHoldGaugeCompleted", {});
  const uintptr_t reduce = method_code("chainsaw.TreasureInventoryController", "reduceTicket", {"chainsaw.ItemID", "System.Int32"});
  const long takes = hold.call_to(reduce);
  const auto id = static_cast<uint32_t>(ticket_id);
  check(takes >= 0 &&
            hold.before(takes, {0x41, 0xB8, static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                                static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24)}) &&
            hold.before(takes, {0x41, 0xB9, 0x01, 0x00, 0x00, 0x00}),
        "onHoldGaugeCompleted: the ticket's way calls reduceTicket(the ticket's id, 1)");
  check(hold.call_to(method_code("chainsaw.TreasureInventoryController", "reduceTicket01", {"System.Int32"})) >= 0,
        "onHoldGaugeCompleted: the DLC's ticket has a way of its own (reduceTicket01)");
  const uintptr_t chosen = method_code("chainsaw.gui.commandmenu.LimitBreakSelectListGui", "get_CurrSelectedCommand", {});
  const uintptr_t selectable = method_code("chainsaw.gui.commandmenu.LimitBreakSelectListGui", "get_CurrSelectable", {});
  const long asks = hold.call_to(selectable), which = hold.call_to(chosen);
  check(asks >= 0 && which > asks && takes > which,
        "onHoldGaugeCompleted: asks the list whether the way chosen can be taken and which it is, before it pays");
  // The reduce: the first entry valid with the id, then the operation that empties
  // and removes it (a ticket count of 1 would leave the treasures without one).
  const Code rt = code_at(reduce);
  const uintptr_t find = method_code("chainsaw.gui.treasure.TreasureInventory", "getInventoryItem", {"chainsaw.ItemID"});
  const uintptr_t op = method_code("chainsaw.gui.treasure.TreasureInventory", "operation",
                                   {"chainsaw.gui.treasure.ReduceItemCountOperation"});
  const long found = rt.call_to(find);
  check(found >= 0 && rt.call_to(op, static_cast<size_t>(found)) > found,
        "reduceTicket: the treasures' entry for the id, then a reduce of its count");
  const int valid_slot = re::method_vt_index(re::find_method(re::find_type("chainsaw.InventoryItemBase"), "get_IsValid", {}));
  check(valid_slot > 0 && code_at(find).virtual_call(valid_slot) >= 0, "getInventoryItem: asks each entry get_IsValid");
  const Code valid = code_of("chainsaw.gui.treasure.TreasureInventoryItem", "get_IsValid", {});
  check(valid.call_to(method_code("chainsaw.Item", "get_IsValid", {})) >= 0,
        "TreasureInventoryItem.get_IsValid: its item's own (a Guid), then its slot");
  check(code_at(op).call_to(method_code(entries, "RemoveAll",
                                        {"System.Predicate`1<chainsaw.gui.treasure.TreasureInventoryItem>"})) >= 0,
        "the reduce removes an entry it empties from the treasures (List.RemoveAll)");

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
