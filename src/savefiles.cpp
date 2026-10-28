#include "savefiles.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "call.h"
#include "game.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "savefiles_rules.h"
#include "stamp.h"

namespace re4cc::savefiles {
namespace {

// --- names, found once (discover) -------------------------------------------------------------------
int g_missing = 0;
uint32_t need_type(const char* name) {
  const uint32_t t = re::find_type(name);
  if (!t && g_missing++ < 8) logf("save files: no class %s", name);
  return t;
}
re::Field need_field(uint32_t t, const char* name) {
  if (!t) {
    ++g_missing;
    return {};
  }
  const re::Field f = re::find_field(t, name);
  if (!f.valid() && g_missing++ < 8) logf("save files: no field %s.%s", re::full_name(t).c_str(), name);
  return f;
}
int need_enum(const char* type_name, const char* literal, int fallback) {
  const uint32_t t = re::find_type(type_name);
  const int v = t ? re::enum_value(t, literal, INT32_MIN) : INT32_MIN;
  if (v == INT32_MIN) {
    if (g_missing++ < 8) logf("save files: no enum value %s.%s", type_name, literal);
    return fallback;
  }
  return v;
}

// The screen: chainsaw.SaveLoadMenuGuiBehavior.
struct ScreenNames {
  uint32_t t = 0;
  re::Field phase, sub, mode, campaign, auto_slot, item_max, slot_details, file_details;
  int idle = 0, dialog_wait = 5, sub_none = 0, sub_wait = 1;
  int load[4] = {0, 2, 4, -100};  // DefaultLoad, InGameLoad, InGamePauseLoad
  int save = 3;                   // InGameSave (a typewriter's)
  int campaign_main = 1, campaign_sw = 2;
} Scr;

// What a row reads.
struct RowNames {
  uint32_t slot_detail = 0, slot_info = 0, file_detail = 0, detail = 0;
  re::Field sd_empty, sd_info, fd_detail, fd_empty;
  re::Field info_slot, info_chapter, info_difficulty, info_saves, info_clears;
  // The fields copied from one slot's information to another's (all but SlotIndex).
  re::Field info_copy[10];
  int ncopy = 0;
  // The chapters' numbers: HighwayGuiManager.<ChapterMisc>[id]._ScenarioNo.
  uint32_t highway = 0;
  re::Field hw_inst, hw_misc, hw_saveload, misc_no, sl_table;
} R;

// The game's saves: share.SaveDataManager and the flows it runs.
struct SaveNames {
  uint32_t t = 0;
  re::Field inst, process, queue, current, flow, remove_flow, flow_phase, flow_queue, remove_phase;
  re::Field queue_size, flow_queue_size;  // each queue's _size (Queue`1<T>: a class per T)
  int idle = 1, processing = 2, flow_wait = 1, remove_standby = 1;
} S;

// The engine's copy: the save containers its game save fills, their keys, the
// size of a game save, and the save service's answers.
struct CopyNames {
  uint32_t data = 0, validate = 0;  // share.SaveDataManager.GameSaveData, GameSaveValidateData
  re::Field size;                   // chainsaw.ChainsawSaveDataDefinition.GAME_SAVE_DATA_SIZE (static)
  int key_main = 7, key_main_validate = 9, key_sw = 8, key_sw_validate = 10;  // share.SaveLoadKey
  int sw_first = 100, sw_last = 149;  // Separate Ways' slots (share.AppSaveSlot)
  int idle = 1, success = 2;          // SaveState.IDLE, SaveResult.Success
  int system_slot = -1;               // AppSaveSlot.Share_SystemSave
} K;

// What comes out of the game's code: SaveFileDetail's getters (the engine's
// detail has no fields in the database) - decoded by savefiles_rules.h.
struct Code {
  int32_t slot = -1, stamp = -1, invalid_slot = 0;
} C;
std::atomic<bool> g_code_ok{false};
std::atomic<bool> g_names_ok{false};
std::atomic<bool> g_copy_names_ok{false};
char g_status[200] = "Not searched yet.";

// --- state shared with the panel, the hook and the copy thread -----------------------------------------------
CRITICAL_SECTION g_cs;  // g_rows, g_queue, the notes
bool g_cs_ready = false;

struct Rows {
  int screen = -1;          // Screen
  bool separate_ways = false;
  int count = 0;
  bool consistent = false;  // every row a detail of its own slot
  Row row[kMaxRows];
  unsigned generation = 0;  // the screen set itself up again this many times (a read after a refresh)
};
Rows g_rows;  // the list as last read (the hook writes it, under g_cs)

char g_note[240] = {};
bool g_note_error = false;

struct Req {
  bool copy = false;
  int from = -1, to = -1;  // rows (a delete: `to`)
  Row from_seen, to_seen;
};
constexpr int kQueue = 4;
Req g_queue[kQueue];
int g_nq = 0;

// The screen's frames (the hook): when it was last seen up on a list the panel
// sits beside, and last seen idle with its rows read.
std::atomic<DWORD> g_shown{0}, g_idle{0};
std::atomic<int> g_shown_screen{-1};
std::atomic<uintptr_t> g_screen{0};
// The screen held still for a copy (its own DialogWait phase: no input, no
// closing), and told to wait for the engine's table and set itself up again
// after a change: the hook writes them, on the screen's own thread, after the
// screen's own update.
std::atomic<bool> g_want_still{false}, g_want_wait{false};
std::atomic<uintptr_t> g_still_screen{0};  // the screen held still (0: none)
std::atomic<DWORD> g_still_at{0}, g_wait_written{0};
std::atomic<bool> g_busy{false};  // for the panel

// (Moments, stamp.h: the hook's frame can be in the millisecond the reader's clock shows.)
bool recent(const std::atomic<DWORD>& at) { return stamp::within(at.load(std::memory_order_acquire), 400); }

void set_status(const char* text) {
  if (g_cs_ready) EnterCriticalSection(&g_cs);
  std::snprintf(g_status, sizeof(g_status), "%s", text);
  if (g_cs_ready) LeaveCriticalSection(&g_cs);
}

void note(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(bool error, const char* fmt, ...) {
  char text[240];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  logf("save files: %s", text);
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_cs);
}

const char* slot_name(int slot, char* buf, size_t n) {
  if (slot == 0 || slot == 100) std::snprintf(buf, n, "the auto-save");
  else if (slot > 100) std::snprintf(buf, n, "slot %d", slot - 100);
  else if (slot > 0) std::snprintf(buf, n, "slot %d", slot);
  else std::snprintf(buf, n, "that slot");
  return buf;
}

template <typename T>
bool get(uintptr_t obj, const re::Field& f, T* out) {
  return game::get(obj, f, out);
}
template <typename T>
bool put(uintptr_t obj, const re::Field& f, const T& v) {
  return game::put(obj, f, v);
}
uintptr_t ref(uintptr_t obj, const re::Field& f) { return game::ref(obj, f); }

// --- the game's code ---------------------------------------------------------------------------------------------
int32_t getter_offset(uintptr_t code, bool wide) {
  uint8_t b[10] = {};
  return code && mem::copy_from(b, code, sizeof(b)) ? rules::getter_offset(b, sizeof(b), wide) : -1;
}
bool getter_constant(uintptr_t code, int32_t* out) {
  uint8_t b[6] = {};
  return code && mem::copy_from(b, code, sizeof(b)) && rules::getter_constant(b, sizeof(b), out);
}

// The engine's own classes are the runtime's to link: their code is read from
// the tick, until it reads.
bool resolve_code(bool log_failure) {
  Code c;
  auto getter = [](const char* name) { return R.detail ? re::method_code(R.detail, name, 0) : 0; };
  c.slot = getter_offset(getter("get_Slot"), false);
  c.stamp = getter_offset(getter("get_LastUpdateTimeStamp"), true);
  const bool invalid = getter_constant(getter("get_InvalidSlot"), &c.invalid_slot);
  const bool ok = c.slot > 0 && c.stamp > 0 && invalid;
  if (ok || log_failure)
    logf("save files: %s: SaveFileDetail slot +0x%X, time stamp +0x%X, an empty slot reads 0x%X",
         ok ? "the game's code read" : "ERROR: the game's code not understood after a minute (still trying)", c.slot,
         c.stamp, static_cast<uint32_t>(c.invalid_slot));
  if (!ok) return false;
  C = c;
  set_status("");
  g_code_ok.store(true, std::memory_order_release);
  return true;
}
bool code_ok() { return g_code_ok.load(std::memory_order_acquire); }

// --- reading the screen's rows (the hook) ------------------------------------------------------------------------
int chapter_number(int chapter_id) {
  const uintptr_t hw = R.highway ? mem::read_ptr(re::static_addr(R.hw_inst)) : 0;
  const uintptr_t misc = hw && re::is_a(hw, R.highway) ? ref(hw, R.hw_misc) : 0;
  const uintptr_t setting = misc ? game::dict_ref(misc, chapter_id) : 0;
  int n = -1;
  return setting && get(setting, R.misc_no, &n) && n >= 0 && n < 100 ? n : -1;
}

int difficulty_index(int value) {
  for (int i = 0; i < game::kDifficulties; ++i)
    if (game::difficulty_value(i) == value) return i;
  return -1;
}

// Row i of the list: its slot's file detail and information, as the screen has them.
bool read_row(uintptr_t sd, uintptr_t fd, int slot, Row* r, bool* fits) {
  *r = Row{};
  r->slot = slot;
  *fits = true;
  bool file_empty = true, info_empty = true;
  if (fd && re::is_a(fd, R.file_detail)) {
    get(fd, R.fd_empty, &file_empty);
    const uintptr_t d = ref(fd, R.fd_detail);
    if (!file_empty && d) {
      int32_t s = C.invalid_slot;
      long long stamp = 0;
      mem::read_safe(d + static_cast<uintptr_t>(C.slot), &s);
      mem::read_safe(d + static_cast<uintptr_t>(C.stamp), &stamp);
      if (s != slot) *fits = false;
      else r->unix_time = rules::unix_seconds(stamp);
    } else {
      file_empty = true;
    }
  }
  uintptr_t info = 0;
  if (sd && re::is_a(sd, R.slot_detail)) {
    get(sd, R.sd_empty, &info_empty);
    info = info_empty ? 0 : ref(sd, R.sd_info);
  }
  int info_slot = -1, chapter = -1, diff = -1;
  if (info && re::is_a(info, R.slot_info) && get(info, R.info_slot, &info_slot) && info_slot == slot &&
      get(info, R.info_chapter, &chapter) && chapter != -1) {
    get(info, R.info_difficulty, &diff);
    get(info, R.info_saves, &r->saves);
    get(info, R.info_clears, &r->clears);
    r->difficulty = difficulty_index(diff);
    r->chapter = chapter_number(chapter);
  } else {
    info = 0;
  }
  // The screen shows a save where the slot has its file and its information both.
  r->used = !file_empty && *fits && info;
  if (r->used) {
    const long long parts[5] = {slot, r->unix_time, chapter, r->saves, diff};
    r->hash = rules::fnv1a(parts, sizeof(parts));
  } else {
    r->hash = rules::fnv1a(&slot, sizeof(slot));
  }
  return true;
}

void snapshot(uintptr_t screen, int which, bool sw, int first, int count, bool after_setup) {
  Rows rows;
  rows.screen = which;
  rows.separate_ways = sw;
  re::List sl, fl;
  const uintptr_t a = ref(screen, Scr.slot_details), b = ref(screen, Scr.file_details);
  if (!a || !b || !re::read_list(a, &sl) || !re::read_list(b, &fl)) return;
  const int n = count < kMaxRows ? count : kMaxRows;
  rows.count = n;
  rows.consistent = sl.count >= n && fl.count >= n;
  for (int i = 0; i < n; ++i) {
    bool fits = true;
    read_row(re::list_ref(sl, i), re::list_ref(fl, i), first + i, &rows.row[i], &fits);
    rows.consistent = rows.consistent && fits;
  }
  EnterCriticalSection(&g_cs);
  rows.generation = g_rows.generation + (after_setup ? 1 : 0);
  g_rows = rows;
  LeaveCriticalSection(&g_cs);
}

// --- the game's saves ----------------------------------------------------------------------------------------------
uintptr_t save_manager() {
  const uintptr_t at = S.t ? re::static_addr(S.inst) : 0;
  const uintptr_t m = at ? mem::read_ptr(at) : 0;
  return m && re::is_a(m, S.t) ? m : 0;
}

// A Queue<T>'s count: its _size, found once on the class its field is declared
// as (discover), or by name on the queue's own class should it be another.
int queue_size(uintptr_t q, const re::Field& size) {
  int n = 0;
  if (!q) return 0;
  if (get(q, size, &n)) return n;
  const uint32_t t = re::type_of(q);
  const re::Field f = t ? re::find_field(t, "_size") : re::Field{};
  return f.valid() && get(q, f, &n) ? n : 0;
}

// SaveDataManager.get_IsBusy, read: its process, its queue, its save flow - and
// its remove flow. The part that is busy (for the log), nullptr when none is.
const char* busy_part(uintptr_t sdm) {
  int process = -1, phase = -1;
  if (!sdm) return "no save manager";
  if (!get(sdm, S.process, &process)) return "its process unread";
  if (process != S.idle) return "its process";
  if (queue_size(ref(sdm, S.queue), S.queue_size) > 0) return "its request queue";
  const uintptr_t flow = ref(sdm, S.flow);
  if (flow && get(flow, S.flow_phase, &phase) && phase != S.flow_wait) return "its save flow";
  if (flow && queue_size(ref(flow, S.flow_queue), S.flow_queue_size) > 0) return "its save flow's queue";
  const uintptr_t rf = ref(sdm, S.remove_flow);
  if (rf && get(rf, S.remove_phase, &phase) && phase != S.remove_standby) return "its remove flow";
  return nullptr;
}
bool saves_busy(uintptr_t sdm) { return busy_part(sdm) != nullptr; }

// --- the change under way (the tick's) -----------------------------------------------------------------------
// A delete: the engine's removal, the screen told to wait -> kRefreshing.
// A copy: kQuieting (the screen held still) -> kLoading (the source read into the
// mod's containers) -> kSaving (they are written to the target) -> kSettling (let
// go; the target's information; the engine's table read again) -> kSystemSave
// (the game's system save asked for) -> kRefreshing. A copy that fails lets
// everything go once the save service is idle (kAborting). The list, set up
// again, says how it went - unless the change already failed, whose note stays.
enum Phase { kIdle = 0, kQuieting, kLoading, kSaving, kSettling, kSystemSave, kAborting, kRefreshing };
struct Op {
  Phase phase = kIdle;
  bool copy = false;
  int to_row = -1, from_slot = -1, to_slot = -1;
  Row from_seen, to_seen;
  DWORD since = 0;
  bool failed = false;
  bool waited = false;
  bool written = false;  // the target's save was started: it may have changed
  bool held = false;     // the save manager held busy (its process: Processing), so it starts no request
  // The containers, each held by the save manager (its suspension of game saves) once it is set here.
  uintptr_t data = 0, validate = 0;
  int key_data = 0, key_validate = 0;  // their share.SaveLoadKey
  unsigned generation = 0;  // the rows' generation when the screen was told to set itself up again
} g_op;

// (A screen still held keeps its request to be set up again: the hook lets it go
// whenever it sees it next.)
void finish_op() {
  g_op.phase = kIdle;
  g_want_still = false;
}

// A new change: what an earlier one left unclaimed is dropped.
void clear_screen_requests() {
  g_want_still = false;
  g_want_wait = false;
  g_still_screen = 0;
  g_still_at = 0;
}

void enter(Phase p, DWORD now) {
  g_op.phase = p;
  g_op.since = now;
}

// The screen told to wait for the engine's table and set itself up again (the
// hook writes it on the screen's next frame) - idle again first, if the copy
// held it still.
void ask_refresh(const Rows& rows, DWORD now) {
  g_op.generation = rows.generation;
  g_want_wait = true;
  enter(kRefreshing, now);
}

bool check_request(const Req& r, const Rows& rows, uintptr_t sdm) {
  if (!recent(g_idle)) {
    note(true, "Nothing was changed - the save list is busy or not up.");
    return false;
  }
  if (g_op.phase != kIdle) {
    note(true, "A change is still under way - nothing else was changed.");
    return false;
  }
  if (!sdm || saves_busy(sdm)) {
    note(true, "The game is busy with its saves - try again in a moment.");
    return false;
  }
  if (r.to < 0 || r.to >= rows.count || (r.copy && (r.from < 0 || r.from >= rows.count || r.from == r.to))) {
    note(true, "That is not a slot of the game's list - nothing was changed.");
    return false;
  }
  if (!same_save(rows.row[r.to], r.to_seen) || (r.copy && !same_save(rows.row[r.from], r.from_seen))) {
    note(true, "The saves changed since that was clicked - nothing was changed.");
    return false;
  }
  if (!rows.consistent) {
    note(true, "The game's list is not laid out as expected (see the log) - nothing was changed.");
    return false;
  }
  return true;
}

void log_row(const char* what, const Row& r) {
  char when[32] = "?";
  if (r.unix_time > 0) {
    const time_t t = static_cast<time_t>(r.unix_time);
    struct tm tmv;
    if (gmtime_s(&tmv, &t) == 0) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &tmv);
  }
  logf("save files: %s: slot %d, %s, chapter %d, %s, %d save(s), written %s", what, r.slot, r.used ? "a save" : "empty",
       r.chapter, game::difficulty_name(r.difficulty), r.saves, when);
}

void start_delete(const Req& r, const Rows& rows, DWORD now) {
  const Row& row = rows.row[r.to];
  char s[32];
  slot_name(row.slot, s, sizeof(s));
  if (!row.used) return note(true, "%s is already empty.", s);
  if (!call::saves_ready()) return note(true, "The game's own removal of a save was not found - %s", call::status());
  log_row("deleting (SaveService.removeSingle)", row);
  if (!call::remove_slot(row.slot)) return note(true, "The game's save service is busy - nothing was deleted; try again.");
  clear_screen_requests();
  g_op = Op{};
  g_op.to_row = r.to;
  g_op.to_slot = row.slot;
  g_op.to_seen = row;
  ask_refresh(rows, now);
  note(false, "Deleting %s...", s);
}

void start_copy(const Req& r, const Rows& rows, DWORD now) {
  const Row& from = rows.row[r.from];
  const Row& to = rows.row[r.to];
  if (!from.used) return note(true, "That slot has no save to copy.");
  if (!g_copy_names_ok.load() || !call::copy_ready())
    return note(true, "The game's own copy of a save was not found - saves can be deleted, not copied (see the log).");
  // Both slots of the list's own game: its keys, as the game's requests choose them by slot.
  const bool sw_from = from.slot >= K.sw_first && from.slot <= K.sw_last;
  const bool sw_to = to.slot >= K.sw_first && to.slot <= K.sw_last;
  if (sw_from != sw_to || sw_from != rows.separate_ways || from.slot < 0 || to.slot < 0)
    return note(true, "Those slots are not of one game - nothing was changed.");
  if (!call::save_access_ok()) return note(true, "The game is busy with its saves - try again in a moment.");
  log_row("copying from", from);
  if (to.used) log_row("copying over", to);
  g_op = Op{};
  g_op.copy = true;
  g_op.to_row = r.to;
  g_op.from_slot = from.slot;
  g_op.to_slot = to.slot;
  g_op.from_seen = from;
  g_op.to_seen = to;
  g_op.key_data = sw_from ? K.key_sw : K.key_main;
  g_op.key_validate = sw_from ? K.key_sw_validate : K.key_main_validate;
  clear_screen_requests();
  g_want_still = true;
  enter(kQuieting, now);
  char a[32], b[32];
  note(false, "Copying %s to %s...", slot_name(from.slot, a, sizeof(a)), slot_name(to.slot, b, sizeof(b)));
}

uintptr_t save_load_manager() {
  const uintptr_t hw = R.highway ? mem::read_ptr(re::static_addr(R.hw_inst)) : 0;
  return hw && re::is_a(hw, R.highway) ? ref(hw, R.hw_saveload) : 0;
}

// The list's information for the target slot made the source's: the game's
// table of slot information (what its rows read), field by field when it has the
// target already, else a copy of the source's that the game makes and keeps.
bool copy_slot_info(int from, int to) {
  const uintptr_t mgr = save_load_manager();
  const uintptr_t table = mgr ? ref(mgr, R.sl_table) : 0;
  const uintptr_t src = table ? game::dict_ref(table, from) : 0;
  if (!src || !re::is_a(src, R.slot_info)) {
    logf("save files: the game has no information for slot %d to copy", from);
    return false;
  }
  if (const uintptr_t dst = game::dict_ref(table, to); dst && re::is_a(dst, R.slot_info)) {
    for (int i = 0; i < R.ncopy; ++i) {
      uint8_t v[8] = {};
      const uintptr_t sa = re::field_addr(src, R.info_copy[i]), da = re::field_addr(dst, R.info_copy[i]);
      const uint32_t size = re::is_value_type(R.info_copy[i].type) ? re::value_size(R.info_copy[i].type) : 0;
      if (!sa || !da || size == 0 || size > 8 || !mem::copy_from(v, sa, size)) return false;
      if (!mem::copy_to(da, v, size)) return false;
    }
    logf("save files: slot %d's information copied over slot %d's", from, to);
    return true;
  }
  call::Frame frame;  // the copy stays the frame's until the table holds it
  const uintptr_t copy = call::slot_info_copy(mgr, from);
  if (!copy || !re::is_a(copy, R.slot_info) || !put<int32_t>(copy, R.info_slot, to)) return false;
  if (!call::add_slot_info(mgr, copy)) return false;
  logf("save files: slot %d's information added for slot %d", from, to);
  return game::dict_ref(table, to) != 0;
}

// --- the engine's copy, step by step ---------------------------------------------------------------------------
void fail_copy(DWORD now, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void fail_copy(DWORD now, const char* fmt, ...) {
  char text[240];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  note(true, "%s", text);
  g_op.failed = true;
  enter(kAborting, now);  // let go once the save service is idle
}

// The containers registered with the save service under the keys the engine's
// game save uses (inside a Frame: the keys are new strings).
bool add_containers() {
  const uintptr_t kd = call::save_key(g_op.key_data), kv = call::save_key(g_op.key_validate);
  return kd && kv && call::service_add(kd, g_op.data) && call::service_add(kv, g_op.validate);
}

// Everything the copy holds let go (only while the save service is idle): the
// service's containers, the manager's hold on them, the manager itself.
void release_copy(uintptr_t sdm) {
  if (g_op.data || g_op.validate) call::service_clean();
  for (uintptr_t* c : {&g_op.data, &g_op.validate}) {
    if (*c && !call::suspend_game_saves(sdm, *c, false))
      logf("ERROR: save files: the save manager did not let go of the copy's container - the game's saves may wait "
           "for it until the game restarts");
    *c = 0;
  }
  if (g_op.held && sdm) put<int32_t>(sdm, S.process, S.idle);
  g_op.held = false;
}

// T0: the screen is still; the source is read into containers of the mod's own.
void begin_copy(uintptr_t sdm, DWORD now) {
  char a[32];
  slot_name(g_op.from_slot, a, sizeof(a));
  int process = -1;
  if (!sdm || !call::save_access_ok() || ref(sdm, S.current) || !get(sdm, S.process, &process) || process != S.idle)
    return fail_copy(now, "The game is busy with its saves - nothing was changed; try again in a moment.");
  // The manager is held busy: it starts no request of its own while the service is the copy's.
  put<int32_t>(sdm, S.process, S.processing);
  g_op.held = true;
  call::Frame frame;  // the new containers are the frame's until the manager holds them
  const uintptr_t d = call::new_object(K.data), v = call::new_object(K.validate);
  if (d && call::suspend_game_saves(sdm, d, true)) g_op.data = d;
  if (v && call::suspend_game_saves(sdm, v, true)) g_op.validate = v;
  if (!g_op.data || !g_op.validate)
    return fail_copy(now, "The game's save containers could not be made - nothing was changed (see the log).");
  if (!call::service_clean() || !add_containers())
    return fail_copy(now, "The game's save service did not take the copy's containers - nothing was changed.");
  if (!call::service_load(g_op.from_slot))
    return fail_copy(now, "The game's save service did not start reading %s - nothing was changed.", a);
  logf("save files: copy: reading slot %d (SaveService.loadSingle)", g_op.from_slot);
  enter(kLoading, now);
}

// T1: the source read - the same containers written to the target, the size the
// engine's game save gives.
void write_copy(DWORD now) {
  char b[32];
  slot_name(g_op.to_slot, b, sizeof(b));
  uint64_t size = 0;
  const uintptr_t at = re::static_addr(K.size);
  if (!at || !mem::read_safe(at, &size) || size == 0)
    return fail_copy(now, "The game's save size was not found - nothing was changed (see the log).");
  call::Frame frame;
  // (The load lets go of its containers; cleared all the same, so each key is there once.)
  if (!call::service_clean() || !add_containers())
    return fail_copy(now, "The game's save service did not take the copy - nothing was changed.");
  if (!call::service_save(g_op.to_slot, size))
    return fail_copy(now, "The game's save service did not start writing %s - nothing was changed.", b);
  g_op.written = true;
  logf("save files: copy: writing slot %d (SaveService.saveSingle, %llu bytes at most)", g_op.to_slot,
       static_cast<unsigned long long>(size));
  enter(kSaving, now);
}

// T3: written - let go, and the target's information made the source's.
void settle_copy(uintptr_t sdm, DWORD now) {
  char a[32], b[32];
  slot_name(g_op.from_slot, a, sizeof(a));
  slot_name(g_op.to_slot, b, sizeof(b));
  release_copy(sdm);
  if (!copy_slot_info(g_op.from_slot, g_op.to_slot)) {
    note(true, "%s was written as a copy of %s, but the game's list information for it could not be set (see the "
               "log).", b, a);
    g_op.failed = true;
  }
  // As after a save of the game's own: Continue takes the newest save.
  if (!call::clear_load_info(save_load_manager(), g_op.to_slot))
    logf("save files: the game's last loaded slot was not cleared (SaveLoadMenuGuiManager.clearInfo)");
  enter(kSettling, now);
}

void check_result(const Rows& rows) {
  char a[32], b[32];
  slot_name(g_op.from_slot, a, sizeof(a));
  slot_name(g_op.to_slot, b, sizeof(b));
  const Row* got = g_op.to_row >= 0 && g_op.to_row < rows.count && rows.row[g_op.to_row].slot == g_op.to_slot
                       ? &rows.row[g_op.to_row]
                       : nullptr;
  if (got) log_row("the target, as the game lists it now", *got);
  if (!g_op.copy) {
    if (got && !got->used) note(false, "%s was deleted.", b);
    else if (!g_op.failed) note(true, "The game did not delete %s (see the log).", b);
    return;
  }
  if (g_op.failed) return;
  const Row& f = g_op.from_seen;
  if (!got || !got->used || got->chapter != f.chapter || got->saves != f.saves || got->difficulty != f.difficulty)
    note(true, "%s was written, but the game's list does not show it as the copy (see the log).", b);
  else if (const int r = call::service_result(); r != K.success)
    note(true, "%s was copied to %s, but the game's system save after it failed (%d) - the list may not keep it "
               "after a restart; saving once in the game keeps it.", a, b, r);
  else
    note(false, "%s was copied to %s.", a, b);
}

void service_op(DWORD now, uintptr_t sdm, const Rows& rows) {
  char s[32];
  slot_name(g_op.to_slot, s, sizeof(s));
  switch (g_op.phase) {
    case kIdle:
      return;
    case kQuieting:
      // The hook holds the screen still on its next idle frame.
      if (g_still_at.load() && g_still_screen.load()) {
        begin_copy(sdm, now);
      } else if (now - g_op.since > 3000 && g_want_still.exchange(false)) {
        note(true, "The save list stayed busy - nothing was changed.");
        finish_op();
      }
      return;
    case kLoading:
    case kSaving: {
      const int state = call::service_state();
      if (state < 0) return fail_copy(now, "The game's save service could not be asked how the copy went (see the log).");
      if (state != K.idle) {
        if (!g_op.waited && now - g_op.since > 30000) {
          g_op.waited = true;
          note(false, "Still waiting for the game's save service...");
        }
        return;
      }
      const int result = call::service_result();
      char a[32];
      slot_name(g_op.from_slot, a, sizeof(a));
      logf("save files: copy: the save service is idle again, its last result %d", result);
      if (g_op.phase == kLoading) {
        if (result != K.success)
          return fail_copy(now, "%s could not be read (the game's save service answered %d) - nothing was changed.", a,
                           result);
        return write_copy(now);
      }
      if (result != K.success)
        return fail_copy(now, "The game could not write %s (its save service answered %d) - check that slot in the "
                              "game.", s, result);
      return settle_copy(sdm, now);
    }
    case kAborting:
      // Let go only once the service is idle: a load or save under way still writes the containers.
      if (call::service_state() == K.idle || !call::ready()) {
        release_copy(sdm);
        if (!call::ready())
          note(true, "The copy stopped (see the log) - restart the game before saving, so its saves are not held back.");
        if (recent(g_shown) || g_still_screen.load()) ask_refresh(rows, now);
        else finish_op();
      }
      return;
    case kSettling:
      // The engine's table read again from the files (it also mends a copy an
      // earlier version of the mod made), only while the game's saves are idle.
      if (call::save_access_ok()) {
        if (!call::refresh_save_table()) logf("save files: the engine's table was not read again");
        enter(kSystemSave, now);
      } else if (now - g_op.since > 20000) {
        logf("save files: the game's saves stayed busy - the engine's table was not read again");
        enter(kSystemSave, now);
      }
      return;
    case kSystemSave:
      // (The request is queued: the manager runs it when it is free.)
      if (call::save_access_ok() || now - g_op.since > 20000) {
        // The game's system save keeps the lists' slot information, as after a save of its own.
        if (call::request_system_save(sdm, K.system_slot)) {
          logf("save files: copy: the game's system save asked for");
        } else if (!g_op.failed) {
          note(true, "%s was copied, but the game's system save could not be asked for - the list may not keep it "
                     "after a restart; saving once in the game keeps it.", s);
          g_op.failed = true;
        }
        if (recent(g_shown) || g_still_screen.load()) ask_refresh(rows, now);
        else finish_op();
      }
      return;
    case kRefreshing: {
      if (rows.generation != g_op.generation && recent(g_idle)) {
        check_result(rows);
        finish_op();
      } else if (!recent(g_shown) && now - g_op.since > 1000) {
        if (!g_op.failed) note(false, "Done - the game's list shows it the next time it opens.");
        finish_op();
      } else if (now - g_op.since > 20000) {
        if (!g_op.failed) note(true, "The game's list was not set up again after 20 s - reopen it to see the change.");
        finish_op();
      }
      return;
    }
  }
}

}  // namespace

// --- set-up ---------------------------------------------------------------------------------------------------
void init() {
  InitializeCriticalSection(&g_cs);
  g_cs_ready = true;
}

bool discover() {
  g_missing = 0;
  const char* const kScreen = "chainsaw.SaveLoadMenuGuiBehavior";
  Scr.t = need_type(kScreen);
  Scr.phase = need_field(Scr.t, "<CurrPhase>k__BackingField");
  Scr.sub = need_field(Scr.t, "<CurrSubPhase>k__BackingField");
  Scr.mode = need_field(Scr.t, "<CurrMode>k__BackingField");
  Scr.campaign = need_field(Scr.t, "<CurrCampaignIdentifier>k__BackingField");
  Scr.auto_slot = need_field(Scr.t, "<CurrAutoSaveSlotIndex>k__BackingField");
  Scr.item_max = need_field(Scr.t, "<CurrItemMax>k__BackingField");
  Scr.slot_details = need_field(Scr.t, "_CurrSaveSlotDetails");
  Scr.file_details = need_field(Scr.t, "_CurrSaveFileDetails");
  Scr.idle = need_enum("chainsaw.SaveLoadMenuGuiBehavior.Phase", "Idle", 0);
  Scr.dialog_wait = need_enum("chainsaw.SaveLoadMenuGuiBehavior.Phase", "DialogWait", 5);
  Scr.sub_none = need_enum("chainsaw.SaveLoadMenuGuiBehavior.SubPhase", "None", 0);
  Scr.sub_wait = need_enum("chainsaw.SaveLoadMenuGuiBehavior.SubPhase", "WaitUpdateTbl", 1);
  const char* const kMode = "chainsaw.SaveLoadMenuGuiBehavior.OpenMode";
  Scr.load[0] = need_enum(kMode, "DefaultLoad", 0);
  Scr.load[1] = need_enum(kMode, "InGameLoad", 2);
  Scr.load[2] = need_enum(kMode, "InGamePauseLoad", 4);
  Scr.save = need_enum(kMode, "InGameSave", 3);
  Scr.campaign_main = need_enum("share.CampaignIdentifier", "Campaign1001", 1);
  Scr.campaign_sw = need_enum("share.CampaignIdentifier", "Campaign1011", 2);

  R.slot_detail = need_type("chainsaw.gui.SaveSlotInfoDetail");
  R.sd_empty = need_field(R.slot_detail, "<IsEmpty>k__BackingField");
  R.sd_info = need_field(R.slot_detail, "<Info>k__BackingField");
  R.slot_info = need_type("chainsaw.gui.SaveSlotInfo");
  R.info_slot = need_field(R.slot_info, "SlotIndex");
  R.info_chapter = need_field(R.slot_info, "ChapterID");
  R.info_difficulty = need_field(R.slot_info, "Difficulty");
  R.info_saves = need_field(R.slot_info, "SaveCount");
  R.info_clears = need_field(R.slot_info, "MainCampaignClearCount");
  static const char* const kCopy[] = {"ChapterID", "Stage", "Purpose", "Difficulty", "SaveCount",
                                      "MainCampaignClearCount", "JustMainCampaignCleared", "JustChapterCleared",
                                      "LastSaveGimmickCotextID", "SaveSlotProgressNo"};
  R.ncopy = 0;
  for (const char* name : kCopy) R.info_copy[R.ncopy++] = need_field(R.slot_info, name);
  R.file_detail = need_type("share.AppSaveFileDetail");
  R.fd_detail = need_field(R.file_detail, "<SaveFileDetail>k__BackingField");
  R.fd_empty = need_field(R.file_detail, "<IsEmpty>k__BackingField");
  R.detail = need_type("via.storage.saveService.SaveFileDetail");
  R.highway = need_type("chainsaw.HighwayGuiManager");
  R.hw_inst = need_field(R.highway, "_Instance");
  R.hw_misc = need_field(R.highway, "<ChapterMisc>k__BackingField");
  R.hw_saveload = need_field(R.highway, "<SaveLoadMenuGuiManager>k__BackingField");
  R.misc_no = need_field(need_type("chainsaw.ChapterMiscSetting"), "_ScenarioNo");
  R.sl_table = need_field(need_type("chainsaw.SaveLoadMenuGuiManager"), "_CurrSaveSlotInfoTable");

  S.t = need_type("share.SaveDataManager");
  S.inst = need_field(S.t, "_Instance");
  S.process = need_field(S.t, "<CurrentProcess>k__BackingField");
  S.queue = need_field(S.t, "_RequestQueue");
  S.current = need_field(S.t, "_CurrentRequest");
  S.flow = need_field(S.t, "_SaveFlowController");
  S.remove_flow = need_field(S.t, "_RemoveFlowController");
  S.flow_phase = need_field(need_type("chainsaw.SaveFlowController"), "<CurrPhase>k__BackingField");
  S.flow_queue = need_field(need_type("chainsaw.SaveFlowController"), "_RequestQueue");
  S.remove_phase = need_field(need_type("chainsaw.SaveRemoveFlowController"), "<CurrPhase>k__BackingField");
  S.queue_size = re::find_field(S.queue.type, "_size");
  S.flow_queue_size = re::find_field(S.flow_queue.type, "_size");
  S.idle = need_enum("share.SaveDataManager.Process", "Idle", 1);
  S.processing = need_enum("share.SaveDataManager.Process", "Processing", 2);
  S.flow_wait = need_enum("chainsaw.SaveFlowController.Phase", "Wait", 1);
  S.remove_standby = need_enum("chainsaw.SaveRemoveFlowController.Phase", "Standby", 1);

  const bool names = g_missing == 0;
  g_names_ok.store(names, std::memory_order_release);

  // The engine's copy (Copy only: the list and Delete do without it).
  const int before = g_missing;
  K.data = need_type("share.SaveDataManager.GameSaveData");
  K.validate = need_type("share.SaveDataManager.GameSaveValidateData");
  K.size = need_field(need_type("chainsaw.ChainsawSaveDataDefinition"), "GAME_SAVE_DATA_SIZE");
  if (K.size.valid() && !K.size.is_static()) ++g_missing;
  const char* const kKey = "share.SaveLoadKey";
  K.key_main = need_enum(kKey, "Highway_GameSave_Key", 7);
  K.key_main_validate = need_enum(kKey, "Highway_GameSave_Validate_Key", 9);
  K.key_sw = need_enum(kKey, "Chainsaw_GameSave_Key", 8);
  K.key_sw_validate = need_enum(kKey, "Chainsaw_GameSave_Validate_Key", 10);
  K.sw_first = need_enum("share.AppSaveSlot", "Campaign1011_Auto", 100);
  K.sw_last = need_enum("share.AppSaveSlot", "Campaign1011_GameSaveLimit", 149);
  K.system_slot = need_enum("share.AppSaveSlot", "Share_SystemSave", -1);
  K.idle = need_enum("via.storage.saveService.SaveState", "IDLE", 1);
  K.success = need_enum("via.storage.saveService.SaveResult", "Success", 2);
  const bool copy_names = g_missing == before;
  g_copy_names_ok.store(copy_names);
  if (!copy_names) logf("save files: the engine's copy of a save was not understood in this build - no Copy");

  set_status(names ? "Reading the game's code..." : "The game's save list was not understood in this build (see the log).");
  logf("save files: discovery (%d name(s) missing): load modes %d/%d/%d, save mode %d, the engine's copy %d", g_missing,
       Scr.load[0], Scr.load[1], Scr.load[2], Scr.save, copy_names);
  return names;
}

bool ready() { return g_names_ok.load(std::memory_order_acquire) && code_ok(); }

const char* screen_name(int screen) {
  switch (screen) {
    case kLoadList: return "the load list";
    case kSaveList: return "the save list";
    default: return "the save list";
  }
}

void screen_frame(uintptr_t, uintptr_t screen) {
  if (!ready() || !screen || !re::is_a(screen, Scr.t)) return;
  int mode = -1, phase = -1, sub = -1, campaign = -1, auto_slot = -1, item_max = 0;
  if (!get(screen, Scr.mode, &mode) || !get(screen, Scr.phase, &phase) || !get(screen, Scr.sub, &sub) ||
      !get(screen, Scr.campaign, &campaign) || !get(screen, Scr.auto_slot, &auto_slot) ||
      !get(screen, Scr.item_max, &item_max))
    return;
  const bool load = mode == Scr.load[0] || mode == Scr.load[1] || mode == Scr.load[2];
  const bool save = mode == Scr.save;
  const bool sw = campaign == Scr.campaign_sw;
  const bool known = (load || save) && (campaign == Scr.campaign_main || sw) && (auto_slot == 0 || auto_slot == 100) &&
                     item_max > 0 && item_max <= kMaxRows;
  if (!known) return;
  const DWORD now = stamp::now();
  g_screen = screen;
  g_shown = now;
  g_shown_screen = save ? kSaveList : kLoadList;
  static int s_sub = -1;
  const bool set_up_again = s_sub == Scr.sub_wait && sub == Scr.sub_none;
  s_sub = sub;
  if (g_still_screen.load() == screen && phase == Scr.dialog_wait) {
    // Held still for a copy: idle again once it is over, and waiting for the engine's table.
    if (g_want_wait.exchange(false)) {
      put<int32_t>(screen, Scr.phase, Scr.idle);
      put<int32_t>(screen, Scr.sub, Scr.sub_wait);
      g_still_screen = 0;
      g_wait_written = now;
      s_sub = Scr.sub_wait;
    }
    return;
  }
  if (phase != Scr.idle) return;
  if (sub == Scr.sub_none && g_want_still.exchange(false)) {
    // Still for a copy: the screen's own wait for a dialog - no input, and it cannot close.
    put<int32_t>(screen, Scr.phase, Scr.dialog_wait);
    g_still_screen = screen;
    g_still_at = now;
    return;
  }
  if (sub == Scr.sub_none && g_want_wait.exchange(false)) {
    // After the screen's own update: it waits for the engine's table from its next frame on.
    put<int32_t>(screen, Scr.sub, Scr.sub_wait);
    g_wait_written = now;
    s_sub = Scr.sub_wait;
    return;
  }
  if (sub != Scr.sub_none) return;
  g_idle = now;
  static DWORD s_read = 0;
  if (set_up_again || now - s_read >= 200) {
    s_read = now;
    // Load lists start at the auto-save, save lists after it.
    snapshot(screen, save ? kSaveList : kLoadList, sw, load ? auto_slot : auto_slot + 1, item_max, set_up_again);
  }
}

int screen_shown() { return recent(g_shown) ? g_shown_screen.load() : -1; }

View view() {
  View v;
  v.ready = ready();
  v.screen = screen_shown();
  v.up = v.ready && recent(g_idle);
  v.copy_ok = g_copy_names_ok.load() && call::copy_ready();
  v.busy = g_busy.load();
  if (!g_cs_ready) return v;
  EnterCriticalSection(&g_cs);
  v.separate_ways = g_rows.separate_ways;
  if (v.screen >= 0 && g_rows.screen == v.screen) {
    v.count = g_rows.count;
    for (int i = 0; i < v.count; ++i) v.row[i] = g_rows.row[i];
  }
  if (g_note[0]) {
    std::snprintf(v.status, sizeof(v.status), "%s", g_note);
    v.status_error = g_note_error;
  } else if (!v.ready) {
    std::snprintf(v.status, sizeof(v.status), "%s", g_status);
    v.status_error = true;
  }
  LeaveCriticalSection(&g_cs);
  return v;
}

void request_delete(int row, const Row& seen) {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  if (g_nq < kQueue) {
    Req& r = g_queue[g_nq++];
    r = Req{};
    r.to = row;
    r.to_seen = seen;
  }
  g_note[0] = 0;
  LeaveCriticalSection(&g_cs);
}

void request_copy(int from_row, int to_row, const Row& from_seen, const Row& to_seen) {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  if (g_nq < kQueue) {
    Req& r = g_queue[g_nq++];
    r = Req{};
    r.copy = true;
    r.from = from_row;
    r.to = to_row;
    r.from_seen = from_seen;
    r.to_seen = to_seen;
  }
  g_note[0] = 0;
  LeaveCriticalSection(&g_cs);
}

void tick() {
  if (!g_cs_ready || !g_names_ok.load(std::memory_order_acquire)) return;
  const DWORD now = GetTickCount();
  if (!code_ok()) {
    static DWORD s_first = 0, s_last = 0;
    if (!s_first) s_first = now | 1;
    if (now - s_last >= 1000) {
      s_last = now;
      static bool s_logged = false;
      const bool late = stamp::age(s_first, now) > 60000;
      if (!resolve_code(late && !s_logged) && late) s_logged = true;
    }
    if (!code_ok()) return;
  }
  const uintptr_t sdm = save_manager();
  // Whether the game's saves are busy matters only while a list is up (the
  // panel's buttons wait for it) or a change is under way; a request checks
  // again for itself.
  const bool list_up = screen_shown() >= 0;
  const char* const busy_why = list_up || g_op.phase != kIdle ? busy_part(sdm) : nullptr;
  const bool busy = busy_why != nullptr;
  // The game's saves going busy, and idle again, while a list is up (the log
  // keeps a few dozen).
  static int s_busy_seen = -1, s_busy_logs = 0;  // busy (1) or idle (0) as last seen, -1 no list up
  if (!list_up) {
    s_busy_seen = -1;
  } else if (static_cast<int>(busy) != s_busy_seen) {
    if ((s_busy_seen >= 0 || busy) && s_busy_logs < 60) {
      ++s_busy_logs;
      if (busy) logf("save files: the game's saves are busy (%s)", busy_why);
      else logf("save files: the game's saves are idle again");
    }
    s_busy_seen = busy;
  }
  Rows rows;
  Req q[kQueue];
  int nq = 0;
  EnterCriticalSection(&g_cs);
  nq = g_nq;
  for (int i = 0; i < nq; ++i) q[i] = g_queue[i];
  g_nq = 0;
  if (nq > 0 || g_op.phase != kIdle) rows = g_rows;  // what a request or the change under way is checked against
  LeaveCriticalSection(&g_cs);
  for (int i = 0; i < nq; ++i) {
    if (!check_request(q[i], rows, sdm)) continue;
    if (q[i].copy) start_copy(q[i], rows, now);
    else start_delete(q[i], rows, now);
  }
  service_op(now, sdm, rows);
  g_busy = g_op.phase != kIdle || busy;
}

int request_system_save() {
  if (!g_names_ok.load(std::memory_order_acquire) || !call::system_save_ready()) return -1;
  if (g_op.phase != kIdle) return 0;  // a copy asks for one of its own at its end
  const uintptr_t sdm = save_manager();
  if (!sdm) return -1;
  if (saves_busy(sdm)) return 0;
  return call::request_system_save(sdm, K.system_slot) ? 1 : -1;
}

bool game_saves_busy() {
  if (!g_names_ok.load(std::memory_order_acquire)) return true;
  const uintptr_t sdm = save_manager();
  return !sdm || saves_busy(sdm);
}

}  // namespace re4cc::savefiles
