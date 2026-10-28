// Offline test of the results' rules (src/results_rules.h): a whole game's count
// spread over the chapters, and an accuracy element's hits and misses set apart.
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_results.cpp -o tests/build/test_results.exe
//   wine tests/build/test_results.exe
//
// The whole game's count (the panel's "the whole game" column): this chapter
// takes what the finished chapters do not hold - and what it cannot hold goes
// onto the latest finished ones; a count below the finished chapters' leaves
// this chapter none and takes the latest finished ones down, never one below 0;
// a count not known is left alone. The hits keep the misses and the misses the
// hits, the shots fired at most 999,999.
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "../src/results_rules.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

// have: the finished chapters' then this chapter's; every one holding up to `max`.
std::vector<int> spread(std::initializer_list<int> have, int want, bool* ok, int max = 9999) {
  std::vector<int> h(have), room(h.size()), out(h.size());
  for (size_t i = 0; i < h.size(); ++i) room[i] = h[i] >= 0 && h[i] < max ? max - h[i] : 0;
  *ok = re4cc::game::rules::spread_total(h.data(), room.data(), static_cast<int>(h.size()) - 1, want, out.data());
  return out;
}

bool same(const std::vector<int>& a, std::initializer_list<int> b) { return a == std::vector<int>(b); }

}  // namespace

int main() {
  using re4cc::game::rules::set_shots;
  bool ok = false;

  // Finished chapters 3, 5, 2 (10); this chapter 2: the whole game 12.
  check(same(spread({3, 5, 2, 2}, 12, &ok), {3, 5, 2, 2}) && ok, "the same total changes nothing");
  check(same(spread({3, 5, 2, 2}, 20, &ok), {3, 5, 2, 10}) && ok, "a higher total goes onto this chapter");
  check(same(spread({3, 5, 2, 2}, 10, &ok), {3, 5, 2, 0}) && ok, "the finished chapters' sum leaves this chapter none");
  check(same(spread({3, 5, 2, 2}, 5, &ok), {3, 2, 0, 0}) && ok, "a lower total takes the latest finished chapters down");
  check(same(spread({3, 5, 2, 2}, 0, &ok), {0, 0, 0, 0}) && ok, "0 empties every chapter");
  check(same(spread({0, 0, 0, 7}, 3, &ok), {0, 0, 0, 3}) && ok, "nothing finished: this chapter alone");
  check(same(spread({7}, 4, &ok), {4}) && ok, "no finished chapters at all");
  // More than this chapter holds: the rest onto the latest finished chapters.
  check(same(spread({9000, 9990, 9999}, 9000 + 9990 + 9999 + 5, &ok), {9000, 9995, 9999}) && ok,
        "what this chapter cannot hold goes onto the latest finished chapter");
  check(same(spread({9000, 9990, 9999}, 9000 + 9990 + 9999 + 15, &ok), {9006, 9999, 9999}) && ok,
        "... and what that one cannot hold onto the one before");
  check(same(spread({9999, 9999, 9999}, 40000, &ok), {9999, 9999, 9999}) && !ok, "more than they all hold: as full as they go, and false");
  // A finished chapter whose count is not known is left alone, and not counted.
  check(same(spread({3, -1, 2, 1}, 2, &ok), {2, -1, 0, 0}) && ok, "an unknown chapter is skipped when taking down");
  check(same(spread({3, -1, 2, 1}, 9, &ok), {3, -1, 2, 4}) && ok, "an unknown chapter is not part of the sum");
  // This chapter not known: nothing changes.
  check(same(spread({3, 2, -1}, 9, &ok), {3, 2, -1}) && !ok, "this chapter not known: nothing");
  check(same(spread({3, 2, 1}, -1, &ok), {3, 2, 1}) && !ok, "a negative total: nothing");

  // The hits and misses: attempts 40, hits 30 (10 misses).
  int at = 0, hi = 0;
  set_shots(40, 30, false, 35, 999999, &at, &hi);
  check(at == 45 && hi == 35, "hits set: the misses kept, the shots follow");
  set_shots(40, 30, true, 0, 999999, &at, &hi);
  check(at == 30 && hi == 30, "misses set to 0: the shots are the hits");
  set_shots(40, 30, true, 25, 999999, &at, &hi);
  check(at == 55 && hi == 30, "misses set: the hits kept");
  set_shots(40, 30, false, 0, 999999, &at, &hi);
  check(at == 10 && hi == 0, "hits set to 0: the misses stay");
  set_shots(999990, 999980, false, 999999, 999999, &at, &hi);
  check(at == 999999 && hi == 999999, "hits at the most: the misses give way");
  set_shots(10, 2, true, 999999, 999999, &at, &hi);
  check(at == 999999 && hi == 2, "misses at the most: the shots stop at 999,999");
  set_shots(5, 9, true, 3, 999999, &at, &hi);
  check(at == 12 && hi == 9, "hits above the attempts read as no misses");
  // Spread over the misses of chapters: each one's room is what its shots leave.
  {
    const int have[] = {4, 6, 1}, room[] = {999999 - 10, 999999 - 20, 999999 - 5};
    int out[3];
    ok = re4cc::game::rules::spread_total(have, room, 2, 0, out);
    check(ok && out[0] == 0 && out[1] == 0 && out[2] == 0, "the whole game's misses to 0: every chapter's");
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
