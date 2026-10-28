#!/usr/bin/env python3
"""Generate src/items.h - the item table the panel shows - from the game's own files.

    python3 -I tools/gen_items.py --game "$GAME_DIR" --out src/items.h [--tsv items.tsv]

Every item the game defines comes from its item catalogs: chainsaw.ItemDefinitionUserData
in the main game's (_Chainsaw/AppSystem/UI/UserData/ItemDefinitionUserdata.user.2),
Separate Ways' (_AnotherOrder/...), the Mercenaries' (_Mercenaries/...) and each DLC's -
its size in the attache case (ItemSizeKind _RxC_: R rows by C columns), how many one
piece holds (_StackMax), a knife's durability, a gun's magazine. The English names are
the game's: chainsaw.ItemMessageIdSettingUserdata maps an item to a message's GUID in
the message files (.msg.22). What an item is - chainsaw.ItemType (Gun, Ammo, Key...),
its chainsaw.WeaponID and WeaponCategory, and whether it is a gem, a charm, a recipe,
a treasure map, an attache case or its skin - the game answers only in code:
chainsaw.ItemIDDefine's static switches, evaluated over re4.exe by tools/switch_emu.py.
Their code is found through the type database (tools/tdb_dump.py), by name. What the
merchant can upgrade comes from the upgrade catalogs (chainsaw.WeaponCustomUserdata).

The archives are read with tools/pak_msg.py (RE4's encrypted entry tables included)
and the catalogs parsed with tools/rsz.py. Needs objdump (binutils) for the switches.
"""
import argparse, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pak_msg  # noqa: E402
import rsz  # noqa: E402
from switch_emu import Emu  # noqa: E402
from tdb_dump import PE, TDB, F_LITERAL  # noqa: E402

# The catalogs, by path; a DLC's are found by their contents (any file holding the type).
CATALOGS = [
    ("main", "natives/stm/_Chainsaw/AppSystem/UI/UserData/ItemDefinitionUserdata.user.2"),
    ("ao", "natives/stm/_AnotherOrder/AppSystem/UI/UserData/ItemDefinitionUserData_AO.user.2"),
    ("mc", "natives/stm/_Mercenaries/AppSystem/UI/UserData/ItemDefinitionUserData_MC.user.2"),
]
GAME_BITS = {"main": 1, "ao": 2, "mc": 4}

# The panel's banks - the tabs of the case's Add list and of the item box - and
# the lists off the case's grid; then the groups heading the items, bank by bank.
# The knives go with the grenades - the sub-weapons, as RE2 calls them - but for those
# that can be upgraded (the Combat, Fighting and Primal Knives, Separate Ways' Tactical
# and Elite Knives), which are filed with the weapons, as is everything else that can be.
BANKS = [("kBankWeapons", "Weapons"), ("kBankSubWeapons", "Sub-weapons"), ("kBankAttachments", "Attachments"),
         ("kBankAmmo", "Ammo"), ("kBankRecovery", "Recovery"), ("kBankResources", "Resources"),
         ("kBankKey", "Key items"), ("kBankTreasures", "Treasures"), ("kBankOther", "Other items")]
B = {k: i for i, (k, _) in enumerate(BANKS)}
GROUPS = [
    ("Handguns", "kBankWeapons"), ("Shotguns", "kBankWeapons"), ("Submachine guns", "kBankWeapons"),
    ("Rifles", "kBankWeapons"), ("Magnums", "kBankWeapons"), ("Bows and bolt throwers", "kBankWeapons"),
    ("Rocket launchers", "kBankWeapons"), ("Special knives", "kBankWeapons"), ("Other weapons", "kBankWeapons"),
    ("Knives", "kBankSubWeapons"), ("Grenades and eggs", "kBankSubWeapons"),
    ("Scopes, stocks and sights", "kBankAttachments"),
    ("Ammo", "kBankAmmo"),
    ("Herbs and sprays", "kBankRecovery"), ("Food", "kBankRecovery"),
    ("Resources", "kBankResources"),
    ("Key items", "kBankKey"), ("Keys and tokens", "kBankKey"),
    ("Treasures", "kBankTreasures"), ("Treasures with sockets", "kBankTreasures"), ("Gems", "kBankTreasures"),
    ("Tickets", "kBankTreasures"),
    ("Recipes", "kBankOther"), ("Charms", "kBankOther"), ("Treasure maps", "kBankOther"),
    ("Attache cases", "kBankOther"), ("Armor", "kBankOther"), ("Accessories and other", "kBankOther"),
]
G = {name: i for i, (name, _) in enumerate(GROUPS)}
CATEGORY_GROUP = {"Handgun": "Handguns", "Shotgun": "Shotguns", "Submachinegun": "Submachine guns",
                  "AssaultRifle": "Rifles", "Rifle": "Rifles", "Magnum": "Magnums", "Bow": "Bows and bolt throwers",
                  "RocketLauncher": "Rocket launchers", "Knife": "Knives"}

# chainsaw.ItemType values (the enum, checked against the database below).
TYPE_NAMES = ["Key", "Ammo", "Hurb", "Parts", "Powder", "Gun", "Melee", "Throwing", "Durability", "Treasure",
              "CombinableTreasure", "Money", "Partner", "Enemy", "Unique", "Other"]

# What can be upgraded: the upgrade catalogs (chainsaw.WeaponCustomUserdata - the main game's,
# Separate Ways', the Mercenaries'; found by their contents) give a weapon its stages
# (_WeaponStages, by WeaponID) and an item its own (_ItemStages, by ItemID): common, individual
# and exclusive (_LimitBreak) categories, a category's levels the stages of the element it
# names (stage 0 the level it comes with). A category with a level to buy - a second stage,
# or an exclusive upgrade - is an upgrade; a knife's Repair and Polish (one stage each, and
# all the Body Armor has) mend it.
ELEMENT_FIELD = {"UsableAmmoList": "_CustomUsableAmmo", "Others": "_CustomOtherIndividual"}

# A weapon's category by the ammo it fires (the DLC weapons' ids, which getWeaponCategory
# does not answer in a switch): the main game's ammo, and Separate Ways' and the
# Mercenaries' arrows.
AMMO_CATEGORY = [("it_sm70_500", "Handgun"), ("it_sm70_501", "Magnum"), ("it_sm70_502", "Shotgun"),
                 ("it_sm70_503", "Rifle"), ("it_sm70_504", "Submachinegun"), ("it_sm70_505", "Bow"),
                 ("it_sm70_300", "Bow"), ("it_sm70_200", "Bow"), ("it_wp", "Bow")]

# ItemIDDefine's classifiers the table records, by method name.
# (isRepairableKnife, isInfiniteAmmo and isEnableMoveToArmoury read the game's run-time state: not
# switches, so not evaluated here.)
FLAG_METHODS = ["isJewelItem", "isCharm", "isCraftRecipe", "isTreasureMap", "isAttacheCase", "isAttacheCaseSkin",
                "isArmor", "isToken", "isKnife"]


def enum(tdb, name):
    t = tdb.find_type(name)
    return {tdb.literal_value(f): f["name"] for f in tdb.fields_of(t) if f["flags"] & F_LITERAL}


def method_rva(tdb, type_name, name):
    t = tdb.find_type(type_name)
    for m in tdb.methods_of(t):
        if m["name"] == name and m["code"]:
            return m["code"]
    raise SystemExit("no code for %s.%s" % (type_name, name))


def upgrades_in(insts, common_cats, individual_cats):
    """The WeaponIDs and the ItemIDs a parsed upgrade catalog has a level to buy for."""
    def stages(ref):
        v = insts[ref[1]][1] if ref and ref[1] else {}
        return next((x for k, x in v.items() if k.endswith("CustomStages")), [])

    def upgradable(custom):
        for key, cat_field, cats in (("_Commons", "_CommonCustomCategory", common_cats),
                                     ("_Individuals", "_IndividualCustomCategory", individual_cats)):
            for ref in custom.get(key, []):
                v = insts[ref[1]][1]
                cat = cats.get(v[cat_field], "")
                if len(stages(v.get(ELEMENT_FIELD.get(cat, "_Custom" + cat)))) > 1:
                    return True
        return any(stages(insts[ref[1]][1]["_CustomLimitBreak"]) for ref in custom.get("_LimitBreak", []))

    weapons, item_ids = set(), set()
    for cls, v in insts[1:]:
        if cls == "chainsaw.WeaponCustomUserdata.WeaponStage" and v["_WeaponCustom"][1]:
            if upgradable(insts[v["_WeaponCustom"][1]][1]):
                weapons.add(v["_WeaponID"])
        elif cls == "chainsaw.WeaponCustomUserdata.ItemStage" and v["_ItemCustom"][1]:
            if upgradable(insts[v["_ItemCustom"][1]][1]):
                item_ids.add(v["_ItemID"])
    return weapons, item_ids


def msg_table(table):
    """Every message's English text by its GUID (hex)."""
    out = {}
    for _, pak, e in pak_msg.find_msgs(table):
        try:
            msg = pak_msg.Msg(pak.read(e))
        except Exception:
            continue
        for ent in msg.entries:
            out.setdefault(ent[0].hex(), msg.text(ent, 1) or "")
    return out


def user_files_with(table, fqns, max_size=4 << 20):
    """(path key, archive, data) of every user data file holding an instance of one of the type ids."""
    out = []
    for key, (pak, e) in table.items():
        if e[4] > max_size:
            continue
        try:
            head = pak.read(e, limit=4096)
            # A zstd frame yields nothing until its first block is in whole: read on for those.
            if len(head) < 4 and e[3] > 4096:
                head = pak.read(e, limit=256 << 10)
        except Exception:
            continue
        if head[:4] != b"USR\0":
            continue
        try:
            data = pak.read(e)
        except Exception:
            continue
        if set(rsz.instance_types(data)) & fqns:
            out.append((key, os.path.basename(pak.path), data))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tsv", help="also write every item as a tab-separated table")
    a = ap.parse_args()
    exe = os.path.join(a.game, "re4.exe")
    tdb = TDB(PE(exe))
    reader = rsz.Reader(tdb)
    paks, table = pak_msg.open_paks(a.game)
    print("%d archives, %d entries" % (len(paks), len(table)), file=sys.stderr)

    ids = enum(tdb, "chainsaw.ItemID")
    types = enum(tdb, "chainsaw.ItemType")
    sizes = enum(tdb, "chainsaw.ItemSizeKind")
    cats = enum(tdb, "chainsaw.WeaponCategory")
    common_cats, individual_cats = enum(tdb, "chainsaw.CommonCustomCategory"), enum(tdb, "chainsaw.IndividualCustomCategory")
    assert [types.get(i) for i in range(16)] == TYPE_NAMES, types

    # Item definitions: catalog -> {id: (item definition, weapon definition)}.
    def defs_of(data):
        insts, used, size = reader.parse(data)
        out = {}
        for cls, v in insts[1:]:
            if cls == "chainsaw.ItemDefinitionUserData.Data":
                out[v["_ItemId"]] = (insts[v["_ItemDefineData"][1]][1], insts[v["_WeaponDefineData"][1]][1])
        return out
    defs = {}
    for game, path in CATALOGS:
        data = pak_msg.get(table, path)
        if data is None:
            raise SystemExit("no %s" % path)
        defs[game] = defs_of(data)
    fq_def, fq_msg = reader.fqn("chainsaw.ItemDefinitionUserData"), reader.fqn("chainsaw.ItemMessageIdSettingUserdata")
    fq_over = reader.fqn("chainsaw.ItemMessageIdOverwriteSettingUserdata")
    fq_custom = reader.fqn("chainsaw.WeaponCustomUserdata")
    known = {pak_msg.path_key(p) for _, p in CATALOGS}
    msgfiles = []
    up_weapons, up_items, up_catalogs = set(), set(), 0
    for key, archive, data in user_files_with(table, {fq_def, fq_msg, fq_over, fq_custom}):
        types_in = set(rsz.instance_types(data))
        if fq_custom in types_in:
            w, it = upgrades_in(reader.parse(data)[0], common_cats, individual_cats)
            up_weapons |= w
            up_items |= it
            up_catalogs += 1
        if fq_def in types_in and key not in known:
            d = defs_of(data)
            # Separate Ways' archive (its DLC, 2109300) carries a copy of the main game's catalog
            # beside its own: those items are Separate Ways' too. Another full copy in the base
            # archives is the main game's; a small catalog elsewhere is a DLC's (its items the main
            # game's).
            if archive == "re_dlc_stm_2109300.pak":
                defs["ao%08X" % key[0]] = d
            elif len(d) >= 100:
                defs["main%08X" % key[0]] = d
            else:
                defs["dlc%08X" % key[0]] = d
        if fq_msg in types_in or fq_over in types_in:
            msgfiles.append((key, data))
    print("catalogs: %s" % ", ".join("%s %d" % (k, len(v)) for k, v in defs.items()), file=sys.stderr)
    if not up_weapons:
        raise SystemExit("no upgrade catalog (chainsaw.WeaponCustomUserdata) with a weapon to upgrade")
    print("upgrade catalogs: %d - %d weapons and %d other items can be upgraded" % (
        up_catalogs, len(up_weapons), len(up_items)), file=sys.stderr)

    # Names: the item's name message, from any naming file (the main game's first). A DLC
    # names its own items in a chainsaw.ItemMessageIdOverwriteSettingUserdata, over the main
    # game's naming - whose entries for the DLC attache cases point at messages no file
    # holds, and which has none for the DLC charms.
    names_main = pak_msg.path_key("natives/stm/_Chainsaw/AppSystem/UI/Userdata/ItemMessageIdSettingUserdata.user.2")
    msgfiles.sort(key=lambda kd: kd[0] != names_main)
    name_msg, overwrite = {}, {}
    for key, data in msgfiles:
        insts, _, _ = reader.parse(data)
        for cls, v in insts[1:]:
            if cls == "chainsaw.ItemMessageIdSettingUserdata.Setting" and v["_VariationHash"] == 0:
                name_msg.setdefault(v["_ItemId"], v["_NameMsgId"].hex())
            elif cls == "chainsaw.ItemMessageIdOverwriteSettingUserdata.Setting":
                overwrite[v["_ItemId"]] = v["_NameMsgId"].hex()
    name_msg.update(overwrite)
    texts = msg_table(table)
    print("%d names, %d messages" % (len(name_msg), len(texts)), file=sys.stderr)

    emu = Emu(exe)
    rva_type = method_rva(tdb, "chainsaw.ItemIDDefine", "getItemType")
    rva_weapon = method_rva(tdb, "chainsaw.ItemIDDefine", "getWeaponID")
    rva_cat = method_rva(tdb, "chainsaw.ItemIDDefine", "getWeaponCategory")
    rva_flags = {m: method_rva(tdb, "chainsaw.ItemIDDefine", m) for m in FLAG_METHODS}

    rows = []
    all_ids = sorted(set().union(*[set(d) for d in defs.values()]) | {i for i in ids if i > 0})
    for i in all_ids:
        if i <= 0:
            continue
        games = 0
        dlc = False
        src = None
        for game, d in defs.items():
            if i in d:
                games |= GAME_BITS.get(game[:2] if game[:2] in ("ao", "mc") else "main", 1)
                dlc |= game.startswith("dlc")
                if not game.startswith("main") or not src:
                    src = src or d[i]
        t = emu.run(rva_type, i)
        w = emu.run(rva_weapon, i)
        cat = ""
        if w != -1:
            try:
                cat = cats.get(emu.run(rva_cat, w), "")
            except ValueError:
                # getWeaponCategory calls on into other code for the DLC weapons' ids (6000 and up):
                # their category is read off what they fire (below).
                cat = None
        flags = {m: emu.run(rva, i) & 0xFF != 0 for m, rva in rva_flags.items()}
        name = texts.get(name_msg.get(i, ""), "")
        if not src:
            continue  # an id no catalog defines: never in the game
        idef, wdef = src
        d = wdef if w != -1 and wdef else idef
        size = sizes.get(d["_ItemSize"], "")
        m = re.match(r"^_(\d+)x(\d+)_$", size)
        rows_n, cols = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        if cat is None:
            usable = [ids.get(x, "") for x in (wdef["_UsableAmmoList"] if wdef else [])]
            cat = ""
            for prefix, c in AMMO_CATEGORY:
                if any(u.startswith(prefix) for u in usable):
                    cat = c
                    break
            if not cat and t == 6:
                cat = "Knife"
            if not cat and "Rocket" in name:
                cat = "RocketLauncher"
        rows.append(dict(id=i, enum=ids.get(i, ""), type=t, weapon=w, cat=cat, h=rows_n, w=cols,
                         stack=d["_StackMax"], dur=d["_DefaultDurabilityMax"],
                         ammo=(wdef["_AmmoMax"] if w != -1 and wdef else 0), name=name, games=games, dlc=dlc,
                         upgradable=(w != -1 and w in up_weapons) or i in up_items, **flags))

    # Filing: bank and group by what the item is; offered when it has a real name
    # and the panel can put it somewhere (the case, or one of the lists off it).
    def file(r):
        tn = TYPE_NAMES[r["type"]] if 0 <= r["type"] < 16 else ""
        n = r["name"]
        if tn == "Gun":
            if r["w"] * r["h"] <= 1:
                return None  # a set piece's own gun (the minecart's, the mounted ones): no piece of its own
            return CATEGORY_GROUP.get(r["cat"], "Other weapons")
        if tn == "Melee":
            if not r["isKnife"]:
                return None
            return "Special knives" if r["upgradable"] else "Knives"
        if tn == "Throwing":
            return "Grenades and eggs"
        if tn == "Parts":
            return "Scopes, stocks and sights"
        if tn == "Ammo":
            return "Ammo"
        if tn == "Hurb":
            return "Herbs and sprays" if ("Herb" in n or "Spray" in n) else "Food"
        if tn == "Powder":
            return "Resources"
        if tn == "Key":
            return "Keys and tokens" if (r["isToken"] or r["stack"] > 1) else "Key items"
        if tn == "Treasure":
            if r["id"] == 120800000:  # Spinel: the merchant's own count, not an item
                return None
            if r["isJewelItem"]:
                return "Gems"
            return "Tickets" if "Ticket" in n else "Treasures"
        if tn == "CombinableTreasure":
            return "Treasures with sockets"
        if tn == "Unique":
            if r["isAttacheCase"]:
                return None  # the case's size, not an item (inventory.cpp)
            if r["isCraftRecipe"]:
                return "Recipes"
            if r["isCharm"]:
                return "Charms"
            if r["isTreasureMap"]:
                return "Treasure maps"
            if r["isAttacheCaseSkin"]:
                return "Attache cases"
            if r["isArmor"]:
                return "Armor"
            return "Accessories and other"
        return None  # Money, Partner, Enemy, Other: never the player's to add
    out_rows = []
    for r in rows:
        g = file(r)
        nm = r["name"]
        bad = not nm or "#Rejected#" in nm or nm == "?" or "<COLOR" in nm
        if g == "Charms" and not nm.startswith("Charm"):
            r["name"] = nm = "Charm: " + nm
        if g in ("Gems", "Treasures") and r["type"] == 9 and False:
            pass
        r["group"] = G[g] if g else G["Accessories and other"]
        r["bank"] = B[GROUPS[r["group"]][1]]
        r["offered"] = 1 if g and not bad else 0
        if bad:
            r["name"] = nm = ""
        out_rows.append(r)
        # Whatever can be upgraded is filed with the weapons: one filed anywhere else stops the table.
        if g and r["upgradable"] and r["bank"] != B["kBankWeapons"]:
            raise SystemExit("%s (%s) can be upgraded but is filed under %s" % (nm or "?", r["enum"], GROUPS[r["group"]][0]))
    # The panel's order: bank, group, then the game's own (its ids run in the game's order).
    order = sorted(range(len(out_rows)), key=lambda k: (out_rows[k]["bank"], out_rows[k]["group"], out_rows[k]["id"]))
    rank = {k: n for n, k in enumerate(order)}

    def cstr(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("// Generated by tools/gen_items.py from re4.exe (build 22377325) and its archives - do not edit")
    lines.append("// by hand. Every item the game's catalogs define (the main game's, Separate Ways', the")
    lines.append("// Mercenaries' and each DLC's), with what the game itself says it is - chainsaw.ItemType, its")
    lines.append("// WeaponID and category, ItemIDDefine's classifiers - its size in the attache case, how many")
    lines.append("// one piece holds and the English game's name. The game keeps the same data at run time")
    lines.append("// (each item's _ItemDefine), and the mod reads it there for the items it touches: this table")
    lines.append("// is the panel's names, filing and add lists.")
    lines.append("namespace re4cc::items {")
    lines.append("")
    lines.append("// chainsaw.ItemType.")
    lines.append("enum Type : uint8_t {")
    lines.append("  " + ", ".join("k%s = %d" % (n, v) for v, n in enumerate(TYPE_NAMES)) + ",")
    lines.append("};")
    lines.append("")
    lines.append("// The panel's banks - the tabs of the case's Add list and of the item box - then the lists off")
    lines.append("// the case's grid; and the groups the lists head their items with, in the order they show them.")
    lines.append("enum Bank : uint8_t { %s, kBanks };" % ", ".join(k for k, _ in BANKS))
    lines.append("constexpr int kCaseBanks = %d;  // the banks of what takes room in the case" % B["kBankKey"])
    lines.append("inline const char* bank_name(int b) {")
    lines.append("  static const char* const kNames[kBanks] = {%s};" % ", ".join(cstr(n) for _, n in BANKS))
    lines.append('  return b >= 0 && b < kBanks ? kNames[b] : "?";')
    lines.append("}")
    lines.append("struct Group {")
    lines.append("  const char* name;")
    lines.append("  uint8_t bank;")
    lines.append("};")
    lines.append("constexpr int kGroupCount = %d;" % len(GROUPS))
    lines.append("inline constexpr Group kGroups[kGroupCount] = {")
    for n, bk in GROUPS:
        lines.append("    {%s, %s}," % (cstr(n), bk))
    lines.append("};")
    lines.append("")
    lines.append("// Which game's catalog defines it.")
    lines.append("enum Games : uint8_t { kMain = 1, kSeparateWays = 2, kMercenaries = 4 };")
    lines.append("// ItemIDDefine's classifiers, as bits.")
    lines.append("enum Flags : uint16_t {")
    for k, m in enumerate(FLAG_METHODS):
        lines.append("  kF%s = 1 << %d," % (m[2:] if m.startswith("is") else m, k))
    lines.append("};")
    lines.append("")
    lines.append("struct Def {")
    lines.append("  int32_t id;           // chainsaw.ItemID")
    lines.append("  const char* name;     // the English game's (\"\" for one the game never names)")
    lines.append("  uint8_t type;         // chainsaw.ItemType")
    lines.append("  uint8_t bank, group;  // the panel's filing")
    lines.append("  uint8_t offered;      // the panel's add lists offer it")
    lines.append("  uint8_t games;        // Games")
    lines.append("  uint8_t dlc;          // a DLC's catalog defines it")
    lines.append("  uint8_t w, h;         // its piece in the attache case: columns, rows (ItemSizeKind)")
    lines.append("  int32_t stack;        // how many one piece (or one entry off the grid) holds: _StackMax")
    lines.append("  int16_t durability;   // a knife's: _DefaultDurabilityMax")
    lines.append("  int16_t ammo;         // a gun's magazine: _AmmoMax")
    lines.append("  int16_t weapon;       // chainsaw.WeaponID (-1: not a weapon)")
    lines.append("  uint16_t flags;       // Flags")
    lines.append("  int16_t rank;         // its place in the panel's lists")
    lines.append("};")
    lines.append("")
    lines.append("constexpr int kCount = %d;" % len(out_rows))
    lines.append("// By id (find() searches it).")
    lines.append("inline constexpr Def kItems[kCount] = {")
    for k, r in enumerate(out_rows):
        fl = 0
        for b, m in enumerate(FLAG_METHODS):
            if r[m]:
                fl |= 1 << b
        lines.append("    {%d, %s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, 0x%X, %d},  // %s %s; %s" % (
            r["id"], cstr(r["name"]), r["type"], r["bank"], r["group"], r["offered"], r["games"], 1 if r["dlc"] else 0,
            r["w"], r["h"], min(r["stack"], 2147483647), min(r["dur"], 32767), min(r["ammo"], 32767), r["weapon"], fl,
            rank[k], r["enum"], TYPE_NAMES[r["type"]] if 0 <= r["type"] < 16 else r["type"], GROUPS[r["group"]][0]))
    lines.append("};")
    lines.append("// kItems' indices in the panel's order: bank, group, then the game's own.")
    lines.append("inline constexpr int16_t kByRank[kCount] = {")
    for s in range(0, len(order), 16):
        lines.append("    " + ", ".join(str(x) for x in order[s:s + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("inline const Def* find(int32_t id) {")
    lines.append("  int lo = 0, hi = kCount - 1;")
    lines.append("  while (lo <= hi) {")
    lines.append("    const int mid = (lo + hi) / 2;")
    lines.append("    if (kItems[mid].id == id) return &kItems[mid];")
    lines.append("    if (kItems[mid].id < id) lo = mid + 1;")
    lines.append("    else hi = mid - 1;")
    lines.append("  }")
    lines.append("  return nullptr;")
    lines.append("}")
    lines.append("inline int index_of(int32_t id) {")
    lines.append("  const Def* d = find(id);")
    lines.append("  return d ? static_cast<int>(d - kItems) : -1;")
    lines.append("}")
    lines.append("inline const char* name(int32_t id) {")
    lines.append("  const Def* d = find(id);")
    lines.append('  return d && d->name[0] ? d->name : "";')
    lines.append("}")
    lines.append("// Takes room in the attache case (has a piece on its grid).")
    lines.append("inline bool in_case(int32_t id) {")
    lines.append("  const Def* d = find(id);")
    lines.append("  return d && d->bank < kCaseBanks && d->w;")
    lines.append("}")
    lines.append("// A weapon with a state of its own - a gun's rounds and upgrades, a knife's durability: one")
    lines.append("// piece each, never stacked.")
    lines.append("inline bool keeps_state(int32_t id) {")
    lines.append("  const Def* d = find(id);")
    lines.append("  return d && d->weapon >= 0 && (d->type == kGun || d->type == kMelee);")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace re4cc::items")
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s: %d items, %d offered" % (a.out, len(out_rows), sum(r["offered"] for r in out_rows)), file=sys.stderr)
    if a.tsv:
        with open(a.tsv, "w", encoding="utf-8") as f:
            f.write("id\tenum\ttype\tweapon\tcategory\tw\th\tstack\tdurability\tammo\tgames\tdlc\tupgradable\tgroup\t"
                    "offered\tname\n")
            for r in out_rows:
                f.write("%d\t%s\t%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d\t%s\n" % (
                    r["id"], r["enum"], TYPE_NAMES[r["type"]] if 0 <= r["type"] < 16 else r["type"], r["weapon"],
                    r["cat"], r["w"], r["h"], r["stack"], r["dur"], r["ammo"], r["games"], 1 if r["dlc"] else 0,
                    1 if r["upgradable"] else 0, GROUPS[r["group"]][0], r["offered"], r["name"]))


if __name__ == "__main__":
    main()
