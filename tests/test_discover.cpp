// The mod's own discovery run against a real re4.exe, offline:
//
//   python3 tools/tdb_dump.py --exe "$GAME_DIR/re4.exe" --write-methods <scratch>/methods.bin
//   make tests/build/test_discover.exe   (or: every build/*.o but dllmain.o, the imgui objects, the mod's libs)
//   wine tests/build/test_discover.exe 'Z:\...\re4.exe' 'Z:\...\methods.bin'
//
// re4.exe is mapped as an image (no imports resolved, nothing run) and made to look
// like the running game to everything the mod asks: GetModuleHandleA(nullptr) - the
// mod's name for "the game" - answers the mapped exe (this test's import is pointed
// at a stand-in), its type database is rebased in place as the VM rebases it, and
// every method's code offset (TDB 71: an int32 from the engine's decoder's base) is
// filled from the exe's own link records (what the VM links at start-up;
// tools/tdb_dump.py --write-methods reads them). Then
// re::init, game::discover, savefiles::discover, challenges::discover, exshop::discover,
// call::init, events::install, app::find and the save files' first tick run exactly as the mod
// thread runs them, over the real code: the VM global, every class, field and enum
// value the mod names, the call bridge's helpers and the methods it calls, the hooked
// methods' entries (exchanged here as in the game), the frame's entry table, the
// save list's getters. A stand-in VM object
// (the database pointer, an empty static table 0x30 before it) sits in the VM global. What cannot be
// seen offline is what the running game builds - the VM object, static data,
// runtime vtables, the frame's entry table - and the log says so for those.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/app.h"
#include "../src/call.h"
#include "../src/challenges.h"
#include "../src/events.h"
#include "../src/exshop.h"
#include "../src/game.h"
#include "../src/log.h"
#include "../src/mem.h"
#include "../src/re.h"
#include "../src/savefiles.h"

namespace {

HMODULE g_game = nullptr;
using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
GetModuleHandleAFn g_real_gmh = nullptr;

HMODULE WINAPI game_module(LPCSTR name) { return name ? g_real_gmh(name) : g_game; }

}  // namespace

int main(int argc, char** argv) {
  using namespace re4cc;
  if (argc < 3) {
    std::printf("usage: test_discover.exe <re4.exe> <methods.bin from tools/tdb_dump.py --write-methods>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_discover_" + std::to_string(GetCurrentProcessId()) + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  log_init(dir.c_str());
  savefiles::init();  // as DllMain does
  challenges::init();
  exshop::init();

  g_game = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!g_game) {
    std::printf("could not map %s (%lu)\n", argv[1], GetLastError());
    return 1;
  }
  const auto base = reinterpret_cast<uintptr_t>(g_game);
  const mem::Range data = mem::section(g_game, ".data");

  // The database, rebased in place as the VM does.
  uintptr_t tdb = 0;
  uint32_t version = 0;
  for (uintptr_t a = (data.begin + 7) & ~static_cast<uintptr_t>(7); a + 8 < data.end; a += 8) {
    if (std::memcmp(reinterpret_cast<const void*>(a), "TDB\0", 4)) continue;
    version = mem::read<uint32_t>(a + 4);
    if (version == 71) {
      tdb = a;
      break;
    }
  }
  if (!tdb) {
    std::printf("no TDB v71 in %s\n", argv[1]);
    return 1;
  }
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(data.begin), data.size(), PAGE_READWRITE, &old);
  const int first_array = 0x58, arrays = 18;
  for (int i = 0; i < arrays; ++i) {
    auto* p = reinterpret_cast<uint64_t*>(tdb + first_array + 8 * i);
    if (*p) *p += tdb;
  }
  // Every method's code, as the VM links it: its offset from the base the engine's
  // decoder names (`test ecx,ecx; jne; xor eax,eax; ret; movsxd rax,ecx; lea rcx,[base]`).
  const uintptr_t methods = mem::read<uint64_t>(tdb + 0x70);
  const uint32_t n_methods = mem::read<uint32_t>(tdb + 0x10);
  const uint32_t stride = 12, code_at = 8;
  const mem::Range code = mem::section(g_game, ".text");
  const auto decoders = mem::find_all(code, "85 C9 75 03 33 C0 C3 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 03 C1 C3", 2);
  if (decoders.size() != 1) {
    std::printf("%u method-offset decoders in %s\n", static_cast<unsigned>(decoders.size()), argv[1]);
    return 1;
  }
  const uintptr_t code_base = mem::rip_target(decoders[0] + 10, 3, 7);
  FILE* f = std::fopen(argv[2], "rb");
  uint32_t header[4] = {};
  if (!f || std::fread(header, sizeof(header), 1, f) != 1 || header[0] != 0x4D344552 || header[2] != n_methods) {
    std::printf("%s is not a method map of this exe\n", argv[2]);
    return 1;
  }
  std::vector<uint32_t> rvas(n_methods);
  std::fread(rvas.data(), 4, rvas.size(), f);
  std::fclose(f);
  uint32_t linked = 0;
  for (uint32_t i = 0; i < n_methods; ++i)
    if (rvas[i]) {
      *reinterpret_cast<int32_t*>(methods + static_cast<uintptr_t>(i) * stride + code_at) =
          static_cast<int32_t>(static_cast<intptr_t>(base + rvas[i]) - static_cast<intptr_t>(code_base));
      ++linked;
    }

  // "The game" is the mapped exe from here on.
  g_real_gmh = reinterpret_cast<GetModuleHandleAFn>(
      mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&game_module)));
  if (!g_real_gmh) {
    std::printf("could not redirect GetModuleHandleA\n");
    return 1;
  }
  // A stand-in VM in the global the code loads it from (found the way re::init finds
  // it: `mov rcx,[rip+X]; mov edx,-1; call`, one X more than ten times), holding the
  // database pointer and an empty static table 0x30 before it - so re::init runs its
  // whole path over the real code, and call::init finds the VM's helpers.
  const mem::Range text = mem::section(g_game, ".text");
  uintptr_t vm_slot = 0;
  {
    std::vector<std::pair<uintptr_t, int>> seen;
    for (const uintptr_t s : mem::find_all(text, "48 8B 0D ?? ?? ?? ?? BA FF FF FF FF E8", 4096)) {
      const uintptr_t slot = mem::rip_target(s, 3, 7);
      bool found = false;
      for (auto& [k, n] : seen)
        if (k == slot && ++n > 10 && !vm_slot) vm_slot = slot, found = true;
      if (!found && std::none_of(seen.begin(), seen.end(), [&](const auto& e) { return e.first == slot; }))
        seen.emplace_back(slot, 1);
      if (vm_slot) break;
    }
  }
  if (vm_slot) {
    const uint32_t n_types = mem::read<uint32_t>(tdb + 0x0C);
    auto* vm = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* statics = VirtualAlloc(nullptr, static_cast<size_t>(n_types) * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *reinterpret_cast<uintptr_t*>(vm + 0x3000) = tdb;
    *reinterpret_cast<void**>(vm + 0x3000 - 0x30) = statics;
    *reinterpret_cast<uint32_t*>(vm + 0x3000 - 0x28) = n_types;
    *reinterpret_cast<uint8_t**>(vm_slot) = vm;
  }
  // re::init as the mod thread runs it: the database in the image, the VM global
  // by its pattern, the VM's database pointer, the static table.
  bool ready = re::init();
  for (int tries = 0; !ready && tries < 5 && !re::unsupported(); ++tries) ready = re::init();  // polled, as the mod thread does
  logf("TDB v%u at exe+0x%llX: %u of %u methods linked; the VM global exe+0x%llX; re::init %s (%s)", version,
       static_cast<unsigned long long>(tdb - base), linked, n_methods,
       static_cast<unsigned long long>(vm_slot ? vm_slot - base : 0), ready ? "ready" : "NOT ready", re::status());
  if (!ready) return 1;
  game::discover();
  savefiles::discover();
  challenges::discover();
  exshop::discover();
  call::init();
  events::install();
  app::find();
  savefiles::tick();  // its first tick reads the save list's code (the getters, the slots, the list's refresh)
  if (FILE* l = std::fopen((dir + "RE4CabbyCodes.log").c_str(), "r")) {
    char line[4096];
    while (std::fgets(line, sizeof(line), l)) std::fputs(line + (std::strlen(line) > 15 ? 15 : 0), stdout);
    std::fclose(l);
  }
  return 0;
}
