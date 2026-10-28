#!/usr/bin/env python3
"""Generate src/challenges_text.h - the names the challenges window shows - from the game's own files.

    python3 -I tools/gen_challenges.py --game "$GAME_DIR" --out src/challenges_text.h

The challenges are chainsaw.GameRecordSettingUserData's table (one InfoData per
chainsaw.GameRecordID: its name and condition as message GUIDs, its category, whether
the game lists it), and what they unlock is the Extra Content Shop's table
(chainsaw.ExContentsShopUserData: a chainsaw.ExShopBonusID's name and the text the shop
shows while it is locked). Both files are found by their contents. The English texts
come from the message files (.msg.22); a <REF name> inside a text is the text of the
message of that name. Everything else - what a challenge pays, its goal, which bonuses
it unlocks - the mod reads from the game at run time.

A challenge the game keeps off its screen (IsDisable) but still awards - the Assisted
S+ ranks' Wolf Tail and Wolf Set - is named here by what it unlocks, its condition the
shop's text for that bonus. The two game modes the challenges open (Professional, for
the main story and for Separate Ways) are no shop items: their names are given below.
The window's tabs are labelled as the game's challenges screen labels its own.

The Extra Content Shop's window (src/exshop.cpp) lists the same items: for each, the
text the shop shows while it is locked (its LockDetail), and the shop's tabs as it
labels them. Every item of the shop's table must have a name here.
"""
import argparse, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_items  # noqa: E402
import pak_msg  # noqa: E402
import rsz  # noqa: E402
from tdb_dump import PE, TDB  # noqa: E402

# The game modes' bonuses (ExShopBonusID GameMode00/01): what the game reads them for
# (GameRecordManager.IsGameModeMostDifficult, ...Cp1011, the difficulty select).
GAME_MODES = {"GameMode00": "Professional mode", "GameMode01": "Professional mode (Separate Ways)"}

# What a bonus is, by its ExShopBonusID name: the kinds the panel names (challenges.h Kind).
KINDS = [("GameMode", "kMode"), ("CP11_Weapon", "kWeapon"), ("CP11_Costume", "kCostume"),
         ("CP11_Accessory", "kAccessory"), ("CP11_Figure", "kModel"), ("CP12_Costume", "kCostume"),
         ("CP12_Figure", "kModel"), ("Weapon", "kWeapon"), ("Costume", "kCostume"), ("Accessory", "kAccessory"),
         ("Figure", "kModel"), ("ArtWork", "kArt"), ("PhotoStyle", "kFilter")]

# The challenges screen's tabs, the message each is labelled with (RecordListGuiBehavior.initTab),
# in the screen's order: All, Incomplete, then the categories (GameRecordSettingUserData.CategoryType
# Story 1, Tutorials 2, Weapons 3, Battle 4, Collectibles 5, Cp1011 6 - challenges.h Group). The
# screen never uses Category06/08/10/11 (two rejected, PUZZLES, SHOOTING RANGE).
TABS = ["CH_Mes_Main_Sys_Bonus_Record_Category01", "CH_Mes_Main_Sys_Bonus_Record_Category02",
        "CH_Mes_Main_Sys_Bonus_Record_Category03", "CH_Mes_Main_Sys_Bonus_Record_Category04",
        "CH_Mes_Main_Sys_Bonus_Record_Category05", "CH_Mes_Main_Sys_Bonus_Record_Category07",
        "CH_Mes_Main_Sys_Bonus_Record_Category09", "AO_Mes_Main_Sys_Bonus_Record_Category00"]

# The Extra Content Shop's tabs, the message each is labelled with, in exshop.h's Tab order: All,
# then the shop's categories as ExContentsShopGuiBehavior.setup lists them (Weapon, Costume - the
# accessories too -, Figure, Art, and Separate Ways' CP11 with that DLC). The class's static
# TabElementNameTable pairs the first five with via.gui.MsgID's ContentsCategory_00 (None),
# _03 (Weapon), _08 (Costume), _02 (Figure) and _01 (Art); _04-_06, _09-_11 are rejected and
# _07 (ACCESSORIES) labels no tab.
SHOP_TABS = ["CH_Mes_Main_Sys_Bonus_ContentsCategory_00", "CH_Mes_Main_Sys_Bonus_ContentsCategory_03",
             "CH_Mes_Main_Sys_Bonus_ContentsCategory_08", "CH_Mes_Main_Sys_Bonus_ContentsCategory_02",
             "CH_Mes_Main_Sys_Bonus_ContentsCategory_01", "AO_Mes_Main_Sys_Bonus_ContentsCategory_00"]


def clean(text):
    """A message as one line of plain text: the colour tags dropped, lines joined."""
    text = re.sub(r"</?COL[^>]*>|</?COLOR[^>]*>", "", text or "")
    return re.sub(r"\s*[\r\n]+\s*", " ", text).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    tdb = TDB(PE(os.path.join(a.game, "re4.exe")))
    reader = rsz.Reader(tdb)
    paks, table = pak_msg.open_paks(a.game)
    print("%d archives, %d entries" % (len(paks), len(table)), file=sys.stderr)

    record_ids = gen_items.enum(tdb, "chainsaw.GameRecordID")
    bonus_ids = gen_items.enum(tdb, "chainsaw.ExShopBonusID")
    fq_rec, fq_shop = reader.fqn("chainsaw.GameRecordSettingUserData"), reader.fqn("chainsaw.ExContentsShopUserData")
    tables = {}
    for _key, _archive, data in gen_items.user_files_with(table, {fq_rec, fq_shop}):
        insts, _, _ = reader.parse(data)
        for cls, v in insts[1:]:
            if cls in ("chainsaw.GameRecordSettingUserData", "chainsaw.ExContentsShopUserData"):
                if cls in tables:
                    raise SystemExit("two files hold %s" % cls)
                tables[cls] = (insts, v)
    if len(tables) != 2:
        raise SystemExit("the challenges' or the shop's table was not found (%s)" % sorted(tables))

    # Messages by GUID, and by name for <REF name>.
    by_guid, by_name = {}, {}
    for _, pak, e in pak_msg.find_msgs(table):
        try:
            msg = pak_msg.Msg(pak.read(e))
        except Exception:
            continue
        for ent in msg.entries:
            t = msg.text(ent, 1) or ""
            by_guid.setdefault(ent[0].hex(), t)
            by_name.setdefault(ent[1], t)

    def text(guid):
        t = by_guid.get(guid.hex(), "") if guid else ""
        for _ in range(4):
            t2 = re.sub(r"<REF ([^>]+)>", lambda m: by_name.get(m.group(1), m.group(0)), t)
            if t2 == t:
                break
            t = t2
        if "<REF " in t:
            raise SystemExit("unresolved reference in %r" % t)
        return clean(t)

    insts, shop = tables["chainsaw.ExContentsShopUserData"]
    shop_items = {}
    for ref in shop["DataList"]:
        v = insts[ref[1]][1]
        # "A costume for Ashley.": whose a costume or an accessory is (Leon and Ashley each
        # have a Jacket).
        who = re.match(r"An? (?:costume|accessory) for (\w+)", text(v["Detail"]))
        shop_items[v["BonusId"]] = (text(v["Name"]), text(v["LockDetail"]), who.group(1) if who else "")

    bonuses = []
    for bid, enum_name in sorted(bonus_ids.items()):
        kind = next((k for prefix, k in KINDS if enum_name.startswith(prefix)), None)
        if bid < 0 or kind is None or enum_name.endswith("End") or any(b[0] == bid for b in bonuses):
            continue
        name, lock, who = shop_items.get(bid, ("", "", ""))
        name = GAME_MODES.get(enum_name) or name
        if name:
            bonuses.append((bid, kind, name, who, lock))
    unnamed = sorted(set(shop_items) - set(b[0] for b in bonuses))
    if unnamed:
        raise SystemExit("the shop's items %s have no name here" % unnamed)

    insts, rec = tables["chainsaw.GameRecordSettingUserData"]
    challenges = []
    for ref in rec["DataList"]:
        v = insts[ref[1]][1]
        rid = v["RecordId"]
        if rid not in record_ids:
            raise SystemExit("record %d is no GameRecordID" % rid)
        name, cond = text(v["Name"]), text(v["Detail"])
        if v["IsDisable"]:
            # Kept off the game's screen: named by what it unlocks, if anything.
            unlocks = [shop_items[b] for b in v["BonusList"] if b in shop_items] if v["IsEnableBonus"] else []
            if not unlocks:
                continue
            name = ", ".join(u[0] for u in unlocks)
            cond = re.sub(r"^Unlock by completing", "Complete", unlocks[0][1])
        if not name:
            raise SystemExit("record %d has no name" % rid)
        challenges.append((rid, name, cond, bool(v["IsDisable"])))
    challenges.sort()
    print("%d challenges (%d kept off the game's screen), %d bonuses named" %
          (len(challenges), sum(c[3] for c in challenges), len(bonuses)), file=sys.stderr)

    tabs = [clean(by_name.get(name, "")) for name in TABS]
    for name, label in zip(TABS, tabs):
        if not label or "#Rejected#" in label or "<REF " in label:
            raise SystemExit("the challenges screen's tab %s has no label (%r)" % (name, label))
    print("tabs: %s" % ", ".join(tabs), file=sys.stderr)
    shop_tabs = [clean(by_name.get(name, "")) for name in SHOP_TABS]
    for name, label in zip(SHOP_TABS, shop_tabs):
        if not label or "#Rejected#" in label or "<REF " in label:
            raise SystemExit("the Extra Content Shop's tab %s has no label (%r)" % (name, label))
    print("the shop's tabs: %s" % ", ".join(shop_tabs), file=sys.stderr)

    def cstr(s):
        return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')

    out = [
        "#pragma once",
        "",
        "// Generated by tools/gen_challenges.py from the game's files - do not edit.",
        "// The challenges' names and conditions (\"{0}\": the goal) by chainsaw.GameRecordID, and",
        "// the names of what they unlock by chainsaw.ExShopBonusID - the Extra Content Shop's items,",
        "// with the text the shop shows while each is locked - and both screens' tabs.",
        "namespace re4cc::challenges_text {",
        "",
        "enum Kind : int { kMode = 0, kWeapon, kCostume, kAccessory, kModel, kArt, kFilter };",
        "",
        "struct Challenge {",
        "  int id;",
        "  const char* name;",
        "  const char* condition;",
        "  bool hidden;  // kept off the game's screen: named by what it unlocks",
        "};",
        "inline constexpr Challenge kChallenge[] = {",
    ]
    out += ["    {%d, %s, %s, %s}," % (rid, cstr(n), cstr(c), "true" if h else "false") for rid, n, c, h in challenges]
    out += ["};", "inline constexpr int kChallenges = sizeof(kChallenge) / sizeof(kChallenge[0]);", "",
            "struct Bonus {", "  int id;", "  Kind kind;", "  const char* name;",
            "  const char* who;   // whose costume or accessory (\"\": anyone's)",
            "  const char* lock;  // what the Extra Content Shop says while it is locked (\"\": not one of its items)",
            "};",
            "inline constexpr Bonus kBonus[] = {"]
    out += ["    {%d, %s, %s, %s, %s}," % (bid, kind, cstr(n), cstr(w), cstr(l)) for bid, kind, n, w, l in bonuses]
    out += ["};", "inline constexpr int kBonuses = sizeof(kBonus) / sizeof(kBonus[0]);", "",
            "// The challenges screen's tabs as it labels them: All, Incomplete, then the categories",
            "// in challenges.h's Group order.",
            "inline constexpr const char* kTabAll = %s;" % cstr(tabs[0]),
            "inline constexpr const char* kTabIncomplete = %s;" % cstr(tabs[1]),
            "inline constexpr const char* kTabGroup[] = {%s};" % ", ".join(cstr(t) for t in tabs[2:]), "",
            "// The Extra Content Shop's tabs as it labels them, in exshop.h's Tab order: All, then its",
            "// categories.",
            "inline constexpr const char* kShopTab[] = {%s};" % ", ".join(cstr(t) for t in shop_tabs), "",
            "}  // namespace re4cc::challenges_text", ""]
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
