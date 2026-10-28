#include "challenges.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "call.h"
#include "challenges_rules.h"
#include "challenges_text.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "savefiles.h"

namespace re4cc::challenges {
namespace {

// --- names, found once (discover) -------------------------------------------------------------------
int g_missing = 0;
uint32_t need_type(const char* name) {
  const uint32_t t = re::find_type(name);
  if (!t && g_missing++ < 8) logf("challenges: no class %s", name);
  return t;
}
re::Field need_field(uint32_t t, const char* name) {
  if (!t) {
    ++g_missing;
    return {};
  }
  const re::Field f = re::find_field(t, name);
  if (!f.valid() && g_missing++ < 8) logf("challenges: no field %s.%s", re::full_name(t).c_str(), name);
  return f;
}
int need_enum(const char* type_name, const char* literal, int fallback) {
  const uint32_t t = re::find_type(type_name);
  const int v = t ? re::enum_value(t, literal, INT32_MIN) : INT32_MIN;
  if (v == INT32_MIN) {
    if (g_missing++ < 8) logf("challenges: no enum value %s.%s", type_name, literal);
    return fallback;
  }
  return v;
}

// GameRecordID and ExShopBonusID values both stay below this (259 and 432 in this build).
constexpr int kIds = 512;

struct Names {
  // chainsaw.GameRecordManager (an AppSingleton): the challenges, the shop's
  // bonuses, the CP, the request to check "Complete all challenges", the table,
  // the bonuses unlocked at the game's first start.
  uint32_t mgr = 0;
  re::Field records, shop, points, check_all, setting, initial;
  // GameRecordData, one per GameRecordID.
  uint32_t record = 0;
  re::Field r_id, r_done, r_notice, r_hide, r_count, r_bits;
  // ExShopData, one per ExShopBonusID.
  uint32_t bonus = 0;
  re::Field b_id, b_open, b_bought;
  // The table (GameRecordSettingUserData): a challenge's entry, the bonuses
  // unlocked by a challenge, the bonuses unlocked by an item.
  uint32_t table = 0, info = 0, unlock = 0, item = 0;
  re::Field t_list, t_unlocks, t_items;
  re::Field i_id, i_points, i_goal, i_category, i_type, i_enable, i_bonus, i_disable, i_detail;
  re::Field u_id, u_bonus, it_bonus;
  int category[kGroups] = {1, 2, 3, 4, 5, 6};  // CategoryType per Group
  int gauge = 3, progress = 2;  // ConditionType: the counted ones
  int numbered = 1;             // DetailStringType.Variable1: the condition shows its goal
  // The Steam achievements: which challenge carries which (the static table),
  // and the service the game asks (NetworkManager's).
  re::Field achievements;
  uint32_t network = 0;
  re::Field controller;
} N;
std::atomic<bool> g_ok{false};
char g_status[200] = "Not searched yet.";

// --- the game's table, read once per table object -------------------------------------------------------
struct Entry {
  int id = -1;
  int group = -1;
  int cp = 0, goal = -1;
  bool counted = false;   // ProgressGauge (or Progress): a count toward its goal
  bool numbered = false;  // its condition shows "{0}"
  bool disabled = false;  // IsDisable: off the game's screen
  int first = 0, n = 0;   // its bonuses in Table::bonus
};
struct Table {
  uintptr_t setting = 0;  // the table object read (0: none yet)
  int n = 0;
  Entry e[kMaxRows];
  int nb = 0;
  int16_t bonus[kMaxUnlocks];
  bool by_item[kIds] = {};  // a bonus an item found unlocks too (UnlockBonusItemList)
};
Table g_table;  // the tick's

// The bonuses of one list (List<ExShopBonusID>) added to the table's, each once per challenge.
void add_bonuses(Table& t, Entry& e, uintptr_t list) {
  re::List l;
  if (!list || !re::read_list(list, &l) || l.items.elem_size != 4) return;
  for (int k = 0; k < l.count && k < l.items.count && t.nb < kMaxUnlocks; ++k) {
    int32_t b = -1;
    if (!mem::read_safe(l.items.data + static_cast<uintptr_t>(k) * 4, &b) || b < 0 || b >= kIds) continue;
    bool dup = false;
    for (int j = e.first; j < e.first + e.n && !dup; ++j) dup = t.bonus[j] == b;
    if (dup) continue;
    t.bonus[t.nb++] = static_cast<int16_t>(b);
    ++e.n;
  }
}

int group_of(int category) {
  for (int g = 0; g < kGroups; ++g)
    if (N.category[g] == category) return g;
  return -1;
}

bool read_table(uintptr_t setting) {
  if (setting == g_table.setting && g_table.n > 0) return true;
  static Table t;
  t = Table{};
  re::List infos, unlocks, items;
  if (!re::is_a(setting, N.table) || !re::read_list(game::ref(setting, N.t_list), &infos)) return false;
  const bool have_unlocks = re::read_list(game::ref(setting, N.t_unlocks), &unlocks);
  for (int i = 0; i < infos.count && t.n < kMaxRows; ++i) {
    const uintptr_t info = re::list_ref(infos, i);
    if (!info || !re::is_a(info, N.info)) continue;
    Entry& e = t.e[t.n];
    e = Entry{};
    int type = -1, category = -1, detail = -1;
    bool enable = false, disabled = false;
    if (!game::get(info, N.i_id, &e.id) || e.id < 0 || e.id >= kIds) continue;
    game::get(info, N.i_points, &e.cp);
    game::get(info, N.i_goal, &e.goal);
    game::get(info, N.i_category, &category);
    game::get(info, N.i_type, &type);
    game::get(info, N.i_detail, &detail);
    game::get(info, N.i_enable, &enable);
    game::get(info, N.i_disable, &disabled);
    e.group = group_of(category);
    e.counted = (type == N.gauge || type == N.progress) && e.goal > 0;
    e.numbered = detail == N.numbered;
    e.disabled = disabled;
    // Off the game's screen and awarding nothing (a rejected one): not the window's.
    const bool named = name(e.id) != nullptr;
    e.first = t.nb;
    // What unlockRecord unlocks: the entry's own list (when it is enabled), then
    // the challenge's in UnlockBonusList.
    if (enable) add_bonuses(t, e, game::ref(info, N.i_bonus));
    for (int k = 0; have_unlocks && k < unlocks.count; ++k) {
      const uintptr_t u = re::list_ref(unlocks, k);
      int id = -1;
      if (u && re::is_a(u, N.unlock) && game::get(u, N.u_id, &id) && id == e.id) add_bonuses(t, e, game::ref(u, N.u_bonus));
    }
    if (disabled && (!named || e.n == 0)) {
      t.nb = e.first;
      continue;
    }
    ++t.n;
  }
  if (re::read_list(game::ref(setting, N.t_items), &items)) {
    Entry scratch;
    for (int k = 0; k < items.count; ++k) {
      const uintptr_t it = re::list_ref(items, k);
      if (!it || !re::is_a(it, N.item)) continue;
      scratch.first = t.nb;
      scratch.n = 0;
      add_bonuses(t, scratch, game::ref(it, N.it_bonus));
      for (int j = scratch.first; j < scratch.first + scratch.n; ++j) t.by_item[t.bonus[j]] = true;
      t.nb = scratch.first;  // only marked
    }
  }
  if (t.n == 0) return false;
  t.setting = setting;
  g_table = t;
  logf("challenges: the game's table read: %d challenges, %d bonuses they unlock", g_table.n, g_table.nb);
  return true;
}

// --- the game's challenges now ---------------------------------------------------------------------------
struct Now {
  uintptr_t mgr = 0;
  uintptr_t record[kIds] = {};  // GameRecordData by GameRecordID
  uintptr_t bonus[kIds] = {};   // ExShopData by ExShopBonusID
  bool initial[kIds] = {};      // unlocked at the game's first start (_InitialReleaseBonus)
  int achievement[kIds] = {};   // the game's number for the challenge's Steam achievement (0: none)
  bool achievements_read = false;
  int points = -1;
};

uintptr_t manager() {
  const uintptr_t m = N.mgr ? re::singleton(N.mgr) : 0;
  return m && re::is_a(m, N.mgr) ? m : 0;
}

// Every element of a reference array: `each(object)`.
template <typename F>
void each_ref(uintptr_t arr, F&& each) {
  re::Array a;
  if (!arr || !re::read_array(arr, &a) || a.elem_size != 8) return;
  for (int i = 0; i < a.count && i < 4096; ++i)
    if (const uintptr_t o = mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8)) each(o);
}

// Dictionary<GameRecordID, Int32> (an int to an int): every key and value.
void read_int_dictionary(uintptr_t dict, int* out, int n) {
  const uint32_t dt = dict ? re::type_of(dict) : 0;
  if (!dt) return;
  const re::Field fe = re::find_field(dt, "_entries"), fc = re::find_field(dt, "_count");
  re::Array a;
  int count = 0;
  const uintptr_t entries = game::ref(dict, fe);
  if (!entries || !game::get(dict, fc, &count) || !re::read_array(entries, &a) || a.elem_size != 16) return;
  for (int i = 0; i < count && i < a.count && i < 4096; ++i) {
    const uintptr_t e = a.data + static_cast<uintptr_t>(i) * a.elem_size;
    int32_t hash = -1, key = -1, value = 0;
    if (!mem::read_safe(e, &hash) || hash < 0 || !mem::read_safe(e + 8, &key) || !mem::read_safe(e + 12, &value)) continue;
    if (key >= 0 && key < n) out[key] = value;
  }
}

bool read_now(Now* c) {
  *c = Now{};
  c->mgr = manager();
  if (!c->mgr || !read_table(game::ref(c->mgr, N.setting))) return false;
  int records = 0;
  each_ref(game::ref(c->mgr, N.records), [&](uintptr_t o) {
    int id = -1;
    if (re::is_a(o, N.record) && game::get(o, N.r_id, &id) && id >= 0 && id < kIds) c->record[id] = o, ++records;
  });
  each_ref(game::ref(c->mgr, N.shop), [&](uintptr_t o) {
    int id = -1;
    if (re::is_a(o, N.bonus) && game::get(o, N.b_id, &id) && id >= 0 && id < kIds) c->bonus[id] = o;
  });
  re::Array a;
  if (re::read_array(game::ref(c->mgr, N.initial), &a) && a.elem_size == 4)
    for (int i = 0; i < a.count && i < kIds; ++i) {
      int32_t b = -1;
      if (mem::read_safe(a.data + static_cast<uintptr_t>(i) * 4, &b) && b >= 0 && b < kIds) c->initial[b] = true;
    }
  if (N.achievements.valid()) {
    // The table exists once the class's static constructor has run (the game's
    // first unlockRecord at the latest).
    const uintptr_t at = re::static_addr(N.achievements);
    const uintptr_t dict = at ? mem::read_ptr(at) : 0;
    if (dict) {
      read_int_dictionary(dict, c->achievement, kIds);
      c->achievements_read = true;
    }
    static int s_logged = -1;  // the tick's
    if (s_logged != (c->achievements_read ? 1 : 0)) {
      s_logged = c->achievements_read ? 1 : 0;
      int n = 0;
      for (int id = 0; id < kIds; ++id) n += c->achievement[id] > 0;
      if (c->achievements_read) logf("challenges: %d challenge(s) carry a Steam achievement (GameRecordIdDefinition)", n);
      else logf("challenges: the game's table of the challenges' achievements is not set up yet");
    }
  }
  game::get(c->mgr, N.points, &c->points);
  return records > 0;
}

bool done(const Now& c, int id) {
  bool v = false;
  return id >= 0 && id < kIds && c.record[id] && game::get(c.record[id], N.r_done, &v) && v;
}

// Undoing challenge `id` leaves bonus `b` unlocked: it is bought, the game gives
// it otherwise (its first start, an item, its code), or another challenge that is
// done unlocks it too.
bool kept(const Now& c, int id, int b) {
  bool bought = false;
  if (b < 0 || b >= kIds) return true;
  if (c.bonus[b] && game::get(c.bonus[b], N.b_bought, &bought) && bought) return true;
  if (c.initial[b] || g_table.by_item[b] || rules::given_by_code(b)) return true;
  for (int i = 0; i < g_table.n; ++i) {
    const Entry& e = g_table.e[i];
    if (e.id == id || !done(c, e.id)) continue;
    for (int j = e.first; j < e.first + e.n; ++j)
      if (g_table.bonus[j] == b) return true;
  }
  return false;
}

const Entry* entry(int id) {
  for (int i = 0; i < g_table.n; ++i)
    if (g_table.e[i].id == id) return &g_table.e[i];
  return nullptr;
}

// --- state shared with the panel ------------------------------------------------------------------------
CRITICAL_SECTION g_cs;  // g_view, g_queue, g_note
bool g_cs_ready = false;
View* g_view = nullptr;
struct Req {
  int id = -1;
  bool on = false;
};
constexpr int kQueue = 256;
Req g_queue[kQueue];
int g_nq = 0;
char g_note[200] = {};
bool g_note_error = false;

// A save the mod asked for (the tick's).
bool g_save_pending = false;
std::atomic<bool> g_saving{false};
DWORD g_save_asked = 0;

void note(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(bool error, const char* fmt, ...) {
  char text[200];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  logf("challenges: %s", text);
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_cs);
}

const char* label(int id) {
  const char* n = name(id);
  return n ? n : "(a challenge)";
}

// The game's achievement request for challenge `id`, as unlockRecord makes it.
bool unlock_achievement(const Now& c, int id) {
  const int number = id >= 0 && id < kIds ? c.achievement[id] : 0;
  if (number <= 0) return false;
  if (!call::achievements_ready()) {
    static bool s_logged = false;  // the tick's
    if (!s_logged) {
      s_logged = true;
      logf("challenges: no achievement is asked for: %s", call::status());
    }
    return false;
  }
  const uintptr_t nm = N.network ? re::singleton(N.network) : 0;
  const uintptr_t ctl = nm && re::is_a(nm, N.network) ? game::ref(nm, N.controller) : 0;
  if (!ctl) {
    static bool s_logged = false;  // the tick's
    if (!s_logged) {
      s_logged = true;
      logf("challenges: the game's achievement service is not up - no achievement asked for");
    }
    return false;
  }
  return call::unlock_achievement(ctl, number);
}

// One challenge switched. true: something was written.
bool switch_one(Now& c, int id, bool on) {
  const Entry* e = entry(id);
  const uintptr_t rec = id >= 0 && id < kIds ? c.record[id] : 0;
  if (!e || !rec) {
    note(true, "That challenge is not in the game's list - nothing was changed.");
    return false;
  }
  const bool was = done(c, id);
  int count = 0;
  uint32_t bits = 0;
  game::get(rec, N.r_count, &count);
  game::get(rec, N.r_bits, &bits);
  const bool counted = e->counted && !rules::counted_in_game_save(id);
  char changed[120] = "";
  size_t used = 0;
  auto noted = [&](const char* fmt, int v) {
    if (used < sizeof(changed)) used += static_cast<size_t>(std::snprintf(changed + used, sizeof(changed) - used, fmt, v));
  };
  int bonuses = 0;
  if (on) {
    // unlockRecord's writes - GameRecordData.setUnlock(true): done, not hidden;
    // not its NEW flag, which brings up the game's "challenges completed" pop-up
    // (GameRecordManager.checkRecordNotice) - its CP, its bonuses, and the game
    // asked to check "Complete all challenges".
    game::put<uint8_t>(rec, N.r_done, 1);
    game::put<uint8_t>(rec, N.r_hide, 0);
    if (counted && rules::count_on(count, e->goal) != count) {
      game::put<int32_t>(rec, N.r_count, rules::count_on(count, e->goal));
      noted(", count %d", rules::count_on(count, e->goal));
    }
    if (!was && e->cp && c.points >= 0) {
      c.points = rules::points_on(c.points, e->cp);
      game::put<int32_t>(c.mgr, N.points, c.points);
      noted(", +%d CP", e->cp);
    }
    for (int j = e->first; j < e->first + e->n; ++j) {
      const int b = g_table.bonus[j];
      bool open = false;
      if (c.bonus[b] && game::get(c.bonus[b], N.b_open, &open) && !open && game::put<uint8_t>(c.bonus[b], N.b_open, 1))
        ++bonuses;
    }
    if (bonuses) noted(", %d bonus(es) unlocked", bonuses);
    game::put<uint8_t>(c.mgr, N.check_all, 1);
    const bool asked = unlock_achievement(c, id);
    note(false, "\"%s\" on%s%s", label(id), changed,
         !c.achievement[id] ? "" : asked ? " - its Steam achievement asked for" : " - its Steam achievement could not be asked for (see the log)");
    return true;
  }
  // Undone: setUnlock(false)'s writes (done and its NEW flag), its CP back, its
  // count just short, its bonuses locked unless something else keeps them.
  game::put<uint8_t>(rec, N.r_done, 0);
  game::put<uint8_t>(rec, N.r_notice, 0);
  if (counted) {
    const int now = rules::count_off(count, e->goal);
    if (now != count) {
      game::put<int32_t>(rec, N.r_count, now);
      noted(", count %d", now);
    }
    const uint32_t b = rules::bits_off(bits, now);
    if (b != bits) game::put<uint32_t>(rec, N.r_bits, b);
  }
  if (was && e->cp && c.points >= 0) {
    c.points = rules::points_off(c.points, e->cp);
    game::put<int32_t>(c.mgr, N.points, c.points);
    noted(", -%d CP", e->cp);
  }
  for (int j = e->first; j < e->first + e->n; ++j) {
    const int b = g_table.bonus[j];
    bool open = false;
    if (!c.bonus[b] || !game::get(c.bonus[b], N.b_open, &open) || !open || kept(c, id, b)) continue;
    if (game::put<uint8_t>(c.bonus[b], N.b_open, 0)) ++bonuses;
  }
  if (bonuses) noted(", %d bonus(es) locked", bonuses);
  note(false, "\"%s\" off%s", label(id), changed);
  return true;
}

// --- the panel's snapshot ----------------------------------------------------------------------------------
void publish(const Now* c, bool up) {
  View* v = new View;
  v->up = up;
  if (c) {
    v->known = true;
    v->cp = c->points;
    v->achievements_known = c->achievements_read;
    for (int i = 0; i < g_table.n && v->count < kMaxRows; ++i) {
      const Entry& e = g_table.e[i];
      Row& r = v->row[v->count++];
      r.id = e.id;
      r.group = e.group;
      r.done = done(*c, e.id);
      r.hidden = e.disabled;
      r.achievement = c->achievement[e.id];
      r.cp = e.cp;
      r.goal = e.counted || e.numbered ? e.goal : -1;
      if (e.counted && !rules::counted_in_game_save(e.id) && c->record[e.id]) {
        int n = 0;
        if (game::get(c->record[e.id], N.r_count, &n)) r.progress = n;
      }
      r.first_unlock = v->n_unlock;
      for (int j = e.first; j < e.first + e.n && v->n_unlock < kMaxUnlocks; ++j) {
        const int b = g_table.bonus[j];
        Unlock& u = v->unlock[v->n_unlock++];
        u.bonus = b;
        if (c->bonus[b]) {
          game::get(c->bonus[b], N.b_open, &u.open);
          game::get(c->bonus[b], N.b_bought, &u.bought);
        }
        u.kept = kept(*c, e.id, b);
        ++r.n_unlocks;
      }
    }
  }
  v->achievements = call::achievements_ready();
  v->saving = g_saving || g_save_pending;
  EnterCriticalSection(&g_cs);
  std::snprintf(v->status, sizeof(v->status), "%s", g_note[0] ? g_note : (g_ok ? "" : g_status));
  v->status_error = g_note[0] ? g_note_error : !g_ok;
  View* old = g_view;
  g_view = v;
  LeaveCriticalSection(&g_cs);
  delete old;
}

// --- the save --------------------------------------------------------------------------------------------------
// The game's system save, asked for once its saves are free (savefiles), then
// watched until they are free again.
void service_save(DWORD now) {
  static DWORD s_polled = 0, s_pending_since = 0;
  if (!g_saving && !g_save_pending) return;
  if (now - s_polled < 250) return;
  s_polled = now;
  if (g_saving) {
    if (!savefiles::game_saves_busy() && now - g_save_asked > 500) {
      g_saving = false;
      logf("challenges: saved");
    } else if (now - g_save_asked > 30000) {
      g_saving = false;
      logf("challenges: the save the mod asked for has not finished after 30 s");
    }
    return;
  }
  if (!s_pending_since) s_pending_since = now;
  const int r = savefiles::request_system_save();
  if (r > 0) {
    g_save_pending = false;
    s_pending_since = 0;
    g_saving = true;
    g_save_asked = now;
    logf("challenges: the game's system save asked for (SaveDataManager.requestSaveSystemData)");
  } else if (r < 0) {
    g_save_pending = false;
    s_pending_since = 0;
    note(true, "The change could not be saved - the game saves it with its next save of its settings.");
  } else if (now - s_pending_since > 60000) {
    // The game's saves have been busy for a minute (an error dialog?): its own next system save takes the change.
    g_save_pending = false;
    s_pending_since = 0;
    logf("challenges: the game's saves have been busy for a minute - the change is saved the next time the game saves");
  }
}

}  // namespace

// --- set-up ---------------------------------------------------------------------------------------------------
void init() {
  if (g_cs_ready) return;
  InitializeCriticalSection(&g_cs);
  g_cs_ready = true;
}

bool discover() {
  if (g_ok) return true;
  if (!re::ready()) return false;
  g_missing = 0;
  N.mgr = need_type("chainsaw.GameRecordManager");
  N.records = need_field(N.mgr, "GameRecordDataList");
  N.shop = need_field(N.mgr, "ExShopDataList");
  N.points = need_field(N.mgr, "RecordExPoint");
  N.check_all = need_field(N.mgr, "requestCheckAllUnlock");
  N.setting = need_field(N.mgr, "SettingInfoData");
  N.initial = need_field(N.mgr, "_InitialReleaseBonus");
  N.record = need_type("chainsaw.GameRecordData");
  N.r_id = need_field(N.record, "_RecordID");
  N.r_done = need_field(N.record, "_IsUnlock");
  N.r_notice = need_field(N.record, "_IsNotice");
  N.r_hide = need_field(N.record, "_IsHide");
  N.r_count = need_field(N.record, "_Count");
  N.r_bits = need_field(N.record, "_ProgressBitFlag");
  N.bonus = need_type("chainsaw.ExShopData");
  N.b_id = need_field(N.bonus, "_BonusID");
  N.b_open = need_field(N.bonus, "_IsUnlock");
  N.b_bought = need_field(N.bonus, "_IsBuy");
  N.table = need_type("chainsaw.GameRecordSettingUserData");
  N.t_list = need_field(N.table, "DataList");
  N.t_unlocks = need_field(N.table, "UnlockBonusList");
  N.t_items = need_field(N.table, "UnlockBonusItemList");
  N.info = need_type("chainsaw.GameRecordSettingUserData.InfoData");
  N.i_id = need_field(N.info, "RecordId");
  N.i_points = need_field(N.info, "AddPoint");
  N.i_goal = need_field(N.info, "AchievementNum");
  N.i_category = need_field(N.info, "Category");
  N.i_type = need_field(N.info, "Type");
  N.i_enable = need_field(N.info, "IsEnableBonus");
  N.i_bonus = need_field(N.info, "BonusList");
  N.i_disable = need_field(N.info, "IsDisable");
  N.i_detail = need_field(N.info, "DetailStringType");
  N.unlock = need_type("chainsaw.GameRecordSettingUserData.UnlockBonusInfo");
  N.u_id = need_field(N.unlock, "RecordId");
  N.u_bonus = need_field(N.unlock, "BonusList");
  N.item = need_type("chainsaw.GameRecordSettingUserData.UnlockBonusItemInfo");
  N.it_bonus = need_field(N.item, "BonusList");
  const char* const kCategory = "chainsaw.GameRecordSettingUserData.CategoryType";
  static const char* const kCategories[kGroups] = {"Story", "Tutorials", "Weapons", "Battle", "Collectibles", "Cp1011"};
  for (int g = 0; g < kGroups; ++g) N.category[g] = need_enum(kCategory, kCategories[g], g + 1);
  N.gauge = need_enum("chainsaw.GameRecordSettingUserData.ConditionType", "ProgressGauge", 3);
  N.progress = need_enum("chainsaw.GameRecordSettingUserData.ConditionType", "Progress", 2);
  N.numbered = need_enum("chainsaw.GameRecordSettingUserData.DetailStringType", "Variable1", 1);
  const int core = g_missing;
  // The achievements (Switching on unlocks them; without them the rest works).
  N.achievements = need_field(need_type("chainsaw.GameRecordIdDefinition"), "_Table");
  if (N.achievements.valid() && !N.achievements.is_static()) N.achievements = {}, ++g_missing;
  N.network = need_type("chainsaw.NetworkManager");
  N.controller = need_field(N.network, "_AchievementServiceController");
  const bool ok = core == 0;
  std::snprintf(g_status, sizeof(g_status), "%s",
                ok ? "" : "The game's challenges were not understood in this build (see the log) - nothing can be switched.");
  g_ok = ok;
  logf("challenges: discovery (%d name(s) missing): the challenges %d, their achievements %d", g_missing, ok,
       N.achievements.valid() && N.controller.valid());
  return ok;
}

bool ready() { return g_ok; }
const char* status() { return g_status; }

const char* tab_name(int tab) {
  static_assert(sizeof(challenges_text::kTabGroup) / sizeof(challenges_text::kTabGroup[0]) == kGroups);
  if (tab == kTabAll) return challenges_text::kTabAll;
  if (tab == kTabIncomplete) return challenges_text::kTabIncomplete;
  return tab >= 0 && tab < kGroups ? challenges_text::kTabGroup[tab] : "?";
}

const char* name(int id) {
  for (const auto& c : challenges_text::kChallenge)
    if (c.id == id) return c.name;
  return nullptr;
}
const char* condition(int id) {
  for (const auto& c : challenges_text::kChallenge)
    if (c.id == id) return c.condition;
  return nullptr;
}
const char* bonus_name(int bonus) {
  for (const auto& b : challenges_text::kBonus)
    if (b.id == bonus) return b.name;
  return nullptr;
}
const char* bonus_who(int bonus) {
  for (const auto& b : challenges_text::kBonus)
    if (b.id == bonus) return b.who;
  return nullptr;
}
const char* bonus_kind(int bonus) {
  static const char* const kKinds[] = {"Mode", "Weapon", "Costume", "Accessory", "Model", "Art", "Filter"};
  for (const auto& b : challenges_text::kBonus)
    if (b.id == bonus) return b.kind >= 0 && b.kind < 7 ? kKinds[b.kind] : nullptr;
  return nullptr;
}

View view() {
  View v;
  if (!g_cs_ready) return v;
  EnterCriticalSection(&g_cs);
  if (g_view) v = *g_view;
  LeaveCriticalSection(&g_cs);
  return v;
}

void request(int id, bool on) { request_all(&id, 1, on); }

void request_all(const int* ids, int n, bool on) {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  for (int i = 0; i < n && g_nq < kQueue; ++i) g_queue[g_nq++] = Req{ids[i], on};
  LeaveCriticalSection(&g_cs);
}

void tick(const game::Run& run) {
  if (!g_cs_ready || !g_ok) return;
  static bool s_up = false;
  static DWORD s_published = 0;
  const DWORD now = GetTickCount();
  const bool up = run.challenges;
  const bool opened = up && !s_up;
  if (up != s_up) logf("challenges: the game's challenges screen %s", up ? "is up" : "closed");
  s_up = up;

  Req q[kQueue];
  int nq = 0;
  EnterCriticalSection(&g_cs);
  nq = g_nq;
  std::memcpy(q, g_queue, sizeof(Req) * static_cast<size_t>(nq));
  g_nq = 0;
  if (opened) g_note[0] = '\0';  // a note belongs to the screen it was made beside
  LeaveCriticalSection(&g_cs);
  static Now c;  // the tick's alone (large)
  bool changed = false, have = false;
  if (nq > 0) {
    if (!up) {
      note(true, "Challenges are only switched while the game's challenges screen is open.");
    } else if (!(have = read_now(&c))) {
      note(true, "The game's challenges could not be read - nothing was changed.");
    } else {
      // One asked for is switched even when it already is (its achievement asked
      // for again, as the game does); of many, the ones that change.
      int n_on = 0, n_off = 0;
      for (int i = 0; i < nq; ++i)
        if ((done(c, q[i].id) != q[i].on || nq == 1) && switch_one(c, q[i].id, q[i].on)) ++(q[i].on ? n_on : n_off);
      if (nq > 1 && n_on && n_off) note(false, "%d challenge(s) switched on, %d off", n_on, n_off);
      else if (nq > 1) note(false, "%d challenge(s) switched %s", n_on + n_off, n_on ? "on" : "off");
      changed = n_on + n_off > 0;
    }
  }
  if (changed) g_save_pending = true;
  service_save(now);

  // The window's figures: as the screen opens, after a change, and twice a second while it is up.
  if (up && (opened || changed || now - s_published >= 500)) {
    if (!have) have = read_now(&c);
    publish(have ? &c : nullptr, up);
    s_published = now;
  } else if (!up && s_published) {
    publish(nullptr, false);
    s_published = 0;
  }
}

}  // namespace re4cc::challenges
