# Always Scan —— Nexus Mods 发布文案

> 这个文件是**上传 Nexus 时要粘贴的文字**，不放进压缩包（包内只有 `README.txt`）。
>
> 用法：
> 1. **Mod name** 与 **Summary** 直接抄下面「① 上传字段」里的两行；
> 2. **Description** 用「② 英文正文」（Nexus 是英文站，玩家与审核都看英文）；
> 3. 中文版（③）留作备用 —— 发中文社区（3DM / NGA / 贴吧）或自己对照时用；
> 4. Nexus 的描述编辑器吃 Markdown/BBCode。若粘贴后格式丢失，把 `**粗体**` 换成
>    `[b]粗体[/b]`、`- 列表项` 换成 `[list][*]列表项[/list]` 即可。
>
> 上传的文件就是 `dist\StarfieldAlwaysScan-<version>.zip`（`tools\package-nexus.ps1` 生成）。

---

## ① 上传字段（Nexus 表单里直接填）

| 字段 | 内容 |
| --- | --- |
| **Mod name** | `Always Scan - Always-On Scanner Highlighting (SFSE)` |
| **Summary**（约 250 字符以内） | `Keep the scanner highlight on at all times. No need to hold the handheld scanner: everything inside a configurable radius gets the vanilla outline, color-coded by category. Full-radius highlighting, one toggle hotkey, fully configurable via INI. SFSE plugin.` |
| **Version** | `2.0`（引擎 build 5.1.0；Nexus 上的 1.0 → … → 1.8.1 → 2.0。★ 2026-09-27 又修了四个坑：「扫描后不变色」×2 + 「帧数仍有下降」+「**动态场景（走动 / 战斗）卡顿**」，**公开版号仍按用户要求保持 2.0**） |
| **Category** | `Gameplay`（Alternate suggestion: `Items and Objects - Gameplay`） |
| **Requirements（依赖）** | `Starfield Script Extender (SFSE) 0.2.21+`、`(1.16.244.0) SFSE Address Library`、游戏版本 `1.16.244.0` |
| **主文件（Main file）** | `StarfieldAlwaysScan-2.0.zip` |
| **Permissions** | 允许转载需注明出处？建议：**不得重新上传（No re-uploading）**；允许个人修改（源码已含 Papyrus 部分） |

---

## ② 英文正文（Description）

**Always Scan — always-on scanner highlighting**

In the vanilla game you have to equip the handheld scanner to see which objects can be picked up, searched or used — and while the scanner is up, your weapon and other tools are unavailable. Even then, only what sits inside the small circle in the middle of the screen gets highlighted.

**Always Scan removes both limitations.** The highlight stays on whenever you want it, and everything around you inside a configurable radius (default **50 m**) is outlined — no circle, no need to raise the scanner.

**Visually it is the vanilla feature.** The mod does not draw its own glow: it drives the game's own outline system, so shape, thickness and pulse behave exactly like the handheld scanner — only the per-category colors are assigned by the mod (and every one of them is configurable).

### Features

- **Always on** — highlighting stays visible while you run, fight, loot or fly. The scanner stays in your pocket (or on your back).
- **Full radius** — objects inside the configured radius are highlighted in every direction, not just the center circle. Radius is configurable from 5 to 500 meters.
- **Category colors** — every kind of object gets its category color on the outline (all configurable; categories that share a colour share one group). Pick-up items follow the game's own inventory categories:

  | Category | Example | Default color |
  | --- | --- | --- |
  | Weapons & throwables | guns, melee, grenades, mines | **red** |
  | Spacesuits / helmets / packs / clothing | anything in the Apparel tab | **red** (one colour for the whole equipment set since 1.8.0, same as weapons; the game's own light cyan-blue in 1.7.9) |
  | Ammo & aid | ammo, meds, food, drinks | **the game's own green** (states 4/5 are shared with the vanilla "scanned planet target" green since 1.7.6; `ColorAmmoAid=00FF66` brings bright green back) |
  | Notes | notes, data slates, magazines, books | **purple** (same colour as resources since 1.8.0; the game's own cyan in 1.7.9) |
  | Resources | iron, aluminium, helium-3, organics, ... | **purple** |
  | Misc items | digipicks, credits, toys | **blue (unchanged from 1.5)** |
  | Containers | crates, safes, lockers | **orange** (back since 1.7.9; briefly the game's own cyan in 1.7.8) |
  | Bodies / corpses | dead people, creatures, and wrecked robots / turrets | **orange** (same colour as containers; back since 1.7.9) |
  | Interactive devices | switches, terminals, workbenches | **the game's own green** (same note as above; `ColorDevice=00E5FF` brings cyan back) |
  | Doors | — | **white** (back since 1.7.9; briefly the game's own cyan in 1.7.8) |
  | Flora | plants — the game stores ores, gas vents and liquid pools in this same record type too (see below) | **the game's own colours**: cyan pulse while unscanned, **green** once scanned — with or without the scanner up; a confirmed "surveyed" verdict is remembered **per reference** (since 2.0 build 5.1.0; per species in 1.8.1), across scenes and across game sessions, so a target cannot drop back to cyan — and scanning one plant no longer turns the rest of its species green |

  Colors are written by the mod through the engine's per-state colour block, and **only five of the eleven states are written at all** (misc blue, notes & resources purple, doors white, containers / corpses orange, equipment red). The other six are owned by the game and are never touched: states **0 / 1** — which the game itself uses while the scanner is up to outline plain references, **people included** (native cyan / light cyan; that is why NPCs look vanilla since 1.7.8) — plus **4 / 5** (scanned planet targets, native green; since 1.7.6) and **7 / 8** (unscanned planet targets, native cyan; since 1.7.5). **Grouping since 1.8.0:** the whole equipment set (weapons, throwables, suits, helmets, packs, clothing) shares one **red**, notes share the resources' **purple**, and ammo & aid keep the game's own **green** — every requested colour now fits inside the five borrowable states with nothing left over, so no colour has to step aside anymore. (1.7.9 still had to give up apparel and notes to bring back "containers orange / doors white"; that trade-off is history — both are back to their group colours now.)

  Since **1.7.3** the outline is drawn as a **contour instead of a fill**: the mod writes alpha=0 into the outline's base color — the same "no fill" state the engine itself uses for its scannable targets — so you see the item's own material and shape in full, with the category color sitting on the contour. Set `NoFill=0` in the INI to restore the old filled look (1.7.1 / 1.7.2). Note for upgraders from 1.7.2: that release's opacity options (`AlphaXxx`) turned out to have **no visible effect at all** — pixel measurements on a screenshot showed the covered area was pixel-identical at 40% and 100%, because the engine ignores that alpha byte for the fill. `AlphaXxx` now only changes the pulse (contour) transparency.

  **Planet surveying (1.7.5 / 1.7.6 / 1.7.7).** The game stores ores, gas vents, liquid pools **and** plants in the same record type (FLOR) and colors them by "scanned / not scanned" through outline states that the vanilla scanner owns: an unscanned target gets a **cyan pulse** (two states), a scanned one gets **green** (two other states). The mod paints this category with the game's own "scannable target" colour while the scanner is away, and it **never writes the colours of any of those four states** — 1.7.5 handed the first pair back, 1.7.6 the second, after a bug report that scanned deposits never turned green (they had been showing the "devices" / "ammo & aid" colours instead, because those two categories were sharing the very states the game uses for scanned targets). While you hold the scanner up the mod steps aside for this category only (`YieldTargetsWhileScanning=1`, on by default): it writes nothing, so the game's own "scanned / not scanned / being scanned" colours are exactly what you see — unscanned deposits cyan, scanned ones **green**, near or far. Every other category is still refreshed while the scanner is up, so highlighting beyond the scanner's centre circle keeps working during a survey. (1.7.4 switched this category off entirely to leave it to the vanilla scanner — which turned out to mean "no color at all on planets", because the vanilla scanner only paints those targets while you hold it up.)

  **1.7.7 finishes that story for the other half of the time.** Once you *put the scanner away* the mod re-hangs the highlight itself, and until 1.7.7 it only ever used the vanilla "not scanned" state — so a surveyed deposit went back to cyan and "already done" was indistinguishable from "still to do". 1.7.7 asks the game instead of guessing, from three independent sources (any one is enough): **(a)** the game's own "has this reference been surveyed?" query — the very native function the game exposes as `IsScanned`, which answers per reference (the game resolves plants, deposits and creatures internally), so it is correct for plants as well; **(b)** the outline state the game itself painted while the scanner was up (remembered for the session); **(c)** the game's own check function (the one the vanilla scanner uses to decide whether a resource has entered your survey data) plus the game's own record chain (flora record → the item it produces → that item's resource) — this one only applies to flora that produce a **level list**, i.e. the ore / gas / liquid deposits, exactly as the game itself requires. Ones you have already surveyed come back **green** (the same state and colour the game uses while the scanner is up), unscanned ones stay cyan — scan something, lower the scanner, and it turns green within about 0.2 s. *(Build 4.28.0 fixed a report that **plants** showed the "already surveyed" green before being scanned: source (c) had been applied to plants too, whose produced item is a plain item instead of a level list, so "the resource this plant yields is in your survey data" was mistaken for "this plant has been surveyed". Plants now answer to sources (a) / (b) only; ores / gas / liquids are unchanged.)* `FloraScannedByEngineState=0` / `FloraScannedByResource=0` (or `StateFloraScanned=7`) restores the old behaviour without swapping the DLL. Startup log line to look for: `flora scanned: … ready`; the stats line `planet targets (窗口内): 未扫描=… 已扫描=… | 引擎状态: 问=… 已扫描=…` shows the split (and `flora scan:` lines list the individual checks).

  **1.7.8 stops the mod from repainting people.** While the scanner is up, the game itself outlines ordinary references — pedestrians included — with two outline states whose native colours are cyan (far) / light cyan (near). Until 1.7.7 the mod had been overwriting exactly those two states with the "weapons red" and "apparel magenta" colours, so **every NPC showed the red / magenta the game reserves for its bounty markers** (the bug report that started this release: three pedestrians, one red, two magenta). Both states are now left completely alone — not a single byte written — and people went back to the vanilla cyan. Weapons and apparel (still red / magenta) moved to two low-traffic states; containers / corpses and doors, which used to occupy those two, now use the game's own cyan as well. Net effect: the mod recolours five states in total, and everything the game itself paints keeps its native colour.

  Resources are recognised from the item record itself (the game marks them with its own `ResourceType` keywords), so vanilla and mod-added resources both get the resource color. 1.7.4 and earlier read that keyword array in the wrong byte order, so **no item ever matched** and every resource silently fell back to the misc color; 1.7.5 reads it correctly (and re-calibrates the offset by itself if the game ever moves it — the log says which layout was adopted). If the check cannot be applied the mod falls back to the misc color — the log tells you which one happened.

- **Bodies are highlighted** — pre-placed corpses and everything you kill: humans, creatures, robots and turrets alike (including the wrecked machines already lying around). They behave like containers: empty them and the outline goes away.
- **Looted means dark** — a container or a body that has nothing left to take stops being outlined (about a second later), so an outline always means "there is still something in there".
- **Display cases included** — weapon cases, weapon racks, helmet / backpack / datapad racks and the outpost display cases glow even while they are closed, and they go dark once you have taken everything out of them. Taking only part of the contents, or opening one without taking anything, keeps it lit (by design).

- **Toggle hotkey** — press **F8** (any key can be configured) to switch everything on or off, with a short HUD confirmation (`Always Scan: ON / OFF`). You can also choose to start with the feature disabled.
- **Vanilla scanner friendly** — using the handheld scanner yourself is perfectly fine. When you put it down, the mod automatically re-applies its highlights within a second, so nothing "goes dark" after scanning.
- **Interaction safe** — this mod only decides **which objects the engine outlines**. Activation, pick-up prompts, dialogue, doors, computers and crafting all work exactly like vanilla.
- **Never highlights living beings** — living NPCs and creatures are excluded by design; only dead bodies are outlined (and `CorpseUnconscious=1` optionally includes knocked-out NPCs, off by default).
- **Stays on while you walk** — crossing the invisible border between two outdoor cells no longer drops the highlight: what was glowing keeps glowing, and the objects on the other side of the border are outlined too while you are near it.
- **Performance aware** — the world is scanned every 200 ms with per-pass budgets and a leave-grace period, so walking around does not cause stutter. The surrounding cells (kept outlined across cell borders) are walked in **rotating slices** — one slice per pass — so a single pass never walks the whole neighbourhood, and the slice count is capped so a full rotation always finishes inside the leave-grace period (slicing can never make a highlight flicker). While the vanilla scanner is up, its own book-keeping is only ever read, never written. The log breaks the scan time down if you ever need to verify.

### Requirements

- **Starfield 1.16.244.0** (the DLL resolves engine functions by address-library ID; a different game version needs a rebuild)
- **SFSE — Starfield Script Extender 0.2.21** or newer
- **"(1.16.244.0) SFSE Address Library"**

### Installation

Install the archive with **Mod Organizer 2** or **Vortex**. The archive root is the `Data` folder layout:

```
StarfieldAlwaysScan.esm              -> Data\
SAS_AlwaysScan.ini                   -> Data\                (config, next to the esm)
Scripts\SAS_Bridge.pex               -> Data\Scripts\
Scripts\Source\SAS\SAS_Bridge.psc    -> Data\Scripts\Source\SAS\  (source, optional)
SFSE\Plugins\SAS_AlwaysScan.dll      -> Data\SFSE\Plugins\
```

Enable `StarfieldAlwaysScan.esm` in your load order and play.

**Manual installation:** extract the archive into `...\Starfield\Data\` and merge folders.

### Configuration

Everything lives in `SAS_AlwaysScan.ini`, **next to `StarfieldAlwaysScan.esm`** (with Mod Organizer 2 that is inside the mod's own folder; manual install: `…\Starfield\Data\SAS_AlwaysScan.ini`) — heavily commented, read once at game start. The old location (`Data\SFSE\Plugins\`) is still read as a fallback if the new file does not exist.

Most used options:

- `HotkeyVK=119` — toggle key (119 = F8; the INI has a table of common key codes; set to 0 to disable the hotkey)
- `RadiusMeters=50` — highlight radius around the player
- `StartEnabled=1` — on by default; set to 0 to start disabled
- `StateWeapon=10` / `StateApparel=10` / `StateAmmoAid=5` / `StateNote=3` / `StateResource=3` / `StateLoot=2` / `StateContainer=9` / `StateDevice=4` / `StateDoor=6` / `StateFlora=7` — outline color state per category (the INI documents all 11 available native colors). States **0 / 1** are what the game itself uses while the scanner is up to outline plain references and **people** (native cyan / light cyan; never written since 1.7.8 — that is the NPC fix), states **7 / 8** are the vanilla scanner's "not-yet-scanned planet target" slots (never written since 1.7.4) and states **4 / 5** are its "already scanned" slots (native green, never written since 1.7.6). Grouping since 1.8.0: equipment (weapons / throwables / suits / helmets / packs / clothing) = state 10 (red), notes & resources = state 3 (purple), containers & corpses = state 9 (orange), doors = state 6 (white), ammo & aid / devices = states 5 / 4 (the game's own green), flora (ores / gas / liquid deposits / plants) = state 7 (the game's own cyan). Five states are written in total (2 / 3 / 6 / 9 / 10); no other state is touched.
- `ColorWeapon=FF2E2E` / `ColorApparel=FF2E2E` / `ColorNote=B36BFF` / `ColorResource=B36BFF` / `ColorContainer=FF9500` / `ColorCorpse=FF9500` / `ColorDoor=FFFFFF` / `ColorLoot=1F8EE2` — exact RGB per category, **written by default** (five states in total: 2 / 3 / 6 / 9 / 10). `ColorFlora` is left unset on purpose (the game's own cyan), and so are `ColorAmmoAid` / `ColorDevice` since 1.7.6 (the game's own green). They write the engine's global per-state color block, so the same state of the vanilla scanner changes as well (the INI explains it — change or clear them freely). Log line to watch: `outline colors: state=N 覆盖为 #RRGGBB <- <category>`.
- `NoFill=1` — draw the outline as a **contour only** (default since 1.7.3): the fill layer's alpha is written as 0, so the item's own material stays visible. Set `NoFill=0` for the old filled look (1.7.1 / 1.7.2). Log line: `config: noFill=1 ...`, plus the "no fill" wording on every `outline colors: state=N ...` line.
- `AlphaWeapon=0` / `AlphaApparel=0` / `AlphaDoor=0` / … — **pulse (contour) opacity** per category (0-255). 0 = keep the engine's own value (the default), 255 = a fixed, non-breathing contour. **This cannot make the fill transparent** (the engine ignores that alpha byte for the fill — that is why 1.7.2 had no visible effect); use `NoFill` for that. Log lines: `config: colorAlpha ...`, and the `a=` column in `outline colors[...]` / `renderer params[...]`.
- `ResourceByKeyword=1` — recognise resources from the item record's own `ResourceType` keywords (default on). 1.7.5 fixed the byte order this array was read with (1.7.4 and earlier never matched anything) and added a self-calibrating offset; the log prints `关键词数组标定 = base+0x…` once. Set to 0 and every MISC item counts as misc (the 1.5 behaviour).
- `FloraScannedByResource=1` — **new in 1.7.7**: ask the game itself whether a planet target (ore / gas / liquid deposit, plant) has been surveyed, and paint surveyed ones green with the scanner away (default on). The mod calls the very function the vanilla scanner uses and follows the game's own record chain; failures are non-fatal (the target then simply stays cyan, and the log says why). 0 = old behaviour: the whole flora category uses `StateFlora` only.
- `StateFloraScanned=5` — **new in 1.7.7**: outline state used for **already-surveyed** planet targets. Default **5** = the game's own "scanned planet target" state, native colour **green `#27C684`** — the same slot and colour the game uses while the scanner is up, so nothing new is introduced. Set it to 7 (= `StateFlora`) to make scanned and unscanned look the same again.
- `FloraScanProbeMax=8` — **new in 1.7.7**, diagnostic: how many `flora scan:` lines to write per session (0 = off). Each line is one real query and carries the pointers of the record chain (`base=… produceItem=…(LVLI) misc=…(MISC) irES=…(IRES) -> 已扫描=1/0`), which is what you would send in a bug report if the split ever looks wrong.
- `YieldTargetsWhileScanning=1` — while you hold the scanner up, leave the flora / mineral-deposit category alone (default on). The vanilla scanner paints scannable planet targets itself, so keeping this on is what preserves the game's own "scanned / not scanned / being scanned" colours — including the **green** for already-scanned targets (states 4 / 5, protected since 1.7.6). Every other category is still refreshed (highlighting beyond the scanner's centre circle keeps working). 0 = old behaviour (refresh that category even while the scanner is up); the log's `skip (窗口内)` line shows `yield=N` growing while you survey.
- `ManagerOccupancyProbe=1` — diagnostic (default on): 1.5 s after you raise the scanner, one log line lists how many entries each of the 11 outline states holds (`manager occupancy[举着扫描仪]: 0=5 1=2 … 10=1`). This is direct evidence of which states the game itself is writing; at most 6 lines per session, read-only. Keep it on if you might report a colour issue.
- `EnableLoot=1` … `EnableOther=0` — per-category on/off switches (`EnableWeapon` / `EnableApparel` / `EnableAmmoAid` / `EnableNote` / `EnableResource` / `EnableLoot` = misc items / containers / devices / doors / flora / bodies). `EnableFlora=1` (default since 1.7.5; it was 0 in 1.7.4) outlines flora / mineral deposits in the game's own cyan; set it to 0 to leave that category entirely to the vanilla scanner (you will only see those targets while the scanner is up). `EnableOther` covers movable statics (cardboard boxes, tables, crates). It is **off by default** because they cannot be picked up and the vanilla scanner does not outline them either.
- `EnableCorpse=1` — outline dead bodies (people and creatures); `StateCorpse=9` sets their color.
- `SkipEmptyLoot=1` — stop outlining containers / bodies once they are empty (see below).
- `TreatNullInvAsEmpty=1` — a reference whose inventory was never created counts as empty (default on). Only relevant if you ever find a body / container you never looted staying dark; set it to 0 to revert to "always outline".
- `CorpseUnconscious=1` — also outline "unconscious" units. In the vanilla data most of those are **wrecked robots and turrets you can loot** (that is why it is on by default); a handful are knocked-out living characters, which are lootable as well. Set it to 0 if you ever see a *walking* NPC outlined.
- `CorpseLifeState=1` — decide "dead" through the engine's own life-state check. This is what makes enemies, creatures, robots and turrets light up the **moment you kill them**; set it to 0 only for debugging. `CorpseBleedout=1` also includes downed / bleeding-out units (default on; they are lootable).
- `SkipNonPlayableLoot=1` / `SkipEquippedLoot=1` — "looted means dark" only counts what you can actually take: the invisible NPC-only gear every body carries, and gear a killed actor is still wearing, are ignored (both default on; set to 0 only for debugging).
- `SkipDisplayCaseEmpty=1` — display cases (weapon cases, racks, outpost displays) glow even while closed (default on): the game does not keep their contents in the normal container inventory until the case is opened, so the usual "empty" check does not apply to them. Set to 0 only to compare with the old behaviour.
- `DisplayCaseUiEmpty=1` — a display case you have emptied goes dark and stays dark (default on); the mod tracks what you take out of it, item by item.
- `ActorChangeProbeMax=32` / `LootProbeMax=16` — diagnostic only: the log prints a line whenever an actor's verdict changes (e.g. the moment you kill it) and lists what is left inside bodies that still count as "has loot" (0 = off). Leave them alone unless you are reporting a highlight bug.
- `ActorProbeMax=24` — diagnostic only: how many per-actor lines (`actor probe:`) the log prints per session (0 = off). Leave it alone unless you are reporting a highlight bug.
- `ExteriorContinuous=1` — treat walking across an outdoor cell border as a continuous transition: highlights are kept, and the cell you came from is scanned as well (default on). Set to 0 only to compare with the old behaviour.
- `SettleOnCellCrossMs=300` — how long to pause after such a border cross (0 = no pause).
- `StreamJumpTolerance=256` — while you walk, the engine streams objects in and out; small changes like that no longer interrupt scanning. 0 = old behaviour.
- `Verify3DPerScan=32` — per pass, re-check this many outlined objects and re-apply the outline if the engine rebuilt their 3D (streaming). This is what cures the rare "it was glowing, then suddenly went dark" case out in the open. 0 = off.
- `RingSliceMaxRounds=5` — new in build 5.1.0: the cells around the one you stand in (kept outlined across borders) are walked in rotating slices — one slice per pass — so a single pass never walks the whole neighbourhood (together those lists may hold up to 60,000 references, and walking all of them took ~55 ms per pass in a busy scene — the "frame rate still drops" fix). The cell you stand in is always walked in full, and lists of 2000 references or fewer are never sliced. The slice count is capped automatically so one full rotation finishes inside the leave-grace period (`UnhighlightGraceMs`), i.e. slicing can never make a highlight flicker. 0 = disabled (walk everything every pass).
- `MaxTargets=256` — how many objects can be outlined at once
- `OnlyInFront=0` — if you prefer "only what you actually face", set to 1 (original scanner-like behaviour)

### Compatibility

- Only decides outline states for references in the current cell/space — the same reach the vanilla scanner has — limited by the configured radius. Outdoors, the cells you have recently walked through count as part of that reach, so highlights survive a border cross.
- No known conflicts: other mods that change loot, NPCs or level lists are unaffected.
- Works together with the vanilla scanner (see above).

### Troubleshooting

Log: `SAS_AlwaysScan.log` — since **1.5** it is written **next to the mod's `StarfieldAlwaysScan.esm`** (with Mod Organizer 2: inside the mod's own folder; manual install: `…\Starfield\Data\SAS_AlwaysScan.log`). In 1.4 and older it was `Documents\My Games\Starfield\SFSE\Logs\`.

At startup the log prints the active config and whether the native outline functions were found (`native outline ready`). During play it prints a stats line every 5 seconds: object counts, categories, timings. If something looks wrong, that log is enough to diagnose it.

- **Nothing is highlighted** — check `StarfieldAlwaysScan.esm` is enabled, SFSE is installed, and the address library matches your game version.
- **Something is highlighted that you don't want** — set its category to 0 in the INI, or send me the log's `candTypes` line (it shows the form type of everything considered).
- **Colors too similar** — since 1.7.1 every category has its own high-contrast color, so this should not happen. If you still want different ones: edit the `ColorXxx` values (the `colorOverride` line in the log lists what is active, and the `renderer params` lines show what the renderer actually received).
- **The color covers the object so you cannot see its material** — that cannot happen since 1.7.3 (the outline is a contour, not a fill). If you set `NoFill=0` yourself, set it back to 1. Note that `AlphaXxx` cannot fix a fill: it only affects the pulse, because the engine ignores the fill's alpha byte (this is why 1.7.2 had no visible effect).

### Notes / known limitations

- Objects are outlined inside the current cell / loading space, same as the vanilla scanner. When you cross a loading door, the new area is highlighted within about a second.
- Outdoors, walking across the invisible border between two cells no longer drops the highlights: what was glowing stays glowing, and the objects on the other side of the border are outlined too while you are near it. If a model is unloaded and rebuilt by the engine's streaming (very common outdoors), the outline is re-applied automatically (`Verify3DPerScan`).
- "Empty" is decided by reading the reference's inventory (item stacks whose count is zero do not count). This needs a one-time memory-offset calibration, which **keeps retrying until it succeeds** (it samples nearby containers and actors) — so it also starts working if the area you started in had nothing to sample from. Until it succeeds the mod simply keeps outlining containers / bodies as before (the log says so) — it never crashes and never hides something that still has loot.
- A reference whose inventory list was **never created** by the engine counts as empty (`TreatNullInvAsEmpty=1`) — such a reference has nothing to loot by definition. If you ever meet a body / container that stays dark but should glow, set that option to 0.
- "Looted means dark" only counts what you can **take**: the invisible NPC-only gear (`_NOTPLAYABLE`) that every body carries, and gear a killed actor is still wearing, are ignored — that is what makes a fully looted body really go dark. With *Simple Immersive Looting* (Nexus 12677) installed, using its **Strip** option unequips a body's gear, so the body starts glowing again (the gear is takeable now); loot it and the outline goes away.
- Display cases (weapon cases, weapon racks, outpost display cases) are handled separately from normal containers: they glow even while closed, because the game keeps their contents out of the normal container inventory until the case is opened. Once you have taken everything out of one, its outline goes away and stays away; taking only part of the contents leaves it lit.
- The SFSE log (`SAS_AlwaysScan.log`) is capped at 1 MiB: once it grows past that, it is emptied and starts over.
- Quest objects are not marked specially.
- Outlines can only appear inside the current cell / space (engine limit) — bodies in another loading area are not highlighted.
- `CorpseUnconscious=1` (default) also lights up the rare knocked-out *living* character. They are lootable while they are down; if one ever stands up while staying outlined, set the option to 0 in the INI.
- `ColorXxx=RRGGBB` lets you set exact colors, but be aware this writes into the engine's global color table, so the vanilla scanner's color for the same state changes too. Leaving the `ColorXxx` lines empty (default) changes nothing.
- `NoFill=1` writes alpha=0 into the outline's base color ("no fill" — contour only, default since 1.7.3); `NoFill=0` keeps the 1.7.1/1.7.2 filled look. `AlphaXxx=0-255` sets the **pulse** opacity per category (0 = keep the engine's own value, 255 = a fixed, non-breathing contour); it cannot make the fill transparent. Both are written into the same per-state color block as `ColorXxx`.

### Version history

*(This release is **2.0** — the build number inside the plugin is 5.1.0. The list below uses the plugin's internal build numbers where an older release is concerned.)*

- **2.0** *(plugin builds 5.0.0 → 5.1.0)* — **Own outline colour channels (the headline feature; the entry right below describes it in detail), plus four fixes, all from player reports.** **(1) "Ores and plants are green before *and* after scanning."** The mod remembers a confirmed "surveyed" verdict so a target cannot fall back to cyan (added in 1.7.9 / 1.8.1), but that memory was keyed by **species** only and was kept across save files — so a plant surveyed once in one save turned that whole species green in every other save as well, before you had surveyed anything there, which is exactly what "green before I scan it" looks like. (Inside one save, spreading a confirmed verdict to the whole species is *correct* — the game itself tracks species, not single plants; see fix (4) below.) The mod now records the **individual reference** you actually confirmed, and the species-wide shortcut is rebuilt **per session from what this save confirmed** — it never leaks into another save. `FloraLearnPersist` still writes the per-object records to `SAS_AlwaysScan.flora-learn.txt` (one `reference + species` pair per line); old files are read but their species-only lines are ignored (the log reports how many) — delete the file for a clean start. **(2) "Frame rate keeps dropping the longer I play (1.8.1)."** The "read the state the game itself painted" step added in 1.8.1 called the game's own lookup-or-**add** function. When the object is not in the game's table yet, that function **inserts** an entry and takes a reference count on the object; the step runs for every plant / deposit around you several times a minute, so the game's table — and the memory pinned by those references — grew for the whole session. That matches the report exactly (and 1.7.3, which predates that step, was fine). The mod now walks that table **read-only** (same data, zero side effects), and the periodic log line gained `引擎状态表: 条目=N` so the table's entry count can be watched directly — it should stay roughly flat however long you play. **(3) "A few targets stay cyan after you scan them."** (reported 2026-09-27; low-frequency, a small part of everything you scan) Reading the state the game itself painted is done by walking the game's "reference → state" table **read-only** — but the address constant had been computed one digit short (a 4-byte rip-relative displacement read as 5 digits), so that walk always came up empty: the periodic log line showed a nonsense entry count (`引擎状态表: 条目=18446744073709551615`, `读=0`) and the strongest kind of evidence — *the game literally painted this one green* — was never actually used. What was left was the game's own "has this reference been surveyed?" query, and for about one object in five that query answers "I don't know" (neither surveyed nor not), so those stayed cyan however often they were scanned. On top of that, a "not surveyed yet" note learned for an object used to end the check right there, so nothing could ever upgrade it later. Both are fixed: the address is corrected (0x5F39CE0 — it also cross-checks with the manager array that sits exactly 0x10 past it, and the tree header's entry count is now a real number), and a "not surveyed" note is a hint only — the game's own query and the survey-data check can still turn the object green, while a confirmed "surveyed" verdict stays one-way. Nothing to configure. **(4) "Some plants stay cyan after I scan them — opening the scanner and closing it again turns them green."** (reported 2026-09-27) "Surveyed" is a **species-wide** fact in the game: survey one plant and the game paints every plant of that species green. The mod's memory, however, was keyed by the individual object, so plants the game never painted green during your scanner sweep — out of its evaluation range, behind you, or past its per-frame limit — were never learned and stayed cyan until a later sweep happened to paint them, which is why raising and lowering the scanner "fixed" them. A confirmed verdict is now spread to the whole species / resource, exactly like the game does it: surveying any one instance turns every instance of it green. The species table lives in **memory only** and is never written to disk, so it cannot leak into another save; after a game restart it is rebuilt from the per-object records as you walk past them. Nothing to configure; the stats line gained `按物种扩散(★v5.1.2)` counters. **(5) "The frame rate still drops."** (reported 2026-09-27, after fix (2)) Fix (2) stopped the mod from growing the game's own table; what remained was the cost of the scan itself. Five times a second the mod walks the object list of the cell you are in *and* of the recently visited cells around it (that is what keeps objects just across a border outlined). Together those lists may hold up to 60,000 references, and walking that much took about 55 ms per pass in a busy scene — a fifth of a second's worth of main-thread work, five times a second, which shows up as a steady frame-rate drop outdoors and in dense areas (visible in the log: `timing2: ... loop avg=55ms`). Each surrounding cell's list is now walked **in slices** — one slice per pass, rotating — so a pass touches about a fifth of it. The cell you stand in is always walked in full, so targets in view react immediately, and small lists (2000 references or fewer) are never sliced. The number of slices is capped automatically so that one full rotation finishes well inside the existing grace period (5 slices x 200 ms = 1 s against a 1500 ms grace), so slicing can never make a highlight flicker. Nothing to configure; `RingSliceMaxRounds` (default 5, 0 disables the slicing) tunes it, and the periodic log line now breaks the scan time down (`refs/scan: cur=… ring=…`, `walk/loot/flora`, `3D`). Gameplay, colours and every other INI option are unchanged from the first 2.0 build.
- **2.0 — the colour channels in detail** *(plugin build 5.0.0)* — **The mod now brings its own outline colour channels instead of borrowing the game's.** Background: the game has eleven outline "states" (0..10), and six of them are painted by the game itself while the scanner is up (ordinary references such as people; planet targets before / after surveying). Previous releases had to borrow the remaining five — that is why colours had to be grouped, and why recolouring one also recoloured the same state inside the vanilla scanner. Underneath, however, the renderer keeps two per-id tables that grow on demand (per-manager colour parameters, and a "which manager paints this reference" mapping); the eleven states are only the game's own book-keeping on top of that. 2.0 creates its own colour channels — **thirteen** of them, one per category plus one for surveyed plants — and never touches the game's state table or its colour blocks. What you get: **(a) vanilla colours are never modified** — people, planet targets and the scanner itself look exactly stock; **(b) every category is independent** — weapons / apparel / notes / resources keep the grouped colours (red / red / purple / purple) only because that is the requested layout; give them separate `ColorXxx` values and they will differ; **(c) the game can no longer wipe the mod's highlights** when it tears down its own scanner book-keeping (the mod's channels are not part of it). New INI options: `ChannelMode` (default 1 = own channels; set 0 for the 1.8.1 behaviour of writing into the game's own states) and `ColorFloraScanned` (the colour of an already-surveyed plant / deposit; default the game's green `#27C684`). If the engine's function signatures ever change, the mod **falls back to the 1.8.1 path automatically** and logs a warning line starting with `channel:` — no manual edit needed.
- **1.8.1** *(plugin build 4.33.0)* — **"Surveyed plants sometimes drop back to cyan" — second pass, so that it actually stays fixed.** 1.7.9 added a one-way "already surveyed" memory, but that memory had three holes that only showed up in a long test session: (1) it was wiped on every load / cell change (the game's "loading screen closed" event), (2) it lived in memory only, so every game session started from zero and had to re-learn by raising the scanner once, and (3) the "keep the previous verdict when a re-check cannot answer" rule had a condition that could never be true for plants, so it never actually protected them — which is why the report could still reproduce after 1.7.9: after a reload the whole area went cyan until the scanner was raised once. All three are fixed: the memory is **no longer cleared** on load / cell change (`FloraLearnClearOnLoad=0`, new default — "surveyed" is a one-way fact per species), it is **written to a file next to the esm** (`SAS_AlwaysScan.flora-learn.txt`) and read back on startup (`FloraLearnPersist=1`, new default — a new session starts already knowing), and the keep-rule now covers plants too. Two more improvements: a "not scanned" verdict is re-checked every **5 s** instead of 30 s (`FloraUnscannedTtlMs`), and the mod now also reads the state the game itself painted (green = surveyed) whenever its own checks cannot answer — so the game's own verdict is picked up even with the scanner put away. Note: the file is keyed by species, not by save file — if you play several saves, a species surveyed in one shows green in another (a colour-only effect; `FloraLearnPersist=0` restricts it to the session, and deleting the file resets it). Two new log counters: `学习表: 命中=… 沿用=… 捡漏=… 落盘=… 写入=…`.
- **1.8.0** *(plugin build 4.32.0)* — **Colour classification, final grouping — one red for the whole equipment set, and notes join resources on purple.** All colours and the number of outline states the mod writes are unchanged from 1.7.9; what changed is which categories share a colour: **red** = weapons / throwables AND spacesuits / helmets / packs / clothing (the equipment set is one colour again — the suits were the game's own light cyan-blue in 1.7.9), **orange** = containers / corpses, **purple** = notes AND resources (notes were the game's own cyan in 1.7.9), **green** = ammo & aid (the game's own green), **white** = doors, **blue** = misc items (the game's own blue, unchanged). Still exactly five outline states are written (2 / 3 / 6 / 9 / 10), and states 0 / 1 (people), 4 / 5 and 7 / 8 (planet targets) remain untouched — NPCs and planet surveying are unchanged. This grouping is a pure re-assignment: no new state, no new colour, and (unlike 1.7.9) nothing had to step aside. Every value is one INI line, as always.
- **1.7.9** *(plugin build 4.31.0)* — **Containers / corpses are orange again and doors are white again; and an already-surveyed plant can no longer fall back to cyan.** (1) 1.7.8 had returned the two "people" states to the game, leaving exactly five borrowable states — one short of the full colour set — so this release puts containers / corpses back on **orange** (state 9) and doors back on **white** (state 6, the only state the game itself never writes, so recolouring it cannot affect anything vanilla), while apparel and notes step aside to the game's own colours (light cyan-blue / cyan). Each swap is a single INI edit; the INI spells both out. (2) The mod re-checks every plant against the game every 30 s; occasionally that fresh check answers "no answer / not scanned" (a game component gets rebuilt, or timing), and the previous "surveyed" verdict was discarded along with it — which is exactly the "surveyed plants sometimes drop back to the unscanned colour, until you raise and lower the scanner" report. Survey data never goes backwards, so a confirmed verdict is now remembered per species for the rest of the session ("one-way memory"), and a re-check that cannot answer authoritatively keeps the previous verdict instead of erasing it. Two new counters (`学习表: 命中=… 沿用=…`) appear in the periodic log line.
- **1.7.8** *(plugin build 4.30.0)* — **Fix: while the scanner was up, every NPC showed the red / magenta the game reserves for its "bounty" markers.** The game itself outlines plain references — pedestrians included — with two outline states whose native colours are cyan (far) / light cyan (near); the mod had been overwriting exactly those two states with the "weapons red" / "apparel magenta" colours, so every pedestrian in sight took the "weapons" / "apparel" colours while you held the scanner up (reported with a screenshot: three pedestrians, one red, two magenta). Both states are now left completely alone — not a single byte written — so NPCs are back to the vanilla cyan. Weapons and apparel (still red / magenta) moved to two low-traffic states (9 / 10), and containers / corpses / doors — which used to occupy those two — now use the game's own cyan as well (they are no longer orange / white). Net effect: the mod now recolours **five of the eleven** outline states in total (`StateWeapon=9` / `StateApparel=10` / `StateContainer=1` / `StateDoor=0`; every value is documented in the INI and can be changed back key by key, no DLL swap). New startup log line: `config: state0_1 归还引擎 -> …`.
- **1.7.7** *(plugin builds 4.26.0 → 4.27.0 → 4.28.0 → 4.29.0)* — **Fix: with the scanner put away, already-surveyed planet targets (ores / gas vents / liquid pools / plants) still showed the "not scanned" cyan.** 1.7.6 fixed the half where the vanilla scanner is up; this release fixes the half where it is not. Once you lower the scanner the mod re-hangs the highlight itself, and until now it only ever used the vanilla "not scanned" state — so a surveyed deposit went back to cyan and "already done" was indistinguishable from "still to do". The mod now asks the game itself, from **two independent sources** (either one is enough): **(a)** while you hold the scanner up it reads which outline state the game paints each target with — the game's own green means "already surveyed", and that knowledge is remembered for the session; **(b)** with the scanner away it calls the very function the vanilla scanner uses (the one that decides whether a resource has entered your survey data) and follows the game's own record chain (flora record → the item it produces → that item's resource), **auto-detecting the record layout** if the game ever moves it (the first attempt at this only knew one layout and silently found nothing — the log now names the offsets it adopted, and dumps the raw pointer window if it cannot resolve the chain at all). Surveyed targets come back **green** — the game's own "scanned planet target" state and colour, the very same slot the vanilla scanner uses — while unscanned ones keep the cyan pulse; scan something, lower the scanner, and it turns green within about 0.2 s. New INI options: `FloraScannedByResource` (default 1; 0 = old all-cyan behaviour), `StateFloraScanned` (default 5 = the game's green; set 7 for "no difference") and `FloraScanProbeMax` (diagnostic, default 8). Two supporting fixes that this needed: a state change on an already-hung outline now takes effect immediately (the old code only re-checked on a re-assert timer, which is disabled by default — so "cyan → green" would never have been applied), and the "step aside while the scanner is up" protection is now tied to the category instead of one state number (otherwise the scanned-green half of that category would have been un-protected and could have removed the game's own highlight). Diagnostics: startup line `flora scanned: … ready`, stats line `planet targets (窗口内): 未扫描=… 已扫描=… | 判据: 查询=… 命中=…`, and per-target `flora scan:` lines. **This release was rebuilt with plugin build 4.27.0**: a log-based check of a live session showed that source **(b)** above had a silent bug — the mod's own sanity check rejected the record type it had to recognise, so every single chain query failed and only source **(a)** was ever doing the work (you would not notice while surveying a local deposit, but a target surveyed earlier / further away stayed cyan). The check is fixed, both sources are active, and the log names the record offsets adopted. No other behaviour changes. **Rebuilt again with plugin build 4.28.0**, after the report that **plants** showed the "already surveyed" green *before* being scanned (ores, gas and liquids were fine): the record chain of source **(b)** was being applied to plants as well, but a plant's produced item is a plain item instead of a level list — and the game itself only walks that chain when the produced item **is** a level list. So "the resource this plant yields is in your survey data" had been read as "this plant has been surveyed", turning whole plant species green on sight. Build 4.28.0 promotes the game's own per-reference query — the native function the game exposes as `IsScanned`, which answers 1 = not surveyed / 2 = surveyed — to the primary check (new INI option `FloraScannedByEngineState`, default 1) and restricts the record chain to level-list producers (the deposits). Plants are cyan until you actually scan them, then turn the vanilla green like everything else. Diagnostics gained the raw answer and which check decided: `flora scan: … | 引擎状态=N | …` and the stats segment `| 引擎状态: 问=… 已扫描=… 未扫描=…`. **Rebuilt once more with plugin build 4.29.0**: lowering the scanner now restores the highlight much faster in busy scenes. Every time you lower the scanner the game tears its highlight managers down, so the mod has to re-hang every target it keeps lit; that pass ran at 64 items per frame regardless of scene size, so with 268 lit targets (the test session) plants and ores visibly lagged about a second behind everything else — including the "cyan → green" repaint of something you had just scanned. The first 2.5 s after lowering the scanner now run at a larger per-frame budget (192), and targets whose colour has to change are repainted before the rest of the queue. Two new INI options to tune or disable it: `ResyncBoostMs` (default 2500) and `ResyncBoostBudget` (default 192); setting either to 0 restores the old behaviour without swapping the DLL.
- **1.7.6** *(plugin build 4.24.0)* — **Fix: scanned planet targets did not turn green.** The game paints an already-scanned planet target (ore / gas / liquid deposit or plant) with two outline states whose native colour is **green**; the mod had been colouring those exact two states as "devices cyan" and "ammo & aid bright green", so with the scanner up a scanned deposit showed up cyan (when far) or bright green (when near) and never turned the vanilla green. Both states are now left alone — not a single byte written — so the game's own "scanned" green is back, near and far, and the unscanned cyan pulse is unchanged. Practical consequences: ammo & aid and interactive devices still use those two states and therefore now show the game's own green (write `ColorAmmoAid=00FF66` / `ColorDevice=00E5FF` to bring the old colours back — that hides the vanilla green again). Also new: `ManagerOccupancyProbe=1` (default on) logs the entry count of all 11 outline states 1.5 s after you raise the scanner — useful evidence for any colour report. Also corrected in the docs: the earlier claim that states 4 / 5 / 6 / 10 are "mod only" was wrong; the engine can write every state except 6.
- **1.7.5** *(plugin build 4.23.0)* — **Planet targets light up again, and the real reason "resources" looked like "misc".** (1) With 1.7.4, ores / gas vents / liquid pools / plants had **no colour at all** on planets: 1.7.4 had switched that category off to hand it back to the vanilla scanner, but the vanilla scanner only colors those targets while you are holding it up — and this mod exists exactly for the times when you are not. The category is on again, painted with the game's own "scannable target" colour (a cyan pulse, its palette entry is never overwritten), and a new option `YieldTargetsWhileScanning=1` (default on) makes the mod **step aside for this category only** while you hold the scanner up, so the vanilla "scanned / not scanned / being scanned" colours stay exactly the game's; every other category is still refreshed, so highlighting beyond the scanner's centre circle keeps working during a survey. (2) "Resources" and "misc" had the same colour because of a **read bug**: the mod read the item keyword array with the wrong byte order (`data` first instead of `size` / `capacity` first), so no item ever matched and every resource silently fell back to the misc colour. Fixed, plus a self-calibrating offset (the game data has been known to move) and two log lines for diagnosis: `resource keyword: 关键词数组标定 = base+0x…` and `misc kw probe: …`. Resources are purple again, misc items stay blue. No other behaviour changes.

- **1.7.4** *(plugin build 4.22.0)* — **Two colour fixes: planet surveying and resources.** (1) Ores, gas vents, liquid pools, plants and creatures now keep the game's own "scanned / not scanned" colors. The mod used to paint every FLOR record bright green — but in the game's data the mineral deposits (ores, gas, liquids) are FLOR records too, not just plants, so that always-on colour hid the vanilla distinction while you surveyed a planet. Flora is now off by default (`EnableFlora=1` brings the always-on green back) and the two outline states the vanilla scanner uses for those targets (states 7 and 8) are **no longer written at all**; the resource colour moved to another state. (2) "Resources" and "misc" items no longer look the same: the resource check is validated at startup against known items, and if that sample item never loads into memory the old code silently froze resources onto the misc colour — the check is now on by default (it keeps its own safety checks) and only a clearly failed test turns it off. New log line on success: `resource keyword: 首个资源命中 base=0x... -> ...`. No other behaviour changes.
- **1.7.3** *(plugin build 4.21.0)* — **Fix: the 1.7.2 opacity setting had no effect at all, and the outline is now a contour instead of a fill.** Pixel measurements on a screenshot showed the covered area was pixel-identical at 40% and at 100% — the engine ignores that alpha byte, so "semi-transparent" never happened on screen. The outline's base color is now written with alpha=0, the "no fill" state the engine itself uses for its scannable targets: you see the object's own material in full while the category color stays on the contour / pulse. New INI option `NoFill` (default 1; set `NoFill=0` to restore the 1.7.1 / 1.7.2 filled look). `AlphaXxx` now only affects the pulse (contour) transparency. No other behaviour changes.
- **1.7.2** *(plugin build 4.20.0)* — **The category colors are now semi-transparent where it matters.** With 1.7.1, a strongly colored outline (red weapons, magenta suits, white doors) could cover a nearby object so completely that you could no longer tell what the item itself looked like. Doors, weapons and apparel are now drawn at ~40% opacity, so the object's own material shows through while the color stays easy to read. New INI options `AlphaWeapon` / `AlphaApparel` / `AlphaDoor` (0-255 per category; 0 = the engine's own value = the 1.7.1 look; 77 is ~30%, 51 is ~20%) — every other category is unchanged from 1.7.1. The log now prints the opacity in use: `config: colorAlpha ...`, plus an `a=` column in `outline colors[...]` and `renderer params[...]` (that is the opacity the renderer actually draws with). No other behaviour changes.
- **1.7.1** *(plugin build 4.19.0)* — **The category colors are now actually visible, and picked to be easy to tell apart.** Two problems fixed at once: (1) the color values were being written into the wrong engine table (a float constant area), so nothing on screen ever changed — the correct per-state color block is used now, and both the pulse colors and the base outline color are set; (2) even the correct engine defaults were near-useless for this purpose, because the states used by pick-up items are all blues (cyan / pale blue / blue / blue). New default palette: weapons **red**, spacesuits & clothing **magenta**, ammo & aid **bright green**, notes **yellow**, resources **purple**, misc items **blue** (unchanged), containers & bodies **orange**, devices **cyan**, doors **white**, flora **bright green** — the six pick-up colors are at least ~44° apart in hue. Also new: a `RendererProbe` diagnostic that logs the colors the renderer actually receives, so any future color issue can be diagnosed from the log alone. No behaviour changes.
- **1.7** *(plugin build 4.18.0)* — **Fix: the 1.6 category colors could end up not showing at all.** If guns, spacesuits and data slates all looked the same colour, this was why: the custom colors were written into the engine's color tables only *after* the highlight managers had already been created, and a manager reads those tables only at creation time (it is never refreshed afterwards). The colors are now written **before any manager exists**, so the outlines really use them. No INI changes, no other behaviour changes.
- **1.6** *(plugin build 4.17.0)* — **Lootable items are now colour-coded by inventory category**: weapons & throwables (cyan), spacesuits / helmets / packs / clothing (pale blue), ammo & aid (green), notes (yellow) and resources (purple). **Misc items keep their old blue**, so nothing that was blue before changes. Resources are recognised from the item record's own `ResourceType` keywords, so mod-added resources are covered too. New INI options: `StateWeapon` / `StateApparel` / `StateAmmoAid` / `StateNote` / `StateResource`, `EnableWeapon` … `EnableResource`, `ColorNote` / `ColorResource`, `ResourceByKeyword`. **The configuration file now lives next to the mod's esm** (same folder as the log) — the old `SFSE\Plugins\` copy is still read as a fallback, and the log prints which file was used. Under the hood the mod also now patches the engine's per-state colour correctly (its unused states are made visible), which is what makes the two new colours possible at all.
- **1.5** *(plugin build 4.16.0)* — **The log file now lives next to the plugin**: with Mod Organizer 2 it is written inside the mod's own folder, beside `StarfieldAlwaysScan.esm` (manual install: `…\Starfield\Data\SAS_AlwaysScan.log`), instead of `Documents\My Games\…`. Uninstalling the mod now removes its log too — nothing is left behind. No gameplay or INI changes.
- **1.4** *(plugin build 4.15.0)* — **Crash fixes only, no gameplay changes.** Two crashes found in crash-dump analysis are gone: (1) the game could crash while *shutting down* — the plugin's own cleanup ran after the address-library mapping had already been torn down, so it ended up calling into the engine one last time; the plugin now keeps its state alive instead of destroying it on exit and touches nothing engine-side while the game is closing. (2) a rare crash during cell transitions / loading, where an object the plugin was still watching had already been deleted and its memory reused by unrelated data — reference objects are now validated (readable, valid vtable, valid form type) and every inventory read goes through exception-free memory reads, so a stale object can only ever be treated as "unknown" (kept lit) instead of crashing. No INI changes.
- **1.3** *(plugin build 4.14.0)* — Display cases and racks (weapon cases, weapon racks, outpost display cases) are now handled properly: they glow even while closed, and they reliably go dark once you have taken everything out of them — emptying is tracked item by item as you take things out, so "take it all, then close the case" is caught as well. Taking only part of the contents, or opening one without taking anything, keeps it lit (by design). This supersedes the interim attempts in builds 4.8–4.13.
- **4.7.0** — Highlights survive walking across cell borders outdoors: the area is no longer re-scanned from scratch (both sides of the border stay outlined), and the rare "it was glowing, then suddenly went dark" case is fixed by re-applying an outline when the engine rebuilds an object's 3D. Scanning is also no longer interrupted by small streaming changes while you walk.
- **4.6.0** — Looted bodies of enemies you kill now go dark reliably: entries the dead actor is still *wearing* (which cannot be taken) no longer count as loot. The log file is now capped at 1 MiB.
- **4.5.0** — "Looted means dark" no longer counts the invisible NPC-only gear (`_NOTPLAYABLE`) that every body carries — that was why a fully looted body used to keep glowing.
- **4.4.0** — Kills are detected through the engine's own life-state check, so enemies, creatures, robots and turrets light up the moment they die. Added diagnostic log lines (`actor probe (changed):`, `loot probe:`) that make bug reports verifiable from one log.
- **4.3.0** — "Looted means dark" is now much more reliable: the inventory-offset calibration retries until it succeeds instead of giving up on the first area (so an empty body no longer keeps glowing just because you started the game in space or in a small room), and a reference whose inventory was never created by the engine counts as empty. Added per-actor diagnostic log lines (`actor probe:`) that make "this body should / should not glow" bug reports verifiable in one log.
- **4.2.1** — Wrecked robots and turrets (the "unconscious" units you can loot) are highlighted too.
- **4.2.0** — Bodies are highlighted now (pre-placed corpses and anything you kill, people and creatures alike), and both bodies and containers stop being outlined once you have looted them empty.
- **4.1.0** — Movable statics (cardboard boxes, tables, bars) are no longer outlined by default; added per-category enable switches.
- **4.0.2** — Category colors retuned to the five visually distinct native colors (blue / orange / green / red / pale blue), doors switched to red.
- **4.0** — Category colors, HUD toggle notification, cleanup of the legacy shader path.
- **3.2** — Highlights automatically re-apply after using the vanilla handheld scanner.

### Credits

Built with **SFSE** and **CommonLibSF**. Huge thanks to their authors and to everyone who tested and reported issues.

---

## ③ 中文版（备用/中文社区）

**Always Scan —— 不举扫描仪，也能一直有扫描仪高亮**

原版游戏里，想看清地上哪些东西能捡、能搜、能用，必须切到手持扫描仪；而举着扫描仪时又没法用枪、用工具，而且只有屏幕中央那个小圆圈里的东西才会亮。

**本 MOD 同时解决这两件事**：高亮常驻（背不背扫描仪都行），范围改成玩家周围一个可配置的半径（默认 **50 米**），不再受中央圆圈限制。

**观感就是原版本身**：MOD 不自己画发光，而是直接驱动游戏引擎自带的高亮描边系统 —— 颜色、粗细、呼吸感与原版扫描仪完全一致。

**功能一览**

- **常驻高亮**：跑动、战斗、搜刮时一直有效，不需要掏出扫描仪；
- **全半径**：半径内四周所有方向都亮（5~500 米可配）；
- **分类分色**（1.6 起**按物品栏分类**给可拾取物品上色；1.8.0 起**分组定稿**，大部分可配）：
  **红 = 武器 / 投掷物 + 太空服 / 背包 / 头盔 / 服饰（整组一个红）**、
  **橙 = 容器 / 尸体**、**紫 = 笔记 + 资源**、
  **绿 = 弹药 / 救援（原版绿）**、**白 = 门**、
  **蓝 = 杂项（原版蓝 —— 与 1.5 一模一样，没有变化）**；
  **植物 / 矿脉 / 气泉 / 液池（原版青色脉冲 = 未扫描；扫描完成后 = 原版绿 ——
  这四个槽位的颜色 MOD 一个字节都不改；**背不背扫描仪都一样**，见下）**；
  **NPC / 普通目标（原版青 / 亮青 —— 一个字节都不写，1.7.8 起）**；
- **★★★ 2.0（引擎 5.0.0 → 5.1.0）：自建颜色通道 + 三个报告修复** ——
  ① **自建 13 条颜色通道**（每类一条 + 「植物已扫描」一条），不再借用引擎那 11 个槽位：
  **不写引擎状态表、也不覆盖引擎配色块** ⇒ 原版扫描仪 / NPC / 星球目标颜色 100% 原版，
  引擎拆扫描仪 HUD 时也不会再把 MOD 的高亮一起清掉（`ChannelMode=0` 回退旧路径）；
  ② 修「矿石 / 植物扫描前后都是绿色」—— 记忆粒度 base → **引用**，（同物种扩散只在
  本存档内动态重建）**不跨存档外溢**；
  ③ 修「少部分扫描后不变色」—— 只读走树的状态表探针地址订正（0x5F39CE0）、
  「青」记忆不再短路判据；仅按引用记下的绿仍是单向的；
  ④ 修「帧数仍有下降」—— **环内 cell 分片遍历**（每轮只扫一片、玩家所在 cell 永远全扫、
  小 cell 不分片，`RingSliceMaxRounds=5`）：单轮环内遍历量降到约 1/5，
  分片周期被自动钳在「离开宽限期」之内 ⇒ 永不闪烁；
  ⑤ 修「**静态场景帧数正常、动态场景（战斗 / 走动时扫到新的高亮物品）卡顿**」——
  ④ 的假设（耗时 ∝ 遍历量）**被日志证伪**：同样 ~1540 个引用，站着不动 10ms、
  走动 63ms，而同窗口的引擎调用耗时两边都是 7ms。真正变贵的是**单次内存操作**：
  MOD 读游戏内存 / 判断可读性用的 `ReadProcessMemory` 与 `VirtualQuery` 都要拿
  **进程地址空间锁**，而游戏流式加载（走动 / 战斗）时那把锁被抢 ⇒ 单次从 ~10µs
  涨到几百 µs，一轮几百次就是几十毫秒。两条路现在都**不进内核**：直接读 + 硬件异常
  兜底（`FastReadMem=1`，启动自检会验证并打印两种读法的耗时对照，失败自动回退）；
  另外「容器 / 尸体是否搜空」的库存检查**按对象缓存**（`LootCacheTtlMs=1500`），
  失效靠**事件**（拿 / 放物品、搜刮界面开闭）⇒ 「拿空即灭」的延迟与旧版完全一致。
  统计行新增 `timing3:`（µs 级细分 + 调用次数 + **卡顿检测** + 最慢一轮的墙钟 / CPU），
  下次若还卡，一行日志就能判定「是我们的指令慢，还是线程被抢 / 等内存」；
- **★★★ 1.8.1：修「已扫描植物低概率变回青色」的根** —— 1.7.9 已经加了「单向记忆」，
  但实测日志发现它有三个洞：① 每次**载入 / 换场景**都被清空（游戏「载入画面关闭」事件）；
  ② 只活在内存里 ⇒ **每次重开游戏都要再开一遍扫描仪重新学**；③「重问失败时沿用旧结论」
  的条件对植物**永远不成立**（植物的记录结构恰好让它看起来「已有结论」）⇒ 从来没保护过
  植物。三条一起修：记忆**不再随载入清空**（`FloraLearnClearOnLoad=0` 新默认）、
  **落盘到 esm 旁边**（`SAS_AlwaysScan.flora-learn.txt`，`FloraLearnPersist=1` 新默认，
  重开游戏直接读回）、沿用规则覆盖植物。另外：判成「未扫描」的重问间隔从 30 秒降到
  **5 秒**（`FloraUnscannedTtlMs`），且判据答不上来时**顺手读一眼引擎亲手画的状态表**
  （画过绿 = 已扫描）—— 放下扫描仪后引擎留过的绿也不会白丢。
  ⚠️ 该文件按**物种**记录、不区分存档：多个存档混玩时，一个存档里扫过的物种在另一个
  存档也会显示绿色（只是颜色观感）；想严格按存档设 `FloraLearnPersist=0`（只在本会话
  有效），删掉那个文件即可重置。
- **★★ 1.8.0：配色分组定稿** —— 红 = 武器 / 投掷物 + 太空服 / 背包 / 头盔 / 服饰、
  橙 = 容器 / 尸体、紫 = 笔记 + 资源、绿 = 弹药 / 救援（原版绿）、白 = 门、
  蓝 = 杂项（原版蓝）；写出的槽位仍是 2/3/6/9/10 五个，0/1/4/5/7/8 一个字节都不写。
- **★★ 1.7.8：修「扫描中的 NPC 全部变成赏金色」** —— 举着扫描仪时，引擎自己会用
  两个槽位（原生色 = 青 / 亮青，远处 / 近处）给**普通人形目标（行人）**画轮廓；
  而 MOD 之前把这两个槽位覆盖成了「武器红 / 服饰品红」⇒ 所有行人变成红 / 品红，
  看起来像游戏里「带赏金的目标」的标记色（用户报的就是这个：截图里三个人形，
  一个红两个品红）。现在**这两个槽位一个字节都不写**（与 1.7.5 归还 7/8、
  1.7.6 归还 4/5 同一个道理）⇒ NPC 恢复原版青色轮廓。武器 / 服饰（仍是红 / 品红）
  搬到另外两个「引擎目标很少见」的槽位（9 / 10）；原本住在那里的容器 / 尸体 / 门
  改用原版青色（不再是橙 / 白）。⇒ MOD 现在总共只覆盖 **11 个槽位里的 5 个**；
  每个值都在 INI 里有说明，可以逐项改回去（不用换 DLL）。
- **★★ 1.7.7：修「收起扫描仪以后，扫过的星球目标还是扫描前的青色」** —— 1.7.6 修的是
  **举着扫描仪**那一半（引擎画绿色）；而**放下扫描仪**之后高亮是 MOD 自己重挂的，之前
  只用「未扫描」那一个状态 ⇒ 扫过的和没扫过的一模一样。现在 MOD **用两条互相独立的
  证据**（任一成立即可）：
  ① **引擎亲手画过的颜色**：举着扫描仪时（MOD 让位中）只读一眼引擎给这个目标写的
     outline 状态 —— 引擎画绿（state 4/5）= 「已扫描」，「已扫描」是单向的，
     所以学到之后本会话一直有效；
  ② **问引擎自己那个判据函数**（「这个资源已经进勘测数据了吗」）+ 走引擎自己的记录链
     （植物记录 → 它产出的物品 → 该物品的资源）；记录布局**自动探测**（引擎把它挪过
     位置也没关系），标定结果会写进日志，实在走不通时会打印原始指针窗口。
  ⇒ **扫过的 = 原版绿**（与举着扫描仪时引擎画的是同一个槽位、同一个颜色）、
  没扫过的 = 青色脉冲；**扫完放下扫描仪约 0.2 秒内由青转绿**。
  新增开关 `FloraScannedByResource`（默认 1；设 0 = 退回全青）、
  `StateFloraScanned`（默认 5 = 原版绿；设 7 = 不区分）、`FloraScanProbeMax`（诊断）；
- **★★ 1.7.6：修「扫描后没有变成原版扫描后的绿色」** —— 引擎把**已经扫描过**的
  星球目标写进 state 4 / 5（这两个槽位的原生色就是绿色 `#27C684`），而 MOD 之前
  把 4 当「设备青」、5 当「弹药救援亮绿」给覆盖了 ⇒ 扫过的目标显示成青（远）/
  亮绿（近），永远不是原版绿。现在 **4 / 5 的颜色一个字节都不写**（与 1.7.5 归还
  7/8 同一个道理）⇒ 扫描前青、扫描后绿，远处近处都对；「弹药 / 救援」「设备」
  继续用这两个槽位（颜色 = 原版绿）。另新增诊断 `ManagerOccupancyProbe=1`
  （举着扫描仪 1.5 秒后打 11 个槽位各自的元素数）；
- **★★ 1.7.5：星球目标重新点亮 + 「资源」判据真根因** —— ① 矿石 / 气体 / 液体 /
  植物在数据里同属 FLOR 记录：上一版把整类关掉「交还原版」，但**原版只在举着扫描仪
  时才给它们上色**，而本 MOD 的卖点正是「不举扫描仪也有高亮」⇒ 星球上等于什么都看不见。
  现在该类重新默认开（state 7 + **原版色**），并新增 `YieldTargetsWhileScanning=1`：
  **举着扫描仪时这一类让位原版**（不挂、不重申、也不摘）⇒ 原版的
  「扫描前 / 扫描后 / 正在扫描」颜色一个都不被盖；其余类别照常重申，
  所以**「屏幕中央圆圈之外也高亮」不受影响**。② 「资源和杂物同色」的真根因是
  **读物品关键词数组时字节序读反了**（`data` 在前 / `size`、`capacity` 在后写反），
  ⇒ 任何物品都匹配不上、资源**静默**归杂项；现已订正并加了偏移自适应与
  `关键词数组标定 = base+0x…` 一行日志。**资源 = 紫、杂物 = 蓝**；
- **尸体高亮**：预置的尸体和你打死的敌人都亮 —— **人类、怪物、机器人、炮塔一视同仁**（在游戏数据里它们同构），连地图上原本就躺着的机器报废体也会亮；
- **搜空即熄灭**：容器 / 尸体被拿空后约 1 秒熄灭 —— 只统计你真能拿走的东西（拿不走的隐形 NPC 装备、尸体身上还穿着的装备都不算），有描边就代表「里面还有东西」；
- **展示柜也管**：武器箱 / 武器架 / 头盔架 / 背包架 / 数据板架 / 前哨展示柜 —— **关着也亮**（游戏在关闭状态下不把内容放进容器库存），拿走最后一件东西后熄灭；
- **开关热键**：默认 **F8**，按一下开/关，HUD 弹一行提示（`Always Scan: ON / OFF`），可改键或关闭提示；
- **与原版扫描仪完全不冲突**：你自己举扫描仪扫完放下，MOD 的高亮会在 1 秒内自动重挂回来；
- **不影响任何交互**：对话、拾取、开门、用终端、工作台，全部与原版行为一致 —— MOD 只决定「哪些东西被描边」，从不碰交互逻辑；
- **活着的 NPC / 生物永不描边**（只亮尸体）；
- **走动不灭**：星球表面跨 cell 边界不再整片熄灭 —— 原本亮着的继续亮，**边界对面**的东西也一起亮；
  引擎流式重建模型（外景很常见）导致描边丢失时会自动重挂；
- **性能友好**：每 200ms 扫描一轮、变更分批应用、有离开宽限期，走动不卡顿；
  周围 cell（跨边界保持高亮的那几个）**分片轮扫** —— 单轮永远不会走完整个邻域；
  举着扫描仪时只读引擎自己的记录、从不写入。

**依赖**：Starfield 1.16.244.0、SFSE 0.2.21+、对应版本 Address Library。

**安装**：用 MO2 / Vortex 安装压缩包即可（包内就是 Data 目录结构），记得启用 `StarfieldAlwaysScan.esm`；手动安装则解压到 `...\Starfield\Data\` 合并文件夹。

**配置**：`SAS_AlwaysScan.ini`，**就在 `StarfieldAlwaysScan.esm` 旁边**（MO2 用户在 mod 自己的目录里；手动安装是 `...\Starfield\Data\`）—— 注释齐全，改完重进游戏生效：
半径、热键、各类颜色、每类是否高亮、目标上限、只亮正前方 等，全部可调。
其中 `EnableOther`（纸箱 / 桌椅这类搬得动但捡不起来的物件）**默认关闭**，与「原版扫描仪不亮它们」保持一致 —— 想复原改 1 即可。

> **4.3.0 起「搜空即熄灭」更可靠**：判定所需的内存标定会**一直重试直到成功**（采样附近的容器与角色），
> 所以就算你开局在太空 / 小房间里（附近没样本）也不会整局失效；另外「引擎从未建过库存」的引用直接算空
> （`TreatNullInvAsEmpty=1`，若遇到「没搜过的身体不亮」可设 0 退回）。
> 新增 `actor probe:` 诊断日志（每会话 ≤ `ActorProbeMax`=24 行），反馈问题时带上它即可精确定位。
>
> **4.4.0 ~ 4.6.0 又补齐两件事**：①「打死的敌人 / 生物 / 机器人 / 炮塔」在**死亡那一刻**就点亮
> （判死改用引擎自己的生命状态枚举）；②「面板拿空 = 熄灭」彻底可靠 —— 判空只算**你真能拿走**的东西，
> 拿不走的隐形 NPC 装备（`_NOTPLAYABLE`）与尸体身上**还穿着的**装备都被忽略
> （`SkipNonPlayableLoot=1` / `SkipEquippedLoot=1`）。装了 Simple Immersive Looting 时，
> 用它的「扒取装备」把身上装备卸下 ⇒ 该尸体重新亮起（装备变成可拿的了），拿空后再熄灭。
> 日志文件恒定 ≤ 1 MiB（超过即清空重写）。
>
> **4.7.0**：星球表面**跨 cell 边界走动不再整片熄灭** —— 账本保留、边界对面的 cell 一起扫
> （`ExteriorContinuous=1` / `SettleOnCellCrossMs=300`）；引擎**流式重建 3D**（LOD ↔ 真模型）导致
> 单条描边丢失时**自动重挂**（`Verify3DPerScan=32`，日志里 `3D复检: reassert=` 在涨）；
> 走动时引用数的**轻微变化不再打断扫描**（`StreamJumpTolerance=256`）。
>
> **1.7（引擎 4.18）**：**修「1.6 的分色没生效」** —— 如果你看到枪、太空服、数据板
> 描边都是一个色，就是这个原因：自定义颜色原本写在「高亮管理器已经建好之后」，
> 而管理器**只在创建那一刻**读配色表、之后永不刷新 ⇒ 写了个寂寞。现在把写表**提前到
> 任何管理器创建之前**，描边才真的用上新颜色（玩法 / INI 均无变化）。
>
> **1.6（引擎 4.17）**：**可拾取物品按「物品栏分类」分色** —— 武器 / 投掷物（青）、
> 太空服 / 头盔 / 背包 / 服饰（淡蓝白）、弹药 / 救援（绿）、笔记（黄）、资源（紫）；
> **杂项保持原来的蓝**（原先蓝的东西一个都没变）。「资源」判据取自物品记录自己的
> `ResourceType` 关键词 ⇒ 第三方 mod 新增的资源同样会被上色；判据不生效时自动退回
> 杂项色（日志里 `resource keyword:` 一行会说明）。新增 INI：`StateWeapon` …
> `StateResource`、`EnableWeapon` … `EnableResource`、`ColorNote` / `ColorResource`、
> `ResourceByKeyword`。**配置文件也搬到了「和 esm 同级」**（与日志同一个目录）——
> 老位置 `SFSE\Plugins\` 仍然兼容（新位置没有时才读它，日志 `config file:` 一行会说明）。
>
> **1.5（引擎 4.16）**：**日志文件改到「和 esm 同级」** —— MO2 用户在 mod 自己的目录里、
> 手动安装在 `Data\`（此前在 `文档\My Games\Starfield\SFSE\Logs\`）。删 mod 时日志一起删掉，
> 不再往文档目录留残留。玩法与 INI 均无变化。
>
> **1.4（引擎 4.15）**：**只修崩溃，不改玩法。** ① 退出游戏时不再崩（本 MOD 自己的清理
> 代码原本跑在「地址库映射已经拆掉」之后，等于退出时又调了一次引擎；现在状态改成"退出时
> 不销毁"、关闭过程中不碰引擎）；② 换场景 / 载入途中偶发的崩溃修掉（观察表里可能留着一个
> 已经被删除、内存被别的数据复用的引用；现在引用要过「可读 + vtable + formType」三关，
> 而且所有库存读取都不会再抛访问异常 —— 陈旧对象最多被判成「未知（保持亮）」，不会崩）。
> INI 无变化。
>
> **1.3（引擎 4.14）**：**展示柜（武器箱 / 武器架 / 头盔架 / 背包架 / 数据板架 / 前哨展示柜）**
> 现在「关着也亮」，并且**拿走最后一件东西后就熄灭** —— 拿空按「你真正拿走的物品」**逐件记账**，
> 所以「拿空后立刻关掉面板」也不会漏判；拿一部分 / 打开不拿就关掉都保持亮（设计如此）。

**日志**：`SAS_AlwaysScan.log`（**1.5 起写在和 esm 同级的地方** —— MO2 用户看 mod 自己的目录、手动安装看 `Data\`；1.4 及以前在 `文档\My Games\Starfield\SFSE\Logs\`），启动打配置、游戏中每 5 秒打一行统计，排查问题看它即可。
