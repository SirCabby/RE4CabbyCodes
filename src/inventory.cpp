#include "inventory.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "call.h"
#include "events.h"
#include "items.h"
#include "log.h"
#include "stamp.h"

namespace re4cc::inventory {
namespace {

CRITICAL_SECTION g_cs;  // g_view, g_queue
bool g_ready = false;
View* g_view = nullptr;  // what the panel copies (on the heap: it is large)
std::atomic<unsigned> g_generation{0};  // bumped as g_view is replaced (under the lock)
std::atomic<DWORD> g_drawn_at{0};       // the panel last drew the lists (a stamp.h moment)

enum Op {
  kOpCount, kOpWeapon, kOpRemove, kOpAdd, kOpSize, kOpStore, kOpTake, kOpTakeStored, kOpBoxCount, kOpStoredCount,
  kOpDiscard, kOpDiscardStored, kOpImport
};
struct Request {
  Op op = kOpCount;
  Row seen;              // count, weapon, remove, store, the game's storage's
  int a = -1, b = -1;    // count / rounds and durability / item and count / size / box count / stack
  bool has_levels = false;
  int levels[16] = {};
  int index = -1;        // the box's entry
  itembox::Entry entry;  // ...as the panel showed it
  // The batch it is one of (0 none): its number, how many requests it has, and
  // the word its note has for one done.
  uint32_t batch = 0;
  int batch_size = 0;
  const char* batch_done = "";
};
std::vector<Request> g_queue;  // the panel's requests, oldest first
// The batch the panel is making (its thread alone): queued whole by end_batch.
bool g_making = false;
std::vector<Request> g_making_list;
uint32_t g_batches = 0;

// The tick works through the queue a few requests a frame: no more than these,
// and no more once this long has gone on them.
constexpr int kPerTick = 12;
constexpr int kTickBudgetUs = 4000;

// The last change's note, kept for the view (the tick's alone).
char g_note[400] = {};
bool g_note_error = false;

void note(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(bool error, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(g_note, sizeof(g_note), fmt, a);
  va_end(a);
  g_note_error = error;
  logf("inventory: %s", g_note);
}

const char* name_of(int32_t id) {
  const char* n = items::name(id);
  return n[0] ? n : "that item";
}

void push(const Request& r) {
  if (!g_ready) return;
  if (g_making) {
    g_making_list.push_back(r);
    return;
  }
  EnterCriticalSection(&g_cs);
  g_queue.push_back(r);
  LeaveCriticalSection(&g_cs);
}

// A batch the tick is working through: how far it has got, and why the ones
// that came to nothing did (each reason once, the first few).
struct Progress {
  uint32_t batch = 0;
  int size = 0, done = 0, failed = 0;
  const char* done_word = "";
  char why[3][200] = {};
  int whys = 0, more = 0;
};
Progress g_progress;

void batch_step(const Request& r, bool failed) {
  Progress& p = g_progress;
  if (r.batch != p.batch) {
    p = Progress{};
    p.batch = r.batch;
    p.size = r.batch_size;
    p.done_word = r.batch_done;
  }
  if (!failed) {
    ++p.done;
  } else {
    ++p.failed;
    bool known = false;
    for (int i = 0; i < p.whys && !known; ++i) known = std::strcmp(p.why[i], g_note) == 0;
    if (!known && p.whys < 3) std::snprintf(p.why[p.whys++], sizeof(p.why[0]), "%s", g_note);
    else if (!known) ++p.more;
  }
  if (p.done + p.failed < p.size) return;
  // The last of it: one note for the lot.
  if (!p.failed) {
    note(false, "%d of %d %s", p.done, p.size, p.done_word);
  } else {
    char whys[400];
    size_t k = 0;
    whys[0] = '\0';
    for (int i = 0; i < p.whys && k + 1 < sizeof(whys); ++i)
      k += static_cast<size_t>(std::snprintf(whys + k, sizeof(whys) - k, "%s%s", i ? "; " : "", p.why[i]));
    if (p.more && k + 1 < sizeof(whys))
      std::snprintf(whys + k, sizeof(whys) - k, "; and %d more (see the log)", p.more);
    note(true, "%d of %d %s - not %s: %s", p.done, p.size, p.done_word, p.done_word, whys);
  }
  p = Progress{};
}

// --- the most each upgrade goes to (WeaponCustomManager.getMaxCustomLevel), asked once -----------------
struct MaxLevel {
  int16_t weapon;
  int8_t list, category;
  int8_t max;
};
MaxLevel g_max[256];
int g_nmax = 0;
int max_level(int weapon, int list, int category) {
  for (int i = 0; i < g_nmax; ++i)
    if (g_max[i].weapon == weapon && g_max[i].list == list && g_max[i].category == category) return g_max[i].max;
  if (!call::levels_ready()) return -1;
  const int v = call::max_level(list, category, weapon);
  if (g_nmax < 256) g_max[g_nmax++] = MaxLevel{static_cast<int16_t>(weapon), static_cast<int8_t>(list),
                                               static_cast<int8_t>(category), static_cast<int8_t>(v < 0 || v > 9 ? -1 : v)};
  return v < 0 || v > 9 ? -1 : v;
}

// A row for an item of the game's.
void make_row(const game::ItemView& v, int kind, Row* r) {
  *r = Row{};
  r->v = v;
  r->kind = kind;
  const items::Def* d = items::find(v.id);
  r->stack = d && d->stack > 0 ? d->stack : 1;
  r->weapon = items::keeps_state(v.id);
  r->gun = d && d->type == items::kGun;
  r->knife = d && d->type == items::kMelee;
  r->durability_max = r->knife ? d->durability : 0;
  if (r->weapon && (kind == kCase || kind == kStorage) && game::weapon_levels(v.item, &r->levels))
    for (int i = 0; i < r->levels.n; ++i)
      r->max_level[i] = kind == kCase && d ? max_level(d->weapon, r->levels.e[i].list, r->levels.e[i].category) : -1;
}

// Whether a change can be made now, and if not why.
bool allowed(const game::Run& run, char* why, size_t n) {
  const char* w = nullptr;
  if (!game::parts().inventory) w = "the attache case was not found in the game (see the log)";
  else if (!run.in_game) w = "no game is being played";
  else if (!run.paused) w = "items change only while the pause menu is up";
  else if (run.case_scene) w = "close the attache case's own screen first";
  else if (!call::items_ready()) w = call::ready() ? "the game's item functions were not found (see the log)" : call::status();
  if (why) std::snprintf(why, n, "%s", w ? w : "");
  return !w;
}

// The item the panel showed, as the game has it now - the same Guid and item.
bool current(const game::Inventories& inv, const Row& seen, game::ItemView* now) {
  static game::ItemView buf[kMaxRows];
  const int n = game::read_items(inv, seen.kind, buf, kMaxRows);
  for (int i = 0; i < n; ++i)
    if (game::guid_equal(buf[i].guid, seen.v.guid) && buf[i].id == seen.v.id) {
      *now = buf[i];
      return true;
    }
  return false;
}

uintptr_t list_owner(const game::Inventories& inv, int kind, bool for_remove) {
  switch (kind) {
    case kKeyItems: return inv.key;
    case kTreasures: return inv.treasure;
    case kUnique: return for_remove ? inv.unique_inventory : inv.unique;
    default: return inv.cs;
  }
}

// A weapon's upgrades set as `want` asks (one level per entry of `seen`, matched
// by list and category), each within the most it goes to.
bool set_levels(uintptr_t item, int32_t id, const game::Levels& seen, const int* want, const int* max) {
  const items::Def* d = items::find(id);
  const uintptr_t custom = game::weapon_custom_levels(item);
  game::Levels now;
  if (!d || !custom || !game::weapon_levels(item, &now)) return false;
  bool changed = false, exclusive = false, exclusive_set = false;
  for (int j = 0; j < now.n; ++j) {
    for (int i = 0; i < seen.n; ++i) {
      if (seen.e[i].list != now.e[j].list || seen.e[i].category != now.e[j].category) continue;
      int lv = want[i] < 0 ? 0 : want[i];
      if (max && max[i] >= 0 && lv > max[i]) lv = max[i];
      if (now.e[j].list == 2) {
        exclusive_set = true;
        exclusive = lv > 0;
      }
      if (lv != now.e[j].level && call::set_level(now.e[j].list, now.e[j].category, lv, custom, d->weapon)) changed = true;
      break;
    }
  }
  // The exclusive upgrade is also a mark on the item, as the merchant's sets it.
  const int cond = exclusive_set ? game::condition_of(item) : -1;
  if (cond >= 0) {
    const int want_cond = exclusive ? (cond | game::kConditionLimitBreak) : (cond & ~game::kConditionLimitBreak);
    if (want_cond != cond) changed |= game::set_condition(item, want_cond);
  }
  return changed;
}

// --- the item box: its entries and the game's items ----------------------------------------------------------
// What the box keeps of an item of the game's: everything of a stack, or a
// weapon with its state.
itembox::Entry entry_of(const game::ItemView& v) {
  itembox::Entry e;
  e.id = v.id;
  e.num = v.count;
  e.weapon = items::keeps_state(v.id);
  if (e.weapon) {
    const items::Def* d = items::find(v.id);
    e.num = 1;
    e.ammo = v.ammo > 0 ? v.ammo : 0;
    e.ammo_id = v.ammo_id;
    e.durability = d && d->type == items::kMelee ? v.durability : -1;
    e.condition = v.condition & (game::kConditionBroken | game::kConditionLimitBreak);
    game::Levels lv;
    if (game::weapon_levels(v.item, &lv))
      for (int i = 0; i < lv.n && e.nlv < itembox::kMaxLevels; ++i)
        e.lv[e.nlv++] = itembox::Level{static_cast<int8_t>(lv.e[i].list), static_cast<int8_t>(lv.e[i].category),
                                       static_cast<int8_t>(lv.e[i].level)};
  }
  return e;
}

// A new item, `count` of the entry's (a weapon: itself, with its state): the
// game's factory, then a weapon's upgrades through the merchant's setters, its
// condition, its rounds and its durability. Inside a frame (the tick's
// call::Frame, a hook's InHook), which holds it until something else does; 0
// when the game made none.
uintptr_t make_item(const itembox::Entry& e, int count) {
  const items::Def* d = items::find(e.id);
  if (!d || count < 1) return 0;
  const bool gun = d->type == items::kGun;
  const uintptr_t item =
      call::generate_item(e.id, e.weapon ? 1 : count, -1, e.weapon && gun ? e.ammo_id : -1, e.weapon && gun ? e.ammo : 0);
  if (!item || !e.weapon) return item;
  if (e.nlv && call::levels_ready()) {
    game::Levels seen;
    int want[16] = {};
    for (int i = 0; i < e.nlv && i < 16; ++i) {
      seen.e[i].list = e.lv[i].list;
      seen.e[i].category = e.lv[i].category;
      want[i] = e.lv[i].level;
      seen.n = i + 1;
    }
    set_levels(item, e.id, seen, want, nullptr);
  }
  if (e.condition) game::set_condition(item, e.condition);
  if (gun) {
    game::set_loaded(item, e.ammo);
    call::weapon_customized(item);
  }
  if (e.durability >= 0) game::set_durability(item, e.durability);
  return item;
}

// --- the game's storage ---
bool storage_usable() { return game::parts().storage && call::storage_ready(); }

// Its entry of an item (it keeps one per item), and how many it holds of it.
bool stored_entry(int32_t id, game::ItemView* out) {
  static game::ItemView buf[kMaxRows];
  const int n = game::read_items(game::Inventories{}, game::kStorageKind, buf, kMaxRows);
  for (int i = 0; i < n; ++i)
    if (buf[i].id == id) {
      *out = buf[i];
      return true;
    }
  return false;
}
int stored_total(int32_t id) {
  static game::ItemView buf[kMaxRows];
  const int n = game::read_items(game::Inventories{}, game::kStorageKind, buf, kMaxRows);
  int sum = 0;
  for (int i = 0; i < n; ++i)
    if (buf[i].id == id) sum += buf[i].stored > 0 ? buf[i].stored : 1;
  return sum;
}

// `item` (of `id`) into the game's storage through its own store, when its own
// rule takes it and it has room - one entry per item, a stack (the First Aid
// Spray) up to its most: its store drops anything else without a word. 1 it went
// in, 0 the storage does not take it, -1 a call failed.
int storage_put(uintptr_t item, int32_t id, int count) {
  const int rule = call::storage_accepts(item);
  if (rule != 1) return rule;
  game::ItemView s;
  if (stored_entry(id, &s) && !(s.stored_max > 1 && s.stored + count <= s.stored_max)) return 0;
  const int before = stored_total(id);
  if (!call::storage_add(game::armoury_manager(), item)) return -1;
  return stored_total(id) > before ? 1 : 0;
}

// --- the mod's part ---
bool box_usable(char* why, size_t n) {
  const char* w = nullptr;
  if (!itembox::live()) w = "the item box is not this game's yet (go back to the title and load again)";
  else if (!events::box_hooked())
    w = events::box_hook_failed() ? "the mod cannot reach the game's saves (see the log)" : "waiting for the game's saves";
  else if (!call::box_save_ready())
    w = call::ready() ? "the game's functions it needs were not found (see the log)" : call::status();
  if (why) std::snprintf(why, n, "%s", w ? w : "");
  return !w;
}

// The box's items in a save carry a Guid of their own: these eight bytes, then
// the entry's number and the save's - so a load tells them from an old save's
// own items (the game kept its storage in this list before an update).
constexpr uint8_t kBoxMark[8] = {'R', 'E', '4', 'C', 'C', 'B', 'O', 'X'};
bool box_marked(const game::ItemView& v) { return std::memcmp(v.guid, kBoxMark, 8) == 0; }
bool mark(uintptr_t item, uint32_t entry, uint32_t save) {
  uint8_t g[16];
  std::memcpy(g, kBoxMark, 8);
  std::memcpy(g + 8, &entry, 4);
  std::memcpy(g + 12, &save, 4);
  return game::set_item_guid(item, g);
}

// --- the changes ---------------------------------------------------------------------------------------------
void do_count(const game::Inventories& inv, const Request& r) {
  game::ItemView now;
  if (!current(inv, r.seen, &now) || now.count != r.seen.v.count)
    return note(true, "%s changed before the count could be set - nothing was done", name_of(r.seen.v.id));
  const int n = r.a < 1 ? 1 : r.a > r.seen.stack ? r.seen.stack : r.a;
  if (n == now.count) return;
  const bool ok = r.seen.kind == kCase ? call::case_change_count(inv.cs_inventory, now.guid, n)
                                       : game::set_item_count(now.item, n);
  if (ok) note(false, "%s: %d -> %d", name_of(now.id), now.count, n);
  else note(true, "the game did not change the count of %s", name_of(now.id));
}

void do_weapon(const game::Inventories& inv, const Request& r) {
  game::ItemView now;
  if (r.seen.kind != kCase || !current(inv, r.seen, &now) || !now.weapon)
    return note(true, "%s changed before it could be edited - nothing was done", name_of(r.seen.v.id));
  call::Frame frame;
  bool changed = false;
  if (r.has_levels && call::levels_ready()) {
    if (set_levels(now.item, now.id, r.seen.levels, r.levels, r.seen.max_level)) {
      changed = true;
      call::weapon_customized(now.item);  // its rounds within its (new) magazine, as the merchant's upgrade does
    }
  }
  if (r.a >= 0 && r.seen.gun) {
    if (game::set_loaded(now.item, r.a)) {
      call::weapon_customized(now.item);  // no more than the magazine holds
      changed = true;
    }
  }
  if (r.b >= 0 && r.seen.knife) {
    if (game::set_durability(now.item, r.b)) changed = true;
    // A broken knife given durability is mended, as the merchant's repair mends it.
    if (r.b > 0 && (now.condition & game::kConditionBroken)) game::set_condition(now.item, now.condition & ~game::kConditionBroken);
  }
  if (!changed) return;
  call::case_update_reload(inv.cs_inventory);
  game::ItemView after;
  if (!current(inv, r.seen, &after)) return note(false, "%s changed", name_of(now.id));
  if (r.seen.gun) note(false, "%s: %d round(s) loaded", name_of(after.id), after.ammo);
  else note(false, "%s: durability %d", name_of(after.id), after.durability);
}

void do_remove(const game::Inventories& inv, const Request& r) {
  game::ItemView now;
  if (!current(inv, r.seen, &now)) return note(true, "%s is not there any more", name_of(r.seen.v.id));
  if (now.force_get) return note(true, "%s is the game's to take back - it was left alone", name_of(now.id));
  const bool ok = r.seen.kind == kCase ? call::case_remove(inv.cs, now.guid)
                                       : call::list_remove(r.seen.kind, list_owner(inv, r.seen.kind, true), now.guid);
  if (ok) note(false, "removed %s", name_of(now.id));
  else note(true, "the game did not remove %s", name_of(now.id));
}

void do_add(const game::Inventories& inv, const Request& r) {
  const items::Def* d = items::find(r.a);
  if (!d) return note(true, "no such item");
  const int kind = kind_of_item(d->id);
  const bool weapon = items::keeps_state(d->id);
  const int n = weapon ? 1 : r.b < 1 ? 1 : r.b > (d->stack > 0 ? d->stack : 1) ? (d->stack > 0 ? d->stack : 1) : r.b;
  int added = -1;
  {
    call::Frame frame;  // the new item stays the frame's until its list holds it
    const uintptr_t item = call::generate_item(d->id, n, -1, -1, d->type == items::kGun ? d->ammo : 0);
    if (!item) return note(true, "the game made no %s", name_of(d->id));
    if (!game::item_defined(item))
      return note(true, "the game does not know %s in this game (is its DLC installed?) - nothing was added",
                  name_of(d->id));
    added = kind == kCase ? call::case_add(inv.cs, item) : call::list_add(kind, list_owner(inv, kind, false), item);
  }
  if (added < 0) return note(true, "the game did not take %s (see the log)", name_of(d->id));
  if (added == 0)
    return note(true, kind == kCase ? "no room in the case for %s" : "the game did not take %s", name_of(d->id));
  if (kind == kCase) call::case_update_reload(inv.cs_inventory);
  note(false, "added %d x %s%s", added, name_of(d->id), added < n ? " (all there was room for)" : "");
}

void do_size(const game::Inventories& inv, const Request& r) {
  game::CaseSize c;
  if (!game::case_size(inv.cs_inventory, &c)) return note(true, "the case's size was not found");
  const int want = r.a < 0 ? 0 : r.a > 4 ? 4 : r.a;
  if (want <= c.size) return note(true, "the case only grows - what it holds must still fit");
  if (call::case_change_size(inv.cs, want)) note(false, "the case is now %s", game::case_size_name(want));
  else note(true, "the game did not change the case's size");
}

void do_store(const game::Run& run, const game::Inventories& inv, const Request& r) {
  if (!has_box(run.mode)) return note(true, "%s has no item box", game::mode_name(run.mode));
  game::ItemView now;
  if (r.seen.kind != kCase || !current(inv, r.seen, &now) || now.count != r.seen.v.count)
    return note(true, "%s changed before it could be stored - nothing was done", name_of(r.seen.v.id));
  if (now.force_get) return note(true, "%s is the game's to take back - it was left alone", name_of(now.id));
  // The game's storage first, for what it takes and has room for: its own store
  // copies the item in, then the case lets it go.
  if (storage_usable()) {
    const int put = storage_put(now.item, now.id, now.count);
    if (put < 0) return note(true, "the game's storage could not be asked to take %s (see the log)", name_of(now.id));
    if (put > 0) {
      if (call::case_remove(inv.cs, now.guid)) return note(false, "stored %s in the game's storage", name_of(now.id));
      game::ItemView s;  // out of the storage again: one off its entry of the item
      if (stored_entry(now.id, &s)) call::storage_reduce(game::armoury_manager(), s.item, 1);
      return note(true, "the game did not take %s out of the case - it stays there", name_of(now.id));
    }
  }
  // The rest, and what the storage has no room for: the box's own part.
  char why[160];
  if (!box_usable(why, sizeof(why))) return note(true, "%s cannot go in the item box: %s", name_of(now.id), why);
  const itembox::Entry e = entry_of(now);
  if (!itembox::put(e)) return note(true, "the item box is not this game's yet - %s stays in the case", name_of(e.id));
  if (!call::case_remove(inv.cs, now.guid)) {
    itembox::unput(e);
    return note(true, "the game did not take %s out of the case - it stays there", name_of(e.id));
  }
  note(false, "stored %d x %s", e.num, name_of(e.id));
}

// The entry of the box's own part a request is for: where the panel saw it while
// it is still that, else the first one like it (an earlier change of a batch took
// an entry before it out). -1: none is.
int box_index(const Request& r) {
  const itembox::View v = itembox::view();
  const int n = static_cast<int>(v.entry.size());
  if (r.index >= 0 && r.index < n && itembox::same(v.entry[r.index], r.entry)) return r.index;
  for (int i = 0; i < n; ++i)
    if (itembox::same(v.entry[i], r.entry)) return i;
  return -1;
}

void do_take(const game::Run& run, const game::Inventories& inv, const Request& r) {
  if (!has_box(run.mode)) return note(true, "%s has no item box", game::mode_name(run.mode));
  const int index = box_index(r);
  if (index < 0)
    return note(true, "the item box changed before %s could be taken - nothing was done", name_of(r.entry.id));
  const itembox::Entry& e = r.entry;
  const items::Def* d = items::find(e.id);
  if (!d || kind_of_item(e.id) != kCase) return note(true, "%s does not go in the case", name_of(e.id));
  const int stack = d->stack > 0 ? d->stack : 1;
  const int n = e.weapon ? 1 : e.num < stack ? e.num : stack;
  int added = -1;
  {
    call::Frame frame;  // the new item stays the frame's until the case holds it
    const uintptr_t item = make_item(e, n);
    if (!item) return note(true, "the game made no %s", name_of(e.id));
    if (!game::item_defined(item))
      return note(true, "the game does not know %s in this game (is its DLC installed?) - it stays in the box",
                  name_of(e.id));
    added = call::case_add(inv.cs, item);
  }
  if (added < 0) return note(true, "the game did not take %s (see the log)", name_of(e.id));
  if (added == 0) return note(true, "no room in the case for %s", name_of(e.id));
  call::case_update_reload(inv.cs_inventory);
  if (!itembox::take(index, r.entry, added))
    return note(true, "took %d x %s - but the item box had changed: it still lists them", added, name_of(e.id));
  note(false, "took %d x %s%s", added, name_of(e.id), added < n ? " (all there was room for)" : "");
}

// Out of the game's storage as its own take does it: the stored item itself
// while it is the last of its entry (a weapon with everything it has), else a new
// one off the stack; the storage then gives one up.
void do_take_stored(const game::Run& run, const game::Inventories& inv, const Request& r) {
  if (!has_box(run.mode)) return note(true, "%s has no item box", game::mode_name(run.mode));
  if (!storage_usable()) return note(true, "the game's storage cannot be reached (see the log)");
  game::ItemView now;
  if (r.seen.kind != kStorage || !current(inv, r.seen, &now) || now.stored != r.seen.v.stored || now.stored < 1)
    return note(true, "the game's storage changed before %s could be taken - nothing was done", name_of(r.seen.v.id));
  if (kind_of_item(now.id) != kCase) return note(true, "%s does not go in the case", name_of(now.id));
  int added = -1;
  {
    call::Frame frame;
    const uintptr_t item = now.stored > 1 ? call::generate_item(now.id, 1, -1, -1, 0) : now.item;
    if (!item) return note(true, "the game made no %s", name_of(now.id));
    added = call::case_add(inv.cs, item);
  }
  if (added < 0) return note(true, "the game did not take %s (see the log)", name_of(now.id));
  if (added == 0) return note(true, "no room in the case for %s", name_of(now.id));
  call::case_update_reload(inv.cs_inventory);
  if (!call::storage_reduce(game::armoury_manager(), now.item, 1))
    return note(true, "took %s - but the game's storage did not give it up (see the log)", name_of(now.id));
  note(false, "took %s from the game's storage", name_of(now.id));
}

void do_stored(const game::Inventories& inv, const Request& r) {
  game::ItemView now;
  if (r.seen.kind != kStorage || !current(inv, r.seen, &now) || now.stored != r.seen.v.stored)
    return note(true, "the game's storage changed before %s could be changed - nothing was done", name_of(r.seen.v.id));
  if (r.op == kOpStoredCount) {
    if (game::set_storage_stack(now.holder, r.a)) note(false, "%s in the game's storage: %d", name_of(now.id), r.a);
    else note(true, "the count of %s in the game's storage could not be set (1 to %d)", name_of(now.id), now.stored_max);
    return;
  }
  if (!storage_usable()) return note(true, "the game's storage cannot be reached (see the log)");
  if (call::storage_reduce(game::armoury_manager(), now.item, now.stored))
    note(false, "threw away %s from the game's storage", name_of(now.id));
  else note(true, "the game's storage did not give up %s (see the log)", name_of(now.id));
}

void do_box(const game::Run& run, const Request& r) {
  if (!has_box(run.mode)) return note(true, "%s has no item box", game::mode_name(run.mode));
  const int index = box_index(r);
  if (r.op == kOpBoxCount) {
    if (index >= 0 && itembox::set_count(index, r.entry, r.a)) note(false, "%s in the box: %d", name_of(r.entry.id), r.a);
    else note(true, "the count of %s in the box could not be set", name_of(r.entry.id));
  } else {
    if (index >= 0 && itembox::discard(index, r.entry)) note(false, "threw away %s from the box", name_of(r.entry.id));
    else note(true, "%s could not be thrown away", name_of(r.entry.id));
  }
}

// The game's files beside the mod into the box: each item where a Store would
// put it - the game's storage, one at a time while it takes them, made by its
// factory as a Take makes them; the rest into the box's own part. The files are
// held back after, whatever went where, and go once a save keeps the box.
void do_import(const game::Run& run) {
  itembox::File f;
  if (!box_file(run.mode, &f)) return note(true, "%s has no item box", game::mode_name(run.mode));
  char why[160];
  if (!box_usable(why, sizeof(why))) return note(true, "nothing was moved in: %s", why);
  const std::vector<itembox::Entry> entries = itembox::read_file(f);
  if (entries.empty()) return note(true, "%s holds nothing that could be read (see the log)", itembox::file_name(f));
  int stored = 0, boxed = 0;
  for (const itembox::Entry& e : entries) {
    int left = e.weapon ? 1 : e.num;
    while (left > 0 && storage_usable()) {
      {
        call::Frame frame;  // the new item stays the frame's until the storage holds its copy
        const uintptr_t item = make_item(e, 1);
        if (!item || !game::item_defined(item) || storage_put(item, e.id, 1) != 1) break;
      }
      --left;
      ++stored;
      // A stack (the First Aid Spray): the rest onto the entry it went to, up to
      // its most, as the panel's count sets it - the storage holds no more of it.
      game::ItemView s;
      if (left > 0 && stored_entry(e.id, &s) && s.stored_max > 1) {
        const int more = left < s.stored_max - s.stored ? left : s.stored_max - s.stored;
        if (more > 0 && game::set_storage_stack(s.holder, s.stored + more)) {
          left -= more;
          stored += more;
        }
        break;
      }
    }
    if (left > 0) {
      itembox::Entry rest = e;
      if (!rest.weapon) rest.num = left;
      if (itembox::put(rest)) boxed += rest.weapon ? 1 : left;
    }
  }
  itembox::moved_in(f);
  note(false, "moved in from %s: %d item(s) into the game's storage, %d into the box - the file goes once you save",
       itembox::file_name(f), stored, boxed);
}

// One of the panel's requests (its note says what came of it).
void apply(const game::Run& run, const Request& r) {
  if (r.op == kOpBoxCount || r.op == kOpDiscard) return do_box(run, r);  // the box's own part alone: no game state
  char why[160];
  game::Inventories inv;
  if (!allowed(run, why, sizeof(why))) return note(true, "%s", why);
  if (!game::inventories(&inv)) return note(true, "the player's attache case was not found just now");
  switch (r.op) {
    case kOpCount: return do_count(inv, r);
    case kOpWeapon: return do_weapon(inv, r);
    case kOpRemove: return do_remove(inv, r);
    case kOpAdd: return do_add(inv, r);
    case kOpSize: return do_size(inv, r);
    case kOpStore: return do_store(run, inv, r);
    case kOpTake: return do_take(run, inv, r);
    case kOpTakeStored: return do_take_stored(run, inv, r);
    case kOpStoredCount:
    case kOpDiscardStored: return do_stored(inv, r);
    case kOpImport: return do_import(run);
    default: return;
  }
}

// What the panel shows: the lists, read while the panel is up.
void build(const game::Run& run, View* v) {
  static game::ItemView buf[kMaxRows];
  v->mode = run.mode;
  v->nrows = 0;
  for (int& c : v->count) c = 0;
  game::Inventories inv;
  v->known = run.in_game && game::inventories(&inv);
  v->editable = v->known && allowed(run, v->why, sizeof(v->why));
  v->has_box = has_box(run.mode);
  v->storage = v->has_box && storage_usable();
  std::snprintf(v->storage_why, sizeof(v->storage_why), "%s",
                v->storage ? ""
                : !game::parts().storage ? "the game's storage was not found (see the log)"
                : call::ready()          ? "the game's storage functions were not found (see the log)"
                                         : call::status());
  v->box_ok = v->has_box && box_usable(v->box_why, sizeof(v->box_why));
  itembox::File f;
  v->box_files = box_file(run.mode, &f) ? itembox::file_entries(f) : 0;
  if (!v->known) {
    std::snprintf(v->why, sizeof(v->why), "%s",
                  !game::parts().inventory ? "the attache case was not found in the game (see the log)"
                                           : "no game is being played");
  } else {
    if (!game::case_size(inv.cs_inventory, &v->size)) v->size = game::CaseSize{};
    for (int kind = 0; kind < kKinds; ++kind) {
      if (kind == kStorage && !v->has_box) continue;
      const int n = game::read_items(inv, kind, buf, kMaxRows - v->nrows);
      for (int i = 0; i < n; ++i) make_row(buf[i], kind, &v->row[v->nrows++]);
      v->count[kind] = n > 0 ? n : 0;
    }
  }
  std::snprintf(v->note, sizeof(v->note), "%s", g_note);
  v->note_error = g_note_error;
}

}  // namespace

const char* kind_name(int kind) {
  static const char* const kNames[kKinds] = {"the attache case", "key items", "treasures", "unique items",
                                             "the game's storage"};
  return kind >= 0 && kind < kKinds ? kNames[kind] : "?";
}

int kind_of_item(int32_t id) {
  const items::Def* d = items::find(id);
  if (!d) return kCase;
  switch (d->type) {
    case items::kKey: return kKeyItems;
    case items::kTreasure:
    case items::kCombinableTreasure: return kTreasures;
    case items::kUnique: return kUnique;
    default: return kCase;
  }
}

bool has_box(game::Mode m) { return m == game::Mode::kMain || m == game::Mode::kSeparateWays; }

bool box_file(game::Mode m, itembox::File* out) {
  switch (m) {
    case game::Mode::kMain: *out = itembox::File::kMain; return true;
    case game::Mode::kSeparateWays: *out = itembox::File::kSeparateWays; return true;
    default: return false;
  }
}

void box_into_save(uintptr_t data) {
  itembox::File file = itembox::File::kMain;
  box_file(game::run().mode, &file);
  const std::vector<itembox::Entry> box = itembox::contents();
  if (box.empty()) return itembox::saved(true, file);
  static uint32_t s_saves = 0;
  ++s_saves;
  const uintptr_t list = game::storage_save_list(data);
  const int before = game::list_size(list);
  bool ok = before >= 0 && call::box_save_ready();
  if (!ok) logf("ERROR: itembox: this save cannot carry the item box: %s", before < 0 ? "its item list is not there" : call::status());
  int carried = 0;
  for (size_t i = 0; ok && i < box.size(); ++i) {
    const itembox::Entry& e = box[i];
    const uintptr_t item = make_item(e, e.num);
    // The count whole (the factory makes no more than a piece holds), and the box's own Guid.
    ok = item && (e.weapon || game::set_item_count(item, e.num)) && mark(item, static_cast<uint32_t>(i), s_saves) &&
         call::item_list_add(list, item);
    if (ok) ++carried;
    else logf("ERROR: itembox: %s x%d could not be put in this save (%s)", items::name(e.id), e.num, call::status());
  }
  if (!ok && carried > 0 && !game::truncate_list(list, before))
    logf("ERROR: itembox: the save keeps %d of the box's entries - the file beside the mod has them all", carried);
  if (ok) logf("itembox: this save carries the item box: %d entr%s", carried, carried == 1 ? "y" : "ies");
  itembox::saved(ok, file);
}

uintptr_t box_from_save(uintptr_t data) {
  if (!game::is_storage_save(data)) {  // nothing for the storage: the game empties it, and the box goes with it
    itembox::loaded({});
    return 0;
  }
  const uintptr_t list = game::storage_save_list(data);
  std::vector<itembox::Entry> box;
  int theirs = 0;
  re::List l;
  if (list && re::read_list(list, &l))
    for (int i = 0; i < l.count; ++i) {
      game::ItemView v;
      if (!game::read_item_view(re::list_ref(l, i), &v) || !box_marked(v)) ++theirs;
      else box.push_back(entry_of(v));
    }
  if (theirs) {
    // An old save: the game's own storage, kept in this list before an update.
    logf("itembox: this save's item list holds %d item(s) of the game's own (an old save's storage) - left to the game; "
         "the item box comes out empty",
         theirs + static_cast<int>(box.size()));
    itembox::loaded({});
    return 0;
  }
  itembox::loaded(box);
  return box.empty() ? 0 : list;
}

void init() {
  InitializeCriticalSection(&g_cs);
  g_view = new View();
  g_ready = true;
}

void view(View* out) {
  if (!g_ready) {
    *out = View{};
    return;
  }
  EnterCriticalSection(&g_cs);
  *out = *g_view;
  LeaveCriticalSection(&g_cs);
}

unsigned view_generation() { return g_generation.load(std::memory_order_acquire); }

void lists_drawn() { g_drawn_at.store(stamp::now(), std::memory_order_relaxed); }

void begin_batch() {
  g_making = true;
  g_making_list.clear();
}

void end_batch(const char* done) {
  g_making = false;
  if (!g_ready || g_making_list.empty()) return;
  const int n = static_cast<int>(g_making_list.size());
  if (n > 1) {  // (one alone is a request like any other, with its own note)
    const uint32_t batch = ++g_batches;
    for (Request& r : g_making_list) {
      r.batch = batch;
      r.batch_size = n;
      r.batch_done = done;
    }
    logf("inventory: %d changes asked for together (panel)", n);
  }
  EnterCriticalSection(&g_cs);
  g_queue.insert(g_queue.end(), g_making_list.begin(), g_making_list.end());
  LeaveCriticalSection(&g_cs);
  g_making_list.clear();
}

void request_count(const Row& seen, int count) {
  Request r;
  r.op = kOpCount;
  r.seen = seen;
  r.a = count;
  push(r);
}

void request_weapon(const Row& seen, int ammo, int durability, const int* levels) {
  Request r;
  r.op = kOpWeapon;
  r.seen = seen;
  r.a = ammo;
  r.b = durability;
  if (levels) {
    r.has_levels = true;
    for (int i = 0; i < seen.levels.n && i < 16; ++i) r.levels[i] = levels[i];
  }
  push(r);
}

void request_remove(const Row& seen) {
  Request r;
  r.op = kOpRemove;
  r.seen = seen;
  push(r);
}

void request_add(int32_t id, int count) {
  Request r;
  r.op = kOpAdd;
  r.a = id;
  r.b = count;
  push(r);
}

void request_case_size(int size) {
  Request r;
  r.op = kOpSize;
  r.a = size;
  push(r);
}

bool set_tickets(const game::Run& run, int count, char* why, size_t n) {
  auto fail = [&](const char* w) {
    std::snprintf(why, n, "%s", w);
    return false;
  };
  if (!allowed(run, why, n)) return false;
  game::Inventories inv;
  if (!game::inventories(&inv) || !inv.treasure) return fail("the player's treasures were not found just now");
  const int32_t id = game::upgrade_ticket_id();
  const items::Def* d = id ? items::find(id) : nullptr;
  if (!d) return fail("the Exclusive Upgrade Ticket was not found in the game (see the log)");
  const int most = d->stack > 0 ? d->stack : 1;
  const int want = count < 0 ? 0 : count > most ? most : count;
  static game::ItemView buf[kMaxRows];
  const int rows = game::read_items(inv, kTreasures, buf, kMaxRows);
  if (rows < 0) return fail("the treasures could not be read");
  int had = 0, first = -1;
  for (int i = 0; i < rows; ++i)
    if (buf[i].id == id) {
      had += buf[i].count;
      if (first < 0) first = i;
    }
  // Every entry of it but the first goes (all of them for none), by the treasures' own remove.
  for (int i = 0; i < rows; ++i)
    if (buf[i].id == id && (i != first || want == 0) && !call::list_remove(kTreasures, inv.treasure, buf[i].guid))
      return fail("the game did not take a ticket out of the treasures");
  if (want > 0 && first >= 0) {
    if (buf[first].count != want && !game::set_item_count(buf[first].item, want))
      return fail("the tickets' count could not be written");
  } else if (want > 0) {
    call::Frame frame;  // the new ticket stays the frame's until the treasures hold it
    const uintptr_t item = call::generate_item(id, want, -1, -1, 0);
    if (!item) return fail("the game made no ticket");
    if (!game::item_defined(item)) return fail("the game does not know the Exclusive Upgrade Ticket in this game");
    const int added = call::list_add(kTreasures, inv.treasure, item);
    if (added <= 0)
      return fail(added < 0 ? "the treasures did not take the ticket (see the log)" : "the treasures took no ticket");
  }
  note(false, "%s: %d -> %d", name_of(id), had, want);
  return true;
}

void request_store(const Row& seen) {
  Request r;
  r.op = kOpStore;
  r.seen = seen;
  push(r);
}

void request_take(int box_index, const itembox::Entry& seen) {
  Request r;
  r.op = kOpTake;
  r.index = box_index;
  r.entry = seen;
  push(r);
}

void request_take_stored(const Row& seen) {
  Request r;
  r.op = kOpTakeStored;
  r.seen = seen;
  push(r);
}

void request_box_count(int box_index, const itembox::Entry& seen, int num) {
  Request r;
  r.op = kOpBoxCount;
  r.index = box_index;
  r.entry = seen;
  r.a = num;
  push(r);
}

void request_stored_count(const Row& seen, int num) {
  Request r;
  r.op = kOpStoredCount;
  r.seen = seen;
  r.a = num;
  push(r);
}

void request_discard(int box_index, const itembox::Entry& seen) {
  Request r;
  r.op = kOpDiscard;
  r.index = box_index;
  r.entry = seen;
  push(r);
}

void request_discard_stored(const Row& seen) {
  Request r;
  r.op = kOpDiscardStored;
  r.seen = seen;
  push(r);
}

void request_import() {
  Request r;
  r.op = kOpImport;
  push(r);
}

void tick(const game::Run& run) {
  if (!g_ready) return;
  static bool s_paused = false, s_known = false;
  static DWORD s_built = 0;
  const bool opened = run.paused && !s_paused;
  s_paused = run.paused;
  // Between games (the title, the main menus) the item box of the last game is
  // let go: a new game starts with none, and a load brings its save's.
  if (run.menus) itembox::clear();

  // The panel's requests, in the order they were made: a few this frame, the
  // rest the next.
  LARGE_INTEGER freq{}, start{}, at{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  bool changed = false;
  for (int i = 0; i < kPerTick; ++i) {
    Request r;
    EnterCriticalSection(&g_cs);
    const bool any = !g_queue.empty();
    if (any) {
      r = g_queue.front();
      g_queue.erase(g_queue.begin());
    }
    LeaveCriticalSection(&g_cs);
    if (!any) break;
    changed = true;
    g_note_error = false;
    apply(run, r);
    if (r.batch) batch_step(r, g_note_error);
    QueryPerformanceCounter(&at);
    if ((at.QuadPart - start.QuadPart) * 1000000 / (freq.QuadPart ? freq.QuadPart : 1) >= kTickBudgetUs) break;
  }

  // The panel's lists: while it draws them, four times a second, and at once
  // after a change and as the pause menu opens.
  const DWORD now = GetTickCount();
  const bool shown = stamp::within(g_drawn_at.load(std::memory_order_relaxed), 500, now);
  if (!(changed || opened || (shown && now - s_built >= 250) || (s_known && !run.in_game))) return;
  s_built = now;
  static View s_build;
  build(run, &s_build);
  s_known = s_build.known;
  EnterCriticalSection(&g_cs);
  s_build.waiting = static_cast<int>(g_queue.size());
  *g_view = s_build;
  g_generation.fetch_add(1, std::memory_order_release);
  LeaveCriticalSection(&g_cs);
}

}  // namespace re4cc::inventory
