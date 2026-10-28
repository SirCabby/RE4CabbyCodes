// Offline test of the panel's item lists (src/overlay.cpp) and the requests they
// make (src/inventory.cpp): drawn headless with ImGui over a made-up game - the
// case, the key items, the treasures, the other items and the item box - and
// driven by clicks and typing, as a player drives them. ImGui's test-engine hooks
// report each widget's label and place, so a click lands on the widget it names.
// Both sources are built into this file (their insides are reached by name), and
// ImGui with the hooks:
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O1 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DIMGUI_ENABLE_TEST_ENGINE
//       -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -Icontrib/imgui -static tests/test_panel.cpp src/keyboard.cpp
//       contrib/imgui/imgui.cpp contrib/imgui/imgui_draw.cpp contrib/imgui/imgui_tables.cpp
//       contrib/imgui/imgui_widgets.cpp contrib/imgui/backends/imgui_impl_win32.cpp
//       $(ls build/*.o | grep -v -e dllmain.o -e overlay.o -e overlay_d3d.o -e inventory.o -e keyboard.o -e exshop.o)
//       -luser32 -lshell32 -lole32 -lgdi32 -ldwmapi -ldxguid -luuid -ldinput8 -o tests/build/test_panel.exe
//   wine tests/build/test_panel.exe 'Z:\some\scratch\dir' [shots]
//
// (`make` first: the other objects are the mod's.) With "shots" each step's panel
// is also drawn to a picture (.ppm) in the scratch directory.
//
// - order: the lists off the case's grid come in the Add list's order - group by
//   group, each in the game's own order - whatever order the game holds them in.
// - ticks: a row's tick box, All (and a group's heading) for every row; Store for
//   the ticked rows of the case asks nothing and makes one batch; Remove asks once,
//   listing them; the game's own items (a scene's weapon) are never ticked.
// - edits: a count typed into two rows, then Apply all - one question, one batch of
//   two; a count typed back to what the game has is no change; Undo all.
// - Add: ticks in an Add list, a count for one, Add all - one question for all of
//   them, one batch; Tick all listed takes what the search keeps.
// - tabs: every knife that can be upgraded under Weapons, after the rocket launchers -
//   the main game's Combat, Fighting and Primal Knives, Separate Ways' Tactical and Elite
//   Knives too; the Kitchen and Boot Knives under Sub-weapons.
// - the item box: Take for ticked entries of both parts, its Throw away asked once.
// - the game tick: a batch is worked through a few requests a frame and summed up
//   in one note (each reason once); entries of the box's own part are found again
//   after an earlier change of the batch took one before them out.
// - the Extra Content Shop's window (src/exshop.cpp built in too, its view made up):
//   the shop's tabs (Separate Ways' only when the shop shows it), a row's tick asks
//   for that item at once, a purchased one cannot be unticked; Unlock all and Lock
//   all ask once for every item listed (the tab's, the filter's) that would change.
#include "../src/exshop.cpp"
#include "../src/inventory.cpp"
#include "../src/overlay.cpp"

#include <string>
#include <vector>

// overlay.cpp's swap chain side (overlay_d3d.cpp) is not part of this test.
namespace re4cc::overlay::d3d {
void install_early() {}
bool adopting() { return false; }
bool presenting() { return false; }
bool install() { return false; }
void uninstall() {}
void service() {}
void log_rates() {}
}  // namespace re4cc::overlay::d3d

// --- ImGui's test-engine hooks: what was drawn where ---------------------------------------
// A widget's place and its label come in two calls, in either order (a tab says
// its label first): each goes to the latest record of its id still missing it.
struct Drawn {
  ImGuiID id = 0;
  ImRect bb;
  std::string label;
  ImGuiItemStatusFlags flags = 0;
  bool placed = false, named = false;
};
std::vector<Drawn> g_drawn;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
  for (auto it = g_drawn.rbegin(); it != g_drawn.rend(); ++it)
    if (it->id == id && !it->placed) {
      it->bb = bb;
      it->placed = true;
      return;
    }
  g_drawn.push_back(Drawn{id, bb, "", 0, true, false});
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
  for (auto it = g_drawn.rbegin(); it != g_drawn.rend(); ++it)
    if (it->id == id && !it->named) {
      it->label = label ? label : "";
      it->flags = flags;
      it->named = true;
      return;
    }
  g_drawn.push_back(Drawn{id, ImRect(), label ? label : "", flags, false, true});
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

namespace {

using namespace re4cc;
namespace ov = re4cc::overlay;
namespace inv = re4cc::inventory;

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

// --- the made-up game --------------------------------------------------------------------------
inv::View g_view;
itembox::View g_box;
game::Mode g_game_mode = game::Mode::kMain;  // the game the Add lists are for
uint8_t g_next_guid = 1;

inv::Row& add_row(int kind, int32_t id, int count) {
  inv::Row& r = g_view.row[g_view.nrows++];
  game::ItemView v;
  v.id = id;
  v.count = count;
  v.guid[0] = g_next_guid++;
  v.guid[15] = static_cast<uint8_t>(kind + 1);
  inv::make_row(v, kind, &r);  // (no item of the game's behind it: nothing is read)
  ++g_view.count[kind];
  return r;
}

void make_game() {
  g_view = inv::View{};
  g_view.known = g_view.editable = true;
  g_view.mode = game::Mode::kMain;
  g_view.size.size = 1;
  g_view.has_box = g_view.storage = g_view.box_ok = true;
  // The case: a gun with upgrades, ammo, gunpowder, a knife - and a weapon a scene handed over.
  inv::Row& red9 = add_row(inv::kCase, 274838656, 1);  // Red9
  red9.gun = red9.weapon = true;
  red9.v.ammo = 8;
  red9.levels.n = 2;
  red9.levels.e[0] = {0, 0, 2};  // Power 3
  red9.levels.e[1] = {0, 2, 1};  // Capacity 2
  red9.max_level[0] = red9.max_level[1] = 4;
  add_row(inv::kCase, 112800000, 30);  // Handgun Ammo
  add_row(inv::kCase, 117600000, 5);   // Gunpowder
  add_row(inv::kCase, 112803200, 7);   // Shotgun Shells
  inv::Row& scene = add_row(inv::kCase, 274835456, 1);  // SG-09 R, the game's
  scene.v.force_get = true;
  scene.v.ammo = 10;
  // Key items and treasures.
  add_row(inv::kKeyItems, 119204800, 1);   // Insignia Key
  add_row(inv::kKeyItems, 119203200, 1);   // Hexagonal Emblem
  add_row(inv::kTreasures, 120832000, 2);  // Emerald (a gem)
  add_row(inv::kTreasures, 120801600, 1);  // Pearl Pendant
  // The other items as the game might hold them: in the order they came.
  add_row(inv::kUnique, 127238400, 1);  // Charm: Chicken
  add_row(inv::kUnique, 124321600, 1);  // Recipe: Handgun Ammo
  add_row(inv::kUnique, 124001600, 1);  // Body Armor
  add_row(inv::kUnique, 127200000, 1);  // Charm: Don Jose
  add_row(inv::kUnique, 124003200, 1);  // Treasure Map: Village
  add_row(inv::kUnique, 124329600, 1);  // Recipe: Bolts
  add_row(inv::kUnique, 124176000, 1);  // Attache Case: Silver
  add_row(inv::kUnique, 124337600, 1);  // Recipe: Flash Grenade
  // The game's storage: a stack of sprays and a gun.
  inv::Row& spray = add_row(inv::kStorage, 114416000, 1);  // First Aid Spray
  spray.v.stored = 5;
  spray.v.stored_max = 999;
  inv::Row& punisher = add_row(inv::kStorage, 274837056, 1);  // Punisher
  punisher.gun = punisher.weapon = true;
  punisher.v.ammo = 10;
  punisher.v.stored = 1;
  punisher.v.stored_max = 1;
}

std::vector<itembox::Entry> box_entries() {
  auto stack = [](int32_t id, int num) {
    itembox::Entry e;
    e.id = id;
    e.num = num;
    return e;
  };
  return {stack(112800000, 1000), stack(117600000, 50), stack(114401600, 3)};  // Handgun Ammo, Gunpowder, Red Herb
}

// --- frames, clicks and typing ------------------------------------------------------------------
std::string g_shots;  // where pictures of the panel go ("" none)
int g_shot = 0;

void draw_panel_body() {
  ov::g_mode = g_game_mode;
  ov::g_view_gen = 1;
  ov::g_note_placed = false;
  ov::draw_items(g_view, &g_box);
  ov::draw_item_dialogs(g_view);
}
void (*g_body)() = draw_panel_body;  // what the window holds: the pause panel's items, or the shop's window

void picture(const char* name);

void frame() {
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = 1.0f / 60.0f;
  io.DisplaySize = ImVec2(1920, 1080);
  g_drawn.clear();
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(960, 20), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(900, 1050), ImGuiCond_Always);
  ImGui::Begin("Resident Evil 4 - Cabby Codes###RE4CC panel");
  g_body();
  ImGui::End();
  ImGui::Render();
  // The font atlas: taken as the renderer would take it.
  for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
    if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
      t->SetTexID(static_cast<ImTextureID>(1));
      t->SetStatus(ImTextureStatus_OK);
    }
}

void frames(int n) {
  for (int i = 0; i < n; ++i) frame();
}

// The n-th widget (0 first) drawn with this label, at the last frame.
const Drawn* find(const char* label, int nth = 0) {
  for (const Drawn& d : g_drawn)
    if (d.label == label && nth-- == 0) return &d;
  return nullptr;
}
int count_drawn(const char* label) {
  int n = 0;
  for (const Drawn& d : g_drawn) n += d.label == label;
  return n;
}

// The widget with this label on the same line as `anchor` (a row's count beside its name).
const Drawn* beside(const char* label, const Drawn* anchor) {
  if (!anchor) return nullptr;
  for (const Drawn& d : g_drawn) {
    const float mid = (d.bb.Min.y + d.bb.Max.y) * 0.5f;
    if (d.label == label && mid >= anchor->bb.Min.y && mid <= anchor->bb.Max.y) return &d;
  }
  return nullptr;
}

bool click_at(const Drawn* d) {
  if (!d) return false;
  const ImVec2 at((d->bb.Min.x + d->bb.Max.x) * 0.5f, (d->bb.Min.y + d->bb.Max.y) * 0.5f);
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  frame();
  io.AddMouseButtonEvent(0, true);
  frame();
  io.AddMouseButtonEvent(0, false);
  frame();
  frame();
  return true;
}
bool click(const char* label, int nth = 0) {
  if (click_at(find(label, nth))) return true;
  std::printf("      (no \"%s\" #%d on the panel)\n", label, nth);
  return false;
}

void key(ImGuiKey k, bool ctrl = false) {
  ImGuiIO& io = ImGui::GetIO();
  if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(k, true);
  frame();
  io.AddKeyEvent(k, false);
  if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
  frame();
}

// Into a field: everything in it replaced by `text`, then Enter.
bool type_at(const Drawn* d, const char* text) {
  if (!click_at(d)) return false;
  key(ImGuiKey_A, true);
  for (const char* c = text; *c; ++c) {
    ImGui::GetIO().AddInputCharacter(static_cast<unsigned>(*c));
    frame();
  }
  key(ImGuiKey_Enter);
  frames(2);
  return true;
}
bool type_into(const char* label, int nth, const char* text) {
  if (type_at(find(label, nth), text)) return true;
  std::printf("      (no field \"%s\" #%d on the panel)\n", label, nth);
  return false;
}

// The list a row is in scrolled to its end, for a picture.
void scroll_down(const Drawn* row) {
  if (!row) return;
  ImGui::GetIO().AddMousePosEvent(row->bb.Min.x + 4.0f, row->bb.Min.y + 4.0f);
  ImGui::GetIO().AddMouseWheelEvent(0.0f, -100.0f);
  frames(2);
}

// The panel's sections open: only these (the rest folded, so what is checked is on screen).
void open_only(std::initializer_list<bool*> open) {
  for (bool* f : {&ov::g_case_open, &ov::g_add_open, &ov::g_box_open, &ov::g_key_open, &ov::g_key_add_open,
                  &ov::g_treasures_open, &ov::g_treasures_add_open, &ov::g_unique_open, &ov::g_unique_add_open})
    *f = false;
  for (bool* f : open) *f = true;
  frames(3);
}

// --- the requests the panel made ----------------------------------------------------------------
std::vector<inv::Request> take_queue() {
  EnterCriticalSection(&inv::g_cs);
  std::vector<inv::Request> q = inv::g_queue;
  inv::g_queue.clear();
  LeaveCriticalSection(&inv::g_cs);
  return q;
}
bool one_batch(const std::vector<inv::Request>& q, inv::Op op, int n) {
  if (static_cast<int>(q.size()) != n) return false;
  for (const inv::Request& r : q)
    if (r.op != op || (n > 1 && (r.batch == 0 || r.batch != q[0].batch || r.batch_size != n))) return false;
  return true;
}
bool asking_batch() { return ov::g_ask == ov::Ask::kBatch && GImGui->OpenPopupStack.Size > 0; }

// --- pictures of the panel (shots): ImGui's triangles filled in, the font's alpha from its atlas -----
void picture(const char* name) {
  if (g_shots.empty()) return;
  const ImDrawData* dd = ImGui::GetDrawData();
  const int W = 1920, H = 1080, X0 = 0;
  std::vector<float> px(static_cast<size_t>(W) * H * 3, 0.06f);
  for (const ImDrawList* dl : dd->CmdLists) {
    for (const ImDrawCmd& c : dl->CmdBuffer) {
      if (c.UserCallback) continue;
      const ImTextureData* tex = c.TexRef._TexData;
      const int cx0 = std::max(0, static_cast<int>(c.ClipRect.x) - X0), cy0 = std::max(0, static_cast<int>(c.ClipRect.y));
      const int cx1 = std::min(W, static_cast<int>(c.ClipRect.z) - X0), cy1 = std::min(H, static_cast<int>(c.ClipRect.w));
      for (unsigned i = 0; i + 2 < c.ElemCount; i += 3) {
        const ImDrawVert* v[3];
        for (int k = 0; k < 3; ++k) v[k] = &dl->VtxBuffer[c.VtxOffset + dl->IdxBuffer[c.IdxOffset + i + k]];
        const float x0 = v[0]->pos.x - X0, y0 = v[0]->pos.y, x1 = v[1]->pos.x - X0, y1 = v[1]->pos.y;
        const float x2 = v[2]->pos.x - X0, y2 = v[2]->pos.y;
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-6f) continue;
        const int bx0 = std::max(cx0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
        const int by0 = std::max(cy0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
        const int bx1 = std::min(cx1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
        const int by1 = std::min(cy1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));
        for (int y = by0; y < by1; ++y)
          for (int x = bx0; x < bx1; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area;
            const float w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            float col[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
              const float a = ((v[0]->col >> (8 * k)) & 0xFF) / 255.0f, b = ((v[1]->col >> (8 * k)) & 0xFF) / 255.0f;
              const float d = ((v[2]->col >> (8 * k)) & 0xFF) / 255.0f;
              col[k] = a * w0 + b * w1 + d * w2;
            }
            float ta = 1.0f, tc[3] = {1, 1, 1};
            if (tex && tex->Pixels) {
              const float u = v[0]->uv.x * w0 + v[1]->uv.x * w1 + v[2]->uv.x * w2;
              const float t = v[0]->uv.y * w0 + v[1]->uv.y * w1 + v[2]->uv.y * w2;
              const int tx = std::clamp(static_cast<int>(u * tex->Width), 0, tex->Width - 1);
              const int ty = std::clamp(static_cast<int>(t * tex->Height), 0, tex->Height - 1);
              const unsigned char* p = tex->Pixels + (static_cast<size_t>(ty) * tex->Width + tx) * tex->BytesPerPixel;
              if (tex->Format == ImTextureFormat_Alpha8) {
                ta = p[0] / 255.0f;
              } else {
                for (int k = 0; k < 3; ++k) tc[k] = p[k] / 255.0f;
                ta = p[3] / 255.0f;
              }
            }
            const float a = col[3] * ta;
            float* o = &px[(static_cast<size_t>(y) * W + x) * 3];
            for (int k = 0; k < 3; ++k) o[k] = o[k] * (1.0f - a) + col[k] * tc[k] * a;
          }
      }
    }
  }
  char path[600];
  std::snprintf(path, sizeof(path), "%s\\%02d-%s.ppm", g_shots.c_str(), ++g_shot, name);
  if (FILE* f = std::fopen(path, "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (const float c : px) std::fputc(static_cast<int>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f), f);
    std::fclose(f);
  }
}

// --- the checks ------------------------------------------------------------------------------------
void check_order() {
  open_only({&ov::g_treasures_open, &ov::g_unique_open});
  picture("other-items");
  const std::vector<int> rows = ov::offgrid_rows(g_view, inv::kUnique);
  std::vector<int32_t> ids;
  for (const int i : rows) ids.push_back(g_view.row[i].v.id);
  const std::vector<int32_t> want = {124321600, 124329600, 124337600,  // Recipes: Handgun Ammo, Bolts, Flash Grenade
                                     127200000, 127238400,             // Charms: Don Jose, Chicken
                                     124003200,                        // Treasure map: Village
                                     124176000,                        // Attache case: Silver
                                     124001600};                       // Armor
  check(ids == want, "order: the other items come group by group (recipes, charms, maps, cases, armor), each in the "
                     "game's order");
  int groups = 0;
  for (const Drawn& d : g_drawn) groups += d.label == "##group";
  check(groups >= 5 + 2, "order: a heading with its own tick box over each group (other items: 5, treasures: 2)");
}

void check_ticks() {
  // The case's rows, in the game's order: Red9, Handgun Ammo, Gunpowder, Shotgun Shells, SG-09 R.
  open_only({&ov::g_case_open});
  check(find("All") != nullptr, "ticks: the case's line has All");
  click("##tick", 1);  // Handgun Ammo
  click("##tick", 2);  // Gunpowder
  check(ov::g_ticks.size() == 2, "ticks: two rows of the case ticked");
  check(find("Store") != nullptr && find("Throw away") != nullptr, "ticks: the line offers Store and Throw away");
  picture("case-ticked");
  click("Store");  // the line's Store, drawn before the rows'
  std::vector<inv::Request> q = take_queue();
  check(one_batch(q, inv::kOpStore, 2) && q[0].seen.v.id == 112800000 && q[1].seen.v.id == 117600000,
        "ticks: Store for the ticked rows asks nothing: one batch of two stores (Handgun Ammo, Gunpowder)");
  check(ov::g_ticks.empty() && ov::g_ask == ov::Ask::kNone, "ticks: stored rows are unticked");

  // All ticks every row the game does not keep for itself.
  click("All");
  int on = 0;
  for (int i = 0; i < g_view.nrows; ++i)
    if (g_view.row[i].kind == inv::kCase) on += ov::ticked(g_view.row[i]);
  check(on == 4, "ticks: All ticks the case's four rows - not the weapon a scene handed over");
  click("All");
  check(ov::g_ticks.empty(), "ticks: All again unticks them");

  // The other items: a group's heading ticks its rows; Remove asks once for all of them.
  open_only({&ov::g_unique_open});
  click("##group", 1);  // the headings: Recipes, Charms, ...
  int ticked_charms = 0;
  for (int i = 0; i < g_view.nrows; ++i)
    if (g_view.row[i].kind == inv::kUnique && ov::ticked(g_view.row[i])) ++ticked_charms;
  check(ticked_charms == 2, "ticks: the Charms heading ticks both charms");
  click("Remove");  // the line's (the rows' are x)
  check(asking_batch() && ov::g_batch.lines.size() == 2, "ticks: Remove asks once, listing the two");
  picture("remove-asked");
  click("Remove 2");
  std::vector<inv::Request> r = take_queue();
  check(one_batch(r, inv::kOpRemove, 2), "ticks: ...and its answer is one batch of two removes");
  check(ov::g_ask == ov::Ask::kNone && ov::g_ticks.empty(), "ticks: the question is gone, the rows unticked");
}

void check_edits() {
  // The case's count fields (##num): Handgun Ammo's (30), Gunpowder's (5), Shotgun Shells' (7).
  open_only({&ov::g_case_open});
  type_into("##num", 0, "45");
  type_into("##num", 2, "12");
  check(ov::g_edits.size() == 2, "edits: two counts typed");
  check(count_drawn("Apply") == 2 && find("Apply all") != nullptr, "edits: each row has Apply, the line Apply all");
  picture("case-edited");
  // A count typed back to what the game has is no change.
  type_into("##num", 1, "6");
  type_into("##num", 1, "5");
  check(ov::g_edits.size() == 2, "edits: Gunpowder typed back to its 5 is no change");
  click("Apply all");
  check(asking_batch() && ov::g_batch.lines.size() == 2 && ov::g_batch.lines[0] == "Handgun Ammo: 30 -> 45" &&
            ov::g_batch.lines[1] == "Shotgun Shells: 7 -> 12",
        "edits: Apply all asks once: \"Handgun Ammo: 30 -> 45\", \"Shotgun Shells: 7 -> 12\"");
  click("Apply 2");
  std::vector<inv::Request> q = take_queue();
  check(one_batch(q, inv::kOpCount, 2) && q[0].a == 45 && q[1].a == 12, "edits: one batch of two counts, 45 and 12");
  check(ov::g_edits.empty(), "edits: applied rows show the game's counts again");
  type_into("##num", 0, "40");
  click("Undo all");
  check(ov::g_edits.empty() && take_queue().empty(), "edits: Undo all lets the typing go, asking the game nothing");
}

void check_add() {
  // The case's Add list, narrowed to ammo: tick two, give one a count, Add all.
  open_only({&ov::g_case_open, &ov::g_add_open});
  std::snprintf(ov::g_add.query, sizeof(ov::g_add.query), "ammo");
  frames(2);
  click("Handgun Ammo");
  click("Magnum Ammo");
  check(ov::g_add.num[items::index_of(112800000)] == 1 && ov::g_add.num[items::index_of(112801600)] == 1,
        "add: two items of the list ticked");
  type_at(beside("##num", find("Handgun Ammo")), "50");
  check(ov::g_add.num[items::index_of(112800000)] == 50, "add: the count beside a ticked item (50)");
  picture("add-ticked");
  click("Add 2###add add");
  check(asking_batch() && ov::g_batch.lines.size() == 2 && ov::g_batch.lines[0] == "Handgun Ammo x50" &&
            ov::g_batch.lines[1] == "Magnum Ammo",
        "add: Add 2 asks once, listing \"Handgun Ammo x50\" and \"Magnum Ammo\"");
  picture("add-asked");
  click("Add 2");
  std::vector<inv::Request> q = take_queue();
  check(one_batch(q, inv::kOpAdd, 2) && q[0].a == 112800000 && q[0].b == 50 && q[1].a == 112801600 && q[1].b == 1,
        "add: one batch of two adds (Handgun Ammo x50, Magnum Ammo x1)");
  int left = 0;
  for (const int n : ov::g_add.num) left += n > 0;
  check(left == 0, "add: the list's ticks are let go once asked for");
  // Tick all listed: what the search keeps.
  char label[64];
  int listed = 0;
  for (const int16_t i : items::kByRank) {
    const items::Def& d = items::kItems[i];
    listed += ov::accept_case(d, g_view) && ov::item_matches(d.id, d.name, "ammo");
  }
  std::snprintf(label, sizeof(label), "Tick all %d listed###add tick all", listed);
  click(label);
  int ticked = 0;
  for (const int n : ov::g_add.num) ticked += n > 0;
  check(ticked == listed && listed > 2, "add: Tick all listed ticks every item the search keeps");
  click("Untick all###add untick");
  ov::g_add.query[0] = '\0';
  frames(2);
}

void check_tabs() {
  // The Add list's tabs: every knife that can be upgraded is a weapon, after the rocket launchers under a
  // heading of its own; the other knives are sub-weapons, with the grenades. The main game's, then
  // Separate Ways'.
  open_only({&ov::g_case_open, &ov::g_add_open});
  click("Weapons###add1");
  const Drawn* knife = find("Combat Knife");
  const Drawn* fighting = find("Fighting Knife");
  const Drawn* primal = find("Primal Knife");
  const Drawn* rockets = find("Infinite Rocket Launcher");
  const bool weapons = knife && fighting && primal && rockets && knife->bb.Min.y > rockets->bb.Min.y &&
                       fighting->bb.Min.y > knife->bb.Min.y && primal->bb.Min.y > fighting->bb.Min.y &&
                       !find("Kitchen Knife") && !find("Boot Knife") && !find("Hand Grenade");
  scroll_down(find("SG-09 R"));  // the tab's first row
  picture("add-weapons");
  click("Sub-weapons###add2");
  const bool sub = !find("Combat Knife") && !find("Fighting Knife") && !find("Primal Knife") &&
                   find("Kitchen Knife") && find("Boot Knife") && find("Hand Grenade");
  picture("add-sub-weapons");
  const items::Def* r = items::find(276278656);  // Infinite Rocket Launcher
  const items::Def* k = items::find(276438656);  // Kitchen Knife
  const items::Def* b = items::find(276440256);  // Boot Knife
  bool filed = r && k && b && k->bank == items::kBankSubWeapons && b->group == k->group &&
               std::strcmp(items::kGroups[k->group].name, "Knives") == 0;
  // Combat, Fighting and Primal Knives; Separate Ways' Tactical and Elite Knives.
  for (const int32_t id : {276435456, 276437056, 276445056, 278206656, 278208256}) {
    const items::Def* d = items::find(id);
    filed = filed && d && d->bank == items::kBankWeapons && d->group != r->group &&
            std::strcmp(items::kGroups[d->group].name, "Special knives") == 0;
  }
  check(weapons && sub && filed,
        "tabs: the knives that can be upgraded (Combat, Fighting, Primal) under Weapons, after the rocket "
        "launchers, their own heading (Special knives); the Kitchen and Boot Knives under Sub-weapons (Knives)");

  g_game_mode = game::Mode::kSeparateWays;
  click("Weapons###add1");
  const Drawn* sw_primal = find("Primal Knife");
  const Drawn* tactical = find("Tactical Knife");
  const Drawn* elite = find("Elite Knife");
  const bool sw_weapons = sw_primal && tactical && elite && tactical->bb.Min.y > sw_primal->bb.Min.y &&
                          elite->bb.Min.y > tactical->bb.Min.y && !find("Kitchen Knife");
  scroll_down(find("SG-09 R"));
  picture("add-weapons-separate-ways");
  click("Sub-weapons###add2");
  const bool sw_sub = !find("Tactical Knife") && !find("Elite Knife") && find("Kitchen Knife");
  check(sw_weapons && sw_sub,
        "tabs: Separate Ways' Tactical and Elite Knives under Weapons too, after the main game's knives");
  g_game_mode = game::Mode::kMain;
  click("All###add0");
}

void check_box() {
  // The box's tab: Ammo holds Handgun Ammo; Recovery the spray (storage) and the Red Herb.
  open_only({&ov::g_box_open});
  click("Recovery 2###bank4");
  check(find("##tick") != nullptr, "box: its rows have tick boxes");
  click("All");
  const int ticked_box = static_cast<int>(ov::g_box_ticks.size() + ov::g_ticks.size());
  check(ticked_box == 2, "box: All ticks the tab's two rows - the storage's spray and the box's Red Herb");
  picture("box-ticked");
  click("Take");  // the line's, drawn before the rows'
  std::vector<inv::Request> q = take_queue();
  check(q.size() == 2 && q[0].op != q[1].op && (q[0].op == inv::kOpTakeStored || q[0].op == inv::kOpTake) &&
            (q[1].op == inv::kOpTakeStored || q[1].op == inv::kOpTake) && q[0].batch && q[0].batch == q[1].batch,
        "box: Take for both: one batch, the storage's take and the box's");
  click("All");
  click("Throw away");
  check(asking_batch() && ov::g_batch.lines.size() == 2, "box: Throw away asks once for both");
  click("Throw away 2");
  q = take_queue();
  check(q.size() == 2 && q[0].op == inv::kOpDiscardStored && q[1].op == inv::kOpDiscard,
        "box: ...one batch of the storage's and the box's throw-aways");
}

void check_tick() {
  // The game tick with no game: every request of a batch is refused for the same reason, said once.
  game::Run run;
  run.in_game = true;
  run.paused = true;
  run.mode = game::Mode::kMain;
  inv::begin_batch();
  for (int i = 0; i < 30; ++i) inv::request_add(112800000, 1);
  inv::end_batch("added");
  inv::tick(run);
  inv::View v;
  inv::view(&v);
  check(v.waiting >= 30 - inv::kPerTick && v.waiting < 30, "tick: a few requests a frame (at most 12 of 30 the first)");
  for (int i = 0; i < 30 && v.waiting > 0; ++i) {
    inv::tick(run);
    inv::view(&v);
  }
  check(v.waiting == 0 && v.note_error && std::strncmp(v.note, "0 of 30 added - not added: ", 27) == 0 &&
            !std::strstr(v.note, "; "),
        "tick: the batch done over a few frames, summed up in one note, its one reason once");
  std::printf("      (\"%s\")\n", v.note);

  // The box's own part: entries found again after one before them went.
  itembox::loaded(box_entries());
  const itembox::View b = itembox::view();
  inv::begin_batch();
  inv::request_discard(0, b.entry[0]);
  inv::request_discard(1, b.entry[1]);
  inv::request_discard(2, b.entry[2]);
  inv::end_batch("thrown away");
  inv::tick(run);
  check(itembox::view().entry.empty(), "tick: three of the box's entries thrown away together, each found again");
  itembox::loaded(box_entries());
  const itembox::View b2 = itembox::view();
  inv::begin_batch();
  inv::request_box_count(2, b2.entry[2], 7);
  inv::request_discard(0, b2.entry[0]);
  itembox::Entry gone = b2.entry[1];
  gone.num = 49;  // not as the box has it
  inv::request_discard(1, gone);
  inv::end_batch("changed");
  inv::tick(run);
  inv::view(&v);
  const itembox::View after = itembox::view();
  check(after.entry.size() == 2 && after.entry[1].num == 7 && v.note_error &&
            std::strncmp(v.note, "2 of 3 changed - not changed: ", 30) == 0,
        "tick: one of three not as the panel saw it is left alone, and the note says so");
  std::printf("      (\"%s\")\n", v.note);
}


// --- the Extra Content Shop's window -----------------------------------------------------------------
// A made-up shop: a purchased weapon, a locked costume, an accessory more than the
// CP held, a locked model, concept art for sale, a locked Separate Ways costume.
void shop_view(bool separate_ways) {
  namespace es = re4cc::exshop;
  es::View* v = new es::View;
  v->known = v->up = true;
  v->cp = 800;
  for (int t = 0; t < es::kTabs; ++t) v->tab[t] = t != es::kSeparateWays || separate_ways;
  auto add = [&](int id, int tab, int cp, bool unlocked, bool bought, es::State state) {
    es::Row& r = v->row[v->count++];
    r.id = id;
    r.tab = tab;
    r.cp = cp;
    r.unlocked = unlocked;
    r.bought = bought;
    r.state = state;
  };
  add(6, es::kWeapons, 1000, true, true, es::kPurchased);      // Chicago Sweeper
  add(17, es::kApparel, 500, false, false, es::kLocked);       // Jacket (Leon)
  add(38, es::kApparel, 1000, true, false, es::kShort);        // Glasses (Lexington) (Leon)
  add(69, es::kModels, 500, false, false, es::kLocked);        // Leon (Jacket)
  add(270, es::kArt, 300, true, false, es::kForSale);          // Leon S. Kennedy
  add(363, es::kSeparateWays, 2000, false, false, es::kLocked);  // Dress (Ada)
  EnterCriticalSection(&es::g_cs);
  std::swap(es::g_view, v);
  LeaveCriticalSection(&es::g_cs);
  delete v;
}
struct ShopAsk {
  int id;
  bool on;
};
std::vector<ShopAsk> take_shop_queue() {
  namespace es = re4cc::exshop;
  EnterCriticalSection(&es::g_cs);
  std::vector<ShopAsk> q;
  for (int i = 0; i < es::g_nq; ++i) q.push_back({es::g_queue[i].id, es::g_queue[i].on});
  es::g_nq = 0;
  LeaveCriticalSection(&es::g_cs);
  return q;
}
bool asks(const std::vector<ShopAsk>& q, std::initializer_list<int> ids, bool on) {
  if (q.size() != ids.size()) return false;
  size_t i = 0;
  for (const int id : ids) {
    if (q[i].id != id || q[i].on != on) return false;
    ++i;
  }
  return true;
}

void check_shop() {
  re4cc::exshop::init();
  re4cc::exshop::g_ok = true;
  shop_view(true);
  g_body = [] { ov::draw_shop(); };
  frames(3);
  check(find("ALL") && find("WEAPONS") && find("APPAREL") && find("MODELS") && find("CONCEPT ART") &&
            find("SEPARATE WAYS") && count_drawn("##unlocked") == 6,
        "shop: the shop's tabs as it labels them; under ALL its six items, each with its tick box");
  picture("shop");
  // A row's tick: that item at once, nothing asked. The Jacket is the second row.
  click("##unlocked", 1);
  check(asks(take_shop_queue(), {17}, true) && ov::g_shop_ask < 0,
        "shop: ticking the locked Jacket asks for it at once (no question)");
  click("##unlocked", 0);
  check(take_shop_queue().empty(), "shop: a purchased item's tick cannot be cleared");
  click("##unlocked", 4);
  check(asks(take_shop_queue(), {270}, false), "shop: unticking the concept art for sale locks it again");
  // Unlock all: one question for the locked items listed, one request each.
  click("Unlock all");
  check(ov::g_shop_ask == 1 && ov::g_shop_ask_n == 3 && find("Unlock 3"), "shop: Unlock all asks once, for the 3 locked");
  picture("shop-unlock-all");
  click("Unlock 3");
  check(asks(take_shop_queue(), {17, 69, 363}, true) && ov::g_shop_ask < 0,
        "shop: ...and its answer asks for the Jacket, the model and the Dress");
  // Lock all: the unlocked ones not purchased; the purchased one is said to stay.
  click("Lock all");
  check(ov::g_shop_ask == 0 && ov::g_shop_ask_n == 2 && ov::g_shop_ask_bought == 1 && find("Lock 2"),
        "shop: Lock all asks once, for the 2 unlocked that are not purchased (1 purchased stays)");
  click("Cancel");
  check(take_shop_queue().empty() && ov::g_shop_ask < 0, "shop: Cancel asks for nothing");
  // A tab and the filter narrow what is listed - and what all takes. (A row is
  // known by what its tick asks for: the names are plain text.)
  click("APPAREL");
  const int apparel = count_drawn("##unlocked");
  click("##unlocked", 0);
  click("##unlocked", 1);
  const std::vector<ShopAsk> q = take_shop_queue();
  check(apparel == 2 && q.size() == 2 && q[0].id == 17 && q[0].on && q[1].id == 38 && !q[1].on,
        "shop: APPAREL lists the costume and the accessory (the Jacket's tick unlocks it, the Glasses' locks them)");
  click("Unlock all");
  click("Unlock 1");
  check(asks(take_shop_queue(), {17}, true), "shop: Unlock all under APPAREL takes only its locked Jacket");
  click("ALL");
  type_into("##shop_filter", 0, "jacket");
  const int filtered = count_drawn("##unlocked");
  click("Unlock all");
  click("Unlock 2");
  check(filtered == 2 && asks(take_shop_queue(), {17, 69}, true),
        "shop: the filter keeps what it names - the Jacket and the model of Leon in it - and Unlock all takes those");
  ov::g_shop_filter[0] = '\0';
  frames(2);
  // Without Separate Ways' tab in the shop, its items are not listed at all.
  shop_view(false);
  frames(3);
  check(!find("SEPARATE WAYS") && count_drawn("##unlocked") == 5 && !find("Dress (Ada)"),
        "shop: no Separate Ways tab in the shop - neither its tab nor its items here");
  click("Unlock all");
  check(ov::g_shop_ask_n == 2, "shop: ...and Unlock all leaves them out (2 locked, not 3)");
  click("Cancel");
  // The shop closing: nothing can be asked.
  re4cc::exshop::g_view->up = false;
  frames(2);
  click("##unlocked", 1);
  check(take_shop_queue().empty(), "shop: closing - the ticks do nothing");
  g_body = draw_panel_body;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_panel.exe <scratch dir> [shots]\n");
    return 2;
  }
  if (argc > 2 && std::strcmp(argv[2], "shots") == 0) g_shots = argv[1];
  inv::init();
  itembox::init(argv[1]);
  itembox::loaded(box_entries());
  g_box = itembox::view();
  make_game();

  ImGui::CreateContext();
  GImGui->TestEngineHookItems = true;
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.ScaleAllSizes(ov::kScale);
  style.FontScaleMain = ov::kScale;
  open_only({&ov::g_case_open, &ov::g_add_open, &ov::g_box_open});
  picture("start");

  check_order();
  check_ticks();
  check_edits();
  check_add();
  check_tabs();
  check_box();
  check_tick();
  check_shop();

  ImGui::DestroyContext();
  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
  return g_fail ? 1 : 0;
}
