#pragma once

// Infinite spinels' rule on plain values - no game memory here (events.cpp's hook
// on the merchant's screen applies it; tests/test_spinels.cpp checks it).
namespace re4cc::events::rules {

// A change of the spinels the merchant's screen heard, from `seen` (the count
// before it; -1 not known) to `now`. While the screen is up only a trade lowers
// them: a fall is given back while the switch is `on`. A trade of an item to be
// placed in the case takes its spinels as the placement opens (`case_enter`: the
// screen's state then) and a cancel refunds them as it closes (`case_exit`); a
// refund of what that opening gave back (`case_given`) is taken out again, the
// switch on or not by then. Any other rise - spinels found, the merchant's
// rewards - is kept.
struct Spinels {
  int count;       // what to leave them at (`now`: as they are)
  int case_given;  // what the last placement's opening gave back, from now on (0: none)
  int given;       // given back (> 0: a trade), or taken back (< 0: a refund)
};
inline Spinels spinel_change(int seen, int now, bool on, bool case_enter, bool case_exit, int case_given) {
  if (seen < 0 || now < 0 || now == seen) return {now, case_given, 0};
  if (now < seen) {
    const int given = on ? seen - now : 0;
    return {on ? seen : now, case_enter ? given : 0, given};
  }
  if (case_given > 0 && case_exit && now - seen == case_given) return {seen, 0, seen - now};
  return {now, 0, 0};
}

}  // namespace re4cc::events::rules
