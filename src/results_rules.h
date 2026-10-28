#pragma once

// The results' rules on plain values - no game memory here (game.cpp applies
// them; tests/test_results.cpp checks them).
namespace re4cc::game::rules {

// The whole game's count set to `want`, spread over the chapters' counts:
// have[0..n-1] the finished chapters' (oldest first), have[n] this chapter's,
// room[i] how much more each can hold. This chapter takes what the finished
// chapters do not hold, and what it cannot hold goes onto the latest finished
// ones; a count below the finished chapters' leaves this chapter none and takes
// the latest finished ones down. A count not known (-1) is left as it is.
// out[0..n] the new counts; false when they cannot add up to `want`.
inline bool spread_total(const int* have, const int* room, int n, int want, int* out) {
  long long finished = 0;
  for (int i = 0; i <= n; ++i) out[i] = have[i];
  for (int i = 0; i < n; ++i)
    if (have[i] > 0) finished += have[i];
  if (want < 0 || have[n] < 0) return false;
  if (want >= finished) {
    long long rest = want - finished;
    const long long fits = static_cast<long long>(have[n]) + (room[n] > 0 ? room[n] : 0);
    out[n] = static_cast<int>(rest < fits ? rest : fits);
    rest -= out[n];
    for (int i = n - 1; i >= 0 && rest > 0; --i) {
      if (have[i] < 0 || room[i] <= 0) continue;
      const long long add = rest < room[i] ? rest : room[i];
      out[i] = have[i] + static_cast<int>(add);
      rest -= add;
    }
    return rest == 0;
  }
  out[n] = 0;
  long long over = finished - want;
  for (int i = n - 1; i >= 0 && over > 0; --i) {
    if (have[i] <= 0) continue;
    const long long take = over < have[i] ? over : have[i];
    out[i] = have[i] - static_cast<int>(take);
    over -= take;
  }
  return over == 0;
}

// An accuracy element's hits or misses (`misses`) set to v: the other kept,
// the shots fired (attempts = hits + misses) at most `max`. Hits above the
// attempts (never the game's own) read as no misses.
inline void set_shots(int attempts, int hits, bool misses, int v, int max, int* out_attempts, int* out_hits) {
  int h = misses ? hits : v;
  int m = misses ? v : attempts > hits ? attempts - hits : 0;
  if (h < 0) h = 0;
  if (h > max) h = max;
  if (m < 0) m = 0;
  if (m > max - h) m = max - h;
  *out_attempts = h + m;
  *out_hits = h;
}

}  // namespace re4cc::game::rules
