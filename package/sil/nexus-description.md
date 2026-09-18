# Nexus Mods 上传文案：Simple Immersive Looting - Chinese Translation (zh-CN)

> 用途：Nexus「Upload a mod」页面的各字段 + 正文（Description）。
> 正文给两版：**英文**（Nexus 默认语言，建议用这版做正文）与**中文对照**
> （需要时贴到 BBCode 里；Nexus 的编辑器支持直接粘贴富文本）。
> 本包为**完整包**（含原版 .ba2），一步安装、无需先装原 mod。

## 一、表单字段

| 字段 | 建议值 |
| --- | --- |
| Mod name | `Simple Immersive Looting - Chinese Translation (zh-CN)` |
| Summary | `Simplified Chinese translation (full package) — Strip / Transfer 汉化「扒取装备 / 转移」` |
| Category | `Miscellaneous`（翻译类通常放这里） |
| Version | `1.0` |
| Game | Starfield |
| Requirements | 无需前置（原 mod 文件已包含）；可选挂原 mod 链接 `mods/12677` 供玩家点赞 |
| Tags | Chinese、Translation、Localization |
| 上传文件 | `dist\SimpleImmersiveLooting-zh-CN-1.0.zip` |
| Permissions | ★ 本包包含原 mod 的 `.ba2` ⇒ **发布前确认 korodic 的再分发许可**（页面权限或私信同意）；若未获许可，改用 `-TranslationOnly` 只发汉化插件 |

## 二、英文正文（建议直接作为 Description）

```bbcode
[b]Simplified Chinese (zh-CN) translation of Simple Immersive Looting by korodic — full package.[/b]

This archive contains the [b]complete mod[/b], ready to install in one step:

[list]
[*][font=Courier New]SimpleImmersiveLooting.esm[/font] — the translated plugin
[*][font=Courier New]SimpleImmersiveLooting - Main.ba2[/font] — the original Papyrus script archive (unchanged)
[/list]

No separate download required. If you already have the original installed, just let this package overwrite it.

[b]Requires:[/b] Starfield (verified on 1.16.244.0)

[b]What is translated[/b] (every player-visible string):
[list]
[*]Activation menu button [b]Strip[/b] -> [b]扒取装备[/b]
[*]Activation menu button [b]Transfer[/b] -> [b]转移[/b]
[*]Perk name & description
[*]Armor descriptions
[/list]

The Papyrus scripts contain no text and are byte-identical to the original.

[b]Verification[/b]: the translated plugin was diffed against the English original with a structural
verifier — record/subrecord signatures are identical, only 6 string payloads changed
([font=Courier New]records: 6, changed records: 3, changed strings: 6, problems: 0[/font]).

[b]Credits:[/b] original mod & scripts by [b]korodic[/b]
([url=https://www.nexusmods.com/starfield/mods/12677]Simple Immersive Looting[/url]).
Please endorse the original mod.
```

## 三、中文正文（对照）

```
【简介】这是 korodic 的 Simple Immersive Looting 的简体中文汉化**完整包**。

【内容】本包包含完整 mod 文件，一步安装、无需再下载原 mod：
  · SimpleImmersiveLooting.esm          —— 汉化版插件
  · SimpleImmersiveLooting - Main.ba2   —— 原版脚本包（未改动）
如果你已装过原 mod，直接让本包覆盖安装即可。

【汉化内容】插件内全部玩家可见文本：
  · 交互菜单按钮 Strip -> 扒取装备
  · 交互菜单按钮 Transfer -> 转移
  · 技能名称与描述
  · 护甲描述
脚本（.ba2）不含任何文本，与原版逐字节一致。

【校验】汉化插件与原版做过结构比对：记录/子记录签名完全一致，
仅 6 处字符串内容不同（problems: 0）。适配游戏 1.16.244.0。

【鸣谢】原 mod 作者 korodic —— 请去原 mod 页面
（https://www.nexusmods.com/starfield/mods/12677）点赞。
```

## 四、上传前检查清单

- [ ] 把 `package\sil\README.txt` 与 `package\sil\nexus-description.md` 里的
      `<PUT-YOUR-NEXUS-NAME-HERE>` 换成自己的 Nexus 用户名，然后重跑打包脚本
      （README 在包内，改完必须重新打包）。
- [ ] 确认「Files」里上传的是 `dist\SimpleImmersiveLooting-zh-CN-1.0.zip`（完整包，含 .ba2）。
- [ ] ★ **再分发许可**：本包含原作者的 `.ba2`。请先在原 mod 页面看权限说明或私信
      korodic 征得同意；若不允许，就改用
      `& 'tools\package-sil.ps1' -TranslationOnly` 只发汉化插件（包内说明与正文
      相应改用「需先装原 mod」口径）。
- [ ] 可选：在 Requirements 里挂原 mod 链接，方便玩家点赞。
