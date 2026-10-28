#include "itembox.h"

#include <windows.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "files.h"
#include "game.h"
#include "items.h"
#include "log.h"

namespace re4cc::itembox {
namespace {

CRITICAL_SECTION g_lock;  // the box, the files' counts
bool g_ready = false;

// The box of the game being played.
std::vector<Entry> g_box;
bool g_live = false;         // emptied at a menu or loaded since the mod started
bool g_save_failed = false;  // the last save could not carry it
int g_generation = 0;        // a new box at each clear and load (the unsaved file's merge)

struct Lock {
  Lock() { EnterCriticalSection(&g_lock); }
  ~Lock() { LeaveCriticalSection(&g_lock); }
};

// The files beside the mod, per game: the box from before it went into the saves,
// and the box a save could not carry.
struct Files {
  const char* game;      // whose, for the files' own note
  const char* name;      // the box from before
  const char* unsaved;   // what a save could not carry
  char path[MAX_PATH] = {}, unsaved_path[MAX_PATH] = {};
  std::atomic<int> entries{0};  // both files' entries, for the panel
  // The unsaved file as it was before this box's first failed save: a later
  // failure of the same box rewrites it as that plus the box, not plus it again.
  int base_generation = -1;
  std::vector<Entry> base{};
  // Moved into the box of this generation (the panel's Move into this game): the
  // files stay, offered no more, until a save keeps that box - then they go. A
  // load or the menus let the box go first, and they are offered again.
  int moved_generation = -1;
  bool saved_away = false;  // a save kept them, but a file could not be removed
};
Files g_files[2] = {{"the main game", "RE4CabbyCodes.itembox.ini", "RE4CabbyCodes.itembox.unsaved.ini"},
                    {"Separate Ways", "RE4CabbyCodes.itembox.separate-ways.ini",
                     "RE4CabbyCodes.itembox.separate-ways.unsaved.ini"}};
Files& files(File f) { return g_files[f == File::kSeparateWays ? 1 : 0]; }

// The panel's order (the Add list's), and for one item: its stack first, then
// its weapons, the most upgraded first, then the fullest.
int rank_of(int32_t id) {
  const items::Def* d = items::find(id);
  return d ? d->rank : items::kCount;
}
bool before(const Entry& a, const Entry& b) {
  if (a.id != b.id) {
    const int ra = rank_of(a.id), rb = rank_of(b.id);
    return ra != rb ? ra < rb : a.id < b.id;
  }
  if (a.weapon != b.weapon) return !a.weapon;
  if (upgrades(a) != upgrades(b)) return upgrades(a) > upgrades(b);
  if (a.ammo != b.ammo) return a.ammo > b.ammo;
  return a.durability > b.durability;
}

// `e` into a box: onto its item's entry, or (a weapon) into a place of its own.
void insert(std::vector<Entry>* box, const Entry& e) {
  if (!e.weapon)
    for (Entry& b : *box)
      if (!b.weapon && b.id == e.id) {
        const long long sum = static_cast<long long>(b.num) + e.num;
        b.num = static_cast<int>(sum > kMaxCount ? kMaxCount : sum);
        return;
      }
  box->insert(std::upper_bound(box->begin(), box->end(), e, before), e);
}

int total(const std::vector<Entry>& box) {
  long long n = 0;
  for (const Entry& e : box) n += e.num;
  return static_cast<int>(n > kMaxCount ? kMaxCount : n);
}

// --- the files beside the mod ---------------------------------------------------------------------------
// One line per entry:  <item id>:<count>  or, for a weapon,
// <item id>:1:<rounds>:<ammo id>:<durability>:<condition>:<levels>, each followed
// by what it is after ";" (for reading only).
void levels_text(const Entry& e, char* buf, size_t n) {
  if (!e.nlv) {
    std::snprintf(buf, n, "-");
    return;
  }
  size_t used = 0;
  buf[0] = 0;
  for (int i = 0; i < e.nlv && used + 16 < n; ++i)
    used += static_cast<size_t>(std::snprintf(buf + used, n - used, "%s%d.%d.%d", i ? "/" : "", e.lv[i].list,
                                              e.lv[i].category, e.lv[i].level));
}

bool write_file(const char* path, const char* game, const std::vector<Entry>& box) {
  char tmp[MAX_PATH + 8];
  std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE* f = std::fopen(tmp, "w");
  if (!f) {
    logf("ERROR: itembox: cannot write %s", tmp);
    return false;
  }
  std::fprintf(f,
               "# RE4CabbyCodes - an item box of %s that no save carries\n"
               "#\n"
               "# The item box is kept in the game's saves. A save could not carry this one (the mod's\n"
               "# calls into the game were off - see RE4CabbyCodes.log), so it is kept here instead.\n"
               "# Load a game of %s and press Move into this game in the panel's Item box: its\n"
               "# items join that game's box, and this file goes once a save keeps them.\n"
               "#\n"
               "#   <item id>:<count>     everything of one item, on one line\n"
               "#   <item id>:1:<rounds>:<ammo id>:<durability>:<condition>:<upgrades>\n"
               "#                         a weapon, a line of its own\n"
               "#\n"
               "# <rounds> is what a gun has loaded, <ammo id> the ammo it is loaded with (-1 its\n"
               "# own), <durability> a knife's (-1 none), <condition> 1 for a broken knife, 2 for a\n"
               "# weapon with its exclusive upgrade (3 both). <upgrades> are its levels as the game keeps\n"
               "# them, list.category.level (level 0 is the merchant's level 1), joined by \"/\", or\n"
               "# \"-\" for none. The ids are the game's own; what follows \";\" on a line is only\n"
               "# what it is.\n",
               game, game);
  for (const Entry& e : box) {
    char data[128], note[200];
    const char* name = items::name(e.id);
    if (e.weapon) {
      char lv[96];
      levels_text(e, lv, sizeof(lv));
      std::snprintf(data, sizeof(data), "%d:1:%d:%d:%d:%d:%s", e.id, e.ammo, e.ammo_id, e.durability, e.condition, lv);
      char state[160];
      describe(e, state, sizeof(state));
      std::snprintf(note, sizeof(note), "%s - %s", name[0] ? name : "(unnamed)", state);
    } else {
      std::snprintf(data, sizeof(data), "%d:%d", e.id, e.num);
      std::snprintf(note, sizeof(note), "%s", name[0] ? name : "(unnamed)");
    }
    std::fprintf(f, "%-30s ; %s\n", data, note);
  }
  // Onto the disk before the rename: fclose only hands the bytes to the OS
  // cache, so an unclean shutdown could land the rename with nothing behind it.
  bool ok = std::fflush(f) == 0 && !std::ferror(f);
  const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
  if (ok && h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
  ok = std::fclose(f) == 0 && ok;
  // Replaced in one step: a crash halfway leaves the old file, not half of a new one.
  if (!ok || !files::replace(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    logf("ERROR: itembox: cannot replace %s (error %lu)", path, GetLastError());
    DeleteFileA(tmp);
    return false;
  }
  logf("itembox: %s written - %d entr%s, %d item(s)", path, static_cast<int>(box.size()), box.size() == 1 ? "y" : "ies",
       total(box));
  return true;
}

// A number of a line, up to the next ':' (or the end): false when there is none.
bool number(const char** s, long long* v) {
  char* end = nullptr;
  *v = std::strtoll(*s, &end, 10);
  if (end == *s) return false;
  *s = end;
  return true;
}

bool parse_levels(const char* s, Entry* e) {
  if (*s == '-') return s[1] == 0;
  while (*s) {
    long long v[3];
    for (int k = 0; k < 3; ++k) {
      if (!number(&s, &v[k])) return false;
      if (k < 2 && *s++ != '.') return false;
    }
    if (v[0] < 0 || v[0] > 2 || v[1] < 0 || v[1] > 63 || v[2] < 0 || v[2] > 9 || e->nlv >= kMaxLevels) return false;
    e->lv[e->nlv++] = Level{static_cast<int8_t>(v[0]), static_cast<int8_t>(v[1]), static_cast<int8_t>(v[2])};
    if (*s == '/') ++s;
    else if (*s) return false;
  }
  return true;
}

// One line of a file into `out`: false for a line that is no entry (a comment,
// a blank) through `blank`, or one that makes no sense.
bool parse_line(std::string line, Entry* out, bool* blank) {
  *blank = false;
  const size_t cut = line.find_first_of(";#");
  if (cut != std::string::npos) line.resize(cut);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
  const char* s = line.c_str();
  while (*s == ' ' || *s == '\t') ++s;
  if (!*s || *s == '[') {
    *blank = true;
    return false;
  }
  Entry e;
  long long id = 0, num = 0;
  if (!number(&s, &id) || *s++ != ':' || !number(&s, &num)) return false;
  if (!items::find(static_cast<int32_t>(id)) || num < 1 || num > kMaxCount) return false;
  e.id = static_cast<int32_t>(id);
  e.num = static_cast<int>(num);
  if (*s == ':') {
    ++s;
    long long v[4];
    for (int k = 0; k < 4; ++k) {
      if (!number(&s, &v[k]) || *s++ != ':') return false;
    }
    if (v[0] < 0 || v[0] > 99999 || v[1] < -1 || v[2] < -1 || v[2] > 99999 || v[3] < 0 || v[3] > 7) return false;
    e.weapon = true;
    e.ammo = static_cast<int>(v[0]);
    e.ammo_id = static_cast<int32_t>(v[1]);
    e.durability = static_cast<int>(v[2]);
    e.condition = static_cast<int>(v[3]);
    if (!parse_levels(s, &e)) return false;
  } else if (*s) {
    return false;
  }
  // A weapon's line for an item with no state, or a count's line for a weapon:
  // what it means is clear enough, but it is not what the box writes.
  if (e.weapon != items::keeps_state(e.id)) return false;
  *out = e;
  return true;
}

// A file's entries (none when it is not there; `readable` false when it is there
// but could not be read to the end - nothing is taken from it then).
std::vector<Entry> parse_file(const char* path, bool* readable) {
  std::vector<Entry> box;
  *readable = true;
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return box;
  FILE* f = std::fopen(path, "r");
  if (!f) {
    *readable = false;
    logf("ERROR: itembox: %s is there but could not be read", path);
    return box;
  }
  int lines = 0;
  std::string line;
  for (int c = std::fgetc(f);; c = std::fgetc(f)) {
    if (c != '\n' && c != EOF) {
      line += static_cast<char>(c);
      continue;
    }
    ++lines;
    Entry e;
    bool blank = false;
    if (parse_line(line, &e, &blank)) insert(&box, kept(e));
    else if (!blank) logf("itembox: line %d of %s makes no sense - skipped: %s", lines, path, line.c_str());
    line.clear();
    if (c == EOF) break;
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    *readable = false;
    logf("ERROR: itembox: %s could not be read to the end - it is left alone", path);
    box.clear();
  }
  return box;
}

// The files' entries for the panel: none while their items are in a box already.
void count_files(Files& fs) {
  if (fs.moved_generation >= 0 || fs.saved_away) {
    fs.entries.store(0);
    return;
  }
  bool ok = true;
  const int n = static_cast<int>(parse_file(fs.path, &ok).size() + parse_file(fs.unsaved_path, &ok).size());
  fs.entries.store(n);
}

// A save keeps what was moved in from the files: they go. One that cannot be
// removed (held by another program) is offered no more, and tried again at the
// next save.
void finish_move(Files& fs) {
  fs.moved_generation = -1;
  fs.saved_away = false;
  for (const char* path : {fs.path, fs.unsaved_path}) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
    DWORD err = 0;
    if (files::remove(path, &err)) {
      logf("itembox: %s removed - a save keeps its items now", path);
    } else {
      logf("ERROR: itembox: %s could not be removed (error %lu) - a save keeps its items, so it is not offered again",
           path, err);
      fs.saved_away = true;
    }
  }
  count_files(fs);
}

// The box the files were moved into is let go (a load, the menus) before a save
// kept it: they are offered again.
void let_go_moved() {
  for (Files& fs : g_files) {
    if (fs.moved_generation < 0) continue;
    fs.moved_generation = -1;
    count_files(fs);
    logf("itembox: the box %s's files were moved into was let go before a save kept it - they are offered again",
         fs.game);
  }
}

bool valid(const std::vector<Entry>& box, int index, const Entry& seen) {
  return index >= 0 && index < static_cast<int>(box.size()) && same(box[index], seen);
}

}  // namespace

bool same(const Entry& a, const Entry& b) {
  if (a.id != b.id || a.num != b.num || a.weapon != b.weapon || a.ammo != b.ammo || a.ammo_id != b.ammo_id ||
      a.durability != b.durability || a.condition != b.condition || a.nlv != b.nlv)
    return false;
  for (int i = 0; i < a.nlv; ++i)
    if (a.lv[i].list != b.lv[i].list || a.lv[i].category != b.lv[i].category || a.lv[i].level != b.lv[i].level)
      return false;
  return true;
}

int upgrades(const Entry& e) {
  int n = 0;
  for (int i = 0; i < e.nlv; ++i) n += e.lv[i].level;
  return n;
}

Entry kept(Entry e) {
  if (!items::keeps_state(e.id)) {
    e.weapon = false;
    e.ammo = 0;
    e.ammo_id = -1;
    e.durability = -1;
    e.condition = 0;
    e.nlv = 0;
  } else {
    e.weapon = true;
    e.num = 1;
  }
  return e;
}

void describe(const Entry& e, char* buf, int n) {
  int used = 0;
  auto add = [&](const char* fmt, auto... a) {
    if (used < n) used += std::snprintf(buf + used, static_cast<size_t>(n - used), fmt, a...);
  };
  buf[0] = 0;
  const items::Def* d = items::find(e.id);
  if (d && d->type == items::kGun) add("%d round%s", e.ammo, e.ammo == 1 ? "" : "s");
  if (e.durability >= 0) add("%sdurability %d", used ? ", " : "", e.durability);
  if (e.condition & game::kConditionBroken) add("%sbroken", used ? ", " : "");
  for (int i = 0; i < e.nlv; ++i) {
    const char* what = game::level_name(e.lv[i].list, e.lv[i].category);
    if (!what) continue;
    if (e.lv[i].list == 2) {
      if (e.lv[i].level > 0) add("%sexclusive upgrade", used ? ", " : "");
    } else {
      add("%s%s %d", used ? ", " : "", what, e.lv[i].level + 1);
    }
  }
  if (!used) add("as found");
}

void init(const char* dir) {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  for (Files& fs : g_files) {
    std::snprintf(fs.path, sizeof(fs.path), "%s%s", dir, fs.name);
    std::snprintf(fs.unsaved_path, sizeof(fs.unsaved_path), "%s%s", dir, fs.unsaved);
    count_files(fs);
    if (const int n = fs.entries.load())
      logf("itembox: %d entr%s of %s's box wait beside the mod (%s, %s) for the panel's Move into this game", n,
           n == 1 ? "y" : "ies", fs.game, fs.name, fs.unsaved);
  }
  g_ready = true;
}

View view() {
  View v;
  if (!g_ready) return v;
  Lock lock;
  v.live = g_live;
  v.save_failed = g_save_failed;
  v.entry = g_box;
  return v;
}

bool live() {
  if (!g_ready) return false;
  Lock lock;
  return g_live;
}

void clear() {
  if (!g_ready) return;
  Lock lock;
  let_go_moved();  // even with the box empty: what was moved in may be in the case, unsaved
  if (g_live && g_box.empty()) return;
  if (!g_box.empty())
    logf("itembox: back at the menus - the item box of the game left is let go (%d entr%s; each save keeps the box it "
         "was saved with)",
         static_cast<int>(g_box.size()), g_box.size() == 1 ? "y" : "ies");
  g_box.clear();
  g_live = true;
  g_save_failed = false;
  ++g_generation;
}

void loaded(const std::vector<Entry>& entries) {
  if (!g_ready) return;
  std::vector<Entry> box;
  for (const Entry& e : entries)
    if (items::find(e.id) && e.num >= 1) insert(&box, kept(e));
  Lock lock;
  let_go_moved();
  g_box.swap(box);
  g_live = true;
  g_save_failed = false;
  ++g_generation;
  logf("itembox: a save was loaded - its item box holds %d entr%s, %d item(s)", static_cast<int>(g_box.size()),
       g_box.size() == 1 ? "y" : "ies", total(g_box));
}

std::vector<Entry> contents() {
  if (!g_ready) return {};
  Lock lock;
  return g_box;
}

void saved(bool carried, File game) {
  if (!g_ready) return;
  // (Rare: only while the mod's calls are off. The file is written under the
  // lock, so a tick's Move into this game cannot meet it halfway.)
  Lock lock;
  Files& fs = files(game);
  // The files' items are in this box (or a save keeps them already): never folded in again.
  const bool moved = fs.moved_generation >= 0 || fs.saved_away;
  if (carried && moved) finish_move(fs);
  g_save_failed = !carried && !g_box.empty();
  if (!g_save_failed) return;
  // Not carried: the box into the game's unsaved file, on top of what that file
  // held before this box first came there - or alone, when the files were moved
  // into it (their items are in it once).
  if (moved) {
    fs.base.clear();
    fs.base_generation = g_generation;
  }
  if (fs.base_generation != g_generation) {
    bool ok = true;
    fs.base = parse_file(fs.unsaved_path, &ok);
    if (!ok) {
      logf("ERROR: itembox: the item box (%d entries) could not be kept anywhere - %s is unreadable",
           static_cast<int>(g_box.size()), fs.unsaved_path);
      return;
    }
    fs.base_generation = g_generation;
  }
  std::vector<Entry> out = fs.base;
  for (const Entry& e : g_box) insert(&out, e);
  if (!write_file(fs.unsaved_path, fs.game, out)) return;
  if (moved) {
    // The box from before is in the unsaved file now, with the rest of the box.
    fs.moved_generation = -1;
    fs.saved_away = false;
    DWORD err = 0;
    if (!files::remove(fs.path, &err))
      logf("ERROR: itembox: %s could not be removed (error %lu) - its items are in %s now; delete it by hand", fs.path,
           err, fs.unsaved_path);
  }
  count_files(fs);
}

bool put(const Entry& e) {
  if (!g_ready || !items::find(e.id) || e.num < 1) return false;
  Lock lock;
  if (!g_live) return false;
  insert(&g_box, kept(e));
  return true;
}

void unput(const Entry& given) {
  if (!g_ready) return;
  const Entry e = kept(given);  // as put() kept it
  Lock lock;
  for (size_t i = 0; i < g_box.size(); ++i) {
    Entry& x = g_box[i];
    if (x.id != e.id || x.weapon != e.weapon) continue;
    if (e.weapon && !same(x, e)) continue;
    if (!e.weapon && x.num > e.num) x.num -= e.num;
    else g_box.erase(g_box.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

bool take(int index, const Entry& seen, int num) {
  if (!g_ready) return false;
  Lock lock;
  if (!valid(g_box, index, seen)) {
    logf("ERROR: itembox: entry %d is not %s x%d any more - nothing taken out of the box", index, items::name(seen.id),
         seen.num);
    return false;
  }
  if (g_box[index].weapon || num >= g_box[index].num) g_box.erase(g_box.begin() + index);
  else g_box[index].num -= num;
  return true;
}

bool set_count(int index, const Entry& seen, int num) {
  if (!g_ready || num < 1 || num > kMaxCount) return false;
  Lock lock;
  if (!valid(g_box, index, seen) || g_box[index].weapon) return false;
  g_box[index].num = num;
  return true;
}

bool discard(int index, const Entry& seen) {
  if (!g_ready) return false;
  Lock lock;
  if (!valid(g_box, index, seen)) return false;
  g_box.erase(g_box.begin() + index);
  return true;
}

const char* file_name(File f) { return files(f).name; }

int file_entries(File f) { return g_ready ? files(f).entries.load() : 0; }

std::vector<Entry> read_file(File f) {
  std::vector<Entry> box;
  if (!g_ready) return box;
  Lock lock;
  Files& fs = files(f);
  if (fs.moved_generation >= 0 || fs.saved_away) return box;  // in a box already: not twice
  bool ok = true;
  for (const char* path : {fs.path, fs.unsaved_path}) {
    bool readable = true;
    for (const Entry& e : parse_file(path, &readable)) insert(&box, e);
    ok = ok && readable;
  }
  if (!ok) box.clear();  // one of them could not be read: neither is taken
  return box;
}

void moved_in(File f) {
  if (!g_ready) return;
  Lock lock;
  Files& fs = files(f);
  fs.moved_generation = g_generation;
  fs.base_generation = -1;
  fs.base.clear();
  count_files(fs);
  logf("itembox: %s's files are in the item box now - they stay, offered no more, until a save keeps it, then go",
       fs.game);
}

}  // namespace re4cc::itembox
