#pragma once

#include <cstdint>
#include <cstring>

// The save files' rules on plain values - no game memory here (savefiles.cpp
// applies them; tests/test_savefiles.cpp checks them).
namespace re4cc::savefiles::rules {

// A save's time stamp (SaveFileDetail.LastUpdateTimeStamp: .NET ticks, UTC) as
// Unix seconds; -1 when it is no plausible date (before 2019, past 2100).
inline long long unix_seconds(long long ticks) {
  constexpr long long kTicksPerSecond = 10000000LL;
  constexpr long long kUnixEpochSeconds = 62135596800LL;  // 0001-01-01 to 1970-01-01
  if (ticks <= 0) return -1;
  const long long s = ticks / kTicksPerSecond - kUnixEpochSeconds;
  return s >= 1546300800LL && s < 4102444800LL ? s : -1;
}

// --- reading the engine's objects out of their getters' code ---------------------------------------------
// via.storage.saveService.SaveFileDetail has no fields in the type database, and
// System.String's are not listed either; their getters are a load or two, and
// say where. A managed method is `ret f(context, this, ...)`: the object is rdx.

// `[REX.W] 8B 42 d8 C3` / `[REX.W] 8B 82 d32 C3` - `mov eax/rax,[rdx+d]; ret`:
// the offset d (-1: not that shape). `wide`: the 64-bit load (a reference, an int64).
inline int32_t getter_offset(const uint8_t* b, size_t n, bool wide) {
  size_t i = 0;
  if (wide) {
    if (n < 1 || b[0] != 0x48) return -1;
    i = 1;
  } else if (n >= 1 && (b[0] & 0xF0) == 0x40) {
    return -1;
  }
  if (n < i + 4 || b[i] != 0x8B) return -1;
  if (b[i + 1] == 0x42 && b[i + 3] == 0xC3) return static_cast<int8_t>(b[i + 2]);
  if (n >= i + 7 && b[i + 1] == 0x82 && b[i + 6] == 0xC3) {
    int32_t d = 0;
    std::memcpy(&d, &b[i + 2], 4);
    return d;
  }
  return -1;
}

// `B8 imm32 C3` - `mov eax,imm32; ret`: a getter returning a constant.
inline bool getter_constant(const uint8_t* b, size_t n, int32_t* out) {
  if (n < 6 || b[0] != 0xB8 || b[5] != 0xC3) return false;
  std::memcpy(out, &b[1], 4);
  return true;
}

// FNV-1a over a few values: what a request checks it acts on the save the
// panel showed.
inline uint32_t fnv1a(const void* data, size_t n, uint32_t h = 2166136261u) {
  const auto* b = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
  return h;
}

}  // namespace re4cc::savefiles::rules
