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
| **Version** | `1.7.2`（引擎 build 4.20.0；Nexus 上的 1.0 / 1.1 / 1.2 / 1.3 / 1.4 / 1.5 / 1.6 / 1.7 / 1.7.1 之后的下一版） |
| **Category** | `Gameplay`（Alternate suggestion: `Items and Objects - Gameplay`） |
| **Requirements（依赖）** | `Starfield Script Extender (SFSE) 0.2.21+`、`(1.16.244.0) SFSE Address Library`、游戏版本 `1.16.244.0` |
| **主文件（Main file）** | `StarfieldAlwaysScan-1.7.2.zip` |
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
  | Ammo & aid | ammo, meds, food, drinks | **bright green** |
  | Notes | notes, data slates, magazines, books | **yellow** |
  | Resources | iron, aluminium, helium-3, organics, ... | **purple** |
  | Misc items | digipicks, credits, toys | **blue (unchanged from 1.5)** |
  | Containers | crates, safes, lockers | orange |
  | Bodies / corpses | dead people, creatures, and wrecked robots / turrets | orange |
  | Interactive devices | switches, terminals, workbenches | cyan |
  | Doors | — | white |
  | Flora | harvestable plants | bright green |

  The six pick-up categories sit at hues at least ~44° apart, so they are easy to tell apart at a glance (the engine's own palette was all blues in that range — that is what 1.7.1 fixed). Every color above is written explicitly by the mod and can be changed with the `ColorXxx` INI options.

  Since **1.7.2** the strongest colors are also drawn **semi-transparent**, so you can still see the object's own material and shape: doors, weapons and apparel use ~40% opacity by default. That is set per category with the `AlphaXxx` INI options (0 = keep the engine's own opacity, 255 = fully opaque; 77 is ~30%, 51 is ~20%).

  Resources are recognised from the item record itself (the game marks them with its own `ResourceType` keywords), so vanilla and mod-added resources both get the resource color. If that check cannot be applied the mod falls back to the misc color — the log tells you which one happened.

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
- `StateWeapon=0` / `StateApparel=1` / `StateAmmoAid=5` / `StateNote=6` / `StateResource=7` / `StateLoot=2` / `StateContainer=9` / `StateDevice=4` / `StateDoor=10` / `StateFlora=5` — outline color state per category (the INI documents all 11 available native colors).
- `ColorWeapon=FF2E2E` / `ColorApparel=FF3BD4` / `ColorAmmoAid=00FF66` / `ColorNote=FFD700` / `ColorResource=B36BFF` / `ColorContainer=FF9500` / `ColorDevice=00E5FF` / `ColorDoor=FFFFFF` / `ColorLoot=1F8EE2` / `ColorCorpse=FF9500` / `ColorFlora=00FF66` — exact RGB per category (all set by default since 1.7.1). They write the engine's global per-state color block, so the same state of the vanilla scanner changes as well (the INI explains it — change or clear them freely). Log line to watch: `outline colors: state=N 覆盖为 #RRGGBB <- <category>`.
- `AlphaWeapon=102` / `AlphaApparel=102` / `AlphaDoor=102` — outline opacity per category (0-255) since 1.7.2. 0 = keep the engine's own value (the 1.7.1 look), 255 = fully opaque. Doors / weapons / apparel default to 102 (~40%) so the object's own material shows through; every other category keeps the engine value. Log lines: `config: colorAlpha ...`, and the `a=` column in `outline colors[...]` / `renderer params[...]` (that `a=` is what the renderer actually uses).
- `ResourceByKeyword=1` — recognise resources from the item record's own `ResourceType` keywords (default on). Set to 0 and every MISC item counts as misc (the 1.5 behaviour).
- `EnableLoot=1` … `EnableOther=0` — per-category on/off switches (`EnableWeapon` / `EnableApparel` / `EnableAmmoAid` / `EnableNote` / `EnableResource` / `EnableLoot` = misc items / containers / devices / doors / flora / bodies). `EnableOther` covers movable statics (cardboard boxes, tables, crates). It is **off by default** because they cannot be picked up and the vanilla scanner does not outline them either.
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
- **The color covers the object so you cannot see its material** (worst on doors, weapons and suits at close range) — lower the opacity for that category: `AlphaXxx` in the INI, 0-255 (0 = the engine's own value = the 1.7.1 look; 77 is ~30%, 51 is ~20%). Doors / weapons / apparel already default to 102 (~40%) since 1.7.2.

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
- `AlphaXxx=0-255` sets the outline opacity per category (0 = keep the engine's own value, 255 = fully opaque). It is written into the same per-state color block as `ColorXxx`.

### Version history

*(This release is **1.7.2**. The list below uses the plugin's internal build numbers where an older release is concerned.)*

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
- **分类分色**（1.6 起**按物品栏分类**给可拾取物品上色，全部可配）：
  **武器 / 投掷物（青）、太空服 / 头盔 / 背包 / 服饰（淡蓝白）、弹药 / 救援（绿）、
  笔记（黄）、资源（紫）、杂项（蓝 —— 与 1.5 一模一样，没有变化）**；
  容器 / 尸体（橙）、可交互设备（绿）、门（红）、植物（绿）；
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
