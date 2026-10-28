#include "exshop.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "call.h"
#include "challenges_text.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "savefiles.h"
#include "stamp.h"

namespace re4cc::exshop {
namespace {

// ExShopBonusID values stay below this (Max is 433 in this build).
constexpr int kIds = 512;

// --- the names (challenges_text.h), by ExShopBonusID ---------------------------------------------------
struct Index {
  int16_t at[kIds];
};
constexpr Index make_index() {
  Index x{};
  for (int i = 0; i < kIds; ++i) x.at[i] = -1;
  for (int i = 0; i < challenges_text::kBonuses; ++i)
    if (challenges_text::kBonus[i].id >= 0 && challenges_text::kBonus[i].id < kIds)
      x.at[challenges_text::kBonus[i].id] = static_cast<int16_t>(i);
  return x;
}
constexpr Index kIndex = make_index();

const challenges_text::Bonus* bonus(int id) {
  const int i = id >= 0 && id < kIds ? kIndex.at[id] : -1;
  return i >= 0 ? &challenges_text::kBonus[i] : nullptr;
}

// --- names, found once (discover) -------------------------------------------------------------------
int g_missing = 0;
uint32_t need_type(const char* name) {
  const uint32_t t = re::find_type(name);
  if (!t && g_missing++ < 8) logf("shop: no class %s", name);
  return t;
}
re::Field need_field(uint32_t t, const char* name) {
  if (!t) {
    ++g_missing;
    return {};
  }
  const re::Field f = re::find_field(t, name);
  if (!f.valid() && g_missing++ < 8) logf("shop: no field %s.%s", re::full_name(t).c_str(), name);
  return f;
}
int need_enum(const char* type_name, const char* literal, int fallback) {
  const uint32_t t = re::find_type(type_name);
  const int v = t ? re::enum_value(t, literal, INT32_MIN) : INT32_MIN;
  if (v == INT32_MIN) {
    if (g_missing++ < 8) logf("shop: no enum value %s.%s", type_name, literal);
    return fallback;
  }
  return v;
}

struct Names {
  // chainsaw.GameRecordManager (an AppSingleton): the shop's items' state, its
  // table, the CP.
  uint32_t mgr = 0;
  re::Field states, table, points;
  // ExShopData, one per ExShopBonusID.
  uint32_t data = 0;
  re::Field d_id, d_unlock, d_buy;
  // The table (ExContentsShopUserData): an InfoData per item, in the shop's order.
  uint32_t shop = 0, info = 0;
  re::Field s_list, i_id, i_cp, i_category;
  // ExShopCategory: the shop's tabs' (the accessories go under the costumes' tab).
  int weapon = 1, costume = 2, figure = 3, art = 4, accessory = 7, cp11 = 8;
  // The shop's screen: its tabs; for its page shown again, the hold to buy, the
  // selected entry (an index into its list of the tab's items), and what its
  // updateCurrentGrid reads without a check - the grid and its two lists of entries.
  uint32_t screen = 0;
  re::Field sc_tabs, sc_holding, sc_index, sc_list, sc_grid, sc_cells, sc_unused;
  // The DLC with every main-game item unlocked and bought (isUnlockBonus and
  // checkBuyBonus answer so while share.ExtraContentsManager holds it), and the
  // items it leaves to their data: Separate Ways' (CP11), its Professional mode,
  // the Mercenaries' (CP12).
  uint32_t contents = 0;
  re::Field c_list;
  uint32_t dlc = 0;
  int mode01 = 1, cp11_first = 358, cp11_last = 423, cp12_first = 424, cp12_last = 432;
} N;
std::atomic<bool> g_ok{false};
char g_status[200] = "Not searched yet.";

int tab_of(int category) {
  if (category == N.weapon) return kWeapons;
  if (category == N.costume || category == N.accessory) return kApparel;
  if (category == N.figure) return kModels;
  if (category == N.art) return kArt;
  if (category == N.cp11) return kSeparateWays;
  return -1;
}

// --- the game's table, read once per table object -------------------------------------------------------
struct Entry {
  int id = -1;
  int tab = -1;
  int cp = 0;
};
struct Table {
  uintptr_t obj = 0;  // the table object read (0: none yet)
  int n = 0;
  Entry e[kMaxRows];
};
Table g_table;  // the tick's

bool read_table(uintptr_t shop) {
  if (shop && shop == g_table.obj && g_table.n > 0) return true;
  static Table t;
  t = Table{};
  re::Array a;
  if (!re::is_a(shop, N.shop) || !re::read_array(game::ref(shop, N.s_list), &a) || a.elem_size != 8) return false;
  int per_tab[kTabs] = {}, other = 0;
  for (int i = 0; i < a.count && t.n < kMaxRows; ++i) {
    const uintptr_t info = mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8);
    if (!info || !re::is_a(info, N.info)) continue;
    Entry& e = t.e[t.n];
    e = Entry{};
    int category = -1;
    if (!game::get(info, N.i_id, &e.id) || e.id < 0 || e.id >= kIds) continue;
    game::get(info, N.i_cp, &e.cp);
    game::get(info, N.i_category, &category);
    e.tab = tab_of(category);
    if (e.tab >= 0) ++per_tab[e.tab];
    else ++other;
    ++t.n;
  }
  if (t.n == 0) return false;
  t.obj = shop;
  g_table = t;
  logf("shop: the game's table read: %d items (weapons %d, apparel %d, models %d, concept art %d, Separate Ways %d, "
       "other %d)",
       g_table.n, per_tab[kWeapons], per_tab[kApparel], per_tab[kModels], per_tab[kArt], per_tab[kSeparateWays], other);
  return true;
}

const Entry* entry(int id) {
  for (int i = 0; i < g_table.n; ++i)
    if (g_table.e[i].id == id) return &g_table.e[i];
  return nullptr;
}

// --- the shop's items now ------------------------------------------------------------------------------
struct Now {
  uintptr_t mgr = 0;
  uintptr_t data[kIds] = {};  // ExShopData by ExShopBonusID
  int points = -1;
  bool dlc = false;           // the DLC with every main-game item is owned
};

uintptr_t manager() {
  const uintptr_t m = N.mgr ? re::singleton(N.mgr) : 0;
  return m && re::is_a(m, N.mgr) ? m : 0;
}

// share.ExtraContentsManager's list of the DLC the player owns holds `hash` (its hasContents).
bool owned(uint32_t hash) {
  const uintptr_t m = hash && N.contents ? re::singleton(N.contents) : 0;
  re::List l;
  if (!m || !re::is_a(m, N.contents) || !re::read_list(game::ref(m, N.c_list), &l)) return false;
  if (l.count > 0 && l.items.elem_size != 4) return false;
  for (int i = 0; i < l.count && i < l.items.count && i < 256; ++i) {
    uint32_t h = 0;
    if (mem::read_safe(l.items.data + static_cast<uintptr_t>(i) * 4, &h) && h == hash) return true;
  }
  return false;
}

bool read_now(Now* c) {
  *c = Now{};
  c->mgr = manager();
  if (!c->mgr || !read_table(game::ref(c->mgr, N.table))) return false;
  int states = 0;
  re::Array a;
  if (re::read_array(game::ref(c->mgr, N.states), &a) && a.elem_size == 8)
    for (int i = 0; i < a.count && i < 4096; ++i) {
      const uintptr_t o = mem::read_ptr(a.data + static_cast<uintptr_t>(i) * 8);
      int id = -1;
      if (o && re::is_a(o, N.data) && game::get(o, N.d_id, &id) && id >= 0 && id < kIds) c->data[id] = o, ++states;
    }
  game::get(c->mgr, N.points, &c->points);
  c->dlc = owned(N.dlc);
  return states > 0;
}

bool flag(uintptr_t obj, const re::Field& f) {
  bool v = false;
  return obj && game::get(obj, f, &v) && v;
}

// What the shop shows for an entry: isUnlockBonus and checkBuyBonus (the DLC's
// items, else their data), the CP held against its price.
Row row_of(const Now& c, const Entry& e) {
  Row r;
  r.id = e.id;
  r.tab = e.tab;
  r.cp = e.cp;
  const int id = e.id;
  r.by_dlc = c.dlc && !(id >= N.cp11_first && id <= N.cp11_last) && id != N.mode01 &&
             !(id >= N.cp12_first && id <= N.cp12_last);
  const uintptr_t d = id >= 0 && id < kIds ? c.data[id] : 0;
  r.unlocked = r.by_dlc || flag(d, N.d_unlock);
  r.bought = r.by_dlc || flag(d, N.d_buy);
  r.state = r.bought      ? kPurchased
            : !r.unlocked ? kLocked
            : c.points >= 0 && c.points < e.cp ? kShort
                                                : kForSale;
  return r;
}

// "Costume: Leon's Jacket", "Model: Leon (Jacket)".
const char* label(int id, char* out, size_t n) {
  const char* k = kind(id);
  const char* nm = name(id);
  const char* w = who(id);
  if (!nm) std::snprintf(out, n, "Item %d", id);
  else if (w && w[0]) std::snprintf(out, n, "%s: %s's %s", k ? k : "Item", w, nm);
  else std::snprintf(out, n, "%s: %s", k ? k : "Item", nm);
  return out;
}

// --- state shared with the panel ------------------------------------------------------------------------
CRITICAL_SECTION g_cs;  // g_view, g_queue, g_note
bool g_cs_ready = false;
View* g_view = nullptr;
struct Req {
  int id = -1;
  bool on = false;
};
constexpr int kQueue = 512;
Req g_queue[kQueue];
int g_nq = 0;
char g_note[200] = {};
bool g_note_error = false;

// The tabs the shop's screen shows (a bit per Tab) and its last frame (stamp.h): its hook's.
std::atomic<uint32_t> g_screen_tabs{0};
std::atomic<DWORD> g_screen_seen{0};

// A change of the tick's that the shop's page does not show yet - the shop sets
// its entries up from the items' state only as it opens, on a tab switch, a scroll
// or a purchase: shown by its screen's next frame (redraw).
std::atomic<bool> g_redraw{false};
bool g_redraw_names = false;  // the screen's fields redraw reads were found

// The shop's own code that shows its page again (call.h); test_exshop puts
// recorders here.
struct ShopCode {
  bool (*ready)();
  bool (*page)(uintptr_t screen, int32_t index);
  bool (*caption)(uintptr_t screen, int32_t bonus_id);
};
ShopCode g_code = {call::shop_ready, call::shop_page, call::shop_caption};

bool redraws() { return g_redraw_names && g_code.ready(); }

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
  logf("shop: %s", text);
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_cs);
}

// One item unlocked (on) or locked again. `alone`: the only one asked for - a
// note says what came of it; of many, the tick sums them up. 1 written, 0
// nothing to do (as asked already, or purchased), -1 it could not be.
int switch_one(const Now& c, int id, bool on, bool alone) {
  char what[160];
  label(id, what, sizeof(what));
  const Entry* e = entry(id);
  const uintptr_t d = id >= 0 && id < kIds ? c.data[id] : 0;
  if (!e || !d) {
    note(true, "%s is not in the shop's list - nothing was changed.", what);
    return -1;
  }
  const Row r = row_of(c, *e);
  if (r.by_dlc) {
    if (alone) note(false, "%s is unlocked and purchased by a DLC you own - nothing to change.", what);
    return 0;
  }
  if (on) {
    if (r.unlocked) {
      if (alone) note(false, "%s is unlocked already.", what);
      return 0;
    }
    if (!game::put<uint8_t>(d, N.d_unlock, 1)) {
      note(true, "%s could not be unlocked.", what);
      return -1;
    }
    if (alone) note(false, "%s unlocked: the shop sells it for %d CP.", what, r.cp);
    else logf("shop: %s unlocked", what);
    return 1;
  }
  if (r.bought) {
    if (alone) note(false, "%s is purchased - it stays unlocked.", what);
    return 0;
  }
  if (!r.unlocked) {
    if (alone) note(false, "%s is locked already.", what);
    return 0;
  }
  if (!game::put<uint8_t>(d, N.d_unlock, 0)) {
    note(true, "%s could not be locked.", what);
    return -1;
  }
  if (alone) note(false, "%s locked again.", what);
  else logf("shop: %s locked again", what);
  return 1;
}

// --- the panel's snapshot ----------------------------------------------------------------------------------
void publish(const Now* c, bool up) {
  View* v = new View;
  v->up = up;
  v->tab[kAll] = true;
  if (c) {
    v->known = true;
    v->cp = c->points;
    v->dlc = c->dlc;
    bool any[kTabs] = {};
    for (int i = 0; i < g_table.n && v->count < kMaxRows; ++i) {
      v->row[v->count] = row_of(*c, g_table.e[i]);
      if (v->row[v->count].tab >= 0) any[v->row[v->count].tab] = true;
      ++v->count;
    }
    // The shop's own tabs, as its screen showed them a moment ago; else every
    // tab with an item.
    const uint32_t shown = stamp::within(g_screen_seen.load(std::memory_order_acquire), 2000)
                               ? g_screen_tabs.load(std::memory_order_relaxed)
                               : 0;
    for (int t = kAll + 1; t < kTabs; ++t) v->tab[t] = shown ? (shown >> t) & 1u : any[t];
  }
  v->instant = redraws();
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
      logf("shop: saved");
    } else if (now - g_save_asked > 30000) {
      g_saving = false;
      logf("shop: the save the mod asked for has not finished after 30 s");
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
    logf("shop: the game's system save asked for (SaveDataManager.requestSaveSystemData)");
  } else if (r < 0) {
    g_save_pending = false;
    s_pending_since = 0;
    note(true, "The change could not be saved - the game saves it with its next save of its settings.");
  } else if (now - s_pending_since > 60000) {
    // The game's saves have been busy for a minute (an error dialog?): its own next system save takes the change.
    g_save_pending = false;
    s_pending_since = 0;
    logf("shop: the game's saves have been busy for a minute - the change is saved the next time the game saves");
  }
}

// --- the shop's page shown again ----------------------------------------------------------------------------------
// From the screen's frame, by the shop's own code, in the order a move of its
// cursor shows them (onSelectionChanged, less its hold's reset and NEW mark): the
// selected item's name and text (setCaptionMessage: "???" and its lock text
// while it is locked), then every entry in view set up again from the items'
// state, the cursor and the scroll left where they are (updateCurrentGrid, as
// after its own purchase). Not while the player holds to buy: the entries are
// made anew and the hold's callback is its entry's - the page waits for the
// hold's end, as a scroll would end the hold (endHoldAll).
void redraw(uintptr_t screen) {
  bool holding = false;
  if (game::get(screen, N.sc_holding, &holding) && holding) return;
  g_redraw.store(false, std::memory_order_relaxed);
  if (!redraws()) return;  // the window says the shop shows a change once its tab is switched
  static bool s_shown = false, s_unset = false;
  if (!game::ref(screen, N.sc_grid) || !game::ref(screen, N.sc_cells) || !game::ref(screen, N.sc_unused)) {
    if (!s_unset) logf("shop: the shop's page was not shown again - its grid is not set up (its own set-up shows it)");
    s_unset = true;
    return;
  }
  int32_t index = -1, id = -1;
  re::List l;
  if (game::get(screen, N.sc_index, &index) && re::read_list(game::ref(screen, N.sc_list), &l))
    if (const uintptr_t info = re::list_ref(l, index); info && re::is_a(info, N.info)) game::get(info, N.i_id, &id);
  const bool caption = id >= 0 && g_code.caption(screen, id);  // an item of the shop's table: setCaptionMessage finds it
  if (!g_code.page(screen, index)) {
    logf("shop: the shop's page could not be shown again - it shows the change once its tab is switched");
    return;
  }
  if (!s_shown) {
    char what[160];
    logf("shop: the shop's page shown again by its own code (updateCurrentGrid, as after a purchase%s%s) - each change "
         "shows at once (first time)",
         caption ? "; the selected item's caption: " : "", caption ? label(id, what, sizeof(what)) : "");
  }
  s_shown = true;
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
  N.states = need_field(N.mgr, "ExShopDataList");
  N.table = need_field(N.mgr, "ExShopSettingInfoData");
  N.points = need_field(N.mgr, "RecordExPoint");
  N.data = need_type("chainsaw.ExShopData");
  N.d_id = need_field(N.data, "_BonusID");
  N.d_unlock = need_field(N.data, "_IsUnlock");
  N.d_buy = need_field(N.data, "_IsBuy");
  N.shop = need_type("chainsaw.ExContentsShopUserData");
  N.s_list = need_field(N.shop, "DataList");
  N.info = need_type("chainsaw.ExContentsShopUserData.InfoData");
  N.i_id = need_field(N.info, "BonusId");
  N.i_cp = need_field(N.info, "CPoint");
  N.i_category = need_field(N.info, "Category");
  const char* const kCategory = "chainsaw.ExShopCategory";
  N.weapon = need_enum(kCategory, "Weapon", 1);
  N.costume = need_enum(kCategory, "Costume", 2);
  N.figure = need_enum(kCategory, "Figure", 3);
  N.art = need_enum(kCategory, "Art", 4);
  N.accessory = need_enum(kCategory, "Accessory", 7);
  N.cp11 = need_enum(kCategory, "CP11", 8);
  const int core = g_missing;
  // The shop's tabs (without them every tab is shown), its page shown again
  // (without it the shop shows a change once its tab is switched) and the DLC
  // with every main-game item (without it such an owner sees those items as
  // their data has them, not as the shop shows them).
  N.screen = need_type("chainsaw.gui.ExContentsShopGuiBehavior");
  N.sc_tabs = need_field(N.screen, "CurrentCategoryTabList");
  N.sc_holding = need_field(N.screen, "isHoldItem");
  N.sc_index = need_field(N.screen, "CurrentListIndex");
  N.sc_list = need_field(N.screen, "CurrentExShopList");
  N.sc_grid = need_field(N.screen, "_CurrentScrollGrid");
  N.sc_cells = need_field(N.screen, "CurrentGridList");
  N.sc_unused = need_field(N.screen, "_DisableGridItemList");
  g_redraw_names = N.sc_holding.valid() && N.sc_index.valid() && N.sc_list.valid() && N.sc_grid.valid() &&
                   N.sc_cells.valid() && N.sc_unused.valid();
  N.contents = need_type("share.ExtraContentsManager");
  N.c_list = need_field(N.contents, "ExtraContentsList");
  N.dlc = static_cast<uint32_t>(need_enum("share.ContentsHash", "DLC_3001", 0));
  const char* const kId = "chainsaw.ExShopBonusID";
  N.mode01 = need_enum(kId, "GameMode01", 1);
  N.cp11_first = need_enum(kId, "CP11_Weapon00", 358);
  N.cp11_last = need_enum(kId, "CP11End", 423);
  N.cp12_first = need_enum(kId, "CP12_Costume00", 424);
  N.cp12_last = need_enum(kId, "CP12End", 432);
  const bool ok = core == 0;
  std::snprintf(g_status, sizeof(g_status), "%s",
                ok ? "" : "The Extra Content Shop was not understood in this build (see the log) - nothing can be unlocked.");
  g_ok = ok;
  logf("shop: discovery (%d name(s) missing): the shop's items %d, its tabs %d, its page shown again %d, the DLC with "
       "every item %d",
       g_missing, ok, N.sc_tabs.valid(), g_redraw_names, N.c_list.valid() && N.dlc != 0);
  return ok;
}

bool ready() { return g_ok; }
const char* status() { return g_status; }

const char* tab_name(int tab) {
  static_assert(sizeof(challenges_text::kShopTab) / sizeof(challenges_text::kShopTab[0]) == kTabs);
  return tab >= 0 && tab < kTabs ? challenges_text::kShopTab[tab] : "?";
}

const char* name(int id) {
  const challenges_text::Bonus* b = bonus(id);
  return b ? b->name : nullptr;
}
const char* who(int id) {
  const challenges_text::Bonus* b = bonus(id);
  return b ? b->who : nullptr;
}
const char* kind(int id) {
  static const char* const kKinds[] = {"Mode", "Weapon", "Costume", "Accessory", "Model", "Concept art", "Filter"};
  const challenges_text::Bonus* b = bonus(id);
  return b && b->kind >= 0 && b->kind < 7 ? kKinds[b->kind] : nullptr;
}
const char* lock_text(int id) {
  const challenges_text::Bonus* b = bonus(id);
  return b && b->lock[0] ? b->lock : nullptr;
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

void screen_frame(uintptr_t screen) {
  if (!g_ok || !re::is_a(screen, N.screen)) return;
  re::List l;
  if (N.sc_tabs.valid() && re::read_list(game::ref(screen, N.sc_tabs), &l) && l.count > 0 && l.items.elem_size == 4) {
    uint32_t bits = 1u << kAll;
    for (int i = 0; i < l.count && i < l.items.count && i < 16; ++i) {
      int32_t category = -1;
      if (!mem::read_safe(l.items.data + static_cast<uintptr_t>(i) * 4, &category)) continue;
      const int t = tab_of(category);
      if (t >= 0) bits |= 1u << t;
    }
    if (g_screen_tabs.exchange(bits, std::memory_order_relaxed) != bits)
      logf("shop: the shop's screen shows %d tab(s)%s", __builtin_popcount(bits),
           (bits >> kSeparateWays) & 1u ? ", Separate Ways' too" : " (not Separate Ways': that DLC is not installed)");
    g_screen_seen.store(stamp::now(), std::memory_order_release);
  }
  if (g_redraw.load(std::memory_order_acquire)) redraw(screen);
}

void tick(const game::Run& run) {
  if (!g_cs_ready || !g_ok) return;
  static bool s_up = false;
  static DWORD s_published = 0;
  const DWORD now = GetTickCount();
  const bool up = run.shop;
  const bool opened = up && !s_up;
  if (up != s_up) logf("shop: the Extra Content Shop %s", up ? "is up" : "closed");
  s_up = up;
  if (!up) g_redraw.store(false, std::memory_order_relaxed);  // its next opening sets its page up afresh

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
      note(true, "Items are only unlocked while the Extra Content Shop is open.");
    } else if (!(have = read_now(&c))) {
      note(true, "The shop's items could not be read - nothing was changed.");
    } else {
      // One asked for says what came of it; of many, the ones that change are summed up.
      int unlocked = 0, locked = 0, kept = 0, failed = 0;
      for (int i = 0; i < nq; ++i) {
        const int r = switch_one(c, q[i].id, q[i].on, nq == 1);
        if (r > 0) ++(q[i].on ? unlocked : locked);
        else if (r < 0) ++failed;
        else if (!q[i].on) {
          const Entry* e = entry(q[i].id);
          kept += e && row_of(c, *e).bought;
        }
      }
      if (nq > 1) {
        char what[160];
        if (unlocked && locked) std::snprintf(what, sizeof(what), "%d item(s) unlocked, %d locked again", unlocked, locked);
        else if (locked || (!unlocked && kept)) std::snprintf(what, sizeof(what), "%d item(s) locked again", locked);
        else std::snprintf(what, sizeof(what), "%d item(s) unlocked: the shop sells them for CP", unlocked);
        if (failed) note(true, "%s; %d could not be changed (see the log).", what, failed);
        else if (kept) note(false, "%s; %d purchased stay unlocked.", what, kept);
        else note(false, "%s.", what);
      }
      changed = unlocked + locked > 0;
    }
  }
  if (changed) {
    g_save_pending = true;
    g_redraw.store(true, std::memory_order_release);  // the shop's page, from its screen's next frame
  }
  service_save(now);

  // The window's figures: as the shop opens, after a change, and twice a second while it is up.
  if (up && (opened || changed || now - s_published >= 500)) {
    if (!have) have = read_now(&c);
    publish(have ? &c : nullptr, up);
    s_published = now;
  } else if (!up && s_published) {
    publish(nullptr, false);
    s_published = 0;
  }
}

}  // namespace re4cc::exshop
