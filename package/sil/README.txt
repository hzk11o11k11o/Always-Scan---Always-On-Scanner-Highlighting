================================================================
 Simple Immersive Looting - Simplified Chinese (zh-CN) Translation
 「简单沉浸式搜刮」简体中文汉化
================================================================

Author: <PUT-YOUR-NEXUS-NAME-HERE>
Version: 1.0  (translation of Simple Immersive Looting v1.0)

WHAT THIS IS
------------
A Simplified Chinese translation of "Simple Immersive Looting" by korodic.

This archive contains ONLY the translated plugin (SimpleImmersiveLooting.esm).
The original mod - including "SimpleImmersiveLooting - Main.ba2" - is still
required; no original assets are redistributed here.

本包是原 mod「Simple Immersive Looting」的简体中文汉化，包内只有汉化后的插件
SimpleImmersiveLooting.esm，需要先安装原 mod（含原版的 .ba2 脚本包）。

REQUIREMENTS
------------
- Starfield (translated & verified against game version 1.16.244.0)
- Original mod: https://www.nexusmods.com/starfield/mods/12677

INSTALLATION
------------
MO2 / Vortex (recommended):
  1. Install the original "Simple Immersive Looting" first.
  2. Install this translation pack as a separate mod.
  3. Make this pack win over the original plugin:
       MO2    - put it BELOW the original mod in the left pane
                (or use it in "overwrite"-style priority);
       Vortex - load this pack AFTER the original.
Manual:
  Copy SimpleImmersiveLooting.esm into your Data folder, overwriting
  the English original.

UNINSTALL
---------
Remove this pack (or restore the original SimpleImmersiveLooting.esm).
Nothing else is modified.

WHAT IS TRANSLATED
------------------
Every player-visible string inside the plugin (6 strings):
  * Activation menu button "Strip"    ->  扒取装备
  * Activation menu button "Transfer" ->  转移
  * Perk name  "Simple Immersive Looting Perk" -> 简单沉浸式搜刮
  * Perk description                   ->  在特定条件下为玩家提供额外的尸体搜刮选项。
  * Armor description x2 (internal)    ->  用于解决游戏要求必须穿宇航服时尸体显示为裸体的变通方案。

The Papyrus scripts inside the .ba2 contain no text at all and are used
as-is (the original .ba2 is untouched).

Terminology follows the official Simplified Chinese in-game text,
e.g.  Loot = 搜刮   Corpse = 尸体   Spacesuit = 宇航服.

VERIFICATION
------------
The translated plugin was diffed against the English original with a
structural verifier (every record / subrecord signature must be identical,
only the 6 string payloads may differ):
  records: 6   changed records: 3   changed strings: 6   problems: 0
File size: 2 911 bytes (original) -> 2 876 bytes (translated).

CREDITS
-------
- Original mod, plugin and scripts: korodic
  https://www.nexusmods.com/starfield/mods/12677
- Simplified Chinese translation: <PUT-YOUR-NEXUS-NAME-HERE>

NOTES
-----
- Like the original mod, this plugin is NOT "achievement friendly"
  (a non-official plugin disables achievements in Starfield).
- If the activation options do not show up, make sure the original .ba2 is
  present and the plugin is loaded AFTER Starfield.esm (see the original
  mod page for details).
