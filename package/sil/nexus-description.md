# Nexus Mods 上传文案：Simple Immersive Looting - Chinese Translation (zh-CN)

> 用途：Nexus「Upload a mod」页面的各字段 + 正文（Description）。
> 正文给两版：**英文**（Nexus 默认语言，建议用这版做正文）与**中文对照**
> （需要时贴到 BBCode 里；Nexus 的编辑器支持直接粘贴富文本）。

## 一、表单字段

| 字段 | 建议值 |
| --- | --- |
| Mod name | `Simple Immersive Looting - Chinese Translation (zh-CN)` |
| Summary | `Simplified Chinese translation — Strip / Transfer 汉化「扒取装备 / 转移」` |
| Category | `Miscellaneous`（翻译类通常放这里） |
| Version | `1.0` |
| Game | Starfield |
| Requirements | `Simple Immersive Looting (Nexus mod 12677)` —— 必填挂上原 mod 页链接 |
| Tags | Chinese、Translation、Localization |
| 上传文件 | `dist\SimpleImmersiveLooting-zh-CN-1.0.zip` |
| Permissions | 按原作者页面说明选择（翻译一般需勾选「允许他人修改我的文件/基于我的文件制作」或先私信作者征得同意） |

## 二、英文正文（建议直接作为 Description）

```bbcode
[b]Simplified Chinese (zh-CN) translation of Simple Immersive Looting by korodic.[/b]

[b]REQUIRES THE ORIGINAL MOD[/b] — this archive contains [u]only the translated plugin[/u]
([font=Courier New]SimpleImmersiveLooting.esm[/font]). Install the original first, then let this pack override its plugin
(MO2: place this mod below the original in the left pane / Vortex: load after).

[b]Requires:[/b] [url=https://www.nexusmods.com/starfield/mods/12677]Simple Immersive Looting[/url]

[b]What is translated[/b] (every player-visible string):
[list]
[*]Activation menu button [b]Strip[/b] -> [b]扒取装备[/b]
[*]Activation menu button [b]Transfer[/b] -> [b]转移[/b]
[*]Perk name & description
[*]Armor descriptions
[/list]

The Papyrus scripts (the original .ba2) contain no text and are used as-is.

[b]Verification[/b]: the translated plugin was diffed against the English original with a structural
verifier — record/subrecord signatures are identical, only 6 string payloads changed
([font=Courier New]records: 6, changed records: 3, changed strings: 6, problems: 0[/font]).
Compatible with game version 1.16.244.0.

[b]Credits:[/b] original mod & scripts by [b]korodic[/b]. Please endorse the original mod.
```

## 三、中文正文（对照）

```
【简介】这是 korodic 的 Simple Immersive Looting 的简体中文汉化。

【前置】必须先安装原 mod —— 本包只含汉化后的插件
（SimpleImmersiveLooting.esm），不含任何原 mod 资产。
原 mod：https://www.nexusmods.com/starfield/mods/12677

【安装】先装原 mod，再把本包当作独立 mod 安装并让它覆盖原插件
（MO2：在左侧列表放在原 mod 下方；Vortex：排序在原 mod 之后）。

【汉化内容】插件内全部玩家可见文本：
  · 交互菜单按钮 Strip -> 扒取装备
  · 交互菜单按钮 Transfer -> 转移
  · 技能名称与描述
  · 护甲描述
脚本（原 .ba2）不含任何文本，原样使用。

【校验】汉化版与原版做过结构比对：记录/子记录签名完全一致，
仅 6 处字符串内容不同（problems: 0）。适配游戏 1.16.244.0。

【鸣谢】原 mod 作者 korodic —— 请去原 mod 页面点赞。
```

## 四、上传前检查清单

- [ ] 把 `package\sil\README.txt` 与 `package\sil\nexus-description.md` 里的
      `<PUT-YOUR-NEXUS-NAME-HERE>` 换成自己的 Nexus 用户名，然后重跑打包脚本
      （README 在包内，改完必须重新打包）。
- [ ] 确认「Files」里上传的是 `dist\SimpleImmersiveLooting-zh-CN-1.0.zip`。
- [ ] 在 Requirements 里挂上原 mod 链接。
- [ ] 权限/授权：在原作者允许的前提下发布翻译；如作者页面未授权，
      先通过 Nexus 私信征求同意（礼貌且避免被下架）。
