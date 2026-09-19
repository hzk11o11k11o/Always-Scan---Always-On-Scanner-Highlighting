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
| **Version** | `1.3`（引擎 build 4.14.0；Nexus 上的 1.0 / 1.1 / 1.2 之后的下一版） |
| **Category** | `Gameplay`（Alternate suggestion: `Items and Objects - Gameplay`） |
| **Requirements（依赖）** | `Starfield Script Extender (SFSE) 0.2.21+`、`(1.16.244.0) SFSE Address Library`、游戏版本 `1.16.244.0` |
| **主文件（Main file）** | `StarfieldAlwaysScan-1.3.zip` |
| **Permissions** | 允许转载需注明出处？建议：**不得重新上传（No re-uploading）**；允许个人修改（源码已含 Papyrus 部分） |

---

## ② 英文正文（Description）

**Always Scan — always-on scanner highlighting**

In the vanilla game you have to equip the handheld scanner to see which objects can be picked up, searched or used — and while the scanner is up, your weapon and other tools are unavailable. Even then, only what sits inside the small circle in the middle of the screen gets highlighted.

**Always Scan removes both limitations.** The highlight stays on whenever you want it, and everything around you inside a configurable radius (default **50 m**) is outlined — no circle, no need to raise the scanner.

**Visually it is the vanilla feature.** The mod does not draw its own glow: it drives the game's own outline system, so colors, thickness and pulse are identical to what the handheld scanner draws.

### Features

- **Always on** — highlighting stays visible while you run, fight, loot or fly. The scanner stays in your pocket (or on your back).
- **Full radius** — objects inside the configured radius are highlighted in every direction, not just the center circle. Radius is configurable from 5 to 500 meters.
- **Category colors** — each kind of object gets its own outline color (all configurable):

  | Category | Example | Default color |
  | --- | --- | --- |
  | Lootable items | medkits, books, armor, weapons, ammo | blue |
  | Containers | crates, safes, lockers | orange |
  | Bodies / corpses | dead people, creatures, and wrecked robots / turrets | orange |
  | Interactive devices | switches, terminals, workbenches | green |
  | Doors | — | red |
  | Flora | harvestable plants | green |

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
Scripts\SAS_Bridge.pex               -> Data\Scripts\
Scripts\Source\SAS\SAS_Bridge.psc    -> Data\Scripts\Source\SAS\  (source, optional)
SFSE\Plugins\SAS_AlwaysScan.dll      -> Data\SFSE\Plugins\
SFSE\Plugins\SAS_AlwaysScan.ini      -> Data\SFSE\Plugins\
```

Enable `StarfieldAlwaysScan.esm` in your load order and play.

**Manual installation:** extract the archive into `...\Starfield\Data\` and merge folders.

### Configuration

Everything lives in `Data\SFSE\Plugins\SAS_AlwaysScan.ini` — heavily commented, read once at game start.

Most used options:

- `HotkeyVK=119` — toggle key (119 = F8; the INI has a table of common key codes; set to 0 to disable the hotkey)
- `RadiusMeters=50` — highlight radius around the player
- `StartEnabled=1` — on by default; set to 0 to start disabled
- `StateLoot=2` / `StateContainer=9` / `StateDevice=4` / `StateDoor=10` / `StateFlora=5` — outline color states per category (the INI documents all 11 available native colors)
- `EnableLoot=1` … `EnableOther=0` — per-category on/off switches. `EnableOther` covers movable statics (cardboard boxes, tables, crates). It is **off by default** because they cannot be picked up and the vanilla scanner does not outline them either.
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

Log: `Documents\My Games\Starfield\SFSE\Logs\SAS_AlwaysScan.log`

At startup the log prints the active config and whether the native outline functions were found (`native outline ready`). During play it prints a stats line every 5 seconds: object counts, categories, timings. If something looks wrong, that log is enough to diagnose it.

- **Nothing is highlighted** — check `StarfieldAlwaysScan.esm` is enabled, SFSE is installed, and the address library matches your game version.
- **Something is highlighted that you don't want** — set its category to 0 in the INI, or send me the log's `candTypes` line (it shows the form type of everything considered).
- **Colors too similar** — see `stateByCategory` in the log, then change the `StateXxx` values.

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

### Version history

*(This release is **1.3**. The list below uses the plugin's internal build numbers where an older release is concerned.)*

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
- **分类分色**：可拾取物品（蓝）、容器（橙）、**尸体（橙，含怪物尸体）**、可交互设备（绿）、门（红）、植物（绿），全部可配；
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

**配置**：`Data\SFSE\Plugins\SAS_AlwaysScan.ini`（注释齐全，改完重进游戏生效）：
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
> **1.3（引擎 4.14）**：**展示柜（武器箱 / 武器架 / 头盔架 / 背包架 / 数据板架 / 前哨展示柜）**
> 现在「关着也亮」，并且**拿走最后一件东西后就熄灭** —— 拿空按「你真正拿走的物品」**逐件记账**，
> 所以「拿空后立刻关掉面板」也不会漏判；拿一部分 / 打开不拿就关掉都保持亮（设计如此）。

**日志**：`文档\My Games\Starfield\SFSE\Logs\SAS_AlwaysScan.log`，启动打配置、游戏中每 5 秒打一行统计，排查问题看它即可。
