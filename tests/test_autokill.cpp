// Kill enemies as they appear (game.cpp: kill_targets, hit_started, body_hit_controller, key_hash,
// shot_attack_key, fill_kill_attack, fill_kill_damage - what cheats.cpp's tick uses to
// ask for a hit from the player on every enemy in play, through
// HitController.requestAttack) over objects shaped like the game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_autokill.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_autokill.exe
//   wine tests/build/test_autokill.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then the game's objects are built the way the VM
// lays out managed ones (as test_stats does), every field placed by name from the
// real database: CharacterManager's enemies (their contexts, health, body updaters
// and hit controllers with their damage effects; a Ganado's head, body and
// parasite), and the HitManager's attack data catalog (its merged dictionary, a
// RefCounter per AttackDataID, the AttackHitUserData's list). The enemies a hit
// could kill are picked as the tick picks them - in play only: an enemy whose game
// object has not started is not hit (the first build's crash, 2026-10-26) - a shot's
// attack data is found as the tick finds it, and the request's
// two objects are filled as the tick fills them. The mod's hash of a name is
// checked against the game's own hash function, run in the mapped exe. And the
// game's code the mod relies on (found by name, read in the mapped exe): a request
// reads the attack data's ID and key where the mod writes them and looks the key up
// in the catalog; it queues the hit on the attacker's request pool, which the hit
// controller's update hands to the HitManager; the HitManager works a hit out
// through the damaged one's OnCalculateDamage (where the mod's hook raises it) and
// then its OnDamageHit; the hook knows the mod's hit by the number the request
// carries; the Mercenaries scores and counts a death in the combo whatever killed
// it; and a hit's effect calls the damage effect's players, which only its start
// makes, with no null check.
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
  const uintptr_t o = alloc(0x1000);  // a Ganado's head updater's fields reach 0x778
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
uintptr_t refs(const char* elem_type, std::initializer_list<uintptr_t> items) {
  const uintptr_t arr = array_of(elem_type, items.size(), 8);
  int i = 0;
  for (const uintptr_t it : items) reinterpret_cast<uintptr_t*>(arr + 0x20)[i++] = it;
  return arr;
}
// A List`1 of references (its _items array, its _size).
uintptr_t list_of(const char* list_type, const char* elem_type, std::initializer_list<uintptr_t> items) {
  const uintptr_t list = make(list_type);
  set<uintptr_t>(list, "_items", refs(elem_type, items));
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
  // The first instruction `opcode` (after any REX prefix) whose memory operand is
  // [reg+disp32] (ModRM mod 10, no SIB) at `disp`: the opcode's offset, or -1.
  long on(std::initializer_list<uint8_t> opcode, int32_t disp, size_t from = 0) const {
    for (size_t i = from; i + opcode.size() + 5 <= n; ++i) {
      size_t k = 0;
      for (const uint8_t b : opcode)
        if (u8(i + k) == b) ++k;
        else break;
      if (k != opcode.size()) continue;
      const uint8_t m = u8(i + k);
      if ((m >> 6) == 2 && (m & 7) != 4 && i32(i + k + 1) == disp) return static_cast<long>(i);
    }
    return -1;
  }
  // The same with a one-byte displacement (ModRM mod 01).
  long on8(std::initializer_list<uint8_t> opcode, int8_t disp, size_t from = 0) const {
    for (size_t i = from; i + opcode.size() + 2 <= n; ++i) {
      size_t k = 0;
      for (const uint8_t b : opcode)
        if (u8(i + k) == b) ++k;
        else break;
      if (k != opcode.size()) continue;
      const uint8_t m = u8(i + k);
      if ((m >> 6) == 1 && (m & 7) != 4 && static_cast<int8_t>(u8(i + k + 1)) == disp) return static_cast<long>(i);
    }
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
};
Code code_at(uintptr_t a, size_t max = 0x1000) {
  Code c;
  c.at = a;
  while (c.at && c.n < max && !(c.u8(c.n) == 0xC3 && c.u8(c.n + 1) == 0xCC) &&
         !(c.u8(c.n) == 0xCC && c.u8(c.n + 1) == 0xCC && c.u8(c.n + 2) == 0xCC))
    ++c.n;
  if (c.at && c.u8(c.n) == 0xC3) ++c.n;
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
Code code_of(const char* type, const char* method, std::initializer_list<const char*> params, size_t max = 0x1000) {
  return code_at(method_code(type, method, params), max);
}
int32_t offset_of(const char* type, const char* field) {
  const re::Field f = re::find_field(re::find_type(type), field);
  return f.valid() ? static_cast<int32_t>(f.offset) : -1;
}
// A getter `mov rax/eax,[rdx+disp]; ret` or `movzx eax,byte [rdx+disp]; ret`: where it reads, or -1.
int32_t getter_place(const char* type, const char* method) {
  const Code c = code_of(type, method, {});
  if (!c.at) return -1;
  // A bool's: `cmp byte [rdx+d],0; setne al; ret`.
  if (c.u8(0) == 0x80 && c.u8(1) == 0x7A && c.u8(3) == 0x00 && c.u8(4) == 0x0F && c.u8(5) == 0x95)
    return static_cast<int8_t>(c.u8(2));
  if (c.u8(0) == 0x80 && c.u8(1) == 0xBA && c.u8(6) == 0x00 && c.u8(7) == 0x0F && c.u8(8) == 0x95) return c.i32(2);
  size_t i = 0;
  if (c.u8(0) == 0x48 && c.u8(1) == 0x8B) i = 2;          // mov rax,[rdx+d]
  else if (c.u8(0) == 0x8B) i = 1;                         // mov eax,[rdx+d]
  else if (c.u8(0) == 0x0F && c.u8(1) == 0xB6) i = 2;      // movzx eax,byte [rdx+d]
  else return -1;
  if (c.u8(i) == 0x42 && c.u8(i + 2) == 0xC3) return static_cast<int8_t>(c.u8(i + 1));
  if (c.u8(i) == 0x82 && c.u8(i + 5) == 0xC3) return c.i32(i + 1);
  return -1;
}
int32_t ref_getter_place(const char* type, const char* method) { return getter_place(type, method); }
// An instruction `opcode` on [reg+disp], whichever displacement size the compiler chose.
long on_disp(const Code& c, std::initializer_list<uint8_t> opcode, int32_t disp, size_t from = 0) {
  return disp >= -128 && disp < 128 ? c.on8(opcode, static_cast<int8_t>(disp), from) : c.on(opcode, disp, from);
}
// A 64-bit `op` (after any REX.W prefix: 48, 49, 4C, 4D - whichever registers) on [reg+disp]: the first, or -1.
long on_rex(const Code& c, uint8_t op, int32_t disp, size_t from = 0) {
  long best = -1;
  for (const uint8_t rex : {0x48, 0x49, 0x4C, 0x4D}) {
    const long at = on_disp(c, {rex, op}, disp, from);
    if (at >= 0 && (best < 0 || at < best)) best = at;
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_autokill.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_autokill_" + std::to_string(GetCurrentProcessId()) + "\\";
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
  check(game::parts().auto_kill,
        "discovery: Kill enemies as they appear (the enemies, their hit controllers, a Ganado's parasite, the HitManager's "
        "attack data, a hit request's two objects)");
  check(game::parts().spawn_counts, "discovery: its log's spawn counts (EnemyManager's active count and limit, the Mercenaries')");

  // --- the name of a weapon, and its hash ---
  const uint32_t weapon_id = re::find_type("chainsaw.WeaponID");
  check(re::enum_name(weapon_id, 4000) == "wp4000" && re::enum_name(weapon_id, 4900) == "wp4900" &&
            re::enum_name(weapon_id, -1) == "Invalid" && re::enum_name(weapon_id, 31337).empty() &&
            re::enum_name(0, 4000).empty(),
        "re::enum_name: WeaponID 4000 is wp4000, 4900 wp4900, -1 Invalid; no literal holds 31337");
  // The game's own hash of a name: the call BulletShell.getAttackKeyNameHash ends
  // with (`lea rcx,[string+0x14]` - the string's characters - then the call), run
  // here in the mapped exe on the same text.
  const Code key_code = code_of("chainsaw.BulletShell", "getAttackKeyNameHash", {});
  const long hash_call = key_code.find({0x48, 0x8D, 0x4B, 0x14, 0xE8});
  const uintptr_t game_hash =
      hash_call >= 0 ? key_code.at + static_cast<size_t>(hash_call) + 9 + key_code.i32(static_cast<size_t>(hash_call) + 5) : 0;
  check(game_hash && mem::section(g_game, ".text").contains(game_hash),
        "BulletShell.getAttackKeyNameHash hashes its key through the game's string hash (lea rcx,[str+0x14]; call)");
  if (game_hash) {
    using HashFn = uint32_t (*)(const wchar_t*);
    const auto h = reinterpret_cast<HashFn>(game_hash);
    bool same = true;
    for (const char* name : {"Default", "wp4000", "wp4001", "wp4100", "wp4400", "wp6000", "a", "ab", "abc", "wp4000_Fit"}) {
      std::wstring w(name, name + std::strlen(name));
      if (h(w.c_str()) != game::key_hash(name)) {
        std::printf("      %s: the game's %08X, the mod's %08X\n", name, h(w.c_str()), game::key_hash(name));
        same = false;
      }
    }
    check(same, "game::key_hash is the game's own string hash (murmur3 of the UTF-16 text): weapons' names, odd and even "
                "lengths");
  }
  const uint32_t merc_ctl = re::find_type("chainsaw.MercenariesModeController");
  check(game::key_hash("Default") == static_cast<uint32_t>(re::enum_value(merc_ctl, "ATTACKKEY_HASH_DEFAULT", 0)) &&
            game::key_hash("Default") == 1106175613u,
        "and the hash of \"Default\" is the game's own literal (MercenariesModeController.ATTACKKEY_HASH_DEFAULT)");
  check(game::key_hash("wp4000") == 2306206225u,
        "the handgun's key (wp4000: 2306206225, as the bullets' attack data in the archives holds it)");

  // --- a shot's attack data: the HitManager's catalog ---
  const int bullet = enum_of("chainsaw.collision.AttackDataID", "Bullet");
  const int shot = enum_of("chainsaw.character.AttackType", "Shot");
  const int slash = enum_of("chainsaw.character.AttackType", "Slash");
  check(bullet == 2 && shot == 1 && slash == 2, "AttackDataID.Bullet 2, AttackType Shot 1 and Slash 2");
  check(game::hit_manager() == 0 && game::shot_attack_key(4000) == 0, "no HitManager yet: no attack data");
  const uintptr_t hm = make("chainsaw.HitManager");
  plant_singleton(statics, "chainsaw.HitManager", hm);
  check(game::hit_manager() == hm, "the HitManager: its AppSingleton's _Instance");
  const uintptr_t catalog = make("share.DictionaryCatalog`2<chainsaw.collision.AttackDataID,chainsaw.collision.AttackHitUserData>");
  const uintptr_t merged =
      make("System.Collections.Generic.Dictionary`2<chainsaw.collision.AttackDataID,share.RefCounter`1<chainsaw.collision.AttackHitUserData>>");
  set<uintptr_t>(hm, "AttackDataCatalog", catalog);
  set<uintptr_t>(catalog, "_MergedCatalog", merged);
  auto attack_data = [&](uint32_t key, int type) {
    const uintptr_t ad = make("chainsaw.collision.AttackHitUserData.AttackData");
    set<uint32_t>(ad, "_KeyNameHash", key);
    set<int32_t>(ad, "_AttackType", type);
    return ad;
  };
  auto hit_data = [&](std::initializer_list<uintptr_t> list) {
    const uintptr_t ud = make("chainsaw.collision.AttackHitUserData");
    set<uintptr_t>(ud, "_AttackDataList", refs("chainsaw.collision.AttackHitUserData.AttackData", list));
    const uintptr_t counter = make("share.RefCounter`1<chainsaw.collision.AttackHitUserData>");
    set<uintptr_t>(counter, "_Item", ud);
    return counter;
  };
  // The merged dictionary's entries {hashCode, next, key, value}, as Dictionary`2 lays them out.
  const char* entry_type =
      "System.Collections.Generic.Dictionary`2.Entry<chainsaw.collision.AttackDataID,share.RefCounter`1<chainsaw.collision.AttackHitUserData>>";
  const uint32_t entry_t = re::find_type(entry_type);
  const uint32_t entry_size = re::value_size(entry_t);
  check(entry_size == 0x18 && offset_of(entry_type, "key") == 8 && offset_of(entry_type, "value") == 0x10,
        "the dictionary's entries: 24 bytes, the key at +8, the value last (what game::dict_ref reads)");
  auto catalog_of = [&](std::initializer_list<std::pair<int, uintptr_t>> ids) {
    const uintptr_t arr = array_of(entry_type, ids.size(), entry_size);
    int i = 0;
    for (const auto& [id, counter] : ids) {
      const uintptr_t e = arr + 0x20 + static_cast<uintptr_t>(i++) * entry_size;
      *reinterpret_cast<int32_t*>(e) = id;  // hashCode: >= 0 in use
      *reinterpret_cast<int32_t*>(e + 4) = -1;
      *reinterpret_cast<int32_t*>(e + 8) = id;
      *reinterpret_cast<uintptr_t*>(e + 0x10) = counter;
    }
    set<uintptr_t>(merged, "_entries", arr);
    set<int32_t>(merged, "_count", static_cast<int32_t>(ids.size()));
  };
  const uint32_t k_default = game::key_hash("Default"), k_4000 = game::key_hash("wp4000"),
                 k_4100 = game::key_hash("wp4100"), k_knife = game::key_hash("wp5000");
  catalog_of({{enum_of("chainsaw.collision.AttackDataID", "ch1_c0z0"), hit_data({attack_data(k_default, shot)})}});
  check(game::shot_attack_key(4000) == 0, "a catalog without the bullets' attack data: none");
  const uintptr_t bullets = hit_data({attack_data(k_knife, slash), attack_data(k_default, shot), attack_data(k_4000, shot),
                                      attack_data(k_4100, shot)});
  catalog_of({{enum_of("chainsaw.collision.AttackDataID", "ch1_c0z0"), hit_data({attack_data(k_4000, shot)})},
              {bullet, bullets}});
  bool own = false;
  check(game::shot_attack_key(4000, &own) == k_4000 && own, "the handgun in hand (4000): its own shot, wp4000's key");
  check(game::shot_attack_key(4100, &own) == k_4100 && own, "a shotgun in hand (4100): wp4100's");
  check(game::shot_attack_key(4900, &own) == k_default && !own,
        "a gun the bullets' data has no shot of (4900, a rocket launcher): the first shot in the list (Default)");
  check(game::shot_attack_key(-1, &own) == k_default && !own, "no gun in hand (-1): the first shot");
  catalog_of({{bullet, hit_data({attack_data(k_knife, slash)})}});
  check(game::shot_attack_key(4000) == 0, "a list with no shot in it and not the gun's: none");
  catalog_of({{bullet, bullets}});

  // --- the enemies a hit could kill ---
  game::KillTarget out[8];
  game::KillScan scan;
  check(game::kill_targets(out, 8, &scan) == 0 && scan.listed == -1, "no CharacterManager: no enemies");
  const uintptr_t cm = make("chainsaw.CharacterManager");
  plant_singleton(statics, "chainsaw.CharacterManager", cm);
  // A hit controller with its damage effect (a component of its game object): the
  // effect's 32 players slots, empty until the component's start makes them.
  auto effect_of = [&](bool started) {
    const uintptr_t effect = make("chainsaw.EPVExpertDamageEffect");
    const uintptr_t players = array_of("chainsaw.EPVExpertDamageEffectPlayer", 32, 8);
    if (started)
      for (int i = 0; i < 32; ++i)
        reinterpret_cast<uintptr_t*>(players + 0x20)[i] = make("chainsaw.EPVExpertDamageEffectPlayer");
    set<uintptr_t>(effect, "_Players", players);
    return effect;
  };
  auto hit_controller = [&](bool started = true) {
    const uintptr_t hc = make("chainsaw.HitController");
    set<uintptr_t>(hc, "<DamageEffect>k__BackingField", effect_of(started));
    return hc;
  };
  auto enemy = [&](const char* context_type, int kind, int hp, uintptr_t hc) {
    const uintptr_t ctx = make(context_type);
    const uintptr_t point = make("chainsaw.HitPoint");
    set<int32_t>(point, "<DefaultHitPoint>k__BackingField", hp > 0 ? hp : 1000);
    set<int32_t>(point, "<CurrentHitPoint>k__BackingField", hp);
    set<uintptr_t>(ctx, "<HitPoint>k__BackingField", point);
    set<int32_t>(ctx, "<KindID>k__BackingField", kind);
    set<bool>(ctx, "<Setupped>k__BackingField", true);
    const uintptr_t body = make("chainsaw.Ch1d0z0BodyUpdater");
    set<uintptr_t>(body, "<HitController>k__BackingField", hc);
    set<uintptr_t>(ctx, "_BodyUpdater", body);
    return ctx;
  };
  auto point_of = [&](uintptr_t ctx) { return get<uintptr_t>(ctx, "<HitPoint>k__BackingField"); };
  auto effect_players = [&](uintptr_t hc) {
    return get<uintptr_t>(get<uintptr_t>(hc, "<DamageEffect>k__BackingField"), "_Players");
  };

  // Started or not: the damage effect's players.
  const uintptr_t hc_started = hit_controller(true), hc_new = hit_controller(false);
  const uintptr_t hc_bare = make("chainsaw.HitController");  // no damage effect
  check(game::hit_started(hc_started) == 1 && game::hit_started(hc_new) == 0 && game::hit_started(hc_bare) == -1 &&
            game::hit_started(0) == 0 && game::hit_started(make("chainsaw.HitPoint")) == 0,
        "hit_started: its damage effect's players made (1), its 32 slots still empty (0), no damage effect to tell "
        "(-1); not a HitController (0)");
  reinterpret_cast<uintptr_t*>(effect_players(hc_new) + 0x20)[0] = make("chainsaw.EPVExpertDamageEffectPlayer");
  check(game::hit_started(hc_new) == 0, "the first slot filled and the last not: not yet (the start makes them all)");
  reinterpret_cast<uintptr_t*>(effect_players(hc_new) + 0x20)[0] = 0;

  const uintptr_t hc_saw = hit_controller();
  const uintptr_t chainsaw_man = enemy("chainsaw.EnemyBaseContext", 200008, 6500, hc_saw);
  check(game::body_hit_controller(chainsaw_man) == hc_saw, "an enemy's body hit controller: its CharacterBodyUpdater's");
  // A Ganado: its head updater, its body (with the parasite updater).
  const uintptr_t hc_ganado = hit_controller();
  const uintptr_t ganado = enemy("chainsaw.Ch1c0ContextCommon", 200000, 800, hc_ganado);
  const uintptr_t head = make("chainsaw.Ch1c0z0HeadUpdater_Mercenaries");
  const uintptr_t c0_body = make("chainsaw.Ch1c0z0BodyUpdater_Mercenaries");
  const uintptr_t parasite = make("chainsaw.Ch1c0ParasiteUpdaterCommon");
  set<uintptr_t>(c0_body, "parasiteUpdater", parasite);
  set<uintptr_t>(head, "_BodyUpdater", c0_body);
  set<uintptr_t>(ganado, "_HeadUpdater", head);
  const int ps_none = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "None"),
            ps_wait = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Wait"),
            ps_start = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Start"),
            ps_out = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Appeared"),
            ps_dead = enum_of("chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", "Dead");
  check(ps_none == 0 && ps_wait == 1 && ps_start == 2 && ps_out == 3 && ps_dead == 4,
        "Ch1c0HeadUpdaterCommon.ParasiteState: None 0, Wait 1, Start 2, Appeared 3, Dead 4");
  // Its parasite waiting inside for its death, and the Ganado held alive for it as its start holds it (NoDeath).
  set<int32_t>(head, "parasiteState", ps_wait);
  set<bool>(point_of(ganado), "<NoDeath>k__BackingField", true);
  set<float>(head, "parasiteAppearanceProbability", 50.0f);
  set<bool>(head, "forceParasiteAppearance", true);
  const uintptr_t dead = enemy("chainsaw.EnemyBaseContext", 200000, 0, hit_controller());
  const uintptr_t unset = enemy("chainsaw.EnemyBaseContext", 200000, 800, hit_controller());
  set<bool>(unset, "<Setupped>k__BackingField", false);
  const uintptr_t invincible = enemy("chainsaw.EnemyBaseContext", 200000, 800, hit_controller());
  set<bool>(point_of(invincible), "<Invincible>k__BackingField", true);
  const uintptr_t held = enemy("chainsaw.EnemyBaseContext", 200000, 800, hit_controller());
  set<bool>(point_of(held), "<NoDeath>k__BackingField", true);
  const uintptr_t no_hc = enemy("chainsaw.EnemyBaseContext", 200000, 800, 0);
  const uintptr_t wrong_hc = enemy("chainsaw.EnemyBaseContext", 200000, 800, make("chainsaw.HitPoint"));
  const uintptr_t stranger = make("chainsaw.HitPoint");  // not a context
  // The crash of 2026-10-26: a Ganado in the list, set up, with health - its game object not started yet.
  const uintptr_t arriving = enemy("chainsaw.Ch1c0ContextCommon", 200000, 1000, hit_controller(false));
  const uintptr_t no_effect = enemy("chainsaw.EnemyBaseContext", 200009, 3000, make("chainsaw.HitController"));
  set<uintptr_t>(cm, "<EnemyContextList>k__BackingField",
                 list_of("System.Collections.Generic.List`1<chainsaw.EnemyBaseContext>", "chainsaw.EnemyBaseContext",
                         {chainsaw_man, dead, unset, invincible, held, no_hc, wrong_hc, stranger, arriving, no_effect,
                          ganado}));
  int n = game::kill_targets(out, 8, &scan);
  check(scan.listed == 11 && n == 2, "eleven in the list: two a hit could kill now");
  check(n >= 1 && out[0].context == chainsaw_man && out[0].hit_controller == hc_saw && out[0].kind == 200008 &&
            out[0].hp == 6500 && !out[0].parasite,
        "an enemy in play with health: its body's hit controller, its kind and health");
  check(n >= 2 && out[1].context == ganado && out[1].hit_controller == hc_ganado && !out[1].parasite &&
            out[1].parasite_waits == head,
        "a Ganado with a parasite waiting inside (Wait), held alive for it (NoDeath, as its start holds it): its body's, "
        "the parasite noted - not passed over as one a hit could not kill (the survivors of 2026-10-26)");
  check(n >= 1 && !out[0].parasite_waits, "any other enemy: no parasite noted");
  check(scan.coming == 1 && scan.unknown_kind == 200009,
        "the Ganado whose game object has not started (2026-10-26's crash) is coming, not hit; one with no damage "
        "effect to tell is left alone, its kind noted");
  check(n == 2, "left out too: no health, not set up, invincible (the HitManager would turn the hit away), held alive "
                "(NoDeath) with no parasite waiting, no body hit controller, one that is no HitController, not a context");
  // Its parasite kept in: no chance, nothing forced - then the lethal hit's lot fails and the hit kills.
  check(game::keep_parasite_in(head) && get<float>(head, "parasiteAppearanceProbability") == 0.0f &&
            !get<bool>(head, "forceParasiteAppearance") && get<int32_t>(head, "parasiteState") == ps_wait,
        "keep_parasite_in: the Ganado's chance of a parasite 0 and none forced (its state left to the game)");
  set<float>(head, "parasiteAppearanceProbability", 50.0f);
  set<int32_t>(head, "parasiteState", ps_none);
  check(!game::keep_parasite_in(head) && get<float>(head, "parasiteAppearanceProbability") == 50.0f &&
            !game::keep_parasite_in(0) && !game::keep_parasite_in(point_of(ganado)),
        "not when no parasite waits (None), not for nothing or for what is no Ganado's head updater");
  set<int32_t>(head, "parasiteState", ps_wait);
  for (int i = 0; i < 32; ++i)
    reinterpret_cast<uintptr_t*>(effect_players(get<uintptr_t>(get<uintptr_t>(arriving, "_BodyUpdater"),
                                                                 "<HitController>k__BackingField")) +
                                 0x20)[i] = make("chainsaw.EPVExpertDamageEffectPlayer");
  n = game::kill_targets(out, 8, &scan);
  check(n == 3 && out[1].context == arriving && scan.coming == 0, "once its start has made the players: in play");
  set<int32_t>(head, "parasiteState", ps_start);
  n = game::kill_targets(out, 8, &scan);
  check(n == 2 && out[1].context == arriving && scan.coming == 1,
        "the Ganado's parasite on its way out (Start): coming, left to come out");
  // Out (Appeared): the game makes the Ganado invincible as the parasite comes out (parasiteCheck) and takes
  // that off once it is out (the body updater's onAppearedParasite), its NoDeath with it.
  set<int32_t>(head, "parasiteState", ps_out);
  set<bool>(point_of(ganado), "<Invincible>k__BackingField", true);
  n = game::kill_targets(out, 8, &scan);
  check(n == 2 && scan.coming == 0, "out (Appeared), the Ganado still invincible: left out (the HitManager would turn the hit away)");
  set<bool>(point_of(ganado), "<Invincible>k__BackingField", false);
  n = game::kill_targets(out, 8, &scan);
  check(n == 2 && scan.coming == 0, "out and still held alive (NoDeath): left out - a parasite out no longer waits");
  set<bool>(point_of(ganado), "<NoDeath>k__BackingField", false);
  n = game::kill_targets(out, 8, &scan);
  check(n == 2 && scan.coming == 1, "out, but its appearance not yet played (IsFinishParasiteAppearance): coming");
  set<bool>(parasite, "<IsFinishParasiteAppearance>k__BackingField", true);
  n = game::kill_targets(out, 8, &scan);
  check(n == 3 && out[2].context == ganado && out[2].hit_controller == hc_ganado && out[2].parasite &&
            !out[2].parasite_waits,
        "out and appeared: hit through the Ganado's body - the game hands a hit on the parasite to the Ganado's hit "
        "controller, the two with one health (the parasite's own hit controller took the mod's hits and did nothing "
        "with them, 2026-10-26)");
  set<uintptr_t>(c0_body, "parasiteUpdater", 0);
  n = game::kill_targets(out, 8, &scan);
  check(n == 3 && out[2].context == ganado && out[2].parasite,
        "out with no parasite updater to tell: the Ganado no longer invincible says its appearance played");
  set<uintptr_t>(c0_body, "parasiteUpdater", parasite);
  set<int32_t>(head, "parasiteState", ps_none);
  n = game::kill_targets(out, 8, &scan);
  check(n == 3 && out[2].hit_controller == hc_ganado && !out[2].parasite && !out[2].parasite_waits,
        "no parasite (None): its body's");
  n = game::kill_targets(out, 1, &scan);
  check(scan.listed == 11 && n == 1 && out[0].context == chainsaw_man, "at most `max` of them");

  // --- the spawners: their points' cooldowns, what they hold and have sent ---
  check(game::parts().spawners, "discovery: the Mercenaries' spawners (their list, settings, counts, points' timers)");
  auto timer = [&](bool done, float ran = 5.0f) {
    const uintptr_t t = make("chainsaw.Timer");
    set<bool>(t, "_Completed", done);
    set<float>(t, "_TimeLimit", 32.0f);
    set<float>(t, "_TransitTime", ran);
    return t;
  };
  auto spawner = [&](bool enabled, bool spent, std::initializer_list<uintptr_t> timers, int holds, int sent) {
    const uintptr_t sp = make("chainsaw.CharacterSpawnPointControllerMercenaries");
    set<bool>(sp, "<HasSpawnEnabled>k__BackingField", enabled);
    set<bool>(sp, "<HasAllSpawned>k__BackingField", spent);
    set<int32_t>(sp, "<RequestCount>k__BackingField", 1);
    set<int32_t>(sp, "<ActiveCount>k__BackingField", 2);
    set<int32_t>(sp, "_ActiveCountLimit", 12);
    set<uintptr_t>(sp, "_CoolDownTimerList", list_of("System.Collections.Generic.List`1<chainsaw.Timer>", "chainsaw.Timer", timers));
    std::vector<uintptr_t> params, points;
    for (int i = 0; i < holds; ++i) params.push_back(make("chainsaw.CharacterSpawnParam"));
    for (size_t i = 0; i < timers.size(); ++i) points.push_back(make("chainsaw.CharacterSpawnPointMercenaries"));
    const uintptr_t pa = array_of("chainsaw.CharacterSpawnParam", params.size(), 8);
    for (size_t i = 0; i < params.size(); ++i) reinterpret_cast<uintptr_t*>(pa + 0x20)[i] = params[i];
    set<uintptr_t>(sp, "<SpawnTargets>k__BackingField", pa);
    const uintptr_t pp = array_of("chainsaw.CharacterSpawnPointMercenaries", points.size(), 8);
    for (size_t i = 0; i < points.size(); ++i) reinterpret_cast<uintptr_t*>(pp + 0x20)[i] = points[i];
    set<uintptr_t>(sp, "_PracticalSpawnPoints", pp);
    const uintptr_t spawned = make("System.Collections.Generic.List`1<chainsaw.CharacterSpawnPointControllerMercenaries.SpawnedDataMercenaries>");
    set<uintptr_t>(spawned, "_items", array_of("chainsaw.CharacterSpawnPointControllerMercenaries.SpawnedDataMercenaries", 96, 8));
    set<int32_t>(spawned, "_size", sent);
    set<uintptr_t>(sp, "SpawnedDataList", spawned);
    return sp;
  };
  auto spawner_data = [&](uintptr_t sp, uint32_t hash) {
    const uintptr_t d = make("chainsaw.MercenariesModeController.SpawnerData");
    const uintptr_t setting = make("chainsaw.MercenariesModeController.SpawnerSetting");
    const uintptr_t name = make("chainsaw.NameHash");
    set<uint32_t>(name, "_Hash", hash);
    set<uintptr_t>(setting, "_SpawnControllerKeyNameHash", name);
    set<uintptr_t>(d, "<Setting>k__BackingField", setting);
    set<uintptr_t>(d, "<SpawnController>k__BackingField", sp);
    return d;
  };
  const uintptr_t cool_a = timer(false), cool_b = timer(false), warm = timer(true), fresh = timer(false, 0.4f);
  const uintptr_t idle_t = timer(false), spent_t = timer(false);
  const uintptr_t sending = spawner(true, false, {cool_a, warm, cool_b, fresh}, 81, 40);
  const uintptr_t not_yet = spawner(false, false, {idle_t}, 10, 0);
  const uintptr_t used_up = spawner(true, true, {spent_t}, 5, 5);
  const uintptr_t merc = make("chainsaw.MercenariesModeController");
  set<uintptr_t>(merc, "_SpawnerList",
                 list_of("System.Collections.Generic.List`1<chainsaw.MercenariesModeController.SpawnerData>",
                         "chainsaw.MercenariesModeController.SpawnerData",
                         {spawner_data(sending, 0x8080AB12u), spawner_data(not_yet, 0x11111111u), spawner_data(used_up, 0x22222222u),
                          make("chainsaw.HitPoint")}));
  game::SpawnerState states[8];
  int ns = game::spawner_states(merc, states, 8);
  check(ns == 3 && states[0].name_hash == 0x8080AB12u && states[0].enabled && !states[0].spent && states[0].requests == 1 &&
            states[0].active == 2 && states[0].limit == 12 && states[0].sent == 40 && states[0].holds == 81 &&
            states[0].points == 4 && states[0].cooling == 3,
        "spawner_states: each spawner by its name's hash - may send, not spent, a request waiting, 2 of 12 active, 40 of "
        "the 81 it holds sent, 4 points of which 3 still cool down; what is no SpawnerData left out");
  check(ns == 3 && !states[1].enabled && states[1].holds == 10 && states[2].enabled && states[2].spent && states[2].sent == 5,
        "and one its flags do not let send yet, one spent");
  check(game::spawner_states(0, states, 8) == 0 && game::spawner_states(merc, states, 1) == 1, "none without a controller; at most `max`");
  check(game::ready_spawn_points(merc, 1.0f) == 2 && get<bool>(cool_a, "_Completed") && get<bool>(cool_b, "_Completed") &&
            get<bool>(warm, "_Completed") && !get<bool>(fresh, "_Completed") && get<float>(cool_a, "_TimeLimit") == 32.0f,
        "ready_spawn_points: the cooling points of a spawner that may send are marked finished once they have run a "
        "second (2; one that sent 0.4 s ago waits), their time left as it is; a finished one untouched");
  check(!get<bool>(idle_t, "_Completed") && !get<bool>(spent_t, "_Completed"),
        "not the points of a spawner its flags do not let send yet, nor a spent one's");
  check(game::ready_spawn_points(merc, 1.0f) == 0 && game::ready_spawn_points(0, 1.0f) == 0,
        "nothing more to mark: 0; no controller: 0");
  set<float>(fresh, "_TransitTime", 1.0f);
  check(game::ready_spawn_points(merc, 1.0f) == 1 && get<bool>(fresh, "_Completed"), "the fresh one once it has run its second");
  // A held-up spawner's own rules: its points out of view only, its distance; lifted, then put back.
  re::Array pts;
  re::read_array(get<uintptr_t>(sending, "_PracticalSpawnPoints"), &pts);
  auto point_at = [&](int i) { return reinterpret_cast<uintptr_t*>(pts.data)[i]; };
  set<bool>(point_at(0), "_IsOutOfCameraOnly", true);
  set<bool>(point_at(1), "_IsOutOfCameraOnly", true);
  set<bool>(point_at(2), "_IsOutOfCameraOnly", false);
  set<bool>(point_at(3), "_IsOutOfCameraOnly", true);
  set<float>(sending, "_SpawnDistanceMin", 10.0f);
  check(!game::spawn_rules_lifted() && game::lift_spawn_rules(merc, sending, 2.0f) == 4 &&
            !get<bool>(point_at(0), "_IsOutOfCameraOnly") && !get<bool>(point_at(1), "_IsOutOfCameraOnly") &&
            !get<bool>(point_at(3), "_IsOutOfCameraOnly") && get<float>(sending, "_SpawnDistanceMin") == 2.0f &&
            game::spawn_rules_lifted(),
        "lift_spawn_rules: the out-of-view-only points' rule off (3) and the distance cut to 2 m - 4 values");
  check(game::lift_spawn_rules(merc, sending, 2.0f) == 0 && game::lift_spawn_rules(merc, 0, 2.0f) == -1 &&
            game::lift_spawn_rules(merc, make("chainsaw.HitPoint"), 2.0f) == -1,
        "lifting it again changes nothing; no spawner or not a spawner: -1");
  check(game::restore_spawn_rules(merc) == 4 && get<bool>(point_at(0), "_IsOutOfCameraOnly") &&
            get<bool>(point_at(1), "_IsOutOfCameraOnly") && !get<bool>(point_at(2), "_IsOutOfCameraOnly") &&
            get<bool>(point_at(3), "_IsOutOfCameraOnly") && get<float>(sending, "_SpawnDistanceMin") == 10.0f &&
            !game::spawn_rules_lifted(),
        "restore_spawn_rules: every value put back as the game had it (the point that never asked it stays as it was)");
  game::lift_spawn_rules(merc, sending, 2.0f);
  const uintptr_t other_stage = make("chainsaw.MercenariesModeController");
  check(game::restore_spawn_rules(other_stage) == 0 && !get<bool>(point_at(0), "_IsOutOfCameraOnly") &&
            get<float>(sending, "_SpawnDistanceMin") == 2.0f && !game::spawn_rules_lifted() &&
            game::restore_spawn_rules(merc) == 0,
        "for another controller (another stage: its objects are not the ones lifted) nothing is written and the values "
        "are let go");
  set<bool>(point_at(0), "_IsOutOfCameraOnly", true);
  set<bool>(point_at(1), "_IsOutOfCameraOnly", true);
  set<bool>(point_at(3), "_IsOutOfCameraOnly", true);
  set<float>(sending, "_SpawnDistanceMin", 10.0f);

  // --- the request's two objects ---
  const uintptr_t attack = make("chainsaw.collision.AttackUserData");
  const uintptr_t damage = make("chainsaw.collision.DamageUserData");
  check(game::attack_user_data_type() == re::find_type("chainsaw.collision.AttackUserData") &&
            game::damage_user_data_type() == re::find_type("chainsaw.collision.DamageUserData"),
        "the request's classes: AttackUserData and DamageUserData");
  check(game::fill_kill_attack(attack, k_4000, 0x1234u) && get<int32_t>(attack, "_AttackHitDataID") == bullet &&
            get<uint32_t>(attack, "_KeyNameHash") == k_4000 && get<uint32_t>(attack, "<AttackID>k__BackingField") == 0x1234u &&
            get<int32_t>(attack, "<AttackThroughCount>k__BackingField") == 0,
        "the attack: the bullets' attack data (Bullet), the shot's key, the attack's number, none passed through yet");
  check(!game::fill_kill_attack(attack, 0, 1) && !game::fill_kill_attack(damage, k_4000, 1) && !game::fill_kill_attack(0, k_4000, 1),
        "no key, or not an AttackUserData: not filled");
  check(game::fill_kill_damage(damage) &&
            get<int32_t>(damage, "HitGroup") == enum_of("chainsaw.HitManager.HitGroup", "Enemy") &&
            get<int32_t>(damage, "Priority") == 0 &&
            get<int32_t>(damage, "Parts") == enum_of("chainsaw.character.BodyParts", "Chest") &&
            get<int32_t>(damage, "PartsSide") == enum_of("chainsaw.character.BodyPartsSide", "Center"),
        "the damage: an enemy's (HitGroup Enemy), its chest, centre - as the game's laser turret fills its own");
  check(!game::fill_kill_damage(attack) && !game::fill_kill_damage(0), "not a DamageUserData: not filled");

  // --- the game's code the mod relies on ---
  const char* hc_t = "chainsaw.HitController";
  const Code request = code_of(hc_t, "requestAttack",
                               {"via.GameObject", "chainsaw.collision.AttackUserData", "chainsaw.collision.DamageUserData"});
  // The attack's ID and key: two reads 4 apart, the ID first, as the database places them
  // (the class's header is the VM's, so it is read off the code).
  const int32_t aud_id = offset_of("chainsaw.collision.AttackUserData", "_AttackHitDataID"),
                aud_key = offset_of("chainsaw.collision.AttackUserData", "_KeyNameHash"),
                aud_number = offset_of("chainsaw.collision.AttackUserData", "<AttackID>k__BackingField");
  int32_t aud_header = -1;
  for (int32_t h = 0x10; h <= 0x200 && aud_header < 0; h += 8)
    if (request.on({0x8B}, h + aud_id) >= 0 && request.on({0x8B}, h + aud_key) >= 0 &&
        request.on({0x8B}, h + aud_id) < 0x60 && request.on({0x8B}, h + aud_key) < 0x60)
      aud_header = h;
  check(aud_id == 0 && aud_key == 4 && aud_number == 8 && aud_header > 0,
        "HitController.requestAttack reads its AttackUserData's attack data ID and key first thing (where the mod "
        "writes them)");
  const uintptr_t get_attack_data =
      method_code("chainsaw.collision.AttackHitUserData", "getAttackData", {"System.UInt32"});
  check(request.call_to(get_attack_data) >= 0,
        "and looks the key up in the HitManager's attack data (AttackHitUserData.getAttackData): a key the catalog lacks "
        "makes no hit - why the mod reads the catalog first");
  const uintptr_t pool_add =
      method_code("chainsaw.collision.ContactPool`1<chainsaw.collision.HitInfo>", "add", {"chainsaw.collision.HitInfo"});
  const uintptr_t pool_clear = method_code("chainsaw.collision.ContactPool`1<chainsaw.collision.HitInfo>", "clear", {});
  const int32_t request_pool = ref_getter_place(hc_t, "get_RequestDamagePool");
  const long add_at = request.call_to(pool_add);
  const long pool_read = request.on({0x8B}, request_pool);
  check(request_pool > 0 && add_at > 0 && pool_read > 0 && pool_read < add_at,
        "then queues the hit on its own request pool (<RequestDamagePool>: ContactPool.add)");
  const Code update = code_of(hc_t, "update", {});
  const long u_read = update.on({0x8B}, request_pool), u_add = update.call_to(pool_add),
             u_clear = update.call_to(pool_clear),
             u_attack = update.call_to(method_code(hc_t, "updateAttack", {}));
  check(u_read >= 0 && u_read < 0x40 && u_add > u_read && u_clear > u_add && u_attack > u_clear,
        "HitController.update: the request pool read first, its hits handed on (ContactPool.add), the pool cleared, "
        "then updateAttack");
  const Code attack_update = code_of(hc_t, "updateAttack", {});
  const char* hm_t = "chainsaw.HitManager";
  check(attack_update.call_to(method_code(hm_t, "registerDamage",
                                          {"System.Collections.Generic.List`1<chainsaw.collision.HitInfo>"})) > 0,
        "updateAttack hands them to the HitManager (registerDamage)");
  const Code hm_update = code_of(hm_t, "onUpdate", {"System.Single"});
  const uintptr_t update_damage = method_code(hm_t, "updateDamage", {"chainsaw.collision.HitInfo"});
  check(hm_update.call_to(update_damage) > 0, "the HitManager's update works each one out (updateDamage)");
  const Code damage_code = code_at(update_damage);
  const uintptr_t calc_info = method_code(hm_t, "calcInfo",
                                          {"chainsaw.HitController.DamageInfo", "chainsaw.HitController",
                                           "chainsaw.HitController"});
  const uintptr_t hit_setting = method_code(hm_t, "hitSetting",
                                            {"chainsaw.HitController.DamageInfo", "chainsaw.HitController",
                                             "chainsaw.HitController"});
  const long d_calc = damage_code.call_to(calc_info), d_setting = damage_code.call_to(hit_setting);
  check(d_calc > 0 && d_setting > d_calc, "updateDamage: the damage worked out (calcInfo), then the hit applied (hitSetting)");
  const Code calc = code_at(calc_info);
  check(calc.call_to(method_code(hc_t, "callbackCalculateDamage",
                                 {"chainsaw.HitController.DamageValue", "chainsaw.HitController.CalculateInfo"})) > 0,
        "calcInfo calls the damaged one's OnCalculateDamage (callbackCalculateDamage) - where the mod's hit hook raises "
        "the damage to the enemy's health");
  const Code setting = code_at(hit_setting);
  check(setting.call_to(method_code(hc_t, "callbackDamageHit", {"chainsaw.HitController.DamageInfo"})) > 0,
        "hitSetting then hands the damaged one the hit (callbackDamageHit: OnDamageHit - its death, the kill)");
  // The hook knows the mod's hit by its number: CalculateInfo's constructor copies it from the attack.
  const Code calc_ctor = code_of("chainsaw.HitController.CalculateInfo", ".ctor", {"chainsaw.HitController.DamageInfo"});
  const int32_t calc_number = offset_of("chainsaw.HitController.CalculateInfo", "<AttackID>k__BackingField");
  const long number_read = aud_header > 0 ? calc_ctor.on({0x8B}, aud_header + aud_number) : -1;
  const long number_write = number_read >= 0 ? calc_ctor.on8({0x89}, static_cast<int8_t>(0x10 + calc_number),
                                                             static_cast<size_t>(number_read))
                                             : -1;
  check(calc_number >= 0 && number_read > 0 && number_write > number_read && number_write - number_read < 12,
        "HitController.CalculateInfo's constructor copies the attack's number (AttackUserData.<AttackID>) into its own "
        "<AttackID> - what the hit hook reads");
  // The game's own direct attacks number theirs the same way.
  const Code direct = code_of("chainsaw.BehaviorTreeAction_DirectDamage", "requestAttack", {"System.UInt32"});
  const long generate = direct.call_to(method_code(hm_t, "generateAttackID", {}));
  const long number_store = aud_header > 0 && generate > 0 ? direct.on({0x89}, aud_header + aud_number, static_cast<size_t>(generate)) : -1;
  check(generate > 0 && number_store > generate && number_store - generate < 12 && direct.call_to(method_code(
            hc_t, "requestAttack", {"via.GameObject", "chainsaw.collision.AttackUserData", "chainsaw.collision.DamageUserData"})) > number_store,
        "the game's own direct damage numbers its attack (generateAttackID into <AttackID>) and requests it the same way");
  // The Mercenaries scores a death and counts it in the combo whatever killed it.
  const Code dead_code = code_of("chainsaw.MercenariesModeController", "onEnemyDead",
                                 {"chainsaw.HitController.DamageInfo", "chainsaw.EnemyBaseContext"});
  const uintptr_t add_score =
      method_code("chainsaw.MercenariesScoreCounter", "requestAddScore",
                  {"System.Int32", "System.Int32", "System.Int32", "System.Int32", "via.GameObject",
                   "System.Nullable`1<via.vec3>"});
  const long add_combo = dead_code.call_to(method_code("chainsaw.MercenariesComboCounter", "requestAddCombo", {"System.Int32"}));
  const long reason = dead_code.call_to(method_code("chainsaw.MercenariesModeController", "getEnemyDeadReason",
                                                    {"chainsaw.EnemyBaseContext"}));
  int scores_after = 0;
  for (long at = reason; at >= 0 && (at = dead_code.call_to(add_score, static_cast<size_t>(at) + 5)) >= 0;) ++scores_after;
  check(add_combo > 0 && reason > add_combo,
        "MercenariesModeController.onEnemyDead: the combo counts the kill (requestAddCombo) before it looks at what "
        "killed it");
  check(scores_after >= 2,
        "and the kill's score is asked for on both of its ways on (with a hit's details and without: requestAddScore "
        "twice after the reason is read)");

  // --- the game's code the in-play test relies on (2026-10-26's crash) ---
  // A hit asks the damaged hit controller's damage effect for blood and sparks;
  // the effect calls its players in turn with no null check, and its players are
  // made only by its start - so a hit on a character whose game object has not
  // started calls an empty slot.
  const char* fx = "chainsaw.EPVExpertDamageEffect";
  const int32_t fx_header = getter_place(fx, "get_RootJoint") - offset_of(fx, "_RootJoint");
  const int32_t fx_players = fx_header + offset_of(fx, "_Players");
  const int32_t hc_header = request_pool - offset_of(hc_t, "<RequestDamagePool>k__BackingField");
  const int32_t hc_effect = hc_header + offset_of(hc_t, "<DamageEffect>k__BackingField");
  check(fx_header > 0 && hc_header > 0 && fx_header == hc_header,
        "EPVExpertDamageEffect's and HitController's fields sit where a via.Behavior's do (their getters' reads)");
  const Code fx_ctor = code_of(fx, ".ctor", {});
  check(on_disp(fx_ctor, {0x89}, fx_players) >= 0,
        "EPVExpertDamageEffect's constructor gives it its _Players array (32 empty slots, EffectPlayerNum)");
  const Code fx_start = code_of(fx, "doStart", {});
  const long start_read = on_disp(fx_start, {0x8B}, fx_players);
  const long player_new = fx_start.call_to(method_code("chainsaw.EPVExpertDamageEffectPlayer", ".ctor", {fx}));
  check(start_read >= 0 && player_new > start_read,
        "its doStart (from the component's start, at its game object's first update) makes the players in the slots");
  const Code fx_request = code_of(fx, "request", {"chainsaw.HitController.DamageInfo"});
  const long request_read = on_disp(fx_request, {0x8B}, fx_players);
  const long player_request =
      fx_request.call_to(method_code("chainsaw.EPVExpertDamageEffectPlayer", "request", {"chainsaw.HitController.DamageInfo"}));
  check(request_read >= 0 && player_request > request_read && fx_request.find({0x48, 0x85, 0xD2}) < 0,
        "its request calls each player's request in turn - with no null check (an empty slot is the crash)");
  const Code player_req = code_of("chainsaw.EPVExpertDamageEffectPlayer", "request", {"chainsaw.HitController.DamageInfo"});
  check(player_req.at && player_req.find({0x83, 0x7A}) >= 0 && player_req.find({0x83, 0x7A}) < 0x14,
        "and a player's request reads its own state first thing (`this` an empty slot: the access violation at +0x10)");
  const Code damage_hit = code_of(hc_t, "callbackDamageHit", {"chainsaw.HitController.DamageInfo"});
  const long effect_read = damage_hit.on({0x8B}, hc_effect);
  check(effect_read >= 0 && damage_hit.call_to(method_code(fx, "request", {"chainsaw.HitController.DamageInfo"})) > effect_read,
        "HitController.callbackDamageHit asks its <DamageEffect> for the hit's effect (EPVExpertDamageEffect.request)");
  const Code hc_awake = code_of(hc_t, "awake", {});
  check(hc_awake.on({0x89}, hc_effect) >= 0,
        "HitController.awake takes <DamageEffect> from its own game object (so the effect started means it started)");
  // A parasite's appearance: its start clears the flag, onAppearance sets it.
  const char* par_t = "chainsaw.Ch1c0ParasiteUpdaterCommon";
  const int32_t appeared_at = getter_place(par_t, "get_IsFinishParasiteAppearance");
  check(appeared_at > 0 && appeared_at - offset_of(par_t, "<IsFinishParasiteAppearance>k__BackingField") == hc_header,
        "Ch1c0ParasiteUpdaterCommon.get_IsFinishParasiteAppearance reads the flag the mod reads");
  const Code appearance = code_of(par_t, "onAppearance", {"System.Boolean"});
  const long set_appeared = appearance.find({0xC6, 0x43, static_cast<uint8_t>(appeared_at), 0x01});
  check(set_appeared > 0, "onAppearance (the parasite out) sets it");
  const Code par_start = code_of(par_t, "start", {});
  check(on_disp(par_start, {0x88}, appeared_at) >= 0, "and its start clears it (a parasite not out yet)");

  // --- a Ganado carrying a parasite (the survivors of 2026-10-26) ---
  // Its start holds it alive while its parasite waits; its lethal hit draws the
  // parasite's lot, which a chance of 0 with nothing forced loses - then the
  // Ganado dies of the hit, its NoDeath taken off.
  const char* c0_t = "chainsaw.Ch1c0HeadUpdaterCommon";
  const int32_t c0_header = getter_place(c0_t, "get_LatestDamageAtOnHitDamageCheck") -
                            offset_of(c0_t, "<LatestDamageAtOnHitDamageCheck>k__BackingField");
  const int32_t state_at = c0_header + offset_of(c0_t, "parasiteState"),
                chance_at = c0_header + offset_of(c0_t, "parasiteAppearanceProbability"),
                force_at = c0_header + offset_of(c0_t, "forceParasiteAppearance");
  const int8_t no_death_at = static_cast<int8_t>(0x10 + offset_of("chainsaw.HitPoint", "<NoDeath>k__BackingField"));
  check(c0_header == hc_header, "Ch1c0HeadUpdaterCommon's fields sit where a via.Behavior's do (its getter's read)");
  auto cmp_wait = [&](const Code& c, size_t from = 0) {  // cmp dword [reg+state], Wait
    for (long i = c.on({0x83}, state_at, from); i >= 0; i = c.on({0x83}, state_at, static_cast<size_t>(i) + 1))
      if ((c.u8(static_cast<size_t>(i) + 1) & 0x38) == 0x38 && c.u8(static_cast<size_t>(i) + 6) == ps_wait) return i;
    return -1L;
  };
  auto store_byte = [&](const Code& c, int8_t disp, uint8_t v, size_t from) {  // mov byte [reg+disp8], v
    for (long i = c.on8({0xC6}, disp, from); i >= 0; i = c.on8({0xC6}, disp, static_cast<size_t>(i) + 1))
      if (c.u8(static_cast<size_t>(i) + 3) == v) return i;
    return -1L;
  };
  const Code c0_start = code_of(c0_t, "onStart", {}, 0x2000);
  const long start_wait = cmp_wait(c0_start);
  const long start_hold = start_wait >= 0 ? store_byte(c0_start, no_death_at, 1, static_cast<size_t>(start_wait)) : -1;
  check(start_wait > 0 && start_hold > start_wait && start_hold - start_wait < 0x30,
        "Ch1c0HeadUpdaterCommon.onStart: a parasite waiting (Wait) - the Ganado's HitPoint held alive (NoDeath)");
  const char* display = "chainsaw.Ch1c0HeadUpdaterCommon.<>c__DisplayClass420_0";
  const Code parasite_check = code_of(c0_t, "<onHitDamage>g__parasiteCheck|420_5", {display});
  const long check_wait = cmp_wait(parasite_check);
  const int32_t lot_slot = 8 * re::method_vt_index(re::find_method(re::find_type(c0_t), "checkParasiteAppearance",
                                                                   {"System.Single", "System.Single"}));
  const long chance_read = parasite_check.on({0xF3, 0x0F, 0x10}, chance_at);
  const long lot_call = chance_read >= 0 ? parasite_check.on({0x48, 0x8B}, lot_slot, static_cast<size_t>(check_wait)) : -1;
  const uintptr_t failed_dead = method_code(c0_t, "<onHitDamage>g__parasiteFailedDead|420_8",
                                            {"chainsaw.Ch1c0HeadUpdaterCommon.ParasiteState", display});
  check(check_wait >= 0 && check_wait < 0x20 && lot_slot == 0x818 && chance_read > check_wait && lot_call > check_wait &&
            parasite_check.call_to(failed_dead, static_cast<size_t>(lot_call)) > lot_call,
        "the lethal hit's parasiteCheck: only for a parasite waiting; the lot drawn through the head's own "
        "checkParasiteAppearance (its table) with its parasiteAppearanceProbability; lost, parasiteFailedDead");
  const Code failed = code_at(failed_dead);
  check(store_byte(failed, no_death_at, 0, 0) >= 0 && failed.on({0x89}, state_at) >= 0,
        "parasiteFailedDead takes the Ganado's NoDeath off and notes the parasite failed - the hit kills it");
  for (const char* cls : {"chainsaw.Ch1c0HeadUpdaterCommon", "chainsaw.Ch1c0z0HeadUpdater_Mercenaries",
                          "chainsaw.Ch1c0z1HeadUpdater_Mercenaries", "chainsaw.Ch1c0z2HeadUpdater_Mercenaries"}) {
    const Code lot = code_of(cls, "checkParasiteAppearance", {"System.Single", "System.Single"});
    const long forced = lot.on({0x80}, force_at);
    const long zero = forced >= 0 ? lot.find({0x0F, 0x2E}, static_cast<size_t>(forced)) : -1;  // ucomiss: the chance 0?
    std::string what = std::string(cls + 9) + ".checkParasiteAppearance: forced (forceParasiteAppearance) wins, else a "
                       "chance of 0 loses before any draw";
    check(forced > 0 && zero > forced && zero - forced < 0x20, what.c_str());
  }

  // --- every area sends: the spawn points' conditions ---
  const char* cond = "<chainsaw.CharacterSpawnConditionMercenaries.ConditionType>";
  const std::string base_check = std::string("chainsaw.CustomConditionBase`1") + cond;
  const Code all_check = code_of(base_check.c_str(), "check", {});
  const long data_slot = all_check.find({0x48, 0x8B, 0x41, 0x38});  // mov rax,[rcx+0x38]: the table's slot 7
  check(data_slot > 0 && all_check.find({0xFF, 0xD0}, static_cast<size_t>(data_slot)) == data_slot + 7,
        "CustomConditionBase.check (a spawn point's added conditions) asks each condition through its class's table, "
        "slot 7 - where the mod's three hooks sit");
  const struct {
    const char* cls;
    const char* declaring;
  } conds[3] = {{"chainsaw.CharacterSpawnConditionMercenaries.ConditionArea",
                 "chainsaw.CustomConditionBase`1.ConditionAreaBase<chainsaw.CharacterSpawnConditionMercenaries.ConditionType>"},
                {"chainsaw.CharacterSpawnConditionMercenaries.ConditionFlagMer",
                 "chainsaw.CharacterSpawnConditionMercenaries.ConditionFlagMer"},
                {"chainsaw.CharacterSpawnConditionMercenaries.ConditionFlag",
                 "chainsaw.CustomConditionBase`1.ConditionFlagBase<chainsaw.CharacterSpawnConditionMercenaries.ConditionType>"}};
  for (const auto& c : conds) {
    const uint32_t t = re::find_type(c.cls), d = re::find_type(c.declaring);
    const uint32_t m = d ? re::find_method(d, "check", {}) : 0;
    std::string what = std::string(c.cls + 9) + ": its check is virtual slot 7, its class one of the conditions";
    check(t && m && re::method_vt_index(m) == 7 && re::derives(t, d) &&
              re::derives(t, re::find_type((std::string("chainsaw.CustomConditionBase`1.Condition") + cond).c_str())),
          what.c_str());
  }
  const char* spc = "chainsaw.CharacterSpawnPointControllerMercenaries";
  const Code try_point = code_of(spc, "tryGetSpawnPoint", {"System.Int32"}, 0x1000);
  const long point_conditions = try_point.call_to(method_code(base_check.c_str(), "check", {}));
  const long camera = try_point.call_to(method_code("chainsaw.MainCameraController", "isInside", {"via.vec3"}));
  check(camera > 0 && point_conditions > camera,
        "CharacterSpawnPointControllerMercenaries.tryGetSpawnPoint: a point out of the camera's view where it asks it, "
        "then its added conditions (CustomConditionBase.check) - the conditions the hooks hold");

  // --- the points' cooldowns, the spawners' fixed sets of enemies (the second report of 2026-10-26) ---
  const char* timer_t = "chainsaw.Timer";
  const int32_t done_at = getter_place(timer_t, "get_Completed");
  const int32_t timer_header = done_at - offset_of(timer_t, "_Completed");
  check(timer_header == 0x10, "Timer.get_Completed (virtual 6) reads _Completed, a plain object's field");
  const int32_t sp_header = getter_place(spc, "get_ActiveCount") - offset_of(spc, "<ActiveCount>k__BackingField");
  const int32_t cooldowns_at = sp_header + offset_of(spc, "_CoolDownTimerList");
  const long timers_read = on_rex(try_point, 0x8B, cooldowns_at);
  const long timer_done = timers_read >= 0 ? try_point.find({0x48, 0x8B, 0x41, 0x30}, static_cast<size_t>(timers_read)) : -1;
  check(sp_header == hc_header && timers_read > 0 && timer_done > timers_read && timer_done < camera,
        "tryGetSpawnPoint takes a point only once its own cooldown timer (_CoolDownTimerList[i]) answers finished "
        "(its table's slot 6), before the camera, the distance and the conditions");
  // The two rules a held-up spawner has lifted: the point's own out-of-view-only flag guards the camera test, and
  // the spawner's distance is the floor.
  const int8_t camera_only_at = static_cast<int8_t>(0x10 + offset_of("chainsaw.CharacterSpawnPoint", "_IsOutOfCameraOnly"));
  const long camera_flag = try_point.on8({0x80}, camera_only_at);
  const int32_t distance_at = sp_header + offset_of(spc, "_SpawnDistanceMin");
  long distance_read = -1;
  for (const uint8_t rex : {0x41, 0x45})
    if (distance_read < 0) distance_read = try_point.on({0xF3, rex, 0x0F, 0x10}, distance_at);
  check(camera_flag > timer_done && camera_flag < camera && distance_read > camera && distance_read < point_conditions,
        "tryGetSpawnPoint: the camera test only for a point marked out of view only (CharacterSpawnPoint."
        "_IsOutOfCameraOnly), then the spawner's _SpawnDistanceMin, then the conditions - the two rules lifted for a "
        "held-up spawner");
  const Code sp_start = code_of(spc, "start", {}, 0x2000);
  const uintptr_t timer_ctor = method_code(timer_t, ".ctor", {"System.Single", "chainsaw.Timer.FunctionType", "chainsaw.TimerBase.CountMode"});
  const uintptr_t timer_finish = method_code(timer_t, "finish", {});
  bool one_shot = false;
  for (long at = sp_start.call_to(timer_ctor); at >= 0 && !one_shot; at = sp_start.call_to(timer_ctor, static_cast<size_t>(at) + 5)) {
    const long type0 = sp_start.find({0x45, 0x33, 0xC9}, at > 0x30 ? static_cast<size_t>(at) - 0x30 : 0);  // xor r9d,r9d: Type 0
    const long fin = sp_start.call_to(timer_finish, static_cast<size_t>(at) + 5);
    one_shot = type0 >= 0 && type0 < at && fin > at && fin - at < 0x10;
  }
  check(one_shot, "the spawner's start makes each cooldown timer one-shot (FunctionType 0) and finishes it at once");
  const Code timer_update = code_of(timer_t, "update", {"System.Nullable`1<System.Single>"});
  const long restart = timer_update.find({0xC6, 0x47, static_cast<uint8_t>(done_at), 0x00});
  check(restart > 0 && timer_update.find({0x83, 0xF8, 0x01}) >= 0 && timer_update.find({0x83, 0xF8, 0x01}) < restart,
        "Timer.update un-finishes a finished timer only of FunctionType 1 - a finished one-shot timer stays finished");
  const Code request_spawn = code_of(spc, "requestSpawn", {}, 0x1000);
  const long cooldowns_reread = on_rex(request_spawn, 0x8B, cooldowns_at);
  const long timer_reset = cooldowns_reread >= 0 ? request_spawn.find({0xC6, 0x40, static_cast<uint8_t>(done_at), 0x00},
                                                                        static_cast<size_t>(cooldowns_reread))
                                                 : -1;
  check(cooldowns_reread > 0 && timer_reset > cooldowns_reread,
        "requestSpawn resets the used point's timer (_Completed off): the cooldown the mod takes off again");
  const int32_t spent_at = sp_header + offset_of(spc, "<HasAllSpawned>k__BackingField");
  check(request_spawn.on({0x41, 0xC6}, spent_at) >= 0 &&
            request_spawn.call_to(method_code(spc, "getNextSpawnParam", {})) >= 0,
        "requestSpawn marks the spawner spent (<HasAllSpawned>) once getNextSpawnParam finds none of its enemies left");
  const Code next_param = code_of(spc, "getNextSpawnParam", {});
  check(on_rex(next_param, 0x8B, sp_header + offset_of(spc, "SpawnedDataList")) >= 0 &&
            on_rex(next_param, 0x8B, sp_header + offset_of("chainsaw.CharacterSpawnController", "<SpawnTargets>k__BackingField")) >= 0,
        "getNextSpawnParam: the first of its <SpawnTargets> no SpawnedDataList entry carries - each enemy it holds comes once");
  const char* mc = "chainsaw.MercenariesModeController";
  const int32_t mc_header = getter_place(mc, "get_SpawnEnemyMax") - offset_of(mc, "_SpawnEnemyMax");
  const Code exterminated = code_of(mc, "IsExterminated", {});
  check(mc_header == hc_header && exterminated.on({0x8B}, mc_header + offset_of(mc, "_SpawnEnemyMax")) >= 0 &&
            exterminated.on({0x39}, mc_header + offset_of(mc, "_DeadEnemyCount")) >= 0,
        "MercenariesModeController.IsExterminated: the dead count at the stage's _SpawnEnemyMax - every enemy down "
        "clears the stage");

  std::printf("\n%d check(s) failed\n", g_fail);
  return g_fail ? 1 : 0;
}
