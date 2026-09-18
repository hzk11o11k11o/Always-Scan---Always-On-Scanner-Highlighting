## 项目目标
给B社游戏starfield做一个MOD

## 项目背景及目标
- 原版游戏中，扫描仪可以高亮物品，但玩家必须一直开着扫描仪，此时无法使用其他枪械和工具，不方便
- 这个mod的目的是在玩家不装备扫描仪的情况下，也能开启扫描仪效果

## MOD内容
- 在玩家不装备扫描仪的情况下，也能开启扫描仪效果
- 原版扫描仪只有在圆圈范围内才有高亮效果，这个mod要支持在圆圈范围外也能高亮物品（就是整个屏幕）
- 由于本MOD只是通过另一种方式实现游戏原版内容，尽量直接使用游戏自带能力实现本MOD
- 给一个快捷键开关mod功能
- 不要影响玩家与其他实体的交互能力，比如NPC对话，使用可交互物品等
- 基于SFSE制作

## starfield安装位置
D:\SteamLibrary\steamapps\common\Starfield（sfse已安装）

## starfield Creation Kit安装位置
D:\SteamLibrary\steamapps\common\Starfield

## Mod Organizer 2安装位置
D:\Mod Organizer 2

## DU 系列 Creation 汉化

5 个 du 系列 Creation 的汉化工作说明在 `docs/06-DU系列mod汉化.md`，
工具在 `tools/re/trtool.py` / `tr_pipeline.py` / `tr_verify.py` 等，
词典成果在 `tr/lang/`（**必须入库**，可增量续译）。
4 个已完成并部署，`du_outlaws_01` 的短串、`MESG DESC` 讯息正文与 `BOOK DESC` 书籍正文
均已完成并部署（`map` 译出 4 472/5 027 = 89.0%），只剩一类长文本（`QUST CNAM` 460）。

## 其他 mod 汉化

`above and beyond.esm` / `morelore_mantislegacy.esm` 的汉化说明在
`docs/07-AboveAndBeyond与MantisMoreLore汉化.md`（**已全部完成并部署**）。
同一套流水线；新增辅助工具 `tools/re/tr_official_hits.py`（mod 串查官方逐字命中）、
`tr_term_probe.py` / `tr_zh_probe.py`（按英文/中文子串查官方对照表）、
`tr_sid_probe.py`（按字符串 ID 取官方译名，用于「覆盖原版记录」的定名）、
`tr_mkbatch.py`（按抽取文件行号生成词典批次，避免手抄长英文段落）。

`ase3.esm` / `aseveil.esm` / `aseeverbright.esm` / `kinggathcreations_spaceship.esm`
的汉化说明在 `docs/09-ASE系列与Kinggath汉化.md`：
前三个是**内联字符串**插件，走老流水线，**已完成并部署**；
`kinggathcreations_spaceship` 是**本地化插件（flags 0x81）**，文本在 BA2 的
`STRINGS/` 里（自带 `_zhhans` 其实是英文副本）⇒ 新增
`tools/re/tr_locpack.py`（三件套读写，**按原始字节保真**）与 `tools/re/kg_sanity.py`（自检），
汉化包以**松散文件**部署到 `overwrite\Strings\`（**松散优先于归档，待游戏内实测确认**）；
UI/终端/物品**已全译**，NPC 对白还剩约 4988 行未译（可增量续译）。
另新增 `tools/re/tr_lookup.py`（官方对照表精确/前缀查词）、
`tools/re/tr_check_keys.py`（手写批次英文键逐字命中校验）。

## 注意事项
- Visual Studio 2026已安装，MSVC v143 生成工具已包含
- 已完成内容要记录在：docs/99-当前项目进度.md，其他经验总结文档也可以记录在这个文件夹里，单个文件不得超过100KB，超过后可以简化或删除过老或已经不再重要的记录
- 如果需要操作其他图形化界面程序，尽量使用后台发送命令的方式操作，不要抢占真实键盘和鼠标，不要打扰我操作电脑
- 有任何需要的其他工具，你可自行下载并配置
- “D:\workspace\starfield mod\高亮物品”是之前一个mod项目，里面有各种工具和代码，你可以把需要的工具复制过来用，部分代码可能也有参考价值
- 所有工具的源代码允许你可以自己修改，添加自己想要的能力
- 每次任务完成后，需要提交本地git，同时.gitignore也要添加必要忽略项
- mod文件生成后要部署到Mod Organizer并配置启用
- mod文件生成后帮我打包成nexus mods能接受的上传包文件，并帮我写一个介绍文案（只关注高亮mod）