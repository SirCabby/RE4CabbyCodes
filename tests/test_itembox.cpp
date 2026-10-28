// Offline test of the item box's own part (src/itembox.cpp): the box of the game
// being played and its life, and the files beside the mod - the box from before
// it went into the saves, and one a save could not carry. Each phase is a process
// of its own, as each start of the game is:
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_itembox.cpp
//       src/itembox.cpp src/game.cpp src/re.cpp src/mem.cpp src/log.cpp -o tests/build/test_itembox.exe
//   wine tests/build/test_itembox.exe 'Z:\some\scratch\dir'      (runs every phase)
//
// - box: no box before the first menu or load (a put is refused); put: a stack's
//   counts add up on one entry, a weapon keeps an entry of its own with its
//   rounds, ammo, durability, condition and upgrades, an item with no state keeps
//   none whatever it was given; the panel's order; take, set_count, discard and
//   unput only on an entry still as the panel saw it; a load replaces the box (an
//   unknown item dropped), the menus empty it; a save that carried it writes
//   nothing, one that could not writes the game's unsaved file - once per box, not
//   once per save, and on top of what an earlier box left there.
// - files: the box from before (lines that make no sense skipped) and the unsaved
//   box are read together for Move into this game, a file that cannot be read
//   holds both back; moved in, both stay but are offered no more (never moved in
//   twice) until a save keeps the box - then they go, no copy left; a load or the
//   menus first (the box emptied by Take too): offered again; a failed save: the
//   unsaved file holds the box once and the other goes; Separate Ways' are its own.
// - held: the unsaved file held by another program a moment is written once it
//   lets go.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/itembox.h"

namespace {

using namespace re4cc;

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

std::string read_file(const std::string& path) {
  std::string text;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
  }
  return text;
}

void write_file(const std::string& path, const std::string& text) {
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Items the test uses, from the game's table.
constexpr int32_t kHandgunAmmo = 112800000;
constexpr int32_t kGreenHerb = 114400000;
constexpr int32_t kSG09R = 274835456;     // a gun
constexpr int32_t kCombatKnife = 276435456;  // a knife

itembox::Entry stack(int32_t id, int num) {
  itembox::Entry e;
  e.id = id;
  e.num = num;
  return e;
}

itembox::Entry gun(int ammo, int power, int capacity, bool exclusive) {
  itembox::Entry e;
  e.id = kSG09R;
  e.num = 1;
  e.weapon = true;
  e.ammo = ammo;
  e.ammo_id = kHandgunAmmo;
  e.durability = -1;
  e.condition = exclusive ? 2 : 0;
  e.lv[e.nlv++] = itembox::Level{0, 0, static_cast<int8_t>(power)};
  e.lv[e.nlv++] = itembox::Level{0, 2, static_cast<int8_t>(capacity)};
  e.lv[e.nlv++] = itembox::Level{2, 1, static_cast<int8_t>(exclusive ? 1 : 0)};
  return e;
}

itembox::Entry knife(int durability, bool broken) {
  itembox::Entry e;
  e.id = kCombatKnife;
  e.num = 1;
  e.weapon = true;
  e.durability = durability;
  e.condition = broken ? 1 : 0;
  e.lv[e.nlv++] = itembox::Level{0, 0, 1};
  e.lv[e.nlv++] = itembox::Level{1, 6, 3};
  return e;
}

int find(const std::vector<itembox::Entry>& box, const itembox::Entry& e) {
  for (size_t i = 0; i < box.size(); ++i)
    if (itembox::same(box[i], e)) return static_cast<int>(i);
  return -1;
}

int count_of(const std::vector<itembox::Entry>& box, int32_t id) {
  for (const itembox::Entry& e : box)
    if (e.id == id && !e.weapon) return e.num;
  return -1;
}

// Phase "box": the box of the game being played, from the first menu on.
int phase_box(const std::string& dir) {
  itembox::init(dir.c_str());
  itembox::View v = itembox::view();
  check(!v.live && v.entry.empty() && !itembox::live(), "before any menu or load: no box");
  check(!itembox::put(stack(kHandgunAmmo, 1)), "a put before the box is the game's is refused");
  itembox::clear();
  check(itembox::live() && itembox::view().entry.empty(), "the menus: an empty box of the game to come");
  check(itembox::put(stack(kHandgunAmmo, 50)), "put Handgun Ammo x50");
  check(itembox::put(stack(kHandgunAmmo, 30)), "put Handgun Ammo x30");
  check(itembox::put(gun(10, 2, 3, false)), "put an SG-09 R (10 rounds, upgraded)");
  check(itembox::put(gun(18, 4, 4, true)), "put another SG-09 R (18 rounds, the exclusive)");
  check(itembox::put(knife(700, false)), "put a Combat Knife (700 durability)");
  check(itembox::put(knife(0, true)), "put a broken Combat Knife");
  itembox::Entry herb = stack(kGreenHerb, 3);
  herb.weapon = true;  // nonsense for a herb: kept as a count
  herb.ammo = 99;
  herb.nlv = 1;
  check(itembox::put(herb), "put Green Herb x3 (given a weapon's state it has none of)");
  v = itembox::view();
  bool herb_plain = false;
  int guns = 0, knives = 0;
  for (const itembox::Entry& e : v.entry) {
    if (e.id == kGreenHerb) herb_plain = e.num == 3 && !e.weapon && e.nlv == 0 && e.ammo == 0;
    guns += e.id == kSG09R;
    knives += e.id == kCombatKnife;
  }
  check(count_of(v.entry, kHandgunAmmo) == 80, "the ammo is one entry of 80");
  check(herb_plain, "the herb is a count with no state");
  check(guns == 2 && knives == 2, "each weapon an entry of its own");
  // The panel's order: weapons before ammo before recovery; of one weapon, the most upgraded first.
  const int a = find(v.entry, gun(18, 4, 4, true)), b = find(v.entry, gun(10, 2, 3, false));
  int ammo_at = -1, herb_at = -1;
  for (size_t i = 0; i < v.entry.size(); ++i) {
    if (v.entry[i].id == kHandgunAmmo) ammo_at = static_cast<int>(i);
    if (v.entry[i].id == kGreenHerb) herb_at = static_cast<int>(i);
  }
  check(a >= 0 && b > a && ammo_at > b && herb_at > ammo_at, "the box keeps the panel's order");
  char text[160];
  if (a >= 0) {
    itembox::describe(v.entry[a], text, sizeof(text));
    check(std::strstr(text, "18 rounds") && std::strstr(text, "Power 5") && std::strstr(text, "exclusive"),
          "describe: \"18 rounds, Power 5, Capacity 5, exclusive upgrade\"");
  }
  // Take: a stale `seen` is refused; a real one takes what went into the game.
  itembox::Entry stale = v.entry[ammo_at];
  stale.num = 79;
  check(!itembox::take(ammo_at, stale, 50), "take with a stale count is refused");
  check(itembox::take(ammo_at, v.entry[ammo_at], 50), "take 50 of the 80 rounds");
  v = itembox::view();
  check(v.entry[ammo_at].num == 30, "30 left");
  check(itembox::set_count(ammo_at, v.entry[ammo_at], 999), "set the count to 999");
  const int w = find(v.entry, gun(10, 2, 3, false));
  check(!itembox::set_count(w, v.entry[w], 5), "a weapon has no count to set");
  check(itembox::take(w, v.entry[w], 1), "take the plain SG-09 R");
  v = itembox::view();
  check(find(v.entry, gun(10, 2, 3, false)) < 0, "it is gone from the box");
  // unput: a store the game refused takes the entry back out.
  check(itembox::put(stack(kGreenHerb, 2)), "put 2 more herbs");
  itembox::unput(stack(kGreenHerb, 2));
  check(count_of(itembox::view().entry, kGreenHerb) == 3, "unput takes the 2 back out (3 left)");
  v = itembox::view();
  const int k = find(v.entry, knife(0, true));
  check(itembox::discard(k, v.entry[k]), "throw the broken knife away");
  // What a save carries: the box as it is.
  const std::vector<itembox::Entry> carried = itembox::contents();
  check(carried.size() == 4 && count_of(carried, kHandgunAmmo) == 999 && find(carried, knife(700, false)) >= 0 &&
            find(carried, gun(18, 4, 4, true)) >= 0,
        "a save carries 4 entries: 999 rounds, a knife, the exclusive SG-09 R, the herbs");
  itembox::saved(true, itembox::File::kMain);
  check(!itembox::view().save_failed && !exists(dir + "RE4CabbyCodes.itembox.unsaved.ini"),
        "a save that carried it writes no file");
  // A load: that save's box in place of this one (an item the game has no such id for is dropped).
  std::vector<itembox::Entry> other = {stack(kGreenHerb, 2), stack(999, 1), knife(400, false)};
  itembox::loaded(other);
  v = itembox::view();
  check(v.live && v.entry.size() == 2 && count_of(v.entry, kGreenHerb) == 2 && find(v.entry, knife(400, false)) >= 0,
        "a load replaces the box with its own (2 entries; the unknown item dropped)");
  itembox::loaded(carried);
  check(itembox::view().entry.size() == 4, "loading the first save brings its 4 entries back");
  // A save that could not carry it: the unsaved file, once per box.
  itembox::saved(false, itembox::File::kMain);
  const std::string unsaved = dir + "RE4CabbyCodes.itembox.unsaved.ini";
  check(itembox::view().save_failed && exists(unsaved), "a save that could not carry it: the box goes to the file");
  check(read_file(unsaved).find("this file goes once a save keeps them.") != std::string::npos,
        "the file says it goes once a save keeps its items");
  check(itembox::file_entries(itembox::File::kMain) == 4, "the file holds the box's 4 entries");
  itembox::saved(false, itembox::File::kMain);
  check(itembox::file_entries(itembox::File::kMain) == 4 && read_file(unsaved).find("112800000:999") != std::string::npos,
        "a second failed save of the same box writes it once, not twice (999 rounds, not 1998)");
  itembox::loaded(other);  // another game's box, its save failing too: on top of the first
  itembox::saved(false, itembox::File::kMain);
  const std::vector<itembox::Entry> both = itembox::read_file(itembox::File::kMain);
  check(both.size() == 5 && count_of(both, kGreenHerb) == 5, "another box's failed save adds to the file (herbs 3 + 2)");
  check(!exists(dir + "RE4CabbyCodes.itembox.separate-ways.unsaved.ini"), "Separate Ways' file is untouched");
  // The menus: the box let go, the file kept.
  itembox::clear();
  v = itembox::view();
  check(v.live && v.entry.empty() && !v.save_failed, "the menus empty the box");
  check(itembox::file_entries(itembox::File::kMain) == 5, "the file waits for Move into this game");
  return 0;
}

// Phase "files": a box from before the saves, read for Move into this game, gone once a save keeps it.
int phase_files(const std::string& dir) {
  const std::string old = dir + "RE4CabbyCodes.itembox.ini";
  write_file(old,
             "# a comment\r\n"
             "112800000:40          ; Handgun Ammo\r\n"
             "112800000:2           ; more of it\r\n"
             "999:1                 ; no such item\r\n"
             "114400000:0           ; no count\r\n"
             "274835456:1:9:-1:-1:0:0.0.1/0.2.1   ; SG-09 R\r\n"
             "274835456:1:9:-1:-1:0:9.9.9         ; a level list that makes no sense\r\n"
             "114400000:1:5:-1:-1:0:-             ; a herb with a weapon's state\r\n"
             "\r\n");
  write_file(dir + "RE4CabbyCodes.itembox.separate-ways.ini", "114400000:1\r\n");
  itembox::init(dir.c_str());
  check(itembox::file_entries(itembox::File::kMain) == 2, "the sensible lines are counted: the ammo and the SG-09 R");
  std::vector<itembox::Entry> box = itembox::read_file(itembox::File::kMain);
  check(box.size() == 2 && count_of(box, kHandgunAmmo) == 42, "two lines of one item add up (42 rounds)");
  check(itembox::file_entries(itembox::File::kSeparateWays) == 1, "Separate Ways' file is its own");
  // A file that cannot be read holds every file of its game back.
  const std::string unsaved = dir + "RE4CabbyCodes.itembox.unsaved.ini";
  write_file(unsaved, "114400000:4\r\n");
  const HANDLE lock = CreateFileA(unsaved.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  check(lock != INVALID_HANDLE_VALUE && itembox::read_file(itembox::File::kMain).empty(),
        "one file of the game that cannot be read: nothing is taken from either");
  CloseHandle(lock);
  box = itembox::read_file(itembox::File::kMain);
  check(box.size() == 3 && count_of(box, kGreenHerb) == 4, "read together: the box from before and the unsaved box");
  // Moved in as the panel's Move into this game does it (each entry put in the box,
  // then moved_in): the files stay, but are offered no more and not read again.
  auto move_in = [] {
    for (const itembox::Entry& e : itembox::read_file(itembox::File::kMain)) itembox::put(e);
    itembox::moved_in(itembox::File::kMain);
  };
  itembox::loaded({});  // a game being played
  move_in();
  check(itembox::view().entry.size() == 3, "moved in: the box holds the files' 3 entries");
  check(exists(old) && exists(unsaved), "the files stay until a save keeps them");
  check(itembox::file_entries(itembox::File::kMain) == 0 && itembox::read_file(itembox::File::kMain).empty(),
        "and are offered no more: nothing is moved in twice");
  // A load before any save lets that box go: the files are offered again.
  itembox::loaded({});
  check(itembox::file_entries(itembox::File::kMain) == 3 && itembox::read_file(itembox::File::kMain).size() == 3,
        "a load before a save: the files are offered again");
  // The menus before a save, the box emptied by Take (its items in the case, unsaved): offered again too.
  move_in();
  for (itembox::View v = itembox::view(); !v.entry.empty(); v = itembox::view())
    itembox::take(0, v.entry[0], v.entry[0].num);
  itembox::clear();
  check(itembox::file_entries(itembox::File::kMain) == 3, "the menus before a save, the box emptied: offered again");
  // A save that could not carry the box: the unsaved file holds the box alone - the
  // files' items once, not on top of themselves - and the file from before goes.
  itembox::loaded({});
  move_in();
  itembox::saved(false, itembox::File::kMain);
  box = itembox::read_file(itembox::File::kMain);
  check(!exists(old) && exists(unsaved) && box.size() == 3 && count_of(box, kHandgunAmmo) == 42 &&
            count_of(box, kGreenHerb) == 4,
        "a failed save: the unsaved file holds the box once, the file from before is gone");
  // A save that keeps the box: the files go, and no copy is left.
  itembox::loaded({});
  move_in();
  itembox::saved(true, itembox::File::kMain);
  check(!exists(old) && !exists(unsaved), "a save that keeps the box: the files are gone");
  check(itembox::file_entries(itembox::File::kMain) == 0 && itembox::read_file(itembox::File::kMain).empty(),
        "nothing waits any more");
  check(!exists(old + ".moved") && !exists(unsaved + ".moved") && !exists(dir + "RE4CabbyCodes.itembox.backup.ini") &&
            !exists(dir + "RE4CabbyCodes.itembox.unsaved.backup.ini"),
        "no copy is left behind (.moved, .backup.ini)");
  check(itembox::file_entries(itembox::File::kSeparateWays) == 1, "Separate Ways' file still waits");
  return 0;
}

// Phase "held": the unsaved file held by another program (a virus scanner) a
// moment as a failed save writes it: written once it lets go.
struct Hold {
  std::string path;
  DWORD ms;
};
DWORD WINAPI hold(LPVOID p) {
  const auto* h = static_cast<const Hold*>(p);
  const HANDLE f = CreateFileA(h->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  Sleep(h->ms);
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  return 0;
}

int phase_held(const std::string& dir) {
  const std::string unsaved = dir + "RE4CabbyCodes.itembox.unsaved.ini";
  write_file(unsaved, "114400000:1\r\n");
  itembox::init(dir.c_str());
  itembox::loaded({stack(kHandgunAmmo, 7)});
  Hold h{unsaved, 250};
  const HANDLE t = CreateThread(nullptr, 0, hold, &h, 0, nullptr);
  Sleep(50);
  itembox::saved(false, itembox::File::kMain);
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
  const std::vector<itembox::Entry> box = itembox::read_file(itembox::File::kMain);
  check(box.size() == 2 && count_of(box, kHandgunAmmo) == 7 && count_of(box, kGreenHerb) == 1,
        "held for 250 ms: the file is written once it lets go, on top of what it held");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_itembox.exe <scratch dir> [phase]\n");
    return 2;
  }
  std::string dir = argv[1];
  if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
  if (argc < 3) {
    // Every phase in a process of its own, each in a fresh folder.
    // (No separator at the end: a backslash before the closing quote would escape it.)
    const char* const phases[] = {"box", "files", "held"};
    int failed = 0;
    for (const char* p : phases) {
      char sub[MAX_PATH];
      std::snprintf(sub, sizeof(sub), "%sre4cc_itembox_%lu_%s", dir.c_str(), GetCurrentProcessId(), p);
      CreateDirectoryA(sub, nullptr);
      char cmd[MAX_PATH * 2 + 64];
      std::snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" %s", argv[0], sub, p);
      std::printf("--- %s\n", p);
      std::fflush(stdout);
      STARTUPINFOA si{};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL  could not run phase %s\n", p);
        ++failed;
        continue;
      }
      WaitForSingleObject(pi.hProcess, INFINITE);
      DWORD code = 1;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      failed += code != 0;
    }
    std::printf("%s (%d phase(s) failed)\n", failed ? "FAILED" : "all passed", failed);
    return failed ? 1 : 0;
  }
  const std::string phase = argv[2];
  if (phase == "box") phase_box(dir);
  else if (phase == "files") phase_files(dir);
  else if (phase == "held") phase_held(dir);
  return g_fail ? 1 : 0;
}
