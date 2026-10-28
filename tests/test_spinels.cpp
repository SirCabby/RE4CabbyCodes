// Offline test of Infinite spinels' rule (src/spinels_rules.h): what the merchant's
// screen's hook does with a change of the spinels it hears.
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_spinels.cpp -o tests/build/test_spinels.exe
//   wine tests/build/test_spinels.exe
//
// A trade lowers them and is given back while the switch is on; a trade of an
// item placed in the case takes them as its placement opens, and a cancel's
// refund of what that gave back is taken out again - the switch on or not by
// then - while any other rise (spinels found, the merchant's rewards) is kept,
// and a count not known yet changes nothing.
#include <cstdio>

#include "../src/spinels_rules.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

bool is(const re4cc::events::rules::Spinels& r, int count, int case_given, int given) {
  return r.count == count && r.case_given == case_given && r.given == given;
}

}  // namespace

int main() {
  using re4cc::events::rules::spinel_change;

  // A trade: 20 -> 15.
  check(is(spinel_change(20, 15, true, false, false, 0), 20, 0, 5), "a trade is given back");
  check(is(spinel_change(20, 15, false, false, false, 0), 15, 0, 0), "... not with the switch off");
  check(is(spinel_change(20, 15, true, false, false, 7), 20, 0, 5), "an ordinary trade forgets an earlier placement");

  // A trade placed in the case: its spinels taken as the placement opens, refunded by a cancel.
  check(is(spinel_change(20, 10, true, true, false, 0), 20, 10, 10), "a placement's opening is given back and noted");
  check(is(spinel_change(20, 30, false, false, true, 10), 20, 0, -10), "its cancel's refund is taken back out");
  check(is(spinel_change(20, 30, true, false, true, 10), 20, 0, -10), "... the switch on as well");
  check(is(spinel_change(20, 10, false, true, false, 0), 10, 0, 0), "a placement opened with the switch off is not noted");
  check(is(spinel_change(10, 20, true, false, true, 0), 20, 0, 0), "... and its refund is kept");
  check(is(spinel_change(20, 25, true, false, true, 10), 25, 0, 0), "a refund of another amount is kept");
  check(is(spinel_change(20, 30, true, false, false, 10), 30, 0, 0), "a rise outside the placement's closing is kept");

  // Other rises, and what is not known.
  check(is(spinel_change(20, 23, true, false, false, 0), 23, 0, 0), "spinels found or a reward are kept");
  check(is(spinel_change(-1, 15, true, true, false, 4), 15, 4, 0), "no count noted yet: nothing changes");
  check(is(spinel_change(20, -1, true, false, false, 4), -1, 4, 0), "the count unreadable: nothing changes");
  check(is(spinel_change(20, 20, true, false, true, 4), 20, 4, 0), "no change: nothing changes");
  check(is(spinel_change(9999, 0, true, false, false, 0), 9999, 0, 9999), "the whole count given back");

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
  return g_fail ? 1 : 0;
}
