// The Extra Content Shop window (exshop.cpp) over objects shaped like the game's,
// offline. exshop.cpp is built into this file (its insides reached by name: the
// shop's own code it calls is recorded here, not run):
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_exshop.cpp
//       <every build/*.o but dllmain.o and exshop.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32
//       -lole32 -lgdi32 -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_exshop.exe
//   wine tests/build/test_exshop.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// exshop::discover runs over it. Then the shop is built the way the VM lays out
// managed objects (as test_tickets does): GameRecordManager (its _Instance) with
// the shop's table (ExContentsShopUserData: an InfoData per item, in the shop's
// order), the items' ExShopData and the CP; share.ExtraContentsManager with the
// DLC owned; the shop's screen with its tabs, its page's list and cursor. The
// window's tick unlocks and locks them as the panel asks - only while the shop is
// up, never a purchased one, never the CP - and publishes what the shop shows of
// each (locked, for sale, not enough CP, purchased); after a change the screen's
// next frame has the shop's own code show its page again (the selected item's
// caption, then the entries in view) - once, not while the player holds to buy.
// And the game's code the window relies on (found by name, read in the mapped
// exe): what the shop reads of an item (isUnlockBonus, checkBuyBonus: _IsUnlock
// and _IsBuy unless the DLC with every main-game item is owned), how its grid
// picks a state (the CP against the price) and lets only an item for sale be
// bought, how it shows its page again (as after its own purchase, and as a move
// of its cursor does - nothing but the entries set up, the cursor left where it
// is), which tabs it shows, and that the game's own unlock writes the same flag.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "../src/exshop.cpp"
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

// A managed array: its element class at +0x10, its length at +0x1C, the elements from +0x20.
uintptr_t array_of(const char* elem_type, size_t n, size_t elem_size) {
  const uintptr_t arr = alloc(0x20 + elem_size * (n + 4));
  *reinterpret_cast<uintptr_t*>(arr + 0x10) = re::type_record(re::find_type(elem_type));
  *reinterpret_cast<int32_t*>(arr + 0x1C) = static_cast<int32_t>(n);
  return arr;
}
uintptr_t refs(const char* elem_type, const std::vector<uintptr_t>& items) {
  const uintptr_t arr = array_of(elem_type, items.size(), 8);
  for (size_t i = 0; i < items.size(); ++i) reinterpret_cast<uintptr_t*>(arr + 0x20)[i] = items[i];
  return arr;
}
// A List`1 of 32-bit values (its _items array, its _size).
uintptr_t values_list(const char* list_type, const char* elem_type, std::initializer_list<uint32_t> items) {
  const uintptr_t list = make(list_type);
  const uintptr_t arr = array_of(elem_type, items.size(), 4);
  int i = 0;
  for (const uint32_t v : items) reinterpret_cast<uint32_t*>(arr + 0x20)[i++] = v;
  set<uintptr_t>(list, "_items", arr);
  set<int32_t>(list, "_size", static_cast<int32_t>(items.size()));
  return list;
}

int enum_of(const char* type, const char* literal) { return re::enum_value(re::find_type(type), literal, -1); }

void plant_singleton(uintptr_t* statics, const char* type, uintptr_t obj) {
  const re::Field inst = re::find_field(re::find_type(type), "_Instance");
  if (!inst.valid() || !inst.is_static()) {
    std::printf("FAIL  no _Instance on %s\n", type);
    ++g_fail;
    return;
  }
  if (!statics[inst.declaring]) statics[inst.declaring] = alloc(0x100);
  *reinterpret_cast<uintptr_t*>(statics[inst.declaring] + inst.offset) = obj;
}

// --- the log: what the tick said since the last mark ---------------------------------------------
std::string g_log;
long g_log_mark = 0;
std::string log_since() {
  std::string out;
  if (FILE* f = std::fopen(g_log.c_str(), "rb")) {
    std::fseek(f, g_log_mark, SEEK_SET);
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
  }
  return out;
}
void mark_log() {
  if (FILE* f = std::fopen(g_log.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    g_log_mark = std::ftell(f);
    std::fclose(f);
  }
}
bool logged(const char* text) { return log_since().find(text) != std::string::npos; }

// --- the game's code ---------------------------------------------------------------------------------
// Every method's code (its RVA), in order: a method ends where the next begins.
std::vector<uint32_t> g_starts;
uintptr_t g_base = 0;

// A method's code in the mapped exe: up to the next method's.
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
  // `bytes` within the `window` bytes before offset `end`.
  bool before(long end, std::initializer_list<uint8_t> bytes, long window = 24) const {
    const long at_ = find(bytes, static_cast<size_t>(end > window ? end - window : 0));
    return end >= 0 && at_ >= 0 && at_ < end;
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
Code code_at(uintptr_t a, size_t max = 0x2000) {
  Code c;
  c.at = a;
  if (!a || a <= g_base) return c;
  const auto rva = static_cast<uint32_t>(a - g_base);
  const auto next = std::upper_bound(g_starts.begin(), g_starts.end(), rva);
  c.n = next != g_starts.end() ? std::min<size_t>(max, *next - rva) : max;
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
// Where a class's managed fields start in its objects (the game's REObjectInfo
// says, at run time): after a behaviour's or a user data's native part, else
// after the header.
int32_t start_of(uint32_t t) {
  if (re::derives(t, re::find_type("via.Behavior"))) return 0x50;
  if (re::derives(t, re::find_type("via.UserData"))) return 0x20;
  return 0x10;
}
// A field's place in its objects, by its declaring class.
int32_t place(const char* type, const char* field) {
  const re::Field f = re::find_field(re::find_type(type), field);
  return f.valid() ? static_cast<int32_t>(f.offset) + start_of(f.declaring) : -1;
}

// A List`1 of references (its _items array, its _size).
uintptr_t refs_list(const char* list_type, const char* elem_type, const std::vector<uintptr_t>& items) {
  const uintptr_t list = make(list_type);
  set<uintptr_t>(list, "_items", refs(elem_type, items));
  set<int32_t>(list, "_size", static_cast<int32_t>(items.size()));
  return list;
}

// --- the shop's own code, as the screen's frame asks for it (in place of exshop.cpp's g_code) ----
struct ShopCall {
  char what;  // 'c' its caption (setCaptionMessage), 'p' its page (updateCurrentGrid)
  uintptr_t screen;
  int32_t arg;  // the item's id; the selected entry's index
};
std::vector<ShopCall> g_shop_calls;
bool g_shop_code = true, g_shop_page_ok = true;
bool record_ready() { return g_shop_code; }
bool record_page(uintptr_t screen, int32_t index) {
  g_shop_calls.push_back({'p', screen, index});
  return g_shop_page_ok;
}
bool record_caption(uintptr_t screen, int32_t id) {
  g_shop_calls.push_back({'c', screen, id});
  return true;
}
// The calls since the last look, as "c38 p1" (each against `screen`), then forgotten.
std::string shop_calls(uintptr_t screen) {
  std::string out;
  for (const ShopCall& c : g_shop_calls)
    out += (out.empty() ? "" : " ") + std::string(1, c.screen == screen ? c.what : '?') + std::to_string(c.arg);
  g_shop_calls.clear();
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_exshop.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_exshop_" + std::to_string(GetCurrentProcessId()) + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  log_init(dir.c_str());
  g_log = dir + "RE4CabbyCodes.log";
  exshop::init();  // as DllMain does

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
  g_base = base;
  for (const uint32_t r : rvas)
    if (r) g_starts.push_back(r);
  std::sort(g_starts.begin(), g_starts.end());
  g_starts.erase(std::unique(g_starts.begin(), g_starts.end()), g_starts.end());
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
  check(exshop::discover(), "discovery: the shop's items, their state, its table and the CP");

  const char* const kCategory = "chainsaw.ExShopCategory";
  const int weapon = enum_of(kCategory, "Weapon"), costume = enum_of(kCategory, "Costume");
  const int figure = enum_of(kCategory, "Figure"), art = enum_of(kCategory, "Art");
  const int accessory = enum_of(kCategory, "Accessory"), cp11 = enum_of(kCategory, "CP11"), none = enum_of(kCategory, "None");
  const auto dlc_all = static_cast<uint32_t>(enum_of("share.ContentsHash", "DLC_3001"));
  const auto dlc_sw = static_cast<uint32_t>(enum_of("share.ContentsHash", "DLC_0001"));
  check(weapon > 0 && costume > weapon && accessory > art && cp11 > accessory && none == 0 && dlc_all && dlc_sw,
        "the shop's categories and the DLC hashes are the game's enum values and literals");
  check(std::strcmp(exshop::name(17) ? exshop::name(17) : "", "Jacket") == 0 &&
            std::strcmp(exshop::who(17) ? exshop::who(17) : "", "Leon") == 0 &&
            std::strcmp(exshop::kind(17) ? exshop::kind(17) : "", "Costume") == 0 && exshop::lock_text(17) &&
            std::strcmp(exshop::tab_name(exshop::kApparel), "APPAREL") == 0 && !exshop::lock_text(0) &&
            !exshop::name(500),
        "the names: Costume00 is Leon's Jacket with the shop's lock text, the tabs as the shop labels them");

  // --- the shop: six items of its five tabs, in the shop's order (not by id) ---
  struct Item {
    int id, category, cp;
    bool unlocked, bought;
  };
  const Item items[] = {
      {6, weapon, 1000, true, true},       // Chicago Sweeper: unlocked and bought
      {17, costume, 500, false, false},    // Jacket (Leon): locked
      {38, accessory, 1000, true, false},  // Glasses (Lexington): unlocked, more than the CP held
      {69, figure, 500, false, false},     // Leon (Jacket): locked
      {270, art, 300, true, false},        // Leon S. Kennedy: for sale
      {363, cp11, 2000, false, false},     // Dress (Ada), Separate Ways': locked
  };
  const int n_items = sizeof(items) / sizeof(items[0]);
  const uintptr_t mgr = make("chainsaw.GameRecordManager");
  const uintptr_t table = make("chainsaw.ExContentsShopUserData");
  std::vector<uintptr_t> infos;
  std::map<int, uintptr_t> state;
  for (const Item& it : items) {
    const uintptr_t info = make("chainsaw.ExContentsShopUserData.InfoData");
    set<int32_t>(info, "BonusId", it.id);
    set<int32_t>(info, "CPoint", it.cp);
    set<int32_t>(info, "Category", it.category);
    infos.push_back(info);
    const uintptr_t d = make("chainsaw.ExShopData");
    set<int32_t>(d, "_BonusID", it.id);
    set<bool>(d, "_IsUnlock", it.unlocked);
    set<bool>(d, "_IsBuy", it.bought);
    state[it.id] = d;
  }
  // Data of bonuses the shop does not sell: Professional mode, a Mercenaries model.
  for (const int id : {0, 426}) {
    const uintptr_t d = make("chainsaw.ExShopData");
    set<int32_t>(d, "_BonusID", id);
    state[id] = d;
  }
  std::vector<uintptr_t> datas;
  for (const auto& [id, d] : state) datas.push_back(d);
  set<uintptr_t>(table, "DataList", refs("chainsaw.ExContentsShopUserData.InfoData", infos));
  set<uintptr_t>(mgr, "ExShopSettingInfoData", table);
  set<uintptr_t>(mgr, "ExShopDataList", refs("chainsaw.ExShopData", datas));
  set<int32_t>(mgr, "RecordExPoint", 800);
  plant_singleton(statics, "chainsaw.GameRecordManager", mgr);
  const uintptr_t contents = make("share.ExtraContentsManager");
  const char* const kHashes = "System.Collections.Generic.List`1<System.UInt32>";
  set<uintptr_t>(contents, "ExtraContentsList", values_list(kHashes, "System.UInt32", {dlc_sw}));
  plant_singleton(statics, "share.ExtraContentsManager", contents);
  const uintptr_t screen = make("chainsaw.gui.ExContentsShopGuiBehavior");
  const char* const kTabs = "System.Collections.Generic.List`1<chainsaw.ExShopCategory>";
  auto shop_tabs = [&](std::initializer_list<uint32_t> cats) {
    set<uintptr_t>(screen, "CurrentCategoryTabList", values_list(kTabs, kCategory, cats));
  };

  auto unlocked = [&](int id) { return get<bool>(state[id], "_IsUnlock"); };
  auto bought = [&](int id) { return get<bool>(state[id], "_IsBuy"); };
  auto row = [](const exshop::View& v, int id) -> const exshop::Row* {
    for (int i = 0; i < v.count; ++i)
      if (v.row[i].id == id) return &v.row[i];
    return nullptr;
  };
  game::Run run;
  run.shop = true;
  // A tick (its log from here on); `fresh`: a while after the last, so it publishes without a change too (its
  // 500 ms by GetTickCount, which under Wine can count a little under 500 across a sleep of 510).
  auto tick = [&](const game::Run& r, bool fresh = false, bool mark = true) {
    if (fresh) Sleep(600);
    if (mark) mark_log();
    exshop::tick(r);
    return exshop::view();
  };

  // --- what the window shows ---
  exshop::View v = tick(run);
  bool order = v.count == n_items;
  for (int i = 0; order && i < n_items; ++i) order = v.row[i].id == items[i].id;
  check(v.known && v.up && order, "the shop's six items, in its own order (not by id); bonuses it does not sell left out");
  const exshop::Row* r6 = row(v, 6);
  const exshop::Row* r17 = row(v, 17);
  const exshop::Row* r38 = row(v, 38);
  const exshop::Row* r270 = row(v, 270);
  const exshop::Row* r363 = row(v, 363);
  check(r6 && r17 && r38 && r270 && r363 && r6->state == exshop::kPurchased && r17->state == exshop::kLocked &&
            r38->state == exshop::kShort && r270->state == exshop::kForSale && r363->state == exshop::kLocked &&
            r17->cp == 500 && v.cp == 800,
        "the states the shop shows: purchased, locked, not enough CP (1,000 > 800), for sale; prices and the CP");
  check(r6 && r17 && r38 && r363 && r6->tab == exshop::kWeapons && r17->tab == exshop::kApparel &&
            r38->tab == exshop::kApparel && row(v, 69)->tab == exshop::kModels && r270->tab == exshop::kArt &&
            r363->tab == exshop::kSeparateWays,
        "the tabs: the accessories under Apparel with the costumes, Separate Ways' under its own");
  bool all_tabs = true;
  for (int t = 0; t < exshop::kTabs; ++t) all_tabs = all_tabs && v.tab[t];
  check(all_tabs, "no frame of the shop's screen yet: every tab with an item is shown");
  shop_tabs({0, 1, 2, 3, 4});
  mark_log();
  exshop::screen_frame(screen);
  v = tick(run, true, false);
  check(!v.tab[exshop::kSeparateWays] && v.tab[exshop::kAll] && v.tab[exshop::kWeapons] && v.tab[exshop::kArt] &&
            logged("not Separate Ways'"),
        "the shop's screen without Separate Ways' tab (that DLC not installed): neither is the window's");
  shop_tabs({0, 1, 2, 3, 4, 8});
  exshop::screen_frame(screen);
  v = tick(run, true);
  check(v.tab[exshop::kSeparateWays], "the shop's screen with Separate Ways' tab: the window's too");

  // --- unlocking and locking ---
  exshop::request(17, true);
  v = tick(run);
  check(unlocked(17) && !bought(17) && row(v, 17)->state == exshop::kForSale &&
            get<int32_t>(mgr, "RecordExPoint") == 800 && logged("Costume: Leon's Jacket unlocked: the shop sells it for 500 CP."),
        "unlock: the Jacket unlocked - for sale, not bought, the CP as it was");
  check(logged("The change could not be saved"), "a change asks for the game's save (none here: it says so)");
  exshop::request(6, false);
  v = tick(run);
  check(unlocked(6) && bought(6) && logged("is purchased - it stays unlocked"),
        "lock a purchased item: it stays unlocked, and the note says so");
  exshop::request(38, false);
  v = tick(run);
  check(!unlocked(38) && row(v, 38)->state == exshop::kLocked && logged("locked again."), "lock: the Glasses locked again");
  const int some[] = {69, 270, 363};
  exshop::request_all(some, 3, true);
  v = tick(run);
  check(unlocked(69) && unlocked(270) && unlocked(363) && !bought(69) && !bought(363) &&
            logged("2 item(s) unlocked: the shop sells them for CP."),
        "unlock many: the two locked ones unlocked (one was already), summed up once");
  const int back[] = {17, 6, 69};
  exshop::request_all(back, 3, false);
  v = tick(run);
  check(!unlocked(17) && !unlocked(69) && unlocked(6) && logged("2 item(s) locked again; 1 purchased stay unlocked."),
        "lock many: two locked again, the purchased one stays (said once)");

  // --- the shop's page shown again: by its own code, from its screen's next frame ---
  exshop::screen_frame(screen);
  check(!v.instant && g_shop_calls.empty(),
        "no game calls (none offline): nothing asked of the shop's code, and the window says a change shows once its "
        "tab is switched");
  exshop::g_code = {record_ready, record_page, record_caption};
  const char* const kCells = "System.Collections.Generic.List`1<chainsaw.gui.ExShopGridItemGui>";
  const char* const kCell = "chainsaw.gui.ExShopGridItemGui";
  // The APPAREL tab's page: the Jacket, then the Glasses - the cursor on the Glasses.
  set<uintptr_t>(screen, "CurrentExShopList",
                 refs_list("System.Collections.Generic.List`1<chainsaw.ExContentsShopUserData.InfoData>",
                           "chainsaw.ExContentsShopUserData.InfoData", {infos[1], infos[2]}));
  set<int32_t>(screen, "CurrentListIndex", 1);
  set<uintptr_t>(screen, "_CurrentScrollGrid", make("via.gui.ScrollGrid"));
  set<uintptr_t>(screen, "CurrentGridList", refs_list(kCells, kCell, {}));
  set<uintptr_t>(screen, "_DisableGridItemList", refs_list(kCells, kCell, {}));
  exshop::request(17, true);
  v = tick(run);
  check(v.instant && unlocked(17) && g_shop_calls.empty(),
        "a change: the window says the shop shows it at once; the tick asks nothing of the shop's code");
  exshop::screen_frame(screen);
  check(shop_calls(screen) == "c38 p1" && logged("shop: the shop's page shown again by its own code"),
        "the screen's next frame: the selected item's caption (the Glasses), then the page from the cursor "
        "(updateCurrentGrid at entry 1) - on the screen the frame is the hook's");
  exshop::screen_frame(screen);
  check(shop_calls(screen).empty(), "...once: the frame after asks nothing");
  exshop::request(17, true);
  v = tick(run);
  exshop::screen_frame(screen);
  check(shop_calls(screen).empty(), "no change (the Jacket was unlocked already): nothing asked");
  exshop::request(17, false);
  v = tick(run);
  set<bool>(screen, "isHoldItem", true);
  exshop::screen_frame(screen);
  const std::string held = shop_calls(screen);
  set<bool>(screen, "isHoldItem", false);
  exshop::screen_frame(screen);
  check(held.empty() && shop_calls(screen) == "c38 p1",
        "the player holding to buy: the page waits for the hold's end, then shows the change");
  set<int32_t>(screen, "CurrentListIndex", 5);
  exshop::request(17, true);
  v = tick(run);
  exshop::screen_frame(screen);
  check(shop_calls(screen) == "p5", "the cursor past the tab's items: the page alone, no caption");
  set<int32_t>(screen, "CurrentListIndex", 0);
  set<uintptr_t>(screen, "CurrentGridList", uintptr_t{0});
  exshop::request(17, false);
  v = tick(run);
  exshop::screen_frame(screen);
  set<uintptr_t>(screen, "CurrentGridList", refs_list(kCells, kCell, {}));
  exshop::screen_frame(screen);
  check(shop_calls(screen).empty() && logged("its grid is not set up"),
        "the screen's grid not set up (updateCurrentGrid would read it unchecked): nothing asked, then or later");
  exshop::request(17, true);
  v = tick(run);
  tick(game::Run{});  // the shop closing
  v = tick(run);
  exshop::screen_frame(screen);
  check(shop_calls(screen).empty(), "the shop closed before its next frame: nothing asked (it opens with its page set up)");
  g_shop_code = false;
  exshop::request(17, false);
  v = tick(run);
  exshop::screen_frame(screen);
  check(!v.instant && shop_calls(screen).empty(),
        "the shop's code not to be called (game calls off): nothing asked, and the window says so");
  g_shop_code = true;
  g_shop_page_ok = false;
  exshop::request(17, true);
  v = tick(run);
  exshop::screen_frame(screen);
  check(shop_calls(screen) == "c17 p0" && logged("the shop's page could not be shown again"),
        "a call that fails: logged, nothing more asked (the bridge has turned game calls off: the window then says a "
        "tab switch shows the change)");
  g_shop_page_ok = true;
  exshop::request(17, false);
  v = tick(run);
  exshop::screen_frame(screen);
  shop_calls(screen);

  // --- only while the shop is up; only the shop's items ---
  game::Run closed;
  exshop::request(17, true);
  v = tick(closed);
  check(!unlocked(17) && !v.up && logged("only unlocked while the Extra Content Shop is open"),
        "the shop closed: nothing is unlocked, and the note says why");
  v = tick(run);
  exshop::request(0, true);
  v = tick(run);
  check(!unlocked(0) && logged("is not in the shop's list"), "a bonus the shop does not sell (Professional mode) is left alone");

  // --- the DLC with every main-game item ---
  set<uintptr_t>(contents, "ExtraContentsList", values_list(kHashes, "System.UInt32", {dlc_sw, dlc_all}));
  v = tick(run, true);
  bool dlc_rows = true;
  for (const int id : {6, 17, 38, 69, 270}) {
    const exshop::Row* r = row(v, id);
    dlc_rows = dlc_rows && r && r->by_dlc && r->bought && r->unlocked && r->state == exshop::kPurchased;
  }
  check(v.dlc && dlc_rows && row(v, 363) && !row(v, 363)->by_dlc && row(v, 363)->unlocked == unlocked(363),
        "that DLC owned: every main-game item purchased as the shop shows it; Separate Ways' by its data");
  exshop::request(17, true);
  v = tick(run);
  check(!unlocked(17) && logged("unlocked and purchased by a DLC you own"), "...and nothing written for them");
  set<uintptr_t>(contents, "ExtraContentsList", values_list(kHashes, "System.UInt32", {dlc_sw}));

  // --- the screen's tabs grow stale ---
  Sleep(2100);
  v = tick(run);
  check(v.tab[exshop::kSeparateWays], "no frame of the shop's screen for 2 s: every tab with an item again");

  // --- the game's code the window relies on ---
  const char* const kMgr = "chainsaw.GameRecordManager";
  const uintptr_t get_data = method_code(kMgr, "getExShopData", {"chainsaw.ExShopBonusID"});
  const uintptr_t has = method_code("share.ExtraContentsManager", "hasContents", {"System.UInt32"});
  const auto unlock_at = static_cast<uint8_t>(place("chainsaw.ExShopData", "_IsUnlock"));
  const auto buy_at = static_cast<uint8_t>(place("chainsaw.ExShopData", "_IsBuy"));
  auto mov_r8d = [](uint32_t v32) {
    return std::vector<uint8_t>{0x41, 0xB8, static_cast<uint8_t>(v32), static_cast<uint8_t>(v32 >> 8),
                                static_cast<uint8_t>(v32 >> 16), static_cast<uint8_t>(v32 >> 24)};
  };
  auto has_bytes = [](const Code& c, const std::vector<uint8_t>& b) {
    for (size_t i = 0; i + b.size() <= c.n; ++i) {
      size_t k = 0;
      while (k < b.size() && c.u8(i + k) == b[k]) ++k;
      if (k == b.size()) return true;
    }
    return false;
  };
  // The items the DLC leaves to their data: Separate Ways' (CP11_Weapon00..CP11End), its
  // Professional mode (GameMode01), the Mercenaries' (CP12_Costume00..CP12End).
  const char* const kId = "chainsaw.ExShopBonusID";
  const int cp11_first = enum_of(kId, "CP11_Weapon00"), cp11_last = enum_of(kId, "CP11End");
  const int cp12_first = enum_of(kId, "CP12_Costume00"), cp12_last = enum_of(kId, "CP12End");
  auto lea_eax_rbx = [](int disp) {
    const auto d = static_cast<uint32_t>(-disp);
    return std::vector<uint8_t>{0x8D, 0x83, static_cast<uint8_t>(d), static_cast<uint8_t>(d >> 8),
                                static_cast<uint8_t>(d >> 16), static_cast<uint8_t>(d >> 24)};
  };
  auto dlc_rule = [&](const Code& c) {
    return has_bytes(c, mov_r8d(dlc_all)) && c.call_to(has) >= 0 && has_bytes(c, lea_eax_rbx(cp11_first)) &&
           has_bytes(c, {0x83, 0xF8, static_cast<uint8_t>(cp11_last - cp11_first)}) &&
           has_bytes(c, {0x83, 0xFB, static_cast<uint8_t>(enum_of(kId, "GameMode01"))}) &&
           has_bytes(c, lea_eax_rbx(cp12_first)) &&
           has_bytes(c, {0x83, 0xF8, static_cast<uint8_t>(cp12_last - cp12_first)});
  };
  const Code is_unlock = code_of(kMgr, "isUnlockBonus", {"chainsaw.ExShopBonusID"});
  const long iu_data = is_unlock.call_to(get_data);
  check(iu_data >= 0 && is_unlock.find({0x80, 0x78, unlock_at, 0x00}, static_cast<size_t>(iu_data)) > iu_data &&
            dlc_rule(is_unlock),
        "isUnlockBonus: the item's _IsUnlock - but true for every main-game item with that DLC (not CP11, "
        "GameMode01, CP12)");
  const Code check_buy = code_of(kMgr, "checkBuyBonus", {"chainsaw.ExShopBonusID"});
  const long cb_data = check_buy.call_to(get_data);
  check(cb_data >= 0 && check_buy.find({0x80, 0x78, buy_at, 0x00}, static_cast<size_t>(cb_data)) > cb_data &&
            dlc_rule(check_buy),
        "checkBuyBonus: the item's _IsBuy - true for the same items with that DLC");

  const char* const kScreen = "chainsaw.gui.ExContentsShopGuiBehavior";
  const char* const kGrid = "chainsaw.gui.ExShopGridItemGui";
  const uintptr_t is_unlock_code = method_code(kMgr, "isUnlockBonus", {"chainsaw.ExShopBonusID"});
  const uintptr_t check_buy_code = method_code(kMgr, "checkBuyBonus", {"chainsaw.ExShopBonusID"});
  const uintptr_t setup_item = method_code(kGrid, "setupItem",
                                           {"chainsaw.ExContentsShopUserData.InfoData", "System.Boolean", "System.Boolean",
                                            "System.Boolean", "System.Boolean"});
  const Code grid = code_of(kScreen, "updateCurrentGrid", {"System.Int32", "via.gui.GridScrollDirection"});
  const long g_points = grid.call_to(method_code(kMgr, "getExPoint", {}));
  const long g_unlock = grid.call_to(is_unlock_code), g_buy = grid.call_to(check_buy_code);
  const long g_setup = grid.call_to(setup_item);
  const auto cp_at = static_cast<uint8_t>(place("chainsaw.ExContentsShopUserData.InfoData", "CPoint"));
  check(g_points >= 0 && g_unlock > g_points && g_buy > g_unlock && g_setup > g_buy &&
            grid.before(g_unlock, {0x41, 0x3B, 0x47, cp_at}, 24) && grid.before(g_unlock, {0x40, 0x0F, 0x9C}, 24),
        "updateCurrentGrid: each item's grid entry set up from isUnlockBonus, checkBuyBonus and the CP held "
        "against its price (getExPoint < CPoint)");
  const Code item = code_at(setup_item);
  const auto state_at = static_cast<uint8_t>(place(kGrid, "_CurrentState"));
  const char* const kState = "chainsaw.gui.ExShopItemState";
  check(item.find({0x89, 0x5E, state_at}) >= 0 && enum_of(kState, "Default") == 0 &&
            item.find({0xBB, static_cast<uint8_t>(enum_of(kState, "Purchased")), 0, 0, 0}) >= 0 &&
            item.find({0xBB, static_cast<uint8_t>(enum_of(kState, "Insufficient")), 0, 0, 0}) >= 0 &&
            item.find({0xBB, static_cast<uint8_t>(enum_of(kState, "Lock")), 0, 0, 0}) >= 0,
        "setupItem: the entry's _CurrentState - Purchased, Insufficient, Lock or Default");
  const Code late = code_of(kScreen, "lateUpdateOnActive", {});
  const long hold = late.call_to(method_code(kGrid, "setHold", {"System.Action"}));
  check(hold >= 0 && late.before(hold, {0x83, 0x78, state_at, 0x00}, 64),
        "lateUpdateOnActive: a purchase starts only on an entry whose _CurrentState is Default (for sale)");
  const Code buy = code_of(kScreen, "buyBonusItem", {});
  const long paid = buy.call_to(method_code(kMgr, "addExPoint", {"System.Int32"}));
  const long got = buy.call_to(method_code(kMgr, "setBuyBonus", {"chainsaw.ExShopBonusID", "chainsaw.ExShopCategory"}));
  const long asked = buy.call_to(is_unlock_code);
  check(paid >= 0 && got > paid && (asked < 0 || asked > got),
        "buyBonusItem: pays and buys without asking again (the entry's state is the shop's only gate)");
  const uintptr_t setup_list = method_code(kScreen, "setupList", {"chainsaw.ExShopCategory"});
  const Code tab = code_of(kScreen, "onTabSelectChanged", {});
  const Code list = code_at(setup_list);
  const std::vector<int32_t> list_loads = list.loads();
  auto loads_at = [&](int32_t d) {
    for (const int32_t l : list_loads)
      if (l == d) return true;
    return false;
  };
  const uintptr_t grid_code = method_code(kScreen, "updateCurrentGrid", {"System.Int32", "via.gui.GridScrollDirection"});
  check(tab.call_to(setup_list) >= 0 && list.call_to(grid_code) >= 0,
        "a tab switch sets the list up again (setupList -> updateCurrentGrid): the shop shows the window's changes then "
        "too");

  // How the shop shows its page again - the mod's two calls.
  const uintptr_t caption_code = method_code(kScreen, "setCaptionMessage", {"chainsaw.ExShopBonusID"});
  const int32_t index_at = place(kScreen, "CurrentListIndex");
  // A 32-bit load of CurrentListIndex into r8d (mov r8d,[reg+disp32]) and xor r9d,r9d within the bytes before a call.
  auto passes_index = [&](const Code& c, long call) {
    for (long i = std::max(0L, call - 24); i + 7 <= call; ++i)
      if (c.u8(i) == 0x44 && c.u8(i + 1) == 0x8B && (c.u8(i + 2) & 0xF8) == 0x80 && (c.u8(i + 2) & 7) != 4 &&
          c.i32(i + 3) == index_at)
        return c.before(call, {0x45, 0x33, 0xC9}, 24);
    return false;
  };
  // A 32-bit store to CurrentListIndex (mov [reg+disp32],r32).
  auto stores_index = [&](const Code& c) {
    for (size_t i = 0; i + 6 <= c.n; ++i)
      if (c.u8(i) == 0x89 && (c.u8(i + 1) & 0xC0) == 0x80 && (c.u8(i + 1) & 7) != 4 && c.i32(i + 2) == index_at)
        return true;
    return false;
  };
  const long bought_page = buy.call_to(grid_code);
  check(bought_page > got && passes_index(buy, bought_page) && enum_of("via.gui.GridScrollDirection", "None") == 0,
        "buyBonusItem: the purchase shows itself with updateCurrentGrid(CurrentListIndex, None) - the mod's call");
  const uintptr_t gauge = method_code(kGrid, "setHoldGuage", {"System.Single"});
  check(late.call_to(gauge) >= 0 && code_at(gauge).call_to(method_code("System.Action", "Invoke", {})) >= 0,
        "...from the screen's frame: lateUpdateOnActive fills the hold's gauge, which calls the hold's callback (the "
        "purchase) once full");
  const Code scroll = code_of(kScreen, "onScrollEvent", {"via.gui.GridScrollDirection"});
  const long scroll_page = scroll.call_to(grid_code);
  const int32_t hold_at = place(kScreen, "isHoldItem");
  check(late.find({0x80, 0xBB, static_cast<uint8_t>(hold_at), static_cast<uint8_t>(hold_at >> 8), 0, 0, 0x00}) >= 0 &&
            scroll_page >= 0 &&
            scroll.call_to(method_code(kScreen, "endHoldAll", {}), static_cast<size_t>(scroll_page)) > scroll_page,
        "a hold to buy lasts frames (isHoldItem), and the shop's own scroll ends one after setting its page up: the "
        "mod's page waits for it");
  // Every method updateCurrentGrid calls (the VM's own helpers aside): the grid's items, each entry's set-up, the
  // items' state, its two lists' growth.
  const std::vector<uintptr_t> sets_up = {
      method_code("via.gui.ItemsControl", "get_Items", {}),
      method_code(kCell, ".ctor", {"via.gui.SelectItem"}),
      method_code(kCell, "setIconTexture", {"via.uvsequence.UVSequenceResourceHolder"}),
      setup_item,
      method_code(kCell, "setEnableItem", {"System.Boolean"}),
      method_code(kCell, "setNewMark", {"System.Boolean"}),
      get_data,
      method_code(kMgr, "getExPoint", {}),
      is_unlock_code,
      check_buy_code,
      method_code(kMgr, "checkNewIcon", {"chainsaw.ExShopBonusID", "System.Boolean"}),
      method_code(kCells, "AddWithResize", {kCell}),
  };
  int grid_calls = 0, grid_others = 0;
  for (size_t i = 0; i + 5 <= grid.n; ++i) {
    if (grid.u8(i) != 0xE8) continue;
    const uintptr_t t = grid.at + i + 5 + static_cast<intptr_t>(grid.i32(i + 1));
    if (t <= g_base || !std::binary_search(g_starts.begin(), g_starts.end(), static_cast<uint32_t>(t - g_base))) continue;
    ++grid_calls;
    if (std::find(sets_up.begin(), sets_up.end(), t) == sets_up.end()) {
      ++grid_others;
      std::printf("      updateCurrentGrid calls exe+0x%llX\n", static_cast<unsigned long long>(t - g_base));
    }
  }
  const std::vector<int32_t> grid_loads = grid.loads();
  auto grid_reads = [&](const char* field) {
    return std::find(grid_loads.begin(), grid_loads.end(), place(kScreen, field)) != grid_loads.end();
  };
  check(grid_calls >= 10 && grid_others == 0 && !stores_index(grid),
        "updateCurrentGrid: only sets the entries in view up from the items' state - nothing that moves the cursor, "
        "scrolls, buys or clears a NEW mark");
  check(grid_reads("_CurrentScrollGrid") && grid_reads("CurrentGridList") && grid_reads("_DisableGridItemList") &&
            grid_reads("CurrentExShopList"),
        "...and reads the screen's grid and its lists (the mod sees they are there first)");
  const Code cap = code_at(caption_code);
  const long c_info = cap.call_to(method_code(kMgr, "getExContentsShopInfoData", {"chainsaw.ExShopBonusID"}));
  const long c_unlock = cap.call_to(is_unlock_code), c_buy = cap.call_to(check_buy_code);
  const long c_text = cap.call_to(method_code("chainsaw.GuiExtension", "setMessage", {"via.gui.Text", "System.Guid"}, true),
                                  c_buy > 0 ? static_cast<size_t>(c_buy) : 0);
  check(c_info >= 0 && c_unlock > c_info && c_buy > c_unlock && c_text > c_buy,
        "setCaptionMessage: the item's name and text after asking isUnlockBonus and checkBuyBonus (\"???\" and its lock "
        "text while it is locked)");
  const Code sel = code_of(kScreen, "onSelectionChanged", {});
  const long s_caption = sel.call_to(caption_code);
  const long s_page = sel.call_to(grid_code, s_caption > 0 ? static_cast<size_t>(s_caption) : 0);
  check(stores_index(sel) && s_caption >= 0 && s_page > s_caption,
        "onSelectionChanged (a move of the cursor): CurrentListIndex, the selected item's caption, then the page - the "
        "mod's two calls, in that order");
  check(loads_at(place(kMgr, "ExShopSettingInfoData")) && loads_at(place("chainsaw.ExContentsShopUserData", "DataList")) &&
            has_bytes(list, {0x41, 0x83, 0xFD, static_cast<uint8_t>(costume)}) &&
            has_bytes(list, {0x83, 0xF8, static_cast<uint8_t>(accessory)}) && has_bytes(list, mov_r8d(dlc_sw)) &&
            has_bytes(list, {0x83, 0x7F, static_cast<uint8_t>(place("chainsaw.ExContentsShopUserData.InfoData", "Category")),
                             static_cast<uint8_t>(cp11)}),
        "setupList: the table's items in order (GameRecordManager.ExShopSettingInfoData.DataList), the accessories "
        "under the costumes' tab, Separate Ways' only with that DLC");
  const Code setup = code_of(kScreen, "setup", {"chainsaw.gui.ExContentsShopGuiBehavior.OpenParam"});
  const long sw = setup.find({0x41, 0xB8, static_cast<uint8_t>(dlc_sw), static_cast<uint8_t>(dlc_sw >> 8),
                              static_cast<uint8_t>(dlc_sw >> 16), static_cast<uint8_t>(dlc_sw >> 24)});
  check(sw > 0 && setup.call_to(has, static_cast<size_t>(sw)) > sw &&
            setup.find({0x41, 0xB8, static_cast<uint8_t>(cp11), 0, 0, 0}, static_cast<size_t>(sw)) > sw &&
            setup.find({0x41, 0xB8, static_cast<uint8_t>(art), 0, 0, 0}) < sw,
        "setup: the shop's tabs All, Weapon, Costume, Figure, Art, then CP11 only with that DLC");
  const Code unlock = code_of(kMgr, "setUnlockBonus", {"chainsaw.ExShopBonusID"});
  const long icon = unlock.call_to(method_code(kMgr, "setUnlockNewIcon", {"chainsaw.ExShopBonusID"}));
  check(icon >= 0 && unlock.find({0xC6, 0x43, unlock_at, 0x01}, static_cast<size_t>(icon)) > icon,
        "setUnlockBonus (the game's own unlock): its NEW mark, then _IsUnlock = 1 - the flag the window writes");

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
