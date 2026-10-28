// The frame's entry hook (src/app.cpp) beside another tool's, offline:
//
//   make   (the mod's objects in build/)
//   bash -c 'x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static
//       tests/test_app.cpp $(ls build/*.o | grep -v dllmain.o) $(find build/imgui -name "*.o") -luser32 -lshell32
//       -lole32 -lgdi32 -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_app.exe'
//   WINEPREFIX=<scratch> wine tests/build/test_app.exe 'Z:\...\re4.exe'
//
// re4.exe is mapped as an image, as test_discover maps it, and app::find reads the
// entry table's layout and the Application global out of its code. The table
// itself is what the running game builds, so it is made here: three entries by
// name, group and priority as the game has them, UpdateBehavior's function first
// something that is not code, then a stub in memory of its own that jumps to a
// handler of this test's - REFramework's way: it puts such a stub into every
// entry once the game's first frame is out, and calls on to what it replaced -
// and EndRendering's the game's own code (never called here). Checked:
//  - a function that is not code is not hooked, and the log says so once;
//  - another tool's stub there first: the mod goes in front of it - the entry's
//    function is the mod's, which ticks and calls on to the stub;
//  - another tool's stub put in after the mod: check() leaves it in and says so
//    once (taking it out would take that tool's hook out, putting the mod's back
//    would run the mod twice a frame), and the mod's function, called from that
//    stub, still ticks and calls on;
//  - unhook() gives back only what still holds the mod's function.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/app.h"
#include "../src/dispatch.h"
#include "../src/log.h"
#include "../src/mem.h"

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

HMODULE g_game = nullptr;
using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
GetModuleHandleAFn g_real_gmh = nullptr;
HMODULE WINAPI game_module(LPCSTR name) { return name ? g_real_gmh(name) : g_game; }

std::string g_log_path;
std::string read_log() {
  std::string text;
  if (FILE* f = std::fopen(g_log_path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
  }
  return text;
}
int count_of(const std::string& text, const char* what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

// The tools' functions: a stub in memory of its own (`mov rax, handler; jmp rax`,
// as REFramework's own stubs end) leading to a handler here.
using EntryFn = void (*)(void*);
void* make_stub(EntryFn handler) {
  auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!p) return nullptr;
  p[0] = 0x48;
  p[1] = 0xB8;
  const auto target = reinterpret_cast<uint64_t>(handler);
  std::memcpy(p + 2, &target, 8);
  p[10] = 0xFF;
  p[11] = 0xE0;
  FlushInstructionCache(GetCurrentProcess(), p, 12);
  return p;
}

// The tool that was there first: what the mod calls on to.
int g_first_calls = 0;
void* g_first_arg = nullptr;
void first_tool(void* arg) {
  ++g_first_calls;
  g_first_arg = arg;
}
// The tool that came after the mod: it calls on to what it replaced.
int g_later_calls = 0;
EntryFn g_later_original = nullptr;
void later_tool(void* arg) {
  ++g_later_calls;
  if (g_later_original) g_later_original(arg);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace re4cc;
  if (argc < 2) {
    std::printf("usage: test_app.exe <re4.exe>\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string dir = std::string(temp) + "re4cc_app_" + std::to_string(GetCurrentProcessId()) + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  log_init(dir.c_str());
  g_log_path = dir + "RE4CabbyCodes.log";
  dispatch::init();  // as DllMain does: the tick's locks

  g_game = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!g_game) {
    std::printf("could not map %s (%lu)\n", argv[1], GetLastError());
    return 1;
  }
  const auto base = reinterpret_cast<uintptr_t>(g_game);
  g_real_gmh = reinterpret_cast<GetModuleHandleAFn>(
      mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&game_module)));
  if (!g_real_gmh) {
    std::printf("could not redirect GetModuleHandleA\n");
    return 1;
  }

  std::printf("the entry table's layout, out of re4.exe's code\n");
  check(app::find(), "app::find reads the layout and the Application global");
  // What it found, as it logged it.
  unsigned count_off = 0, table_off = 0, stride = 0, func_off = 0, name_off = 0, prio_off = 0, group_off = 0;
  unsigned long long global = 0;
  {
    const std::string log = read_log();
    const size_t at = log.find("count +0x");
    if (at == std::string::npos ||
        std::sscanf(log.c_str() + at,
                    "count +0x%X, table +0x%X, 0x%X-byte entries (function +0x%X, name +0x%X, priority +0x%X, group "
                    "+0x%X); the Application global is exe+0x%llX",
                    &count_off, &table_off, &stride, &func_off, &name_off, &prio_off, &group_off, &global) != 8) {
      std::printf("the layout line is not in the log\n");
      return 1;
    }
  }
  std::printf("  count +0x%X, table +0x%X, 0x%X-byte entries, function +0x%X, name +0x%X; global exe+0x%llX\n",
              count_off, table_off, stride, func_off, name_off, global);
  const mem::Range text = mem::section(g_game, ".text");
  const mem::Range data = mem::section(g_game, ".data");
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(data.begin), data.size(), PAGE_READWRITE, &old);

  // The Application object as the running game has it: three entries, in order.
  auto* app = static_cast<uint8_t*>(VirtualAlloc(nullptr, table_off + stride * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  static const char kBegin[] = "BeginRendering", kUpdate[] = "UpdateBehavior", kEnd[] = "EndRendering";
  const struct {
    const char* name;
    uint16_t group, priority;
  } kEntries[3] = {{kBegin, 3, 150}, {kUpdate, 3, 187}, {kEnd, 3, 268}};
  auto entry = [&](int i) { return app + table_off + static_cast<size_t>(stride) * i; };
  auto func_of = [&](int i) -> uintptr_t& { return *reinterpret_cast<uintptr_t*>(entry(i) + func_off); };
  for (int i = 0; i < 3; ++i) {
    *reinterpret_cast<const char**>(entry(i) + name_off) = kEntries[i].name;
    *reinterpret_cast<uint16_t*>(entry(i) + group_off) = kEntries[i].group;
    *reinterpret_cast<uint16_t*>(entry(i) + prio_off) = kEntries[i].priority;
  }
  const uintptr_t game_code = text.begin + 0x1000;  // the game's own (never called here)
  auto* not_code = new uint8_t[64]();               // data, not code
  func_of(0) = game_code;
  func_of(1) = reinterpret_cast<uintptr_t>(not_code);
  func_of(2) = game_code + 0x10;
  *reinterpret_cast<uint32_t*>(app + count_off) = 3;
  *reinterpret_cast<uint8_t**>(base + global) = app;

  std::printf("UpdateBehavior's function not code\n");
  bool hooked = app::hook();  // the count is new: it must hold still one more poll
  check(!hooked, "not hooked while the table may still be filling");
  hooked = app::hook();
  check(!hooked, "not hooked: UpdateBehavior's function is not code");
  hooked = app::hook();
  check(!hooked && func_of(1) == reinterpret_cast<uintptr_t>(not_code), "and its entry is left as it is");
  check(func_of(2) != game_code + 0x10 && !text.contains(func_of(2)), "EndRendering (the game's code) is hooked");
  check(count_of(read_log(), "UpdateBehavior's function") == 1, "the log says so once, not at every try");

  std::printf("another tool's function in UpdateBehavior's entry first (REFramework's stub)\n");
  void* first_stub = make_stub(&first_tool);
  func_of(1) = reinterpret_cast<uintptr_t>(first_stub);
  hooked = app::hook();
  check(hooked, "hooked: the mod goes in front of it");
  const uintptr_t mods_update = func_of(1);
  check(mods_update != reinterpret_cast<uintptr_t>(first_stub) && !text.contains(mods_update),
        "the entry's function is the mod's now");
  check(count_of(read_log(), "UpdateBehavior's entry already holds another tool's function") == 1,
        "the log names the other tool's function");
  reinterpret_cast<EntryFn>(mods_update)(reinterpret_cast<void*>(0x1234));
  check(g_first_calls == 1 && g_first_arg == reinterpret_cast<void*>(0x1234),
        "the game's call reaches the other tool's stub through the mod, with the game's argument");
  check(dispatch::snapshot().ticking, "and the mod ticked on the way");
  app::check();
  check(func_of(1) == mods_update && count_of(read_log(), "into UpdateBehavior's entry after the mod") == 0,
        "check() finds the mod's function where it put it");

  std::printf("another tool's function put in after the mod\n");
  g_later_original = reinterpret_cast<EntryFn>(mods_update);
  void* later_stub = make_stub(&later_tool);
  func_of(1) = reinterpret_cast<uintptr_t>(later_stub);
  app::check();
  check(func_of(1) == reinterpret_cast<uintptr_t>(later_stub), "check() leaves the other tool's function in");
  check(count_of(read_log(), "into UpdateBehavior's entry after the mod") == 1, "and says so");
  app::check();
  check(func_of(1) == reinterpret_cast<uintptr_t>(later_stub) &&
            count_of(read_log(), "into UpdateBehavior's entry after the mod") == 1,
        "once: the next check() changes nothing and says nothing");
  check(count_of(read_log(), "moved in the table") == 0, "it is not taken for the table being sorted again");
  reinterpret_cast<EntryFn>(func_of(1))(reinterpret_cast<void*>(0x5678));
  check(g_later_calls == 1 && g_first_calls == 2 && g_first_arg == reinterpret_cast<void*>(0x5678),
        "the game's call goes through that tool, the mod and on to the first one");

  std::printf("unhook\n");
  app::unhook();
  check(func_of(1) == reinterpret_cast<uintptr_t>(later_stub), "the other tool's function stays in UpdateBehavior's entry");
  check(func_of(2) == game_code + 0x10, "EndRendering gets the game's function back");
  check(func_of(0) == game_code, "an entry the mod never hooked is untouched");

  std::printf("\nthe log's app: lines\n");
  {
    const std::string log = read_log();
    for (size_t at = 0; at < log.size();) {
      const size_t nl = log.find('\n', at);
      const std::string line = log.substr(at, (nl == std::string::npos ? log.size() : nl) - at);
      if (line.find("] app: ") != std::string::npos) std::printf("  %s\n", line.c_str());
      if (nl == std::string::npos) break;
      at = nl + 1;
    }
  }
  std::printf("\npassed: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
