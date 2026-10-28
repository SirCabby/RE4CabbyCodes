// God mode for Luis (game.cpp: luis_context, hold_health on his HitPoint - which
// cheats.cpp's tick holds while God mode is on - and level_timer, hold_level_timer:
// Separate Ways' time limit to save him, which events.cpp's hook on the level timer's
// frame holds) over objects shaped like the game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_luis.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_luis.exe
//   wine tests/build/test_luis.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then the partners are built the way the VM lays out
// managed objects (as test_jetski does): CharacterManager in its AppSingleton's
// _Instance, its PartnerContextList holding Ashley's and Luis's contexts with their
// HitPoints; and Separate Ways' level timer (a CustomLevelTimer) with the
// chainsaw.Timer it runs - every field placed by name from the real database. Luis
// is found among the partners and held as the tick holds the player (no damage, his
// health at its maximum); the time limit is held as the hook holds it before each of
// its frames (no time gone - its gauge full, never run out) and the game's frames are
// run over it as the game's rule runs them. And the game's code these rely on (found
// by name, read in the mapped exe): the game's partner kinds are Ashley's and
// Luis's; it rebuilds the partner list every frame; a hit on Luis is cut short of his
// health unless he is in battle, and he dies once his health is gone (his frame's
// ForceDown); the HitPoint takes nothing and decides no death while NoDamage is on;
// the level timer's frame runs its timer, shows it as the Timer_31_1011 gauge and
// sets its end flag once it has run out; the timer runs up its time gone and is
// done only once that reaches its limit.
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
  const uintptr_t o = alloc(0x1000);  // a PartnerBaseContext's fields reach 0x4B8
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
uintptr_t refs(const char* elem_type, std::initializer_list<uintptr_t> items) {
  const uintptr_t arr = alloc(0x20 + 8 * (items.size() + 4));
  *reinterpret_cast<uintptr_t*>(arr + 0x10) = re::type_record(re::find_type(elem_type));
  *reinterpret_cast<int32_t*>(arr + 0x1C) = static_cast<int32_t>(items.size());
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
  // The first instruction `opcode` (its bytes, after any REX prefix) whose memory operand is
  // [reg+disp] (ModRM mod 01 with a disp8, or mod 10 with a disp32; no SIB) at `disp`: the
  // opcode's offset, or -1.
  long on(std::initializer_list<uint8_t> opcode, int32_t disp, size_t from = 0) const {
    for (size_t i = from; i + opcode.size() + 5 <= n; ++i) {
      size_t k = 0;
      for (const uint8_t b : opcode)
        if (u8(i + k) == b) ++k;
        else break;
      if (k != opcode.size()) continue;
      const uint8_t m = u8(i + k);
      if ((m & 7) == 4) continue;
      if ((m >> 6) == 1 && static_cast<int8_t>(u8(i + k + 1)) == disp) return static_cast<long>(i);
      if ((m >> 6) == 2 && i32(i + k + 1) == disp) return static_cast<long>(i);
    }
    return -1;
  }
  // The first jne (75 rel8 or 0F 85 rel32) from `from` on: where it goes (an offset), or -1.
  long jne_target(size_t from) const {
    for (size_t i = from; i + 6 <= n; ++i) {
      if (u8(i) == 0x75) return static_cast<long>(i) + 2 + static_cast<int8_t>(u8(i + 1));
      if (u8(i) == 0x0F && u8(i + 1) == 0x85) return static_cast<long>(i) + 6 + i32(i + 2);
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
Code code_of(const char* type, const char* method, std::initializer_list<const char*> params, size_t max = 0x1000,
             bool is_static = false) {
  return code_at(method_code(type, method, params, is_static), max);
}

// A float getter `movss xmm0,[rdx+disp]; ret`: where it reads, or -1.
int32_t float_getter_place(const char* type, const char* method) {
  const Code c = code_of(type, method, {});
  if (!c.at || c.u8(0) != 0xF3 || c.u8(1) != 0x0F || c.u8(2) != 0x10) return -1;
  if (c.u8(3) == 0x42 && c.u8(5) == 0xC3) return c.u8(4);
  if (c.u8(3) == 0x82 && c.u8(8) == 0xC3) return c.i32(4);
  return -1;
}
int32_t offset_of(const char* type, const char* field) {
  const re::Field f = re::find_field(re::find_type(type), field);
  return f.valid() ? static_cast<int32_t>(f.offset) : -1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_luis.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_luis_" + std::to_string(GetCurrentProcessId()) + "\\";
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
  check(game::parts().players && game::parts().partner,
        "discovery: the characters and the partners (CharacterManager's PartnerContextList)");
  check(game::parts().luis_timer, "discovery: Luis's time limit (CustomLevelTimer's _Timer, the Timer's time gone, "
                                  "done and limit)");
  check(enum_of("chainsaw.CharacterKindID", "ch2_a3z0") == game::kLuisPartnerKind &&
            enum_of("chainsaw.CharacterKindID", "ch2_a1z0") == game::kAshleyPartnerKind,
        "CharacterKindID: Luis ch2_a3z0 200032, Ashley ch2_a1z0 200030");

  // --- the partners, as CharacterManager holds them ---
  check(game::luis_context() == 0, "no CharacterManager yet: no Luis");
  const uintptr_t cm = make("chainsaw.CharacterManager");
  const re::Field inst = re::find_field(re::find_type("chainsaw.CharacterManager"), "_Instance");
  check(inst.valid() && inst.is_static(), "CharacterManager's _Instance (its AppSingleton`1)");
  if (!statics[inst.declaring]) statics[inst.declaring] = alloc(0x100);
  *reinterpret_cast<uintptr_t*>(statics[inst.declaring] + inst.offset) = cm;
  auto partner = [&](int kind, int hp, int max) {
    const uintptr_t ctx = make("chainsaw.PartnerBaseContext");
    const uintptr_t point = make("chainsaw.HitPoint");
    set<int32_t>(ctx, "<KindID>k__BackingField", kind);
    set<uintptr_t>(ctx, "<HitPoint>k__BackingField", point);
    set<int32_t>(point, "<DefaultHitPoint>k__BackingField", max);
    set<int32_t>(point, "<CurrentHitPoint>k__BackingField", hp);
    return ctx;
  };
  const uintptr_t ashley = partner(game::kAshleyPartnerKind, 700, 1000);
  const uintptr_t luis = partner(game::kLuisPartnerKind, 1200, 2000);
  const uintptr_t stranger = make("chainsaw.HitPoint");  // not a context
  const char* const kList = "System.Collections.Generic.List`1<chainsaw.CharacterContext>";
  set<uintptr_t>(cm, "<PartnerContextList>k__BackingField", list_of(kList, "chainsaw.CharacterContext", {ashley}));
  check(game::luis_context() == 0 && game::ashley_context() == ashley, "Ashley alone beside Leon: no Luis");
  set<uintptr_t>(cm, "<PartnerContextList>k__BackingField",
                 list_of(kList, "chainsaw.CharacterContext", {stranger, ashley, luis}));
  check(game::luis_context() == luis, "Luis among the partners (kind 200032): found, past one that is no context");
  check(game::ashley_context() == ashley, "and Ashley is still Ashley");
  set<uintptr_t>(cm, "<PartnerContextList>k__BackingField", list_of(kList, "chainsaw.CharacterContext", {luis}));
  check(game::luis_context() == luis && game::ashley_context() == 0, "Luis beside Ada (Separate Ways): found, no Ashley");

  // --- God mode's hold on him (cheats.cpp hold_god, each tick while the switch is on) ---
  const uintptr_t point = get<uintptr_t>(luis, "<HitPoint>k__BackingField");
  game::Health h;
  check(game::health(luis, &h) && h.hp == 1200 && h.max == 2000 && !h.no_damage,
        "his health read as the panel shows it (1200 of 2000)");
  check(game::hold_health(luis, true) && get<bool>(point, "<NoDamage>k__BackingField") &&
            get<int32_t>(point, "<CurrentHitPoint>k__BackingField") == 2000,
        "held: the game's own NoDamage on, his health back at its maximum");
  check(!game::hold_health(luis, true), "held already: nothing to write");
  set<int32_t>(point, "<CurrentHitPoint>k__BackingField", 0);
  check(!game::hold_health(luis, true) && get<int32_t>(point, "<CurrentHitPoint>k__BackingField") == 0,
        "none left (a death under way): the game's - not raised");
  set<int32_t>(point, "<CurrentHitPoint>k__BackingField", 1500);
  check(game::hold_health(luis, false) && !get<bool>(point, "<NoDamage>k__BackingField") &&
            get<int32_t>(point, "<CurrentHitPoint>k__BackingField") == 1500,
        "off: NoDamage off, his health as it is");
  check(game::health(ashley, &h) && h.hp == 700 && !h.no_damage, "Ashley's untouched by it");

  // --- Separate Ways' time limit to save him: the level timer and its timer ---
  const uintptr_t lt = make("chainsaw.CustomLevelTimer");
  game::LevelTimer v;
  check(!game::level_timer(lt, &v) && !game::hold_level_timer(lt), "before its start (no _Timer yet): not read, not held");
  const uintptr_t timer = make("chainsaw.Timer");
  set<uintptr_t>(lt, "_Timer", timer);
  set<float>(timer, "_TimeLimit", 180.0f);
  set<float>(timer, "_TransitTime", 95.5f);
  check(game::level_timer(lt, &v) && v.limit == 180.0f && v.elapsed == 95.5f && !v.done,
        "its timer read: 180 s, 95.5 gone, not run out");
  check(game::hold_level_timer(lt) && get<float>(timer, "_TransitTime") == 0.0f,
        "held: no time gone (its gauge full)");
  check(!game::hold_level_timer(lt), "held already (nothing gone): nothing to write");
  set<float>(timer, "_TransitTime", 180.0f);
  set<bool>(timer, "_Completed", true);
  check(game::level_timer(lt, &v) && v.done && !game::hold_level_timer(lt) && get<float>(timer, "_TransitTime") == 180.0f,
        "run out (its end flag set, the game over under way): the game's - not held");
  set<bool>(timer, "_Completed", false);
  check(!game::hold_level_timer(timer) && !game::hold_level_timer(luis) && !game::hold_level_timer(0),
        "not a level timer: nothing");
  set<uintptr_t>(lt, "_Timer", point);
  check(!game::level_timer(lt, &v) && !game::hold_level_timer(lt) && get<int32_t>(point, "<CurrentHitPoint>k__BackingField") == 1500,
        "a _Timer that is no Timer: nothing read or written");
  set<uintptr_t>(lt, "_Timer", timer);

  // The game's frames over it, as its rule runs them (the code is checked below): each
  // frame Timer.update adds the frame's time to the time gone and is done once that
  // reaches the limit; the gauge shows (1 - gone / limit) x 100; done, its end flag
  // and the game over. God mode holds it before each frame, as the hook does.
  auto frame = [&](bool god, float dt) {
    if (god) game::hold_level_timer(lt);
    if (get<bool>(timer, "_Completed")) return 0.0f;
    float gone = get<float>(timer, "_TransitTime") + dt;
    if (gone >= get<float>(timer, "_TimeLimit")) {
      gone = get<float>(timer, "_TimeLimit");
      set<bool>(timer, "_Completed", true);
    }
    set<float>(timer, "_TransitTime", gone);
    return (1.0f - gone / get<float>(timer, "_TimeLimit")) * 100.0f;
  };
  set<float>(timer, "_TransitTime", 0.0f);
  float gauge = 100.0f, lowest = 100.0f;
  for (int i = 0; i < 10 * 60 * 60; ++i) {
    gauge = frame(true, 1.0f / 60);
    if (gauge < lowest) lowest = gauge;
  }
  check(!get<bool>(timer, "_Completed") && lowest > 99.9f,
        "held: ten minutes of frames at 60 fps - never run out, the gauge never below 99.9");
  int frames = 0;
  while (!get<bool>(timer, "_Completed") && frames < 400 * 60) {
    frame(false, 1.0f / 60);
    ++frames;
  }
  check(frames > 179 * 60 && frames <= 181 * 60, "off: its 180 s run from full (the hold left it with none gone)");

  // --- the game's code these rely on ---
  // The partner kinds and list.
  {
    const Code is_partner = code_of("chainsaw.CharacterEnumDefine", "isPartner", {"chainsaw.CharacterKindID"}, 0x40, true);
    check(is_partner.find({0x81, 0xFA, 0x5E, 0x0D, 0x03, 0x00}) >= 0 && is_partner.find({0x81, 0xFA, 0x60, 0x0D, 0x03, 0x00}) >= 0,
          "CharacterEnumDefine.isPartner: Ashley's kind and Luis's");
    const int32_t partners_at = offset_of("chainsaw.CharacterManager", "<PartnerContextList>k__BackingField") + 0x10;
    const Code rebuild = code_of("chainsaw.CharacterManager", "onFrameHeadUpdate", {}, 0x2000);
    const uintptr_t add = method_code(kList, "AddWithResize", {"chainsaw.CharacterContext"});
    const long cleared = rebuild.on({0x48, 0x8B}, partners_at);
    const long read = cleared >= 0 ? rebuild.on({0x48, 0x8B}, partners_at, static_cast<size_t>(cleared) + 7) : -1;
    const long added = read >= 0 ? rebuild.call_to(add, static_cast<size_t>(read)) : -1;
    check(cleared >= 0 && cleared < 0x80 && read > cleared && added > read && added - read < 0x40,
          "CharacterManager.onFrameHeadUpdate: the partner list emptied, then each partner context added - every frame");
  }
  // His death: a hit cut short of his health unless he is in battle; none left, he dies.
  {
    const Code driver = code_of("chainsaw.PartnerBodyHitDriver", "onHitDamage", {"chainsaw.HitController.DamageInfo"});
    const long his = driver.find({0x3D, 0x60, 0x0D, 0x03, 0x00});  // cmp eax, 200032
    const int64_t battle = re::enum_value64(re::find_type("chainsaw.PlayerDefine.State"), "Battle", 0);
    long mask = -1;
    for (long i = his; his >= 0 && i + 10 <= static_cast<long>(driver.n) && mask < 0; ++i)
      if (driver.u8(static_cast<size_t>(i)) == 0x48 && driver.u8(static_cast<size_t>(i) + 1) == 0xB8 &&
          mem::read<int64_t>(driver.at + static_cast<size_t>(i) + 2) == battle)
        mask = i;
    const int32_t state_at = offset_of("chainsaw.PlayerBaseContext", "<State>k__BackingField") + 0x10;
    const int32_t damage_at = offset_of("chainsaw.HitController.DamageInfo", "<Damage>k__BackingField") + 0x10;
    const long tested = mask > 0 ? driver.on({0x48, 0x85}, state_at, static_cast<size_t>(mask)) : -1;
    const long cut = tested > 0 ? driver.find({0xFF, 0xC8}, static_cast<size_t>(tested)) : -1;  // dec eax
    check(his > 0 && battle && mask > his && tested > mask,
          "PartnerBodyHitDriver.onHitDamage: for Luis (200032), whether his state holds Battle");
    check(cut > tested && driver.on({0x89}, damage_at, static_cast<size_t>(cut)) == cut + 2,
          "... and out of battle a hit's damage is cut to one point short of his health (written into the hit's)");
    const Code down = code_of("chainsaw.Ch2a3z0HeadUpdater", "ForceDown", {});
    const int32_t invincible_at = offset_of("chainsaw.HitPoint", "<Invincible>k__BackingField") + 0x10;
    const int32_t hp_at = offset_of("chainsaw.HitPoint", "<CurrentHitPoint>k__BackingField") + 0x10;
    const long inv = down.on({0x38}, invincible_at);
    const long none = down.on({0x39}, hp_at);
    const long dies = down.call_to(method_code("chainsaw.PlayerHeadUpdater", "requestDead",
                                               {"chainsaw.PlayerDefine.DeadType", "System.Action`1<System.Boolean>"}));
    check(inv > 0 && none > inv && dies > none,
          "Ch2a3z0HeadUpdater.ForceDown: not invincible and no health left - then his death (requestDead)");
    const Code frame_code = code_of("chainsaw.Ch2a3z0HeadUpdater", "update", {}, 0x4000);
    check(frame_code.call_to(down.at) >= 0, "Ch2a3z0HeadUpdater.update: his ForceDown, every frame");
    const int32_t no_damage_at = offset_of("chainsaw.HitPoint", "<NoDamage>k__BackingField") + 0x10;
    const Code add_damage = code_of("chainsaw.HitPoint", "addDamage", {"System.Int32"});
    const long out = add_damage.jne_target(4);
    check(add_damage.on({0x80}, no_damage_at) == 0 && out > 0 && add_damage.u8(static_cast<size_t>(out)) == 0xC3,
          "HitPoint.addDamage: NoDamage first - on, it returns, taking nothing");
    const Code check_dead = code_of("chainsaw.HitPoint", "checkDead", {"System.Int32"});
    const long no = check_dead.jne_target(4);
    check(check_dead.on({0x80}, no_damage_at) == 0 && no > 0 && check_dead.u8(static_cast<size_t>(no)) == 0x32 &&
              check_dead.u8(static_cast<size_t>(no) + 1) == 0xC0 && check_dead.u8(static_cast<size_t>(no) + 2) == 0xC3,
          "HitPoint.checkDead: NoDamage first - on, no hit kills");
  }
  // The level timer's frame and its timer.
  {
    const char* clt = "chainsaw.CustomLevelTimer";
    int subclasses = 0;
    const uint32_t lt_t = re::find_type(clt);
    for (uint32_t t = 1; t < re::num_types(); ++t)
      if (t != lt_t && re::derives(t, lt_t)) ++subclasses;
    check(lt_t && subclasses == 0, "CustomLevelTimer: no subclasses (its own table's lateUpdate is every one's)");
    const int32_t lt_hdr = float_getter_place(clt, "get_TimerSecond") - offset_of(clt, "_TimerSecond");
    const int32_t timer_at = offset_of(clt, "_Timer") + lt_hdr;
    const int32_t transit_at = float_getter_place("chainsaw.Timer", "get_TransitTime");
    const int32_t limit_at = float_getter_place("chainsaw.TimerBase", "get_TimeLimit");
    check(lt_hdr > 0 && transit_at == offset_of("chainsaw.Timer", "_TransitTime") + 0x10 &&
              limit_at == offset_of("chainsaw.TimerBase", "_TimeLimit") + 0x10,
          "the getters read the fields the mod reads (_TimerSecond, the Timer's _TransitTime and _TimeLimit)");
    const Code late = code_of(clt, "lateUpdate", {}, 0x1000);
    const uintptr_t update = method_code("chainsaw.Timer", "update", {"System.Nullable`1<System.Single>"});
    const long timer_read = late.on({0x48, 0x8B}, timer_at);
    const long ran = late.call_to(update);
    const long rate = late.call_to(method_code("chainsaw.TimerBase", "get_ProgressRate", {}));
    const int gauge_gui = enum_of("chainsaw.GuiType", "Timer_31_1011");
    const long gui = gauge_gui > 0 ? late.find({0x41, 0xB8, static_cast<uint8_t>(gauge_gui), static_cast<uint8_t>(gauge_gui >> 8), 0, 0}) : -1;
    const long done = late.find({0x48, 0x8B, 0x41, 0x30});  // mov rax,[rcx+0x30]: the timer's table, slot 6 (get_Completed)
    const long flag = late.call_to(method_code("chainsaw.ScenarioFlagManager", "requestSetFlag",
                                               {"System.Guid", "System.Boolean"}));
    check(timer_read > 0 && ran > timer_read && rate > ran,
          "CustomLevelTimer.lateUpdate: its _Timer run by Timer.update, then how far it has run (ProgressRate)");
    check(gauge_gui == 156 && gui > rate, "... shown as the Timer_31_1011 gauge (GuiType 156)");
    check(done > gui && flag > done, "... and once the timer is done (its table's get_Completed), its end flag set");
    const Code up = code_of("chainsaw.Timer", "update", {"System.Nullable`1<System.Single>"});
    const long added = up.on({0xF3, 0x0F, 0x11}, transit_at);
    const long completed = up.call_to(method_code("chainsaw.Timer", "updateCompleted", {}));
    check(up.on({0xF3, 0x0F, 0x10}, transit_at) > 0 && added > 0 && completed > added,
          "Timer.update: the frame's time added to the time gone, then whether it is done");
    const Code uc = code_of("chainsaw.Timer", "updateCompleted", {});
    const long lim = uc.on({0xF3, 0x0F, 0x10}, limit_at);
    const long gone = uc.on({0xF3, 0x0F, 0x10}, transit_at);
    check(lim >= 0 && gone > lim && uc.find({0x66, 0x0F, 0x2F}, static_cast<size_t>(gone)) > gone,
          "Timer.updateCompleted: done only once the time gone reaches the limit");
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
