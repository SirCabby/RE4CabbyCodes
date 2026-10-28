#include "cheats.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "call.h"
#include "config.h"
#include "events.h"
#include "game.h"
#include "inventory.h"
#include "log.h"
#include "playtime_rules.h"
#include "stamp.h"

namespace re4cc::cheats {
namespace {

struct Def {
  const char* name;
  const char* key;      // in the ini
  const char* comment;  // the ini's defaults
};
const Def kDefs[kCount] = {
    {"God mode", "GodMode",
     "you take no damage; your health is held at its maximum (Luis's and the water scooter's too, and the time to save "
     "Luis never runs out)"},
    {"Ashley takes no damage", "AshleyGodMode", "the same for Ashley, and a hit no longer puts her down"},
    {"One hit kills", "OneHitKills", "any hit that does damage takes all of an enemy's health"},
    {"Infinite ammo", "InfiniteAmmo", "firing uses no rounds, bolts or grenades"},
    {"No durability loss", "NoDurabilityLoss", "a knife never wears down"},
    {"No armor durability loss", "NoArmorDurabilityLoss", "body armor never wears down, so it never breaks"},
    {"Steady aim", "SteadyAim", "no recoil kick, jolt or sway; the reticle is always focused, and shots go where it points"},
    {"Automatic QTEs", "AutoQTE", "every timed prompt is taken, and every grab broken, by itself"},
    {"Automatic shooting gallery", "AutoGallery", "the merchant's shooting gallery plays itself"},
    {"Change costumes in game", "InGameCostumes", "the pause menu's EXTRAS changes costumes too, not only accessories"},
    {"Freeze play time", "FreezePlaytime", "the play time the game shows and records stops"},
    {"Freeze countdown timer", "FreezeCountdown", "a countdown on screen stops (the Mercenaries' too)"},
    {"Save without counting", "NoSaveCount", "saving adds nothing to the save count"},
    {"Infinite money", "InfiniteMoney", "the merchant takes no pesetas"},
    {"Infinite spinels", "InfiniteSpinels", "the merchant takes no spinels for a trade"},
    {"Infinite upgrade tickets", "InfiniteTickets", "an exclusive upgrade paid with a ticket keeps the ticket"},
    {"Misses don't count", "NoMissCount", "a shot that misses is not counted in the results"},
    {"Deaths don't count", "NoDeathCount", "a death is not counted in the results"},
    {"Damage doesn't count", "NoDamageCount", "taking damage is not counted in the results"},
    {"Endless combo", "EndlessCombo", "the Mercenaries: a combo never runs out"},
    {"Mercenary Mode always on", "MercenaryMode", "the Mercenaries: Mercenary Mode never runs out"},
    {"Kill enemies as they appear", "AutoKill", "the Mercenaries: every enemy dies as it appears, its kill scored as yours"},
};
config::CheatKey g_keys[kCount];

volatile LONG g_on[kCount] = {};
// The panel changed a switch: service() writes them into the ini (config::save_cheats).
std::atomic<bool> g_switches_changed{false};
std::atomic<DWORD> g_switched_at{0};

CRITICAL_SECTION g_cs;  // g_published, g_req and g_last
Status g_published;
char g_last[160] = {};
// The panel's requests, applied by the next tick (-1: none).
struct Requests {
  int armor = -1, pesetas = -1, spinels = -1, tickets = -1, saves = -1, playtime = -1, countdown = -1, difficulty = -1;
  int counter[game::kCounters] = {-1, -1, -1, -1, -1};  // this chapter's
  int total[game::kCounters] = {-1, -1, -1, -1, -1};    // the whole game's
};
Requests g_req;

void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
  char text[160];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  EnterCriticalSection(&g_cs);
  std::snprintf(g_last, sizeof(g_last), "%s", text);
  LeaveCriticalSection(&g_cs);
  logf("%s", text);
}

void clear_note() {
  EnterCriticalSection(&g_cs);
  g_last[0] = 0;
  LeaveCriticalSection(&g_cs);
}

// A results counter's name, for the log.
const char* counter_name(game::Counter c) {
  switch (c) {
    case game::Counter::kDeaths: return "deaths";
    case game::Counter::kDamage: return "times damaged";
    case game::Counter::kKills: return "enemies killed";
    case game::Counter::kHits: return "hits";
    case game::Counter::kMisses: return "misses";
  }
  return "?";
}

// "H:MM:SS" for the log and the notes.
struct Hms {
  char text[24];
  explicit Hms(uint64_t us) {
    const unsigned long long s = us / game::kMicro;
    std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
  }
};

// --- what the tick holds (the tick's alone) ---------------------------------------------------------------
// God mode (the player, Luis) and Ashley: the character whose HitPoint.NoDamage the
// hold put on, so the switch going off takes it off that one again.
uintptr_t g_god_ctx = 0, g_luis_ctx = 0, g_ashley_ctx = 0;
// God mode on the water scooter: the one whose grace after a hit the hold keeps
// (let go when the switch goes off - hold_jet_ski checks it still holds the mod's
// values, so it outlives a game start), and the one the log has named.
uintptr_t g_jet_ski = 0, g_jet_ski_logged = 0;
// Separate Ways' time limit to save Luis, as its last frames before the pause menu
// left it: its frames (the hook's, events.cpp) stop with the game's while the menu
// is up, and the panel shows only then.
struct LuisTime {
  bool ok = false;
  float left = 0.0f;
};
LuisTime g_luis_time;

// Freeze play time: this chapter's lap held, each frame of a game, and the play
// time held carried over a load that kept the run's clock - a Continue after a
// death (playtime_rules.h).
rules::PlayHold g_play;
using rules::kMinLap;
rules::PlayLook look_of(const game::Clock& c) { return {c.now, c.start, c.laps, c.start_element}; }

// Freeze play time also holds the RESULTS screen's total play time: the game's
// system count, which runs on through the menus, cutscenes and pauses, in a game
// or not. Held at a whole second - the screen reads it a frame's growth later.
struct SystemHold {
  bool have = false;
  uint64_t held = 0;
};
SystemHold g_system;

// Freeze countdown: the main game's and Separate Ways' (TimerManager), and the Mercenaries'.
struct CountHold {
  bool have = false;
  bool running = false;    // it was running at the last tick
  float held = 0.0f;
  uintptr_t setting = 0;   // the countdown's settings: another is another countdown
};
CountHold g_count;
struct MercHold {
  bool have = false;
  float held = 0.0f;
  uintptr_t timer = 0;
};
MercHold g_merc_time;

// Save without counting: the count saves keep (the tick takes it while no save list is open).
int g_save_hold = -1;

// Steady aim: the shells' spread the mod took out, to put back. The spread is
// the weapon's user data, shared by every instance of it and kept for as long as
// the weapon's data stays loaded.
struct SpreadPatch {
  uintptr_t info = 0;     // the BulletShellMoveInfo
  int32_t weapon = -1;    // the weapon it was taken from
  float radius = 0.0f, fit = 0.0f;
};
constexpr int kMaxSpread = 64;
SpreadPatch g_spread[kMaxSpread];
int g_spreads = 0;
DWORD g_spread_checked = 0;

// The difficulty switch: CampaignManager.setDifficulty and GameRankSystem.recalcRank,
// the two calls the game makes when a game over's "lower the difficulty" takes
// effect. The pause menu is up (the tick's caller sees to that).
void switch_difficulty(int want) {
  const int was = game::difficulty_index();
  const uintptr_t mgr = game::campaign_manager();
  if (was < 0 || !mgr) return note("the difficulty cannot be switched right now");
  if (was == want) return;
  if (!call::difficulty_ready()) return note("the difficulty cannot be switched: %s", call::status());
  if (!call::set_difficulty(mgr, game::difficulty_value(want)))
    return note("the game did not switch the difficulty - see the log");
  const int now = game::difficulty_index();
  if (now != want) return note("the game kept the difficulty at %s", game::difficulty_name(now));
  clear_note();  // the panel's buttons show it; the log only
  logf("difficulty: %s -> %s", game::difficulty_name(was), game::difficulty_name(now));
}

// The body armor's editor: its durability set (up to its most), mending a broken
// one. An item's state: only while the pause menu is up and the attache case's own
// screen is not (it shows the armor).
void set_armor_durability(const game::Run& run, int v) {
  if (!run.paused || run.case_scene) return note("the body armor changes only while the pause menu is up");
  game::Inventories inv;
  game::Armor a;
  if (!game::inventories(&inv) || !game::body_armor(inv, &a)) return note("the body armor could not be read just now");
  if (!a.item) return note("you have no body armor");
  const int want = v > a.max ? a.max : v;
  if (!game::set_armor(a, want)) return note("the body armor's durability could not be set");
  clear_note();
  logf("body armor: durability %d%s -> %d (of %d)", a.durability, a.broken ? " (broken)" : "", want, a.max);
}

// The play time editor: this chapter's start moved so the game shows `t`. Less
// than the finished chapters hold takes the latest of them down; more than this
// chapter's clock can hold goes onto the latest finished one.
void set_play_time(uint64_t t) {
  game::Clock c;
  if (!game::clock(&c)) return note("the play time could not be set (no game running?)");
  if (t > game::kPlayTimeMax) t = game::kPlayTimeMax;
  if (t < c.laps + kMinLap) {
    game::cap_laps(t > kMinLap ? t - kMinLap : 0);
    if (!game::clock(&c)) return note("the play time could not be set");
  }
  uint64_t lap = t > c.laps ? t - c.laps : 0;
  if (lap < kMinLap) lap = kMinLap;
  bool short_of = false;
  if (lap + kMinLap > c.now) {
    // This chapter's start cannot go below the clock's own start.
    const uint64_t room = c.now > kMinLap ? c.now - kMinLap : 0;
    if (game::raise_last_lap(lap - room)) lap = room;
    else short_of = true, lap = room;
  }
  if (lap < kMinLap || !game::set_chapter_start(c.start_element, c.now - lap))
    return note("the play time could not be set");
  game::Clock after;
  const bool read = game::clock(&after);
  g_play.have = true;
  g_play.carried = false;
  g_play.lap = lap;
  rules::play_wrote(g_play, look_of(read ? after : c), c.now - lap);
  const uint64_t shown = read ? game::play_time(after) : t;
  // The RESULTS screen's total says the same (and a freeze holds it there).
  const uint64_t whole = shown / game::kMicro * game::kMicro;
  if (game::set_system_time(whole)) g_system.held = whole;
  else logf("play time: the RESULTS screen's total play time could not be set");
  if (short_of) {
    note("the play time can only go up to %s in the first chapter", Hms(shown).text);
  } else {
    clear_note();
    logf("play time set to %s", Hms(shown).text);
  }
}

// --- the holds, each frame of a game ---------------------------------------------------------------------
void hold_god(uintptr_t& held, uintptr_t ctx, bool on, const char* who) {
  if (on && ctx) {
    if (game::hold_health(ctx, true) && held != ctx) logf("%s: no damage, health held at its maximum", who);
    held = ctx;
  } else if (!on && held) {
    // Only the character it was put on, and only while it is still there.
    if (held == ctx) game::hold_health(ctx, false);
    held = 0;
  }
}

// The water scooter has a health of its own, which its crashes take: at none it
// sinks and the game is over, whatever Leon's health.
void hold_jet_ski(bool on) {
  const uintptr_t js = game::jet_ski();
  if (on && js) {
    if (game::hold_jet_ski(js, true) && g_jet_ski_logged != js) {
      g_jet_ski_logged = js;
      game::JetSki j;
      game::jet_ski_health(js, &j);
      logf("god mode: the water scooter takes no damage either - its hull held at %d of %d, no crash sinks it", j.hp,
           j.max);
    }
    g_jet_ski = js;
  } else if (!on && g_jet_ski) {
    if (js == g_jet_ski && game::hold_jet_ski(js, false)) logf("god mode: the water scooter's crashes count again");
    g_jet_ski = 0;
  }
}

void freeze_play_time(bool on) {
  if (!on) {
    g_play = rules::PlayHold{};
    return;
  }
  game::Clock c;
  if (!game::clock(&c)) return;
  const rules::PlayLook look = look_of(c);
  const bool had = g_play.have;
  uint64_t start = 0;
  rules::PlayTook took = rules::PlayTook::kOwn;
  if (!rules::play_look(g_play, look, &start, &took)) return;  // no lap under way (the game's own 999:59:59)
  if (took == rules::PlayTook::kGame && had && config::get().trace)
    logf("freeze play time: the game set the chapter's start (a new chapter or a load) - its lap %s is held",
         Hms(c.now - c.start).text);
  if (start != c.start && !game::set_chapter_start(c.start_element, start)) return;
  rules::play_wrote(g_play, look, start);
}

void hold_system_time(bool on) {
  uint64_t now = 0;
  if (!on || !game::system_time(&now)) {
    g_system.have = false;
    return;
  }
  if (!g_system.have) {
    g_system.have = true;
    g_system.held = now / game::kMicro * game::kMicro;
  }
  if (now != g_system.held) game::set_system_time(g_system.held);
}

void freeze_countdown(bool on) {
  game::Countdown c;
  const bool running = on && game::countdown(&c) && c.running;  // (off: nothing to read - a switch going on takes it afresh)
  if (!running) {
    g_count.have = false;
    g_count.running = false;
    return;
  }
  // A countdown that just started (or another one): its time now is what stays.
  if (!g_count.have || !g_count.running || c.setting != g_count.setting) {
    g_count.have = true;
    g_count.held = c.seconds;
    g_count.setting = c.setting;
    logf("freeze countdown: the countdown stays at %.1f s", static_cast<double>(c.seconds));
  } else if (c.seconds != g_count.held) {
    game::set_countdown(g_count.held);
  }
  g_count.running = true;
}

void mercenaries(const game::Mercs& m, bool time_on, bool combo_on, bool rush_on) {
  // The countdown: held, except that a time bonus raising it is kept.
  if (time_on && m.timer && m.timer_busy && !m.timed_out && m.remaining > 0.0f) {
    if (!g_merc_time.have || g_merc_time.timer != m.timer || m.remaining > g_merc_time.held) {
      if (!g_merc_time.have || g_merc_time.timer != m.timer)
        logf("freeze countdown: the Mercenaries' clock stays at %.1f s", static_cast<double>(m.remaining));
      g_merc_time.have = true;
      g_merc_time.timer = m.timer;
      g_merc_time.held = m.remaining;
    } else if (m.remaining < g_merc_time.held) {
      game::set_merc_remaining(m.timer, g_merc_time.held);
    }
  } else {
    g_merc_time.have = false;
  }
  if (combo_on && m.combo) {
    // The combo's timer counts up to its limit while it accepts kills: kept at 0.
    if (m.combo_on && m.combo_timer > 0.0f) game::set_merc_combo_timer(m.combo, 0.0f);
    // What the combo earns the game puts on the score only when the combo is over
    // (never, now): it goes on as it is earned.
    if (m.score && (m.stock_normal > 0 || m.stock_unique > 0)) {
      long long sum = static_cast<long long>(m.score_value) + m.stock_normal + m.stock_unique;
      if (sum > game::kMercScoreMax) sum = game::kMercScoreMax;
      if (game::set_merc_score(m.score, game::MercScore::kScore, static_cast<int>(sum))) {
        game::set_merc_score(m.score, game::MercScore::kStockNormal, 0);
        game::set_merc_score(m.score, game::MercScore::kStockUnique, 0);
        if (config::get().trace)
          logf("endless combo: %d + %d combo bonus onto the score (%lld)", m.stock_normal, m.stock_unique, sum);
      }
    }
  }
  if (rush_on && m.rush) {
    // Mercenary Mode's timer counts down from its limit: kept there while it runs,
    // and the gauge it starts from kept full.
    if (m.rush_busy && m.rush_limit > 0.0f && m.rush_timer < m.rush_limit) game::set_rush(m.rush, m.rush_limit, -1);
    if (m.gauges_max > 0 && m.gauges < m.gauges_max) game::set_rush(m.rush, -1.0f, m.gauges_max);
  }
}

// --- Kill enemies as they appear (the Mercenaries) ---------------------------------------------------------------------
// Every enemy in play gets a hit from the player: a request on the player's body
// hit controller for the enemy's body (a Ganado's whose parasite is out too), as
// the game's own direct attacks make theirs, of a shot of the gun in hand (game.h
// KillTarget). The game works it out with the player's own hits later in the frame;
// the hit hook raises its damage to the enemy's health, knowing it by its attack's
// number (events.h), so the enemy dies its own death and the Mercenaries scores the
// kill as the player's. An enemy is hit only once it is in play (game.h
// kill_targets: its game object started) and has been for a few frames; one still
// standing a moment later (its state turned the hit away) is asked for again, and
// one that stands through every try is left alone. A Ganado carrying a parasite
// keeps it in (game.h keep_parasite_in), so its hit kills it outright; one whose
// parasite came out anyway (the player's own shot, a group's call) is hit once
// the parasite is out. While it acts every area of the stage sends its enemies
// (events.h spawn_everywhere), so they keep coming wherever the player stands.
//   The user (2026-10-26), after the first version: "it seems I needed to walk
// around to trigger new spawns to come out ... some spawns didn't die and were
// attacking me" - a Ganado carrying a parasite is held alive (NoDeath) from its
// start until its lethal hit, and was passed over as one a hit could not kill.
struct KillAsk {
  uintptr_t context = 0;
  DWORD seen_at = 0;   // the last frame it was in play
  int settled = 0;     // frames it has been in play: hit from kKillSettle on
  DWORD asked_at = 0;  // the last hit asked for
  int tries = 0;
};
constexpr int kKillAsks = 64;
constexpr int kKillSettle = 12;      // frames in play before the first hit
constexpr DWORD kKillRetryMs = 400;  // an enemy still standing is asked for again after this
constexpr int kKillTries = 25;       // ... this many times (10 s), then left to the player
constexpr int kKillsPerTick = 8;     // requests a frame at most: a wave comes over a few frames
constexpr float kKillPointCooldown = 1.0f;  // a spawn point's wait between enemies (the game's: 2 to 32 s)
KillAsk g_kill_asks[kKillAsks];
// The log's pace line, every 15 s while it acts.
struct KillPace {
  DWORD since = 0;
  long asked = 0, landed_at = 0, opened_at = 0;
  long readied = 0;  // spawn points' cooldowns taken off
  int kills_at = -1;
  int coming = 0;  // the most seen coming into play at once
  DWORD stall_logged = 0;
};
KillPace g_kill_pace;
// The shot it hits with: the gun in hand's (the catalog read again when the gun
// changes, and every few seconds).
struct KillShot {
  int32_t weapon = INT32_MIN;
  uint32_t key = 0;
  bool own = false;
  DWORD at = 0;
};
KillShot g_kill_shot;
bool g_kill_waiting = false, g_kill_started = false, g_kill_kept_parasite = false, g_kill_readied_logged = false;
int g_kill_about = -1;
// A spawner held up: one that may send, a request waiting and room for another
// enemy, that has sent nothing for kKillStuckMs - none of its points out of the
// camera's view and far enough from the player (the Village's throwers stood so
// at 122 kills, 2026-10-26: their 4 to 6 points round the middle). Its points' own
// rules are then lifted (game.h lift_spawn_rules), put back when the cheat stops.
struct SpawnWatch {
  uintptr_t spawner = 0;
  int sent = -1;
  DWORD since = 0;
  bool lifted = false;
};
constexpr int kSpawnWatches = 96;
constexpr DWORD kKillStuckMs = 2000;
constexpr float kKillStuckDistance = 2.0f;  // the distance a held-up spawner's points send from (the game's: 8 to 20 m)
SpawnWatch g_spawn_watch[kSpawnWatches];
DWORD g_spawn_watch_at = 0;

void reset_auto_kill() {
  for (KillAsk& a : g_kill_asks) a = KillAsk{};
  for (SpawnWatch& w : g_spawn_watch) w = SpawnWatch{};
  g_spawn_watch_at = 0;
  g_kill_pace = KillPace{};
  g_kill_shot = KillShot{};
  g_kill_waiting = false;
  g_kill_started = false;
  g_kill_about = -1;
}

// Four times a second: each spawner held up with an enemy to send for kKillStuckMs
// gets its points' rules lifted, once.
void watch_spawners(const game::Mercs& m, DWORD now) {
  if (now - g_spawn_watch_at < 250) return;
  g_spawn_watch_at = now;
  game::SpawnerState s[96];
  const int n = game::spawner_states(m.controller, s, 96);
  for (int i = 0; i < n; ++i) {
    const game::SpawnerState& x = s[i];
    SpawnWatch* w = nullptr;
    for (SpawnWatch& e : g_spawn_watch)
      if (e.spawner == x.spawner) {
        w = &e;
        break;
      }
    for (SpawnWatch& e : g_spawn_watch)
      if (!w && !e.spawner) {
        e = SpawnWatch{};
        e.spawner = x.spawner;
        w = &e;
      }
    if (!w) continue;
    const bool waiting = x.enabled && !x.spent && x.requests > 0 && x.active >= 0 && x.active < x.limit && x.sent >= 0 &&
                         x.sent < x.holds;
    if (!waiting || x.sent != w->sent) {
      w->sent = x.sent;
      w->since = now;
      continue;
    }
    if (w->lifted || now - w->since < kKillStuckMs) continue;
    w->lifted = true;
    const int changed = game::lift_spawn_rules(m.controller, x.spawner, kKillStuckDistance);
    logf("kill enemies as they appear: spawner %08X held an enemy back for %lu s (%d of %d sent) - none of its %d point(s) "
         "out of view and far enough; their own rules lifted (%d value(s)): it sends in view too, from %.0f m",
         x.name_hash, (now - w->since) / 1000, x.sent, x.holds, x.points, changed, kKillStuckDistance);
  }
}

// The enemy's slot, or a free one taken for it (one out of play for 3 s is gone).
KillAsk* kill_slot(uintptr_t context, DWORD now) {
  KillAsk* free_slot = nullptr;
  for (KillAsk& a : g_kill_asks) {
    if (a.context == context) return &a;
    if (!free_slot && (!a.context || now - a.seen_at > 3000)) free_slot = &a;
  }
  if (free_slot) {
    *free_slot = KillAsk{};
    free_slot->context = context;
  }
  return free_slot;
}

// The kills stalled: what each of the mode's spawners is doing (game.h SpawnerState),
// by its name's hash - the ones that may send in full, the spent and the ones their
// flags (kill counts mostly) do not let send yet by hash alone.
void log_spawners(const game::Mercs& m) {
  g_kill_pace.stall_logged = GetTickCount();
  game::SpawnerState s[96];
  const int n = game::spawner_states(m.controller, s, 96);
  if (n <= 0) {
    logf("kill enemies as they appear: the kills stalled - the spawners could not be read");
    return;
  }
  char sending[2048] = "", spent[1024] = "", waiting[1024] = "";
  size_t ls = 0, lp = 0, lw = 0;
  int ns = 0, np = 0, nw = 0;
  for (int i = 0; i < n; ++i) {
    const game::SpawnerState& x = s[i];
    if (x.enabled && !x.spent) {
      if (ls < sizeof(sending))
        ls += std::snprintf(sending + ls, sizeof(sending) - ls, " %08X requests %d active %d/%d sent %d of %d, %d point(s), "
                            "%d cooling;", x.name_hash, x.requests, x.active, x.limit, x.sent, x.holds, x.points, x.cooling);
      ++ns;
    } else if (x.enabled) {
      if (lp < sizeof(spent)) lp += std::snprintf(spent + lp, sizeof(spent) - lp, " %08X", x.name_hash);
      ++np;
    } else {
      if (lw < sizeof(waiting))
        lw += std::snprintf(waiting + lw, sizeof(waiting) - lw, " %08X (%d of %d sent)", x.name_hash, x.sent, x.holds);
      ++nw;
    }
  }
  logf("kill enemies as they appear: the kills stalled - %d spawner(s) may send:%s %d spent:%s; %d not let send yet:%s",
       ns, sending, np, spent, nw, waiting);
}

void auto_kill(const game::Run& run, const game::Mercs& m) {
  const DWORD now = GetTickCount();
  // The clock running with time left (the panel's "waiting"), the pause menu down, the player standing.
  g_kill_waiting = !(m.active && m.timer_busy && !m.timed_out && m.remaining > 0.0f);
  if (g_kill_waiting || run.paused) return;
  const uintptr_t player = game::player_context();
  game::Health h;
  if (!player || !game::health(player, &h) || h.hp <= 0) return;
  // Every point of a spawner that can still send serves again a second after it sent
  // one (game.h ready_spawn_points; the game's own wait is 2 to 32 s): enemies keep
  // coming wherever the player stands, a few points sharing them.
  if (const int readied = game::ready_spawn_points(m.controller, kKillPointCooldown)) {
    g_kill_pace.readied += readied;
    if (!g_kill_readied_logged) {
      g_kill_readied_logged = true;
      logf("kill enemies as they appear: %d spawn point(s) cut short their cooldown - each sends again a second after "
           "its last (first time)",
           readied);
    }
  }
  watch_spawners(m, now);
  const uintptr_t attacker = game::body_hit_controller(player);
  const uintptr_t mgr = game::hit_manager();
  static bool s_no_attacker = false;
  if (!attacker || !mgr) {
    if (!s_no_attacker) logf("kill enemies as they appear: the player's hit controller or the HitManager is not there - waiting");
    s_no_attacker = true;
    return;
  }
  int32_t weapon = -1;
  game::equipped_gun(player, &weapon);
  if (weapon != g_kill_shot.weapon || !g_kill_shot.key || now - g_kill_shot.at > 5000) {
    g_kill_shot.weapon = weapon;
    g_kill_shot.at = now;
    g_kill_shot.key = game::shot_attack_key(weapon, &g_kill_shot.own);
    static bool s_no_shot = false;
    if (!g_kill_shot.key && !s_no_shot) logf("kill enemies as they appear: the HitManager holds no shot's attack data yet - waiting");
    s_no_shot = !g_kill_shot.key;
  }
  if (!g_kill_shot.key) return;

  game::KillTarget targets[48];
  game::KillScan scan;
  const int n = game::kill_targets(targets, 48, &scan);
  g_kill_about = scan.listed;
  if (!g_kill_pace.since) {
    g_kill_pace.since = now;
    g_kill_pace.landed_at = events::kill_hits();
    g_kill_pace.opened_at = events::spawn_opened();
    g_kill_pace.kills_at = m.kills;
  }
  if (scan.coming > g_kill_pace.coming) g_kill_pace.coming = scan.coming;
  static int s_unknown_logged = -1;
  if (scan.unknown_kind >= 0 && scan.unknown_kind != s_unknown_logged) {
    s_unknown_logged = scan.unknown_kind;
    logf("kill enemies as they appear: an enemy of kind %d is left alone - its hit controller has no damage effect, so "
         "whether it is in play cannot be told",
         scan.unknown_kind);
  }
  // Each enemy in play counts its frames; the ones settled and due a hit are asked for.
  int due[kKillsPerTick];
  int dues = 0;
  for (int i = 0; i < n; ++i) {
    KillAsk* ask = kill_slot(targets[i].context, now);
    if (!ask) continue;
    ask->seen_at = now;
    if (ask->settled < kKillSettle) {
      ++ask->settled;
      continue;
    }
    if (ask->tries >= kKillTries || (ask->tries && now - ask->asked_at < kKillRetryMs) || dues >= kKillsPerTick) continue;
    due[dues++] = i;
  }
  if (dues > 0) {
    call::Frame frame;  // the request's objects stay the frame's until the request holds them
    for (int d = 0; d < dues && frame.open(); ++d) {
      const game::KillTarget& t = targets[due[d]];
      KillAsk* ask = kill_slot(t.context, now);
      if (!ask) continue;
      const uintptr_t target = call::component_game_object(t.hit_controller);
      const uintptr_t attack = target ? call::new_object(game::attack_user_data_type()) : 0;
      const uintptr_t damage = attack ? call::new_object(game::damage_user_data_type()) : 0;
      const uint32_t id = damage ? call::new_attack_id(mgr) : 0;
      if (!id || !game::fill_kill_attack(attack, g_kill_shot.key, id) || !game::fill_kill_damage(damage)) {
        static bool s_logged = false;
        if (!s_logged)
          logf("kill enemies as they appear: a hit request could not be made (game object %d, objects %d/%d, attack number "
               "%u) - %s",
               target != 0, attack != 0, damage != 0, id, call::status());
        s_logged = true;
        break;
      }
      if (t.parasite_waits && game::keep_parasite_in(t.parasite_waits) && !g_kill_kept_parasite) {
        g_kill_kept_parasite = true;
        logf("kill enemies as they appear: a Ganado carrying a parasite - the parasite kept in, so the hit kills it "
             "(first time)");
      }
      events::note_kill_attack(id);  // before the game works it out (later in the frame)
      if (!call::request_attack(attacker, target, attack, damage)) break;
      ask->asked_at = now;
      ++g_kill_pace.asked;
      if (++ask->tries == kKillTries)
        logf("kill enemies as they appear: an enemy (kind %d, %d HP%s) still stands after %d hits asked for - its hits "
             "are turned away; left to you",
             t.kind, t.hp, t.parasite ? ", its parasite out" : "", kKillTries);
      if (!g_kill_started) {
        g_kill_started = true;
        logf("kill enemies as they appear: the first hit asked for - an enemy (kind %d, %d HP%s), with %s shot (attack "
             "data %08X)",
             t.kind, t.hp, t.parasite ? ", its parasite out" : "", g_kill_shot.own ? "the gun in hand's" : "the game's default",
             g_kill_shot.key);
      }
    }
  }
  if (now - g_kill_pace.since >= 15000) {
    const long landed = events::kill_hits(), opened = events::spawn_opened();
    game::SpawnCounts sc;
    char counts[96] = "";
    if (game::spawn_counts(m.controller, &sc))
      std::snprintf(counts, sizeof(counts), "; %d active (spawning stops at %d, the Mercenaries' %d)", sc.active,
                    sc.game_limit, sc.mode_limit);
    logf("kill enemies as they appear: %ld hits asked for in %lu s, %ld reached an enemy; %d kills (+%d), %d enemies "
         "about, up to %d at once coming into play%s; spawn points opened %ld times (%d of 3 checks hooked), %ld taken "
         "off their cooldown",
         g_kill_pace.asked, (now - g_kill_pace.since) / 1000, landed - g_kill_pace.landed_at, m.kills,
         m.kills - g_kill_pace.kills_at, g_kill_about, g_kill_pace.coming, counts, opened - g_kill_pace.opened_at,
         events::spawn_hooked(), g_kill_pace.readied);
    // Kills stalling: every spawner's state, for the next look (at most every 30 s).
    if (m.kills - g_kill_pace.kills_at <= 2 && (!g_kill_pace.stall_logged || now - g_kill_pace.stall_logged >= 30000))
      log_spawners(m);
    g_kill_pace.since = now;
    g_kill_pace.asked = 0;
    g_kill_pace.landed_at = landed;
    g_kill_pace.opened_at = opened;
    g_kill_pace.readied = 0;
    g_kill_pace.kills_at = m.kills;
    g_kill_pace.coming = 0;
  }
}

SpreadPatch* find_spread(uintptr_t info) {
  for (int i = 0; i < g_spreads; ++i)
    if (g_spread[i].info == info) return &g_spread[i];
  return nullptr;
}

void steady_aim(uintptr_t player) {
  // The camera: no recoil, no hand shake (with any gun, the mounted ones too).
  game::steady_camera(player);
  int32_t id = -1;
  const uintptr_t gun = game::equipped_gun(player, &id);
  if (!gun) return;
  game::focus_reticle(gun);
  uintptr_t infos[2];
  const int n = game::shell_move_infos(gun, infos, 2);
  for (int i = 0; i < n; ++i) {
    float r = 0.0f, f = 0.0f;
    if (!game::spread(infos[i], &r, &f) || (r == 0.0f && f == 0.0f)) continue;  // none (the mod's, or its own)
    // Taken again when the weapon's own values are back (its data loaded anew).
    SpreadPatch* p = find_spread(infos[i]);
    if (!p) {
      if (g_spreads >= kMaxSpread) continue;
      p = &g_spread[g_spreads++];
    }
    p->info = infos[i];
    p->weapon = id;
    p->radius = r;
    p->fit = f;
    if (game::set_spread(infos[i], 0.0f, 0.0f))
      logf("steady aim: weapon %d's shots go straight (its spread was %.3f, %.3f focused)", id,
           static_cast<double>(r), static_cast<double>(f));
  }
}

// The spread put back: on each of the player's guns whose shells the mod took it
// out of - the same object, from the same weapon, still without any.
void restore_spread(uintptr_t player) {
  if (!g_spreads || !player) return;
  uintptr_t guns[48];
  int32_t ids[48];
  const int n = game::player_guns(player, guns, ids, 48);
  for (int g = 0; g < n; ++g) {
    uintptr_t infos[2];
    const int m = game::shell_move_infos(guns[g], infos, 2);
    for (int i = 0; i < m; ++i) {
      SpreadPatch* p = find_spread(infos[i]);
      float r = 0.0f, f = 0.0f;
      if (!p || p->weapon != ids[g] || !game::spread(infos[i], &r, &f)) continue;
      if (r == 0.0f && f == 0.0f && game::set_spread(infos[i], p->radius, p->fit))
        logf("steady aim: weapon %d's spread is back (%.3f, %.3f focused)", ids[g], static_cast<double>(p->radius),
             static_cast<double>(p->fit));
      *p = g_spread[--g_spreads];
    }
  }
}

// Save without counting: the count a save keeps, taken while no save list is
// open (events.cpp writes it over the game's +1 in the save's own call). A save
// that fails takes 1 off again once the list has it - put back after one the
// hook held.
void hold_save_count(const game::Run& run, bool on) {
  if (!on || !run.in_game) {
    if (g_save_hold >= 0) events::set_save_hold(-1);
    g_save_hold = -1;
    return;
  }
  if (run.save_load) return;
  const int now = game::save_count();
  if (now < 0) return;
  const unsigned long held_at = events::last_save_held();
  if (g_save_hold > 0 && now == g_save_hold - 1 && stamp::within(held_at, 120000)) {
    if (game::set_save_count(g_save_hold))
      logf("save without counting: a failed save took the count to %d - put back to %d", now, g_save_hold);
    return;
  }
  if (now != g_save_hold) {
    if (g_save_hold < 0) logf("save without counting: saves keep the count at %d", now);
    g_save_hold = now;
    events::set_save_hold(now);
  }
}

// What the panel shows, read while it is up.
void snapshot(Status& s) {
  const uintptr_t player = game::player_context();
  s.health_ok = player && game::health(player, &s.health);
  const uintptr_t ashley = game::ashley_context();
  s.ashley_ok = ashley && game::health(ashley, &s.ashley);
  const uintptr_t luis = game::luis_context();
  s.luis_ok = luis && game::health(luis, &s.luis);
  s.jet_ski_ok = game::jet_ski_health(game::jet_ski(), &s.jet_ski) && s.jet_ski.running;
  s.luis_time_ok = g_luis_time.ok;
  s.luis_time_left = g_luis_time.left;
  s.luis_time_held = enabled(kGodMode);  // (its hook holds it at its next frame)
  game::Inventories inv;
  const bool have_inv = game::inventories(&inv);
  s.armor_ok = have_inv && game::body_armor(inv, &s.armor);
  s.pesetas = game::pesetas();
  s.spinels = game::spinels();
  s.tickets = have_inv ? game::upgrade_tickets(inv) : -1;
  s.saves = game::save_count();
  game::Clock c;
  s.clock_ok = game::clock(&c) && c.start <= c.now;
  s.play_seconds = s.clock_ok ? static_cast<long long>(game::play_time(c) / game::kMicro) : -1;
  s.play_held = enabled(kFreezePlaytime) && g_play.have;
  uint64_t total = 0;
  s.results_seconds = game::system_time(&total) ? static_cast<long long>(total / game::kMicro) : -1;
  s.results_held = enabled(kFreezePlaytime) && g_system.have;
  s.countdown_ok = game::countdown(&s.countdown);
  s.countdown_held = enabled(kFreezeCountdown) && g_count.have;
  s.merc_ok = s.run.mode == game::Mode::kMercenaries && game::mercs(&s.merc);
  s.kill_about = enabled(kAutoKill) ? g_kill_about : -1;
  s.kill_waiting = enabled(kAutoKill) && g_kill_waiting;
  s.stats_ok = game::stats(&s.stats);
  s.difficulty = game::difficulty_index();
  s.rank_ok = game::rank(&s.rank);
  s.enemies = game::enemy_count();
  s.gallery = events::gallery_controller() != 0;
  s.gallery_score = events::gallery_score();
}

}  // namespace

const config::CheatKey* config_keys() {
  for (int i = 0; i < kCount; ++i) g_keys[i] = config::CheatKey{kDefs[i].key, kDefs[i].comment};
  return g_keys;
}

void init() {
  InitializeCriticalSection(&g_cs);
  const config::Settings& c = config::get();
  for (int i = 0; i < kCount; ++i) g_on[i] = c.cheat[i] ? 1 : 0;
}

const char* name(Kind k) { return k >= 0 && k < kCount ? kDefs[k].name : "?"; }
const char* ini_key(Kind k) { return k >= 0 && k < kCount ? kDefs[k].key : "?"; }
bool enabled(Kind k) { return k >= 0 && k < kCount && g_on[k] != 0; }

void set_enabled(Kind k, bool on) {
  if (k < 0 || k >= kCount) return;
  if (InterlockedExchange(&g_on[k], on ? 1 : 0) == (on ? 1 : 0)) return;
  g_switched_at.store(GetTickCount(), std::memory_order_relaxed);
  g_switches_changed.store(true, std::memory_order_release);
}

void service() {
  if (!g_switches_changed.load(std::memory_order_acquire)) return;
  // A few switches clicked in a row are written once, half a second after the last.
  if (GetTickCount() - g_switched_at.load(std::memory_order_relaxed) < 500) return;
  g_switches_changed.store(false, std::memory_order_relaxed);
  bool on[kCount];
  for (int i = 0; i < kCount; ++i) on[i] = enabled(static_cast<Kind>(i));
  static bool s_logged = false;
  if (config::save_cheats(on, kCount) && !s_logged) {
    s_logged = true;
    logf("config: the cheats' switches are saved to RE4CabbyCodes.ini as the panel changes them - the next start "
         "begins with them");
  }
}

bool available(Kind k) {
  const game::Parts& p = game::parts();
  switch (k) {
    case kGodMode: return p.players;
    case kAshleyGod: return p.partner && events::ready(k);
    case kSteadyAim: return p.aim;
    case kCostumes: return p.costumes;
    case kFreezePlaytime: return p.clock;
    case kFreezeCountdown: return p.countdown || p.mercs;
    case kEndlessCombo:
    case kMercMode: return p.mercs;
    case kAutoKill: return p.auto_kill && call::kill_ready() && events::ready(k);
    default: return events::ready(k);
  }
}

const char* why_not(Kind k) {
  const game::Parts& p = game::parts();
  switch (k) {
    case kGodMode: return "the player's health was not found (see the log)";
    case kAshleyGod: return !p.partner ? "Ashley's health was not found (see the log)" : events::why_not(k);
    case kSteadyAim: return "the guns' aim was not found (see the log)";
    case kCostumes: return "the costume menu was not found (see the log)";
    case kFreezePlaytime: return "the play time was not found (see the log)";
    case kFreezeCountdown: return "the countdowns were not found (see the log)";
    case kEndlessCombo:
    case kMercMode: return "the Mercenaries' counters were not found (see the log)";
    case kAutoKill:
      if (!p.auto_kill) return "the enemies or the shots' attack data were not found (see the log)";
      if (!call::kill_ready()) return call::ready() ? "the game's hit request was not found (see the log)" : call::status();
      return events::why_not(k);
    default: return events::why_not(k);
  }
}

Status status() {
  EnterCriticalSection(&g_cs);
  Status s = g_published;
  LeaveCriticalSection(&g_cs);
  return s;
}

namespace {
void request(int Requests::*field, int v) {
  EnterCriticalSection(&g_cs);
  g_req.*field = v < 0 ? 0 : v;
  LeaveCriticalSection(&g_cs);
}
}  // namespace

void request_armor(int durability) { request(&Requests::armor, durability); }
void request_pesetas(int v) { request(&Requests::pesetas, v > game::kPesetasMax ? game::kPesetasMax : v); }
void request_spinels(int v) { request(&Requests::spinels, v > game::kSpinelsMax ? game::kSpinelsMax : v); }
void request_tickets(int v) { request(&Requests::tickets, v); }
void request_saves(int v) { request(&Requests::saves, v > game::kSavesMax ? game::kSavesMax : v); }
void request_playtime(int seconds) { request(&Requests::playtime, seconds); }
void request_countdown(int seconds) { request(&Requests::countdown, seconds > 5999 ? 5999 : seconds); }
void request_difficulty(int index) {
  if (index >= 0 && index < game::kDifficulties) request(&Requests::difficulty, index);
}
void request_counter(game::Counter c, int v) {
  const int i = static_cast<int>(c);
  if (i < 0 || i >= game::kCounters) return;
  EnterCriticalSection(&g_cs);
  g_req.counter[i] = v < 0 ? 0 : v;
  LeaveCriticalSection(&g_cs);
}
void request_total(game::Counter c, int v) {
  const int i = static_cast<int>(c);
  if (i < 0 || i >= game::kCounters) return;
  EnterCriticalSection(&g_cs);
  g_req.total[i] = v < 0 ? 0 : v;
  LeaveCriticalSection(&g_cs);
}

void tick(const game::Run& run) {
  static bool s_in_game = false, s_paused = false;
  static bool s_on[kCount] = {};
  static DWORD s_refreshed = 0;
  const bool started = run.in_game && !s_in_game;
  const bool opened = run.paused && !s_paused;
  s_in_game = run.in_game;
  s_paused = run.paused;
  bool refresh = opened || (started && config::get().always_show);
  if (started) {
    // A game's characters and clock are new: the holds take them afresh - but
    // for a load that kept the run's clock, across which the play time held
    // carries over (playtime_rules.h). Taken afresh there, it took the lap the
    // game shows then: everything since the save that load brought back.
    g_god_ctx = g_luis_ctx = g_ashley_ctx = 0;
    g_luis_time = LuisTime{};
    uint64_t ahead = 0;
    const bool kept = g_play.have && game::clock_kept(&ahead);
    if (kept)
      logf("freeze play time: the game kept its clock across this load (%s past the save it loaded: a Continue after a "
           "death, or the save Continue would load) - the play time held stays %s",
           Hms(ahead).text, Hms(g_play.shown).text);
    rules::play_started(g_play, kept);
    g_count = CountHold{};
    g_merc_time = MercHold{};
    g_save_hold = -1;
    reset_auto_kill();
  }

  // --- the panel's requests ---
  EnterCriticalSection(&g_cs);
  const Requests req = g_req;
  g_req = Requests{};
  LeaveCriticalSection(&g_cs);
  bool any = req.armor >= 0 || req.pesetas >= 0 || req.spinels >= 0 || req.tickets >= 0 || req.saves >= 0 ||
             req.playtime >= 0 || req.countdown >= 0 || req.difficulty >= 0;
  for (int i = 0; i < game::kCounters; ++i) any |= req.counter[i] >= 0 || req.total[i] >= 0;
  if (any) {
    refresh = true;
    if (!run.in_game) {
      note("nothing was changed: no game is being played");
    } else {
      if (req.armor >= 0) set_armor_durability(run, req.armor);
      if (req.pesetas >= 0) {
        if (game::set_pesetas(req.pesetas)) logf("pesetas set to %d", req.pesetas);
        else note("the pesetas could not be set");
      }
      if (req.spinels >= 0) {
        if (game::set_spinels(req.spinels)) logf("spinels set to %d", req.spinels);
        else note("the spinels could not be set");
      }
      if (req.tickets >= 0) {
        char why[160];
        if (!inventory::set_tickets(run, req.tickets, why, sizeof(why))) note("the upgrade tickets could not be set: %s", why);
      }
      if (req.saves >= 0) {
        if (game::set_save_count(req.saves)) {
          if (g_save_hold >= 0) {
            g_save_hold = req.saves;
            events::set_save_hold(req.saves);
          }
          logf("save count set to %d", req.saves);
        } else {
          note("the save count could not be set");
        }
      }
      if (req.playtime >= 0) set_play_time(static_cast<uint64_t>(req.playtime) * game::kMicro);
      if (req.countdown >= 0) {
        game::Countdown c;
        if (game::countdown(&c) && c.running && game::set_countdown(static_cast<float>(req.countdown))) {
          if (g_count.have) g_count.held = static_cast<float>(req.countdown);
          logf("countdown set to %d s", req.countdown);
        } else {
          note("no countdown is running");
        }
      }
      for (int i = 0; i < game::kCounters; ++i) {
        const auto c = static_cast<game::Counter>(i);
        if (req.counter[i] >= 0) {
          if (game::set_counter(c, req.counter[i])) logf("results: this chapter's %s set to %d", counter_name(c), req.counter[i]);
          else note("the results could not be changed");
        }
        if (req.total[i] >= 0) {
          if (game::set_total(c, req.total[i])) logf("results: the whole game's %s set to %d", counter_name(c), req.total[i]);
          else note("the whole game's results could not all be changed");
        }
      }
      if (req.difficulty >= 0) {
        if (run.paused && run.mode != game::Mode::kMercenaries) switch_difficulty(req.difficulty);
        else note("the difficulty is only switched while the pause menu is up");
      }
    }
  }

  // --- Kill enemies as they appear lasts one Mercenaries match ---
  // Switched off as a match ends (its results, a game over, a quit, a retry) and as the
  // next begins - the switch is only shown in a match's pause menu, so nothing the player
  // set is undone there, and a switch the ini kept on from an earlier start does not
  // carry over. The ini is written as for the panel's switches. The user (2026-10-26):
  // "update the auto kill cheat to be disabled after each game so that the player must
  // turn it on after starting a new mercenaries match".
  {
    static bool s_in_match = false;
    const bool in_match = run.in_game && run.mode == game::Mode::kMercenaries;
    if (in_match != s_in_match) {
      s_in_match = in_match;
      if (enabled(kAutoKill)) {
        set_enabled(kAutoKill, false);
        logf(in_match ? "kill enemies as they appear: off as the match begins - it lasts one match; switch it on in the "
                        "pause menu"
                      : "kill enemies as they appear: switched off - the match is over (it lasts one match)");
      }
    }
  }

  // --- switches flipped since the last tick ---
  for (int k = 0; k < kCount; ++k) {
    const bool on = enabled(static_cast<Kind>(k));
    if (on == s_on[k]) continue;
    s_on[k] = on;
    refresh = true;
    if (!on && k == kSteadyAim) g_spread_checked = 0;  // put back at once
  }

  // --- the holds ---
  bool spawn_everywhere = false;  // Kill enemies as they appear at work: every area sends (events.cpp)
  if (run.in_game) {
    // The characters, read only for a switch that is on (or a hold to take off).
    const bool god = enabled(kGodMode), ashley = enabled(kAshleyGod);
    const bool want_player = god || g_god_ctx || enabled(kAutoQte) || enabled(kSteadyAim) || g_spreads;
    const uintptr_t player = want_player ? game::player_context() : 0;
    if (god || g_god_ctx) hold_god(g_god_ctx, player, god, "god mode");
    // Luis beside you has a health of his own: at none he dies and the game is over.
    if (god || g_luis_ctx) hold_god(g_luis_ctx, game::luis_context(), god, "god mode (Luis)");
    if (god || g_jet_ski) hold_jet_ski(god);
    if (!run.paused) g_luis_time.ok = events::luis_time(&g_luis_time.left);
    if (ashley || g_ashley_ctx) hold_god(g_ashley_ctx, game::ashley_context(), ashley, "ashley");
    if (enabled(kAutoQte) && !run.paused && player) game::press_dodge(player);
    if (enabled(kSteadyAim) && player) {
      steady_aim(player);
    } else if (g_spreads && GetTickCount() - g_spread_checked >= 1000) {
      g_spread_checked = GetTickCount();
      restore_spread(player);
    }
    freeze_play_time(enabled(kFreezePlaytime));
    freeze_countdown(enabled(kFreezeCountdown));
    if (run.mode == game::Mode::kMercenaries &&
        (enabled(kFreezeCountdown) || enabled(kEndlessCombo) || enabled(kMercMode) || enabled(kAutoKill))) {
      game::Mercs m;
      const bool active = game::mercs(&m) && m.active;
      if (active)
        mercenaries(m, enabled(kFreezeCountdown), enabled(kEndlessCombo), enabled(kMercMode));
      else
        g_merc_time.have = false;
      if (enabled(kAutoKill) && available(kAutoKill)) {
        spawn_everywhere = active;
        if (active) auto_kill(run, m);
        else g_kill_waiting = true;
      }
    }
  }
  events::spawn_everywhere(spawn_everywhere);
  // The spawn rules held-up spawners had lifted, put back once the cheat stops acting (switched
  // off, the stage over, out of the Mercenaries) - while the stage's objects are still up.
  if (!spawn_everywhere && game::spawn_rules_lifted()) {
    game::Mercs mm;
    game::mercs(&mm);
    const int put_back = game::restore_spawn_rules(mm.controller);
    logf("kill enemies as they appear: the held-up spawners' own rules put back (%d value(s))", put_back);
  }
  if ((!enabled(kAutoKill) || run.mode != game::Mode::kMercenaries) && (g_kill_pace.since || g_kill_waiting))
    reset_auto_kill();
  hold_save_count(run, enabled(kNoSaveCount));
  hold_system_time(enabled(kFreezePlaytime));
  // Infinite spinels: the count a change the merchant's screen hears is measured
  // from (events.cpp), noted every frame - switch on or off, so a cancelled trade's
  // refund of spinels given back is still taken out after the switch went off.
  events::note_spinels(run.in_game ? game::spinels() : -1);

  // Costumes in game: the costumes' entries of the game's EXTRAS opened while the
  // pause menu is up (it is the only place EXTRAS opens from in play), greyed for
  // the moments a costume must not change; put back as the game had them when the
  // switch goes off.
  {
    static bool s_opened = false;
    static DWORD s_costume_at = 0;
    const bool want = enabled(kCostumes) && run.in_game && run.mode != game::Mode::kMercenaries;
    if (want && run.paused && GetTickCount() - s_costume_at >= 250) {
      s_costume_at = GetTickCount();
      const bool held = game::costume_change_unsafe();
      if (game::costume_menu(true, held) && !s_opened)
        logf("costumes in game: the pause menu's EXTRAS changes costumes too%s", held ? " (greyed for now)" : "");
      s_opened = true;
    } else if (!enabled(kCostumes) && s_opened) {
      s_opened = false;
      if (game::costume_menu(false, false)) logf("costumes in game: the costume menu is the game's own again");
    }
  }

  // --- the panel's figures: while it is up, twice a second ---
  const DWORD now = GetTickCount();
  const bool shown = run.paused || run.save_load || config::get().always_show;
  if (shown && now - s_refreshed >= 500) refresh = true;
  if (!refresh && run.in_game == g_published.run.in_game && run.mode == g_published.run.mode) return;
  Status s;
  s.run = run;
  if (run.in_game && refresh) {
    snapshot(s);
    s_refreshed = now;
  } else {
    EnterCriticalSection(&g_cs);
    const Status was = g_published;
    LeaveCriticalSection(&g_cs);
    if (run.in_game) {
      s = was;
      s.run = run;
    }
  }
  EnterCriticalSection(&g_cs);
  std::snprintf(s.last_action, sizeof(s.last_action), "%s", g_last);
  g_published = s;
  LeaveCriticalSection(&g_cs);
}

void remove_hooks() {
  for (auto& v : g_on) InterlockedExchange(&v, 0);
}

}  // namespace re4cc::cheats
