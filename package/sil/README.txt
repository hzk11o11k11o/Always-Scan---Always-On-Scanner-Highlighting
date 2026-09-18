================================================================
 Simple Immersive Looting - Simplified Chinese (zh-CN)
 「简单沉浸式搜刮」简体中文汉化（完整包）
================================================================

Author: <PUT-YOUR-NEXUS-NAME-HERE>
Version: 1.0  (translation of Simple Immersive Looting v1.0)

WHAT THIS IS
------------
A Simplified Chinese translation of "Simple Immersive Looting" by korodic,
shipped as a COMPLETE, ready-to-install package:

    SimpleImmersiveLooting.esm          <- the translated plugin (Chinese)
    SimpleImmersiveLooting - Main.ba2   <- the original Papyrus script archive
                                           (unchanged; contains no text)

本包是原 mod「Simple Immersive Looting」的简体中文汉化**完整包**：
插件本体为汉化版，脚本包（.ba2）为原版文件。可直接安装，无需再单独下载原 mod。

REQUIREMENTS
------------
- Starfield (translated & verified against game version 1.16.244.0)
- Nothing else - all files of the original mod are included.

If you already have the original mod installed, simply let this package
overwrite it (or uninstall the original first).

INSTALLATION
------------
MO2 / Vortex:
  Install this archive as a normal mod and enable it.
Manual:
  Copy both files into your Data folder (overwrite when asked):
    SimpleImmersiveLooting.esm
    SimpleImmersiveLooting - Main.ba2

UNINSTALL
---------
Remove this mod. (To go back to English, reinstall the original mod.)

WHAT IS TRANSLATED
------------------
Every player-visible string inside the plugin (6 strings):
  * Activation menu button "Strip"    ->  扒取装备
  * Activation menu button "Transfer" ->  转移
  * Perk name  "Simple Immersive Looting Perk" -> 简单沉浸式搜刮
  * Perk description                   ->  在特定条件下为玩家提供额外的尸体搜刮选项。
  * Armor description x2 (internal)    ->  用于解决游戏要求必须穿宇航服时尸体显示为裸体的变通方案。

The Papyrus scripts inside the .ba2 contain no text at all and are used
as-is (byte-identical to the original archive).

Terminology follows the official Simplified Chinese in-game text,
e.g.  Loot = 搜刮   Corpse = 尸体   Spacesuit = 宇航服.

VERIFICATION
------------
The translated plugin was diffed against the English original with a
structural verifier (every record / subrecord signature must be identical,
only the 6 string payloads may differ):
  records: 6   changed records: 3   changed strings: 6   problems: 0
Plugin size: 2 911 bytes (original) -> 2 876 bytes (translated).

CREDITS
-------
- Original mod, plugin and scripts: korodic
  https://www.nexusmods.com/starfield/mods/12677
- Simplified Chinese translation: <PUT-YOUR-NEXUS-NAME-HERE>

NOTES
-----
- Like the original mod, this plugin is NOT "achievement friendly"
  (a non-official plugin disables achievements in Starfield).
- If the activation options do not show up, make sure the plugin is enabled
  and loaded after Starfield.esm.
