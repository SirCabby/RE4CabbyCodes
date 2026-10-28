// God mode on the water scooter of the escape at the main game's end (game.cpp:
// jet_ski, jet_ski_health, hold_jet_ski - which cheats.cpp's tick holds while God
// mode is on) over objects shaped like the game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_jetski.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_jetski.exe
//   wine tests/build/test_jetski.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then the scooter is built the way the VM lays out
// managed objects (as test_stats does): JetSkiManager in its AppSingleton's
// _Instance, its MainJetSki (a GmJetSki) and the scooter's settings
// (GmJetSkiUserdata) - every field placed by name from the real database. The hull
// is read as the panel shows it, and held as the tick holds it: at its maximum, the
// grace after a hit kept running and the last damage the most there is - only while
// the ride is under way - and let go when the switch goes off. And the game's code
// the hold relies on (found by name, read in the mapped exe): processDamage passes
// over a hit no worse than the last while the grace runs, before it writes anything;
// only processDamage sends the scooter to its Crash state, and only once its health
// is gone; the game over comes only from that state; a new ride (or a Continue)
// starts with the game's own values; and the no-damage challenge is the game's own
// look at a full hull.
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
  const uintptr_t o = alloc(0x1000);  // a GmJetSki's fields reach 0x840
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

int enum_of(const char* type, const char* literal) { return re::enum_value(re::find_type(type), literal, -1); }

// A method's code in the mapped exe, up to the ret the compiler padded after it - or
// to the padding itself, for one that ends in a jump (processDamage).
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
  // The first instruction `opcode` (its bytes, after any REX prefix) whose memory operand is
  // [reg+disp32] (ModRM mod 10, no SIB) at `disp`, its ModRM reg field `reg` (-1: any): the
  // opcode's offset, or -1. The ModRM byte is at the offset + the opcode's size.
  long on(std::initializer_list<uint8_t> opcode, int32_t disp, int reg = -1, size_t from = 0) const {
    for (size_t i = from; i + opcode.size() + 5 <= n; ++i) {
      size_t k = 0;
      for (const uint8_t b : opcode)
        if (u8(i + k) == b) ++k;
        else break;
      if (k != opcode.size()) continue;
      const uint8_t m = u8(i + k);
      if ((m >> 6) == 2 && (m & 7) != 4 && (reg < 0 || ((m >> 3) & 7) == reg) && i32(i + k + 1) == disp)
        return static_cast<long>(i);
    }
    return -1;
  }
  // A jle at `i` (7E rel8 or 0F 8E rel32): where it goes (an offset), or -1.
  long jle_target(long i) const {
    if (i < 0) return -1;
    const size_t j = static_cast<size_t>(i);
    if (u8(j) == 0x7E) return i + 2 + static_cast<int8_t>(u8(j + 1));
    if (u8(j) == 0x0F && u8(j + 1) == 0x8E) return i + 6 + i32(j + 2);
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

// A getter `mov eax,[rdx+disp32]; ret`: where it reads, or -1.
int32_t getter_place(const char* type, const char* method) {
  const Code c = code_of(type, method, {});
  return c.at && c.u8(0) == 0x8B && c.u8(1) == 0x82 && c.u8(6) == 0xC3 ? c.i32(2) : -1;
}
int32_t offset_of(const char* type, const char* field) {
  const re::Field f = re::find_field(re::find_type(type), field);
  return f.valid() ? static_cast<int32_t>(f.offset) : -1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_jetski.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_jetski_" + std::to_string(GetCurrentProcessId()) + "\\";
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
  check(game::parts().jet_ski, "discovery: the water scooter (JetSkiManager's MainJetSki, its hull, its state, its grace)");

  const int run = enum_of("chainsaw.GmJetSki.GameState", "Run");
  const int crash = enum_of("chainsaw.GmJetSki.GameState", "Crash");
  const int standby = enum_of("chainsaw.GmJetSki.GameState", "Standby");
  check(standby == 0 && run == 1 && crash == 2, "GmJetSki.GameState: Standby 0, Run 1, Crash 2");

  // --- the scooter, as JetSkiManager holds it ---
  check(game::jet_ski() == 0, "no JetSkiManager yet: no scooter");
  const uintptr_t mgr = make("chainsaw.JetSkiManager");
  const re::Field inst = re::find_field(re::find_type("chainsaw.JetSkiManager"), "_Instance");
  check(inst.valid() && inst.is_static(), "JetSkiManager's _Instance (its AppSingleton`1)");
  if (!statics[inst.declaring]) statics[inst.declaring] = alloc(0x100);
  *reinterpret_cast<uintptr_t*>(statics[inst.declaring] + inst.offset) = mgr;
  check(game::jet_ski() == 0, "a JetSkiManager without a MainJetSki (no scooter in the scene): none");
  const uintptr_t js = make("chainsaw.GmJetSki");
  const uintptr_t setting = make("chainsaw.GmJetSkiUserdata");
  set<uintptr_t>(mgr, "<MainJetSki>k__BackingField", js);
  set<uintptr_t>(js, "_JetSkiSetting", setting);
  set<int32_t>(setting, "_MaxHP", 100);
  check(game::jet_ski() == js, "the scooter: JetSkiManager's MainJetSki");
  set<uintptr_t>(mgr, "<MainJetSki>k__BackingField", setting);
  check(game::jet_ski() == 0, "a MainJetSki that is no GmJetSki: none");
  set<uintptr_t>(mgr, "<MainJetSki>k__BackingField", js);

  auto state = [&](int current, int next) {
    set<int32_t>(js, "<CurrentGameState>k__BackingField", current);
    set<int32_t>(js, "_NextGameState", next);
  };
  auto hp = [&]() { return get<int32_t>(js, "_CurrentHP"); };
  auto last = [&]() { return get<int32_t>(js, "_LastDamage"); };
  auto grace = [&]() { return get<float>(js, "_IgnoreDamageTimer"); };
  auto hull = [&](int h, int l, float g) {
    set<int32_t>(js, "_CurrentHP", h);
    set<int32_t>(js, "_LastDamage", l);
    set<float>(js, "_IgnoreDamageTimer", g);
  };

  // --- the panel's figure ---
  game::JetSki j;
  state(standby, standby);
  hull(0, 0, 0.0f);
  check(game::jet_ski_health(js, &j) && j.max == 100 && !j.running && !j.crashed,
        "waiting for the ride (Standby): read, not running (the panel shows nothing)");
  state(run, run);
  hull(70, 30, 0.0f);
  check(game::jet_ski_health(js, &j) && j.hp == 70 && j.max == 100 && j.running && !j.crashed,
        "the ride under way: its health (70 of its settings' 100), running");
  state(run, crash);
  check(game::jet_ski_health(js, &j) && !j.running && j.crashed, "Run with Crash next (its health gone): crashed");
  state(crash, crash);
  check(game::jet_ski_health(js, &j) && !j.running && j.crashed, "Crash: crashed");
  check(!game::jet_ski_health(setting, &j) && !game::jet_ski_health(0, &j), "not a scooter: not read");
  set<uintptr_t>(js, "_JetSkiSetting", 0);
  state(run, run);
  check(!game::jet_ski_health(js, &j), "a scooter without its settings: not read");
  set<uintptr_t>(js, "_JetSkiSetting", setting);

  // --- God mode's hold (cheats.cpp hold_jet_ski, each tick while the switch is on) ---
  state(run, run);
  hull(70, 30, 0.25f);
  check(game::hold_jet_ski(js, true) && hp() == 100 && last() == game::kJetSkiNoWorse && grace() == game::kJetSkiGrace,
        "the ride under way: the hull back at its maximum, the grace held (60 s), the last damage the most there is");
  check(!game::hold_jet_ski(js, true) && hp() == 100, "held already: nothing to write");
  set<float>(js, "_IgnoreDamageTimer", game::kJetSkiGrace / 2 + 0.5f);
  check(!game::hold_jet_ski(js, true) && grace() == game::kJetSkiGrace / 2 + 0.5f,
        "the grace run down by the steps' time, still above half: left");
  set<float>(js, "_IgnoreDamageTimer", game::kJetSkiGrace / 2 - 0.5f);
  check(game::hold_jet_ski(js, true) && grace() == game::kJetSkiGrace, "below half: topped up");
  set<int32_t>(js, "_LastDamage", 12);
  check(game::hold_jet_ski(js, true) && last() == game::kJetSkiNoWorse, "the last damage the game wrote: held again");

  // The game's own rule, as processDamage applies it (its code is checked below): a
  // hit's damage comes off the hull only when the grace has run out or the hit is
  // worse than the last; none left, the scooter's next state is Crash.
  auto take_hit = [&](int damage, float step) {
    float g = grace() - step;
    if (g < 0.0f) g = 0.0f;
    set<float>(js, "_IgnoreDamageTimer", g);
    if (damage <= 0 || (g > 0.0f && damage <= last())) return;
    set<int32_t>(js, "_LastDamage", damage);
    const int left = hp() - damage;
    set<int32_t>(js, "_CurrentHP", left > 0 ? left : 0);
    if (hp() <= 0) set<int32_t>(js, "_NextGameState", crash);
  };
  for (int i = 0; i < 40 * 60; ++i) take_hit(1000000, 1.0f / 60);
  check(hp() == 100 && get<int32_t>(js, "_NextGameState") == run && grace() > 0.0f,
        "held: a hit every step for 40 s, each worse than any the game knows - none takes health, none sends it to Crash");
  check(game::hold_jet_ski(js, true) && grace() == game::kJetSkiGrace,
        "and the tick tops the grace up again (it does every frame; the 40 s ran it below half)");

  hull(40, 30, 0.0f);
  state(run, crash);
  check(!game::hold_jet_ski(js, true) && hp() == 40 && last() == 30 && grace() == 0.0f,
        "a crash under way (Crash next): the game's, nothing written");
  state(crash, crash);
  check(!game::hold_jet_ski(js, true) && hp() == 40, "Crash: nothing written");
  state(standby, standby);
  hull(0, 0, 0.0f);
  check(!game::hold_jet_ski(js, true) && hp() == 0 && last() == 0,
        "waiting for the ride: nothing written (the ride's start sets its own health)");
  check(!game::hold_jet_ski(setting, true) && !game::hold_jet_ski(0, true), "not a scooter: nothing");

  // --- the switch off: the mod's grace let go, only where its values are still there ---
  state(run, run);
  hull(100, 30, 0.0f);
  game::hold_jet_ski(js, true);
  check(game::hold_jet_ski(js, false) && last() == 0 && grace() == 0.0f && hp() == 100,
        "off: no last damage, the grace run out - the next hit counts (the hull as it is)");
  check(!game::hold_jet_ski(js, false), "off again: nothing to let go");
  hull(80, 20, 0.4f);
  check(!game::hold_jet_ski(js, false) && last() == 20 && grace() == 0.4f,
        "off with the game's own grace (a hit just taken): left to run");
  take_hit(15, 1.0f / 60);
  check(hp() == 80, "and the game's rule: a lesser hit within its grace takes nothing");
  take_hit(25, 1.0f / 60);
  check(hp() == 55, "a worse one does");

  // --- the game's code the hold relies on ---
  const char* gm = "chainsaw.GmJetSki";
  // Where the fields are in a GmJetSki (a via.Behavior) and its settings (a via.UserData):
  // the getters' reads, less the database's offsets, give each class's header.
  const int32_t hp_at = getter_place(gm, "get_CurrentHP"), next_at = getter_place(gm, "get_NextGameState"),
                state_at = getter_place(gm, "get_CurrentGameState"),
                max_at = getter_place("chainsaw.GmJetSkiUserdata", "get_MaxHP");
  const int32_t hdr = hp_at - offset_of(gm, "_CurrentHP");
  check(hp_at > 0 && next_at > 0 && state_at > 0 && max_at > 0 && hdr == next_at - offset_of(gm, "_NextGameState") &&
            hdr == state_at - offset_of(gm, "<CurrentGameState>k__BackingField"),
        "GmJetSki's getters read the fields the mod reads (_CurrentHP, _NextGameState, <CurrentGameState>)");
  check(max_at - offset_of("chainsaw.GmJetSkiUserdata", "_MaxHP") > 0, "GmJetSkiUserdata.get_MaxHP reads _MaxHP");
  const int32_t last_at = offset_of(gm, "_LastDamage") + hdr, grace_at = offset_of(gm, "_IgnoreDamageTimer") + hdr;

  const Code pd = code_of(gm, "processDamage",
                          {"System.Int32", "System.Int32", "chainsaw.GmJetSkiUserdata.DamageTier", "System.Single"});
  const long grace_read = pd.on({0xF3, 0x0F, 0x10}, grace_at);
  const long grace_stepped = pd.on({0xF3, 0x0F, 0x11}, grace_at);
  const long hp_write = pd.on({0x89}, hp_at);
  check(grace_read >= 0 && grace_read < 0x40 && grace_stepped > grace_read && hp_write > grace_stepped,
        "processDamage: the grace (_IgnoreDamageTimer) read first, the step's time taken off it");
  const long damage_branch = pd.find({0x45, 0x85, 0xC0, 0x0F, 0x8F});  // test r8d (the damage), r8d; jg
  const long damage_at = damage_branch >= 0 ? damage_branch + 9 + pd.i32(static_cast<size_t>(damage_branch + 5)) : -1;
  const long worse = pd.on({0x3B}, last_at);  // cmp r32 (the damage), [this+_LastDamage]
  const long passed = pd.jle_target(worse + 6);
  check(damage_at > 0 && worse > damage_at && worse - damage_at < 0x20,
        "processDamage: a hit with damage - while the grace runs, compared with the last damage first");
  check(passed > 0 && pd.u8(static_cast<size_t>(passed)) == 0x33 && pd.u8(static_cast<size_t>(passed) + 1) == 0xC0 &&
            passed < damage_at,
        "processDamage: one no worse than the last returns 0 at once (no health off, no shake, no reaction)");
  check(hp_write > worse && pd.on({0x89}, last_at) > worse,
        "processDamage: the health (and the last damage) written only after that test");
  const uintptr_t set_next = method_code(gm, "set_NextGameState", {"chainsaw.GmJetSki.GameState"});
  const long to_crash = pd.call_to(set_next);
  check(to_crash > hp_write && pd.find({0x41, 0xB8, static_cast<uint8_t>(crash), 0, 0, 0}, static_cast<size_t>(hp_write)) > hp_write &&
            pd.find({0x41, 0xB8, static_cast<uint8_t>(crash), 0, 0, 0}, static_cast<size_t>(hp_write)) < to_crash,
        "processDamage: the next state Crash, after the health is written (once none is left)");
  {
    // Nothing else in the game calls set_NextGameState but processDamage and the
    // start of the ride (Run): the only way into Crash is a hit that empties the hull.
    const mem::Range text = mem::section(g_game, ".text");
    int calls = 0, crash_calls = 0;
    for (uintptr_t a = text.begin; a + 5 <= text.end; ++a)
      if (mem::read<uint8_t>(a) == 0xE8 && a + 5 + static_cast<intptr_t>(mem::read<int32_t>(a + 1)) == set_next) {
        ++calls;
        if (a >= pd.at && a < pd.at + pd.n) ++crash_calls;
      }
    check(calls == 1 && crash_calls == 1, "set_NextGameState: called only from processDamage");
    const Code sn = code_at(set_next);
    const long reset = sn.call_to(method_code(gm, "resetCarStartPosture", {}));
    check(sn.on({0x89}, next_at) >= 0 && reset > 0 &&
              sn.find({0x41, 0x83, 0xF8, static_cast<uint8_t>(run)}) >= 0 && sn.find({0x41, 0x83, 0xF8, static_cast<uint8_t>(crash)}) >= 0,
          "set_NextGameState: Run starts a ride afresh (resetCarStartPosture), Crash starts the game-over timer");
  }
  const Code over = code_of(gm, "updateGameOverTimer", {});
  const long is_crash = over.on({0x83}, state_at, 7);  // cmp dword [this+<CurrentGameState>], imm8
  const long game_over =
      over.call_to(method_code("chainsaw.CharacterManager", "requestGameOver",
                               {"chainsaw.gui.GameOverGuiKind", "chainsaw.gui.GameOverDefinition.GameOverCauseType"}));
  check(is_crash >= 0 && is_crash < 0x20 && over.u8(static_cast<size_t>(is_crash) + 6) == static_cast<uint8_t>(crash) &&
            game_over > is_crash,
        "updateGameOverTimer: the game over asked for only in the Crash state");
  const Code start = code_of(gm, "resetCarStartPosture", {}, 0x3000);
  const long max_read = start.on({0x8B}, max_at);
  const long hp_set = start.on({0x89}, hp_at, -1, max_read > 0 ? static_cast<size_t>(max_read) : 0);
  check(max_read >= 0 && hp_set > max_read && start.on({0x48, 0xC7}, last_at, 0) > hp_set &&
            start.on({0x48, 0xC7}, grace_at, 0) > hp_set,
        "resetCarStartPosture: a new ride (a Continue too) starts full, no last damage, no grace - the hold takes it afresh");
  const Code record = code_of(gm, "tryCheckNoDamageRecord", {});
  const long full = record.on({0x39}, hp_at);  // cmp [this+_CurrentHP], r32 (the most)
  const int smooth = enum_of("chainsaw.GameRecordID", "Record_072");
  check(record.on({0x8B}, max_at) >= 0 && full > 0 &&
            record.find({0x41, 0xB8, static_cast<uint8_t>(smooth), 0, 0, 0}) > full &&
            record.jump_to(method_code("chainsaw.GameRecordManager", "unlockRecord", {"chainsaw.GameRecordID"})) > full,
        "tryCheckNoDamageRecord: the no-damage challenge (Smooth Escape, record 72) is the game's look at a full hull");

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
