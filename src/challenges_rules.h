#pragma once

#include <cstdint>

// The challenges window's rules, apart from the game so the tests can check them
// (tests/test_challenges.cpp). What they follow is the game's own code (build
// 22377325, chainsaw.GameRecordManager):
//  - unlockRecord adds a challenge's CP (InfoData.AddPoint) to RecordExPoint and
//    keeps the sum within 0..100,000,000;
//  - a counted challenge is done when its count reaches its goal (checkProgressNum:
//    getProgressNum >= InfoData.AchievementNum); the things it counts once each are
//    bits of GameRecordData._ProgressBitFlag (addProgressNum: a bit already set
//    counts nothing);
//  - getProgressNum reads ten challenges' counts from the game save instead - the
//    treasure maps' "in a single playthrough" and the "without ..." clears;
//  - some of the Extra Content Shop's bonuses the game also gives outside the
//    challenges, in code: the Mercenaries' results (AdditionalBonusManager.
//    canUnlockBonus's set while the Mercenaries runs), a new game
//    (ExShopData.get_isNewGameUnlock), three of the merchant's requests
//    (checkReleaseBonusToQuest).
namespace re4cc::challenges::rules {

constexpr int kMaxPoints = 100000000;

// The CP after a challenge paying `cp` is done: the game's sum, kept in range.
inline int points_on(int now, int cp) {
  const long long v = static_cast<long long>(now) + cp;
  return v < 0 ? 0 : v > kMaxPoints ? kMaxPoints : static_cast<int>(v);
}
// ... and after it is undone: what it paid taken back, never below 0.
inline int points_off(int now, int cp) {
  const long long v = static_cast<long long>(now) - cp;
  return v < 0 ? 0 : v > kMaxPoints ? kMaxPoints : static_cast<int>(v);
}

// A counted challenge's count: at least its goal when it is done; just short of it
// when it is undone, so the next one counted earns it again.
inline int count_on(int count, int goal) { return goal > 0 && count < goal ? goal : count; }
inline int count_off(int count, int goal) { return goal > 0 && count >= goal ? goal - 1 : count; }

inline int bits_set(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}
// The things counted once each, cut down to `keep` - the highest-numbered
// forgotten first - so that finding one of them again counts.
inline uint32_t bits_off(uint32_t bits, int keep) {
  if (keep < 0) keep = 0;
  while (bits && bits_set(bits) > keep) bits &= ~(0x80000000u >> __builtin_clz(bits));
  return bits;
}

// The challenges whose count the game reads from the game save (getProgressNum):
// 077-079 (a treasure map's treasures in one playthrough), 095-097 and 103 (the
// main story without healing, with knives and handguns only, without the
// merchant; Professional without bonus weapons), 247-249 (Separate Ways' three).
// The window shows no count for them and writes none.
inline bool counted_in_game_save(int id) {
  const int d = id - 77;
  return (d >= 0 && d <= 26 && ((0x41C0007u >> d) & 1u)) || id == 247 || id == 248 || id == 249;
}

// The shop's bonuses the game gives outside the challenges too, in its code: the
// Mercenaries' (the Handcannon, a costume and five models), a new game's (two
// models, one in Separate Ways) and three requests' (models). Undoing a
// challenge never locks one of them; the others the game gives otherwise - at
// its first start, for an item found - are read from the game.
inline bool given_by_code(int bonus) {
  return bonus == 7 || bonus == 424 || (bonus >= 426 && bonus <= 430) ||  // canUnlockBonus, the Mercenaries
         bonus == 128 || bonus == 153 || bonus == 406 ||                     // get_isNewGameUnlock
         bonus == 209 || bonus == 211 || bonus == 212;                       // checkReleaseBonusToQuest
}

}  // namespace re4cc::challenges::rules
