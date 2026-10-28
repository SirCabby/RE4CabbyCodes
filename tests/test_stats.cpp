// The results' reads and writes (game.cpp: stats, set_counter, set_total, the
// elements' counts the occurrence hook uses) and the play time's (clock, the
// laps as the game adds them up, the RESULTS screen's total) over objects shaped
// like the game's, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_stats.cpp
//       <every build/*.o but dllmain.o> build/imgui/*.o build/imgui/backends/*.o -luser32 -lshell32 -lole32 -lgdi32
//       -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_stats.exe
//   wine tests/build/test_stats.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped and its type database rebased as in test_discover, and
// game::discover runs over it. Then a GameStatsManager is made the way the VM
// lays out managed objects - the object's first word its REObjectInfo, the
// info's first word the class's database record, the fields' start the int32
// before the info - with its OngoingStats, the kill, death, times damaged and
// accuracy elements (each its own class), and two finished chapters' records in
// its List`1, every field placed by name from the real database. A count read
// through another element class's field is refused (re::field_addr): each
// element must be read through its own class's _Count (2026-10-23: the deaths
// and the times damaged were read through the kills' field, so the panel's
// results never showed and Deaths don't count never acted).
#include <windows.h>

#include <cstdio>
#include <cstring>
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
  const uintptr_t o = alloc(0x200);
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

// A stats object's elements: kill, death, times damaged, accuracy.
struct Elements {
  uintptr_t kill = 0, death = 0, damage = 0, acc = 0;
};
Elements elements(uintptr_t stats, int kills, int deaths, int damage, int attempts, int hits) {
  Elements e;
  e.kill = make("chainsaw.GameStatsKillCountElement");
  e.death = make("chainsaw.GameStatsDeathElement");
  e.damage = make("chainsaw.GameStatsDamageCountElement");
  e.acc = make("chainsaw.GameStatsAccuracyElement");
  set<int32_t>(e.kill, "_Count", kills);
  set<int32_t>(e.death, "_Count", deaths);
  set<int32_t>(e.damage, "_Count", damage);
  set<int32_t>(e.acc, "_HitAttempt", attempts);
  set<int32_t>(e.acc, "_HitSuccess", hits);
  set<uintptr_t>(stats, "<Kill>k__BackingField", e.kill);
  set<uintptr_t>(stats, "<Death>k__BackingField", e.death);
  set<uintptr_t>(stats, "<Damage>k__BackingField", e.damage);
  set<uintptr_t>(stats, "<Accuracy>k__BackingField", e.acc);
  return e;
}

int count(uintptr_t element) { return get<int32_t>(element, "_Count"); }
int attempts(const Elements& e) { return get<int32_t>(e.acc, "_HitAttempt"); }
int hits(const Elements& e) { return get<int32_t>(e.acc, "_HitSuccess"); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_stats.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_stats_" + std::to_string(GetCurrentProcessId()) + "\\";
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
  // The stand-in VM: the database pointer and a static table the test fills.
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
  check(game::parts().stats && game::parts().damage, "discovery: the results and the times damaged");

  // --- the game's objects ---
  // GameStatsManager, its AppSingleton's _Instance, its OngoingStats and history.
  const uintptr_t mgr = make("chainsaw.GameStatsManager");
  const re::Field inst = re::find_field(re::find_type("chainsaw.GameStatsManager"), "_Instance");
  check(inst.valid() && inst.is_static(), "GameStatsManager's _Instance (its AppSingleton`1)");
  const uintptr_t block = alloc(0x100);
  statics[inst.declaring] = block;
  *reinterpret_cast<uintptr_t*>(block + inst.offset) = mgr;
  const uintptr_t os = make("chainsaw.OngoingStats");
  set<uintptr_t>(mgr, "<OngoingStats>k__BackingField", os);
  // This chapter: 12 kills, 1 death, 3 times damaged, 30 hits of 40 shots.
  const Elements now = elements(os, 12, 1, 3, 40, 30);
  // Two finished chapters: 50/2/10, 80 of 100; 58/1/14, 190 of 260.
  const uintptr_t rec_a = make("chainsaw.ChapterStatsRecord"), rec_b = make("chainsaw.ChapterStatsRecord");
  const Elements a = elements(rec_a, 50, 2, 10, 100, 80);
  const Elements b = elements(rec_b, 58, 1, 14, 260, 190);
  const uintptr_t list = make("System.Collections.Generic.List`1<chainsaw.ChapterStatsRecord>");
  const uintptr_t arr = alloc(0x20 + 8 * 4);
  *reinterpret_cast<uintptr_t*>(arr + 0x10) = re::type_record(re::find_type("chainsaw.ChapterStatsRecord"));
  *reinterpret_cast<int32_t*>(arr + 0x1C) = 4;
  reinterpret_cast<uintptr_t*>(arr + 0x20)[0] = rec_a;
  reinterpret_cast<uintptr_t*>(arr + 0x20)[1] = rec_b;
  set<uintptr_t>(list, "_items", arr);
  set<int32_t>(list, "_size", 2);
  set<uintptr_t>(mgr, "<OngoingChapterStatsHistory>k__BackingField", list);

  // --- the elements, each through its own class ---
  check(game::element_count(now.kill) == 12, "a kill element's count");
  check(game::element_count(now.death) == 1, "a death element's count (its own class's _Count)");
  check(game::element_count(now.damage) == 3, "a times damaged element's count (its own class's _Count)");
  check(game::element_count(os) == -1, "something that is no count element: none");
  check(game::set_element_count(now.death, 0) && count(now.death) == 0, "a death taken back (the hook's write)");
  game::set_element_count(now.death, 1);
  game::StatsRefs r;
  check(game::stats_refs(&r) && r.death == now.death && r.damage == now.damage && r.accuracy == now.acc,
        "the hook's elements: the chapter's death, times damaged and accuracy");

  // --- what the panel shows ---
  using C = game::Counter;
  auto idx = [](C c) { return static_cast<int>(c); };
  game::Stats s;
  check(game::stats(&s) && s.ok, "the results are read (the panel's table shows)");
  check(s.chapter[idx(C::kDeaths)] == 1 && s.chapter[idx(C::kDamage)] == 3 && s.chapter[idx(C::kKills)] == 12 &&
            s.chapter[idx(C::kHits)] == 30 && s.chapter[idx(C::kMisses)] == 10,
        "this chapter: 1 death, 3 times damaged, 12 kills, 30 hits, 10 misses");
  check(s.total[idx(C::kDeaths)] == 4 && s.total[idx(C::kDamage)] == 27 && s.total[idx(C::kKills)] == 120 &&
            s.total[idx(C::kHits)] == 300 && s.total[idx(C::kMisses)] == 100 && s.finished == 2,
        "the whole game: 4, 27, 120, 300 hits, 100 misses over 2 finished chapters");

  // --- this chapter's editors ---
  check(game::set_counter(C::kDamage, 0) && count(now.damage) == 0, "this chapter's times damaged set to 0");
  check(game::set_counter(C::kMisses, 0) && attempts(now) == 30 && hits(now) == 30, "this chapter's misses to 0: shots = hits");
  check(game::set_counter(C::kHits, 35) && attempts(now) == 35 && hits(now) == 35, "this chapter's hits: the misses kept (0)");
  check(game::set_counter(C::kDeaths, 123456) && count(now.death) == 9999, "a count past the game's 9,999 stops there");

  // --- the whole game's ---
  game::set_counter(C::kDeaths, 1);
  check(game::set_total(C::kDeaths, 2) && count(a.death) == 2 && count(b.death) == 0 && count(now.death) == 0,
        "the whole game's deaths to 2: this chapter none, the latest finished chapter taken down");
  check(game::set_total(C::kDeaths, 0) && count(a.death) == 0 && count(b.death) == 0 && count(now.death) == 0,
        "the whole game's deaths to 0: every chapter's");
  check(game::set_total(C::kDamage, 30) && count(a.damage) == 10 && count(b.damage) == 14 && count(now.damage) == 6,
        "the whole game's times damaged raised: this chapter takes the difference");
  check(game::set_total(C::kMisses, 0) && attempts(a) == 80 && attempts(b) == 190 && attempts(now) == hits(now) &&
            hits(a) == 80 && hits(b) == 190,
        "the whole game's misses to 0: every chapter's shots are its hits");
  check(game::set_total(C::kHits, 310) && hits(now) == 40 && attempts(now) == 40 && hits(a) == 80 && hits(b) == 190,
        "the whole game's hits: this chapter's raised, its misses kept");
  check(game::stats(&s) && s.total[idx(C::kDeaths)] == 0 && s.total[idx(C::kDamage)] == 30 &&
            s.total[idx(C::kMisses)] == 0 && s.total[idx(C::kHits)] == 310,
        "the panel reads the whole game as set");

  // --- the play time ---
  // The game's clock (GameClock: its game save's times, its system count) and
  // the campaign clock that takes the system count's growth (ChainsawGameClock).
  constexpr uint64_t S = 1000000ull;  // a second, in the clocks' microseconds
  auto singleton = [&](const char* type_name) {
    const uintptr_t obj = make(type_name);
    const re::Field fi = re::find_field(re::find_type(type_name), "_Instance");
    if (!fi.valid() || !fi.is_static()) {
      std::printf("FAIL  %s has no _Instance\n", type_name);
      ++g_fail;
      return obj;
    }
    if (!statics[fi.declaring]) statics[fi.declaring] = alloc(0x100);
    *reinterpret_cast<uintptr_t*>(statics[fi.declaring] + fi.offset) = obj;
    return obj;
  };
  const uintptr_t gc = singleton("share.GameClock");
  const uintptr_t gsd = make("share.GameClock.GameSaveData");
  const uintptr_t ssd = make("share.GameClock.SystemSaveData");
  set<uintptr_t>(gc, "_GameSaveData", gsd);
  set<uintptr_t>(gc, "_SystemSaveData", ssd);
  // The record time: 2 h elapsed, less 10 min of cutscenes and 5 of pauses = 6300 s.
  set<uint64_t>(gsd, "_GameElapsedTime", 7200 * S);
  set<uint64_t>(gsd, "_DemoSpendingTime", 600 * S);
  set<uint64_t>(gsd, "_PauseSpendingTime", 300 * S);
  set<uint64_t>(ssd, "_SystemElapsedTime", 180000 * S);  // 50 h, every save
  // This chapter started 300 s ago.
  const uintptr_t start = make("chainsaw.GameStatsChapterLapTimeElement");
  set<uint64_t>(start, "_Time", 6000 * S);
  set<uintptr_t>(os, "<ChapterStartTime>k__BackingField", start);
  // Separate Ways: chapter 0 (600 s), chapter 1's first part (900 s) and its
  // second part, whose lap holds the first's (1500 s).
  auto lap_record = [&](int32_t chapter, uint64_t seconds, uintptr_t* lap) {
    const uintptr_t r = make("chainsaw.ChapterStatsRecord");
    *lap = make("chainsaw.GameStatsChapterLapTimeElement");
    set<uint64_t>(*lap, "_Time", seconds * S);
    set<uintptr_t>(r, "<ChapterLapTime>k__BackingField", *lap);
    set<int32_t>(r, "_Chapter", chapter);
    return r;
  };
  const uint32_t chapter_id = re::find_type("chainsaw.ChapterID");
  const int sw0 = re::enum_value(chapter_id, "cp11_chp0_1", -1), sw1a = re::enum_value(chapter_id, "cp11_chp1_1", -1),
            sw1b = re::enum_value(chapter_id, "cp11_chp1_2", -1);
  check(sw0 > 0 && sw1a > 0 && sw1b > sw1a, "Separate Ways' chapter ids by name");
  uintptr_t lap0 = 0, lap1a = 0, lap1b = 0;
  reinterpret_cast<uintptr_t*>(arr + 0x20)[0] = lap_record(sw0, 600, &lap0);
  reinterpret_cast<uintptr_t*>(arr + 0x20)[1] = lap_record(sw1a, 900, &lap1a);
  reinterpret_cast<uintptr_t*>(arr + 0x20)[2] = lap_record(sw1b, 1500, &lap1b);
  set<int32_t>(list, "_size", 3);
  auto lap_s = [&](uintptr_t lap) { return get<uint64_t>(lap, "_Time") / S; };
  game::Clock clk;
  check(game::clock(&clk) && clk.now == 6300 * S && clk.start == 6000 * S, "the clock: record time 6300 s, chapter start 6000 s");
  check(clk.laps == 2100 * S, "Separate Ways: a first part is not counted once its second part is recorded (600 + 1500)");
  check(game::play_time(clk) == 2400 * S, "the play time: the counted laps and this chapter's 300 s");
  check(game::cap_laps(1000 * S) && lap_s(lap1b) == 400 && lap_s(lap1a) == 900 && lap_s(lap0) == 600,
        "laps taken down: only counted ones, the latest first");
  check(game::raise_last_lap(100 * S) && lap_s(lap1b) == 500, "a lap raised: the latest counted one");
  // While the first part is the latest record, it counts.
  set<int32_t>(list, "_size", 2);
  check(game::clock(&clk) && clk.laps == 1500 * S, "Separate Ways: a first part still the latest record counts (600 + 900)");
  set<int32_t>(list, "_size", 3);

  // A load that kept the run's clock (a Continue): the clock's second copy
  // holds the save's times, its first the run's.
  uint64_t ahead = 1;
  check(game::parts().clock_copy && !game::clock_kept(&ahead), "no second copy yet: no load kept the clock");
  const uintptr_t reset = make("share.GameClock.GameSaveData");
  set<uintptr_t>(gc, "_GameSaveData_Reset", reset);
  set<uint64_t>(reset, "_GameElapsedTime", 7200 * S);
  set<uint64_t>(reset, "_DemoSpendingTime", 600 * S);
  set<uint64_t>(reset, "_PauseSpendingTime", 300 * S);
  check(!game::clock_kept(&ahead), "the two copies alike: a load of any other kind (or a new game)");
  // The checkpoint's save: 150 s of the clock ago, 10 of them paused.
  set<uint64_t>(reset, "_GameElapsedTime", 7050 * S);
  set<uint64_t>(reset, "_PauseSpendingTime", 290 * S);
  check(game::clock_kept(&ahead) && ahead == 140 * S, "the save's times in the copy alone: the run's clock kept, 140 s past the save");
  set<uint64_t>(reset, "_GameElapsedTime", 7200 * S);
  set<uint64_t>(reset, "_PauseSpendingTime", 300 * S);
  set<uint64_t>(reset, "_InventorySpendingTime", 30 * S);
  check(game::clock_kept(&ahead) && ahead == 0, "copies apart in the case's time alone: kept too (no record time between them)");
  check(game::clock(&clk) && clk.now == 6300 * S, "the play time reads the run's copy");

  // The RESULTS screen's total: the system count, with the campaign clock's last look.
  uint64_t total = 0;
  check(game::parts().system_time && game::system_time(&total) && total == 180000 * S, "the RESULTS screen's total read");
  check(!game::set_system_time(3600 * S) && get<uint64_t>(ssd, "_SystemElapsedTime") == 180000 * S,
        "no campaign clock: the total is left alone (its next look would wrap)");
  const uintptr_t cgc = singleton("chainsaw.ChainsawGameClock");
  set<uint64_t>(cgc, "PrevTime", 180000 * S);
  check(game::set_system_time(3600 * S) && get<uint64_t>(ssd, "_SystemElapsedTime") == 3600 * S &&
            get<uint64_t>(cgc, "PrevTime") == 3600 * S,
        "the total set, and the campaign clock's last look with it");

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
