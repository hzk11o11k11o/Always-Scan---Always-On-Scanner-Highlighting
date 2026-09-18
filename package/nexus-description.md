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
| **Version** | `4.3.0` |
| **Category** | `Gameplay`（Alternate suggestion: `Items and Objects - Gameplay`） |
| **Requirements（依赖）** | `Starfield Script Extender (SFSE) 0.2.21+`、`(1.16.244.0) SFSE Address Library`、游戏版本 `1.16.244.0` |
| **主文件（Main file）** | `StarfieldAlwaysScan-4.3.0.zip` |
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

- **Toggle hotkey** — press **F8** (any key can be configured) to switch everything on or off, with a short HUD confirmation (`Always Scan: ON / OFF`). You can also choose to start with the feature disabled.
- **Vanilla scanner friendly** — using the handheld scanner yourself is perfectly fine. When you put it down, the mod automatically re-applies its highlights within a second, so nothing "goes dark" after scanning.
- **Interaction safe** — this mod only decides **which objects the engine outlines**. Activation, pick-up prompts, dialogue, doors, computers and crafting all work exactly like vanilla.
- **Never highlights living beings** — living NPCs and creatures are excluded by design; only dead bodies are outlined (and `CorpseUnconscious=1` optionally includes knocked-out NPCs, off by default).
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
- `ActorProbeMax=24` — diagnostic only: how many per-actor lines (`actor probe:`) the log prints per session (0 = off). Leave it alone unless you are reporting a highlight bug.
- `MaxTargets=256` — how many objects can be outlined at once
- `OnlyInFront=0` — if you prefer "only what you actually face", set to 1 (original scanner-like behaviour)

### Compatibility

- Only decides outline states for references in the current cell/space — the same reach the vanilla scanner has — limited by the configured radius.
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
- "Empty" is decided by reading the reference's inventory (item stacks whose count is zero do not count). This needs a one-time memory-offset calibration, which **keeps retrying until it succeeds** (it samples nearby containers and actors) — so it also starts working if the area you started in had nothing to sample from. Until it succeeds the mod simply keeps outlining containers / bodies as before (the log says so) — it never crashes and never hides something that still has loot.
- A reference whose inventory list was **never created** by the engine counts as empty (`TreatNullInvAsEmpty=1`) — such a reference has nothing to loot by definition. If you ever meet a body / container that stays dark but should glow, set that option to 0.
- Quest objects are not marked specially.
- Outlines can only appear inside the current cell / space (engine limit) — bodies in another loading area are not highlighted.
- `CorpseUnconscious=1` (default) also lights up the rare knocked-out *living* character. They are lootable while they are down; if one ever stands up while staying outlined, set the option to 0 in the INI.
- `ColorXxx=RRGGBB` lets you set exact colors, but be aware this writes into the engine's global color table, so the vanilla scanner's color for the same state changes too. Leaving the `ColorXxx` lines empty (default) changes nothing.

### Version history

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
- **搜空即熄灭**：容器 / 尸体被拿空后约 1 秒熄灭 —— 有描边就代表「里面还有东西」；
- **开关热键**：默认 **F8**，按一下开/关，HUD 弹一行提示（`Always Scan: ON / OFF`），可改键或关闭提示；
- **与原版扫描仪完全不冲突**：你自己举扫描仪扫完放下，MOD 的高亮会在 1 秒内自动重挂回来；
- **不影响任何交互**：对话、拾取、开门、用终端、工作台，全部与原版行为一致 —— MOD 只决定「哪些东西被描边」，从不碰交互逻辑；
- **活着的 NPC / 生物永不描边**（只亮尸体）；
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

**日志**：`文档\My Games\Starfield\SFSE\Logs\SAS_AlwaysScan.log`，启动打配置、游戏中每 5 秒打一行统计，排查问题看它即可。
