# Always Scan — Always-On Scanner Highlighting (SFSE)

**Nexus Mods:** <https://www.nexusmods.com/starfield/mods/18268> · **Release:** 2.0.1 (plugin build 5.1.0)

Keep the vanilla scanner highlight on at all times — no need to hold the handheld scanner up. Every object inside a configurable radius (default **50 m**, not just the centre circle) is outlined using the game's own outline system, colour-coded by category.

An SFSE plugin for **Starfield 1.16.244.0**.

## Features

- **Always on** — highlighting stays visible while you run, fight, loot or fly; the scanner stays in your pocket. Toggle with **F8** (rebindable), with a short HUD confirmation (`Always Scan: ON / OFF`).
- **Full radius** — objects in every direction inside the configured radius (5–500 m), not just the scanner's centre circle.
- **Vanilla visuals** — the mod drives the game's own outline system (contour, not fill, since 1.7.3), so shape / thickness / pulse behave exactly like the handheld scanner. Since **2.0** it brings its own colour channels (**14** since 2.0.1 — one per category plus surveyed plants) and never writes the game's own outline states or colour blocks — NPCs, planet targets and the vanilla scanner keep their stock colours.
- **Category colours** (all configurable in the INI):

  | Category | Default colour |
  | --- | --- |
  | Weapons / throwables + spacesuits / helmets / packs / clothing | **red** |
  | Notes + resources | **purple** |
  | Ammo & aid | **green** (the game's own) |
  | Digipicks + credits | **yellow** (since 2.0.1; they used to be misc blue) |
  | Misc items | **blue** (the game's own) |
  | Containers + bodies / corpses | **orange** |
  | Doors + computers / terminals / buttons | **magenta** (one colour since 2.0.1; magenta since the 2026-09-28 colour revision) |
  | Flora / ores / gas vents / liquid pools | **vanilla cyan** unscanned → **vanilla green** surveyed (read from the game's own survey progress) |

- **Loot-aware** — containers and bodies stop being outlined once emptied; display cases / racks glow even while closed and go dark once you have taken everything out of them.
- **Interaction safe** — the mod only decides which objects the engine outlines; activation, pick-up prompts, dialogue, doors, computers and crafting all behave exactly like vanilla. Living NPCs and creatures are never outlined (dead bodies and lootable knocked-out units are).
- **Works with the vanilla scanner** — use the handheld scanner normally; highlights re-apply within about a second after you put it down.
- **Performance aware** — 200 ms scan loop with per-pass budgets, sliced ring-cell walking, lock-free memory reads and cached inventory checks, so walking around does not cause stutter.
- **Logging off by default** — `LogMaxMB=0` in the shipped INI (public release writes no log at all). Set it to `1..1024` to enable a size-capped log for bug reports — no code changes needed.

## Requirements

| | |
| --- | --- |
| Starfield | **1.16.244.0** — the plugin resolves engine functions by address-library ID; a different game version needs a rebuild |
| SFSE | Starfield Script Extender **0.2.21** or newer |
| Address Library | **(1.16.244.0) SFSE Address Library** |

## Installation

Install the release archive with **Mod Organizer 2** or **Vortex** (the archive root is the `Data` folder layout), then enable `StarfieldAlwaysScan.esm` in your load order:

```
StarfieldAlwaysScan.esm              -> Data\
SAS_AlwaysScan.ini                   -> Data\                (config, next to the esm)
Scripts\SAS_Bridge.pex               -> Data\Scripts\
Scripts\Source\SAS\SAS_Bridge.psc    -> Data\Scripts\Source\SAS\   (source, optional)
SFSE\Plugins\SAS_AlwaysScan.dll      -> Data\SFSE\Plugins\
```

Manual installation: extract into `...\Starfield\Data\` and merge folders.

## Configuration

Everything lives in `SAS_AlwaysScan.ini` **next to `StarfieldAlwaysScan.esm`** (with MO2: inside the mod's own folder; manual install: `…\Starfield\Data\`). The INI is heavily commented and read once at game start — radius, hotkey, per-category colours / outline states / on-off switches, loot rules, performance knobs and the log switch are all there. The Nexus page documents every option.

The log (`SAS_AlwaysScan.log`, also next to the esm) is **off by default** in the public release (`LogMaxMB=0`); when collecting a log for a bug report set `LogMaxMB=10` and restart the game.

## Building from source

The plugin is C++23, built with **xmake** + **MSVC (v143 / VS 2022+)** against a local [CommonLibSF](https://github.com/Starfield-Reverse-Engineering/CommonLibSF) checkout:

```powershell
cd plugin
xmake f -y -p windows -a x64 -m releasedbg --vs=2022
xmake build SAS_AlwaysScan
```

Large external trees are **not tracked** and must be placed once:

- `tools/commonlibsf-main/` — CommonLibSF source (included by `plugin/xmake.lua`)
- `tools/vendor/xEdit/xSFEdit64.exe` — headless xEdit (only needed to rebuild the ESM)
- a Starfield install + Creation Kit (Papyrus compiler) and Mod Organizer 2 — only needed for the full pipeline below

Two CommonLibSF header offsets are corrected locally (`BGSListForm::arrayOfForms` → `0x38`, `TESGlobal::value` → `0x48`); the build script verifies them and refuses to build if a re-unpacked commonlibsf overwrites the fix.

The full one-shot pipeline — `pwsh -File tools\build-sas.ps1` — rebuilds the ESM (headless xEdit + `tools/xedit-scripts/build_sas.pas`), compiles the Papyrus bridge, builds the DLL, deploys to Mod Organizer 2 and enables the mod. It contains hard-coded paths to this machine's Starfield / Creation Kit / MO2 installs (constants at the top of the script) and is meant to be adapted.

## Repository layout

```
plugin/        C++ SFSE plugin source (src/AlwaysScan.cpp is the main implementation)
scripts/       Papyrus bridge (SAS_Bridge.psc)
resources/     SAS_AlwaysScan.ini — the config template shipped in releases
tools/         build pipeline, packaging, reverse-engineering & research scripts
docs/          engineering notes, research and the project log (Chinese)
package/       files used to build the Nexus release (README.txt, description, preview)
tr/            localization tooling & workspace used while researching third-party mods
crash-reports/ crash-dump analysis notes
```

Versioning: the Nexus release version (currently **2.0.1**) and the plugin's internal build number (currently **5.1.0**) are bumped manually, per release.

## Credits

Built with **SFSE** and **CommonLibSF** — thanks to their authors, and to everyone who tested and reported issues.

Plugin license: **GPL-3.0-or-later** (see `plugin/xmake.lua`).

---

## 中文简介

**Always Scan —— 不举扫描仪，也能一直拥有扫描仪高亮**（Starfield 1.16.244.0 / SFSE 0.2.21+）

原版游戏里，想看清哪些东西能捡、能搜、能用，必须切到手持扫描仪；而举着扫描仪时又没法用枪、用工具，而且只有屏幕中央小圆圈里的目标才会亮。本 MOD 以原版的方式解决这两件事：

- **常驻高亮**：默认 **F8** 开关（可改键），HUD 有提示；
- **全半径**：周围可配半径（默认 50 米，5~500 米）内全方位高亮，不再受中央圆圈限制；
- **观感就是原版**：直接驱动引擎自带 outline 描边（轮廓而非填充），形状 / 粗细 / 呼吸感与原版一致；2.0 起自建颜色通道（2.0.1 起 14 条），原版扫描仪 / NPC / 星球目标的颜色一个字节都不改；
- **分类分色**（全部可在 INI 配置）：红 = 武器 / 投掷物 + 太空服 / 背包 / 头盔 / 服饰，橙 = 容器 / 尸体，紫 = 笔记 + 资源，绿 = 弹药 / 救援（原版绿），**黄 = 开锁器 + 信用币（2.0.1 新增）**，**品红 = 门 / 电脑 / 按钮等所有可互动物品（2.0.1 起设备并入该组；2026-09-28 第二轮由白改品红）**，蓝 = 杂项（原版蓝），植物 / 矿脉 / 气泉 / 液池 = 原版青（未扫描）→ 原版绿（已扫描，直读引擎自身数据）；
- **搜空即熄灭**、展示柜关着也亮、**活着的 NPC / 生物永不描边**、**不影响任何交互**、与原版扫描仪完全不冲突；
- **日志默认完全关闭**（公开版 `LogMaxMB=0`）：排查问题时把 INI 里 `LogMaxMB` 改为 10 重启即可，日志与配置都在 esm 同级目录。

发布页：<https://www.nexusmods.com/starfield/mods/18268>
