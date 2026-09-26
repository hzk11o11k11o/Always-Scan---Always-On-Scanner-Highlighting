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
| **Version** | `1.7.7`（引擎 build 4.26.0；Nexus 上的 1.0 / 1.1 / 1.2 / 1.3 / 1.4 / 1.5 / 1.6 / 1.7 / 1.7.1 / 1.7.2 / 1.7.3 / 1.7.4 / 1.7.5 / 1.7.6 之后的下一版） |
| **Category** | `Gameplay`（Alternate suggestion: `Items and Objects - Gameplay`） |
| **Requirements（依赖）** | `Starfield Script Extender (SFSE) 0.2.21+`、`(1.16.244.0) SFSE Address Library`、游戏版本 `1.16.244.0` |
| **主文件（Main file）** | `StarfieldAlwaysScan-1.7.7.zip` |
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
- **Category colors** — each kind of object gets its own outline color (all configurable). Pick-up items follow the game's own inventory categories:

  | Category | Example | Default color |
  | --- | --- | --- |
  | Weapons & throwables | guns, melee, grenades, mines | **red** |
  | Spacesuits / helmets / packs / clothing | anything in the Apparel tab | **magenta** |
  | Ammo & aid | ammo, meds, food, drinks | **the game's own green** (states 4/5 are shared with the vanilla "scanned planet target" green since 1.7.6; `ColorAmmoAid=00FF66` brings bright green back) |
  | Notes | notes, data slates, magazines, books | **yellow** |
  | Resources | iron, aluminium, helium-3, organics, ... | **purple** |
  | Misc items | digipicks, credits, toys | **blue (unchanged from 1.5)** |
  | Containers | crates, safes, lockers | orange |
  | Bodies / corpses | dead people, creatures, and wrecked robots / turrets | orange |
  | Interactive devices | switches, terminals, workbenches | **the game's own green** (same note as above; `ColorDevice=00E5FF` brings cyan back) |
  | Doors | — | white |
  | Flora | plants — the game stores ores, gas vents and liquid pools in this same record type too (see below) | **the game's own colours**: cyan pulse while unscanned, **green** once scanned — with or without the scanner up (none of those states' colours is ever written; category on by default since 1.7.5, scanned green protected since 1.7.6, scanned targets stay green with the scanner away since 1.7.7) |

  The six pick-up categories sit at hues at least ~44° apart, so they are easy to tell apart at a glance (the engine's own palette was all blues in that range — that is what 1.7.1 fixed). Most colors above are written explicitly by the mod and can be changed with the `ColorXxx` INI options; the deliberate exceptions are the planet-target colors and the two categories that share the game's "scanned planet target" states (see the planet surveying note below).

  Since **1.7.3** the outline is drawn as a **contour instead of a fill**: the mod writes alpha=0 into the outline's base color — the same "no fill" state the engine itself uses for its scannable targets — so you see the item's own material and shape in full, with the category color sitting on the contour. Set `NoFill=0` in the INI to restore the old filled look (1.7.1 / 1.7.2). Note for upgraders from 1.7.2: that release's opacity options (`AlphaXxx`) turned out to have **no visible effect at all** — pixel measurements on a screenshot showed the covered area was pixel-identical at 40% and 100%, because the engine ignores that alpha byte for the fill. `AlphaXxx` now only changes the pulse (contour) transparency.

  **Planet surveying (1.7.5 / 1.7.6 / 1.7.7).** The game stores ores, gas vents, liquid pools **and** plants in the same record type (FLOR) and colors them by "scanned / not scanned" through outline states that the vanilla scanner owns: an unscanned target gets a **cyan pulse** (two states), a scanned one gets **green** (two other states). The mod paints this category with the game's own "scannable target" colour while the scanner is away, and it **never writes the colours of any of those four states** — 1.7.5 handed the first pair back, 1.7.6 the second, after a bug report that scanned deposits never turned green (they had been showing the "devices" / "ammo & aid" colours instead, because those two categories were sharing the very states the game uses for scanned targets). While you hold the scanner up the mod steps aside for this category only (`YieldTargetsWhileScanning=1`, on by default): it writes nothing, so the game's own "scanned / not scanned / being scanned" colours are exactly what you see — unscanned deposits cyan, scanned ones **green**, near or far. Every other category is still refreshed while the scanner is up, so highlighting beyond the scanner's centre circle keeps working during a survey. (1.7.4 switched this category off entirely to leave it to the vanilla scanner — which turned out to mean "no color at all on planets", because the vanilla scanner only paints those targets while you hold it up.)

  **1.7.7 finishes that story for the other half of the time.** Once you *put the scanner away* the mod re-hangs the highlight itself, and until 1.7.7 it only ever used the vanilla "not scanned" state — so a surveyed deposit went back to cyan and "already done" was indistinguishable from "still to do". 1.7.7 asks the game instead of guessing: it calls the game's own check function (the one the vanilla scanner uses to decide whether a resource has entered your survey data) and walks the game's own record chain (flora record → the item it produces → that item's resource). Ones you have already surveyed come back **green** (the same state and colour the game uses while the scanner is up), unscanned ones stay cyan — scan something, lower the scanner, and it turns green within about 0.2 s. `FloraScannedByResource=0` (or `StateFloraScanned=7`) restores the old "all cyan" behaviour without swapping the DLL. Startup log line to look for: `flora scanned: … ready`; the stats line `planet targets (窗口内): 未扫描=… 已扫描=… | 判据: 查询=… 命中=…` shows the split (and `flora scan:` lines list the individual checks).

  Resources are recognised from the item record itself (the game marks them with its own `ResourceType` keywords), so vanilla and mod-added resources both get the resource color. 1.7.4 and earlier read that keyword array in the wrong byte order, so **no item ever matched** and every resource silently fell back to the misc color; 1.7.5 reads it correctly (and re-calibrates the offset by itself if the game ever moves it — the log says which layout was adopted). If the check cannot be applied the mod falls back to the misc color — the log tells you which one happened.

- **Bodies are highlighted** — pre-placed corpses and everything you kill: humans, creatures, robots and turrets alike (including the wrecked machines already lying around). They behave like containers: empty them and the outline goes away.
- **Looted means dark** — a container or a body that has nothing left to take stops being outlined (about a second later), so an outline always means "there is still something in there".
- **Display cases included** — weapon cases, weapon racks, helmet / backpack / datapad racks and the outpost display cases glow even while they are closed, and they go dark once you have taken everything out of them. Taking only part of the contents, or opening one without taking anything, keeps it lit (by design).

- **Toggle hotkey** — press **F8** (any key can be configured) to switch everything on or off, with a short HUD confirmation (`Always Scan: ON / OFF`). You can also choose to start with the feature disabled.
- **Vanilla scanner friendly** — using the handheld scanner yourself is perfectly fine. When you put it down, the mod automatically re-applies its highlights within a second, so nothing "goes dark" after scanning.
- **Interaction safe** — this mod only decides **which objects the engine outlines**. Activation, pick-up prompts, dialogue, doors, computers and crafting all work exactly like vanilla.
- **Never highlights living beings** — living NPCs and creatures are excluded by design; only dead bodies are outlined (and `CorpseUnconscious=1` optionally includes knocked-out NPCs, off by default).
- **Stays on while you walk** — crossing the invisible border between two outdoor cells no longer drops the highlight: what was glowing keeps glowing, and the objects on the other side of the border are outlined too while you are near it.
- **Performance aware** — the world is scanned every 200 ms with per-pass budgets and a leave-grace period, so walking around does not cause stutter. The log can show timings if you ever need to verify.

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
- `StateWeapon=0` / `StateApparel=1` / `StateAmmoAid=5` / `StateNote=6` / `StateResource=3` / `StateLoot=2` / `StateContainer=9` / `StateDevice=4` / `StateDoor=10` / `StateFlora=7` — outline color state per category (the INI documents all 11 available native colors). States **7 / 8** are the vanilla scanner's "not-yet-scanned planet target" slots (never written since 1.7.4) and states **4 / 5** are its "already scanned" slots (native green, never written since 1.7.6). Flora (ores / gas / liquid deposits / plants) uses state 7 with the game's own colour, resources use state 3 (purple), and ammo & aid / devices share states 5 / 4 and therefore show the game's own green.
- `ColorWeapon=FF2E2E` / `ColorApparel=FF3BD4` / `ColorNote=FFD700` / `ColorResource=B36BFF` / `ColorContainer=FF9500` / `ColorDoor=FFFFFF` / `ColorLoot=1F8EE2` / `ColorCorpse=FF9500` — exact RGB per category (written by default). `ColorFlora` is **left unset on purpose** (the game's own cyan) and so are `ColorAmmoAid` / `ColorDevice` since 1.7.6 — writing the first paints over the vanilla scanner's colour for planet targets, writing the latter two hides the vanilla "scanned planet target" green (they now default to the game's own green). They write the engine's global per-state color block, so the same state of the vanilla scanner changes as well (the INI explains it — change or clear them freely). Log line to watch: `outline colors: state=N 覆盖为 #RRGGBB <- <category>`.
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

*(This release is **1.7.7**. The list below uses the plugin's internal build numbers where an older release is concerned.)*

- **1.7.7** *(plugin build 4.26.0)* — **Fix: with the scanner put away, already-surveyed planet targets (ores / gas vents / liquid pools / plants) still showed the "not scanned" cyan.** 1.7.6 fixed the half where the vanilla scanner is up; this release fixes the half where it is not. Once you lower the scanner the mod re-hangs the highlight itself, and until now it only ever used the vanilla "not scanned" state — so a surveyed deposit went back to cyan and "already done" was indistinguishable from "still to do". The mod now asks the game itself, from **two independent sources** (either one is enough): **(a)** while you hold the scanner up it reads which outline state the game paints each target with — the game's own green means "already surveyed", and that knowledge is remembered for the session; **(b)** with the scanner away it calls the very function the vanilla scanner uses (the one that decides whether a resource has entered your survey data) and follows the game's own record chain (flora record → the item it produces → that item's resource), **auto-detecting the record layout** if the game ever moves it (the first attempt at this only knew one layout and silently found nothing — the log now names the offsets it adopted, and dumps the raw pointer window if it cannot resolve the chain at all). Surveyed targets come back **green** — the game's own "scanned planet target" state and colour, the very same slot the vanilla scanner uses — while unscanned ones keep the cyan pulse; scan something, lower the scanner, and it turns green within about 0.2 s. New INI options: `FloraScannedByResource` (default 1; 0 = old all-cyan behaviour), `StateFloraScanned` (default 5 = the game's green; set 7 for "no difference") and `FloraScanProbeMax` (diagnostic, default 8). Two supporting fixes that this needed: a state change on an already-hung outline now takes effect immediately (the old code only re-checked on a re-assert timer, which is disabled by default — so "cyan → green" would never have been applied), and the "step aside while the scanner is up" protection is now tied to the category instead of one state number (otherwise the scanned-green half of that category would have been un-protected and could have removed the game's own highlight). Diagnostics: startup line `flora scanned: … ready`, stats line `planet targets (窗口内): 未扫描=… 已扫描=… | 判据: 查询=… 命中=…`, and per-target `flora scan:` lines. No other behaviour changes.
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
- **分类分色**（1.6 起**按物品栏分类**给可拾取物品上色，大部分可配）：
  **武器 / 投掷物（红）、太空服 / 头盔 / 背包 / 服饰（品红）、
  笔记（黄）、资源（紫）、杂项（蓝 —— 与 1.5 一模一样，没有变化）**；
  **弹药 / 救援（原版绿 —— 1.7.6 起与「已扫描的星球目标」共用原版绿，写
  `ColorAmmoAid=00FF66` 可换回亮绿）**；容器 / 尸体（橙）、
  **可交互设备（原版绿 —— 同上，写 `ColorDevice=00E5FF` 可换回青）**、门（白）、
  **植物 / 矿脉 / 气泉 / 液池（原版青色脉冲 = 未扫描；扫描完成后 = 原版绿 ——
  这四个槽位的颜色 MOD 一个字节都不改；**背不背扫描仪都一样**，见下）**；
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
- **性能友好**：每 200ms 扫描一轮、变更分批应用、有离开宽限期，走动不卡顿。

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
