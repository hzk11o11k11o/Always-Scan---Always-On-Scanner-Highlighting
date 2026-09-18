# 11 · 第三方 mod 兼容性核查 + 汉化：`SimpleImmersiveLooting.esm`（Simple Immersive Looting）

> 问题（用户）：`Simple Immersive Looting-12677-1-0-1733685604.7z`（2024-12-08 的 v1.0）
> "似乎不支持新版本了"，要求做新版本兼容处理，**顺便汉化**。
>
> **结论：把插件从里到外核了一遍（12 项检查），没有找到任何"新版本不兼容"的证据** ——
> 它的格式、引用、脚本、perk 机制与当前 1.16.244.0 全部对得上，
> 且同类特征（ESM+Light / formVersion 576 / 覆盖原版记录）的 `morelore_mantislegacy.esm`
> 在本机已实测工作正常。**已完成汉化并部署到 MO2，待游戏内实测**（判据见第七节）。
>
> 核查日期：2026-09-18。全部结论都有本地字节证据（工具输出）。

---

## 一、被检对象

| 项 | 值 | 来源 |
| --- | --- | --- |
| 名称 / 作者 | **Simple Immersive Looting** / `korodic` | ESM `CNAM`、Nexus 页面 |
| Nexus | `modid=12677`（`url=https://www.nexusmods.com/starfield/mods/12677`） | meta.ini |
| 安装包 | `Simple Immersive Looting-12677-1-0-1733685604.7z`（时间戳 1733685604 = **2024-12-08**） | 文件名 |
| 文件 | `SimpleImmersiveLooting.esm` **2 911 B** + `SimpleImmersiveLooting - Main.ba2` **1 791 B**（2 个 `.pex`） | 7z 清单 |
| Creations 平台 | 同一 mod（ID `ac127db4-…`）**一直在售**，2026-03-20 还加了 PS5 支持 ⇒ 官方管线认为它适用于当前版本 | creations.bethesda.net |
| 功能 | 给玩家加两个交互选项：**Strip**（让尸体卸下衣服/护甲/宇航服/武器，从而可搜刮）与 **Transfer**（原版只在死亡尸体上有，本 mod 让 **被 EM 击晕的 NPC** 上也有） | SNAM / Nexus 描述 |

## 二、它到底是由什么构成的（工具直接读字节）

`tools/re/sf_plugin_check.py` + `tools/re/rec_scan.py` + `tools/re/vmad.py` + `xEdit xDump`：

```
GMST 0013DF09  fEquippedArmorChanceToDrop = 0.0     <- 覆盖原版（原值 0.1）
ARMO 01000809  SIL_ARMO_NakedSpacesuit               <- 新增（模型 = 原版 AA_Naked_Body 等）
ARMO 01000811  SIL_ARMO_NakedSpacesuitCorpseFrozen   <- 新增
QUST 01000807  SIL_QUST_Manager                      <- 新增，Start Game Enabled
PERK 01000806  SIL_PERK_LootingOptions               <- 新增，两个 Add Activate Choice 条目
CNDF 01000815  SIL_CNDF_HumanCorpseRaceStripConditions <- 新增，2 条条件
BA2           scripts/sil_script_manager.pex + sil_script_lootingoptionsperk.pex
```

工作链（全部由 xDump 的解读 + pex 字符串表拼出，无推测）：

1. `QUST SIL_QUST_Manager` 开局启动 → `SIL_Script_Manager.OnQuestInit` → `PlayerRef.AddPerk(SIL_PERK_LootingOptions)`；
2. `PERK` 两个条目：`PRKE Type=Entry Point` + `DATA: Entry Point=Activate, Function=Add Activate Choice`，
   按钮文本 `EPF2` = "Strip" / "Transfer"；条件（CTDA）全是对尸体/昏迷目标的判定：
   `GetIsRace(HumanRace) OR GetIsRace(HumanCorpseRace)`、`GetDead`、`WornCoversBipedSlot(35/3)`、
   `GetEquipped(SIL_ARMO_NakedSpacesuit)` 等；
3. 玩家点选项 → `SIL_Script_LootingOptionsPerk.OnEntryRun` → `akTarget.UnequipAll()` /
   `OpenInventory(True)`（+ `Play(WwiseEvent…)`）。

## 三、兼容性核查（对当前 1.16.244.0）

| # | 检查项 | 实测结果 | 判读 |
| --- | --- | --- | --- |
| 1 | TES4 头 | `flags=0x00000101` = ESM + **Light**；master 仅 `Starfield.esm` | 与官方 `Constellation.esm`(0x181)、`SFBGS004/007/008`(0x181) 及本机在用的 `morelore_mantislegacy.esm`(**0x101**) 同构 ⇒ 合法 |
| 2 | formVersion | 记录全是 **576**（当前 CK 写 581/582） | 方向"更旧"⇒ 引擎向后兼容；本机在用的 `morelore`(576)、`InstantScan`(555) 均正常 |
| 3 | GMST 覆盖命中 | `fEquippedArmorChanceToDrop` 在当前 `Starfield.esm` 中**存在**（`0x0013DF09`，原值 `0.1`） | 覆盖精确成立 |
| 4 | 覆盖唯一性 | `findstr` 扫 MO2 全部 `mods\**\*.esm`：**没有**其它插件改这条 GMST | 零争抢 |
| 5 | CTDA 函数索引 | 用 xEdit（4.1.5f SF 版）解读：`GetIsRace` / `GetDead` / `IsInSpace` / `WornCoversBipedSlot` / `GetEquipped` —— 全部是当前版本的**有效函数**且语义与 mod 意图吻合 | 函数表没有位移 |
| 6 | PERK 结构 | `Entry Point: Activate` + `Function: Add Activate Choice`，`EPFT=04`，`EPF2`=按钮文本、`EPF3`=Script Flags(Replace Default) | 与当前原版同类 perk（`SQ_Captive_PlayerRescueChoices` / `UCR04_HarvestSamplePerk` / `MS03_JunoActivationPromptPerk`）**逐字段一致** |
| 7 | 脚本触发机制 | mod 用**普通脚本 + `OnEntryRun`**（非 CK 的 fragment 路径）。`Data\Scripts\Source\Base\Perk.psc` 原文注释：*"Event called when a perk entry is run … (**in parallel with the fragment**)"* | 官方明确支持这种挂法 |
| 8 | QUST 启动 | xDump 解读 `DNAM: Start Game Enabled, Allow repeated stages, Starts Enabled` | 与自建 `SAS_AlwaysScanQuest`(0x11) 的必需位一致，多一个 "Allow repeated stages"（无害） |
| 9 | pex 格式 | 两个 `.pex` 都是 **magic 0xFA57C0DE / version 3.12 / gameId 4** —— 与本项目今天用当前 PapyrusCompiler 编出的 `SAS_Bridge.pex` 完全同版本 | pex 无版本落差 |
| 10 | 脚本 API | 字符串表显示只用了 `AddPerk/HasPerk/UnequipAll/OpenInventory/Play/GetRace/EquipItem/OnQuestInit/OnEntryRun`，逐个在当前 `Data\Scripts\Source\Base\*.psc` 里能查到定义（含 `Actor.OpenInventory(bool abForceOpen…)`） | 无被删 API |
| 11 | VMAD 属性 | `HumanRace`(0x347D)、`WwiseEvent_ITMDefaultTakeAllSound`(0x183D81)、`PlayerRef`(0x14)、`SIL_ARMO_NakedSpacesuit`(自身 0x01000809) 全部有效 | 无悬空绑定 |
| 12 | ARMO 引用 | 模型指向 `AA_Naked_Body`/`AA_Naked_Hands`/`AA_Skin_Corpse_Frozen_*` 等原版 ARMA；关键字 4 条（`ArmorTypeApparelOrNakedBody` 等）全部存在 | 无悬空引用 |

**⇒ 结论：这是一只「完好的旧插件」**。它"老"的地方（Light 位、formVersion 576）
都有当前正在工作的同构先例，**没有任何一项检查指出它会在 1.16.244.0 上失效**。
真要说风险，只剩"推测层"的东西（如 B 社哪天改了 biped slot 35 的含义），
而这类东西离线无法证伪，只能靠实测排除。

## 四、汉化（本轮完成）

### 4.1 抽出全部玩家可见文本（新扩了 trtool 白名单）

`trtool.py` 原来只认 `*:FULL` 与 `ARMO/BOOK/MESG/…:DESC`，抽不到 PERK 的按钮文本。
本轮给白名单加了 `"PERK": {"DESC", "EPF2"}`（EPF2 = Add Activate Choice 的 Button Label），
重新 `extract` 得到 **6 条**：

| 记录 | 子记录 | 原文 | 译文 |
| --- | --- | --- | --- |
| PERK 01000806 | EPF2 #0 | `Strip` | **扒取装备** |
| PERK 01000806 | EPF2 #1 | `Transfer` | **转移** |
| PERK 01000806 | FULL | `Simple Immersive Looting Perk` | **简单沉浸式搜刮** |
| PERK 01000806 | DESC | `Allows the player to have access to additional options for looting bodies under certain conditions.` | **在特定条件下为玩家提供额外的尸体搜刮选项。** |
| ARMO ×2 | DESC | `A workaround for bodies being naked when the game requires a spacesuit to be worn.` | **用于解决游戏要求必须穿宇航服时尸体显示为裸体的变通方案。** |

译名依据（`tr/loc/starfield_en_zh.tsv`，官方对照）：
**Loot = 搜刮**（"Loot a Ship"→"搜刮飞船"）、**Corpse = 尸体**、
**Spacesuit = 宇航服**（"Spacesuit Workbench"→"宇航服工作台"）。
`Transfer` 沿用 B 社惯例「转移」（且它的 `EPF3` 就是 *Replace Default*，本来就是替原版那个入口）。
`Strip` 官方无此词，自创**「扒取装备」**（比 2 字更明确，避免与「转移」混淆）。

### 4.2 写入与校验

```
trtool.py apply  ->  replaced 6 strings, not applied 0
tr_verify.py     ->  records: 6  changed records: 3  changed strings: 6  problems: 0
重新 extract     ->  0 strings（白名单范围内已无英文残留）
读回二进制       ->  6 条均为合法 UTF-8 中文（见下）
```

```
ARMO DESC (85B): '用于解决游戏要求必须穿宇航服时尸体显示为裸体的变通方案。\x00'
PERK FULL (22B): '简单沉浸式搜刮\x00'
PERK DESC (64B): '在特定条件下为玩家提供额外的尸体搜刮选项。\x00'
PERK EPF2 (13B): '扒取装备\x00'
PERK EPF2 (7B):  '转移\x00'
```

**注意**：这是**非本地化插件**（无 0x80 位），字符串内联写在 ESM 里，统一写 **UTF-8（无 BOM）**
—— 与本项目全部汉化 mod（`above and beyond` / `morelore` / ASE 系列）的做法一致，那些已实测显示正常。

**脚本侧无需汉化**：两个 `.pex` 的字符串表里**没有任何字符串字面量**（全是 API/变量名），
BA2 原样保留。

### 4.3 汉化工具改动

- `tools/re/trtool.py`：白名单新增 `"PERK": {"DESC", "EPF2"}`（带注释说明用途）。
- 新增工具（都已保留复用）：
  `tools/re/pexinfo.py`（pex 头部 + 字符串表解析，零依赖、零错位风险）、
  `tools/re/rec_scan.py`（记录子记录序列扫描/对比）、
  `tools/re/sf_flags_scan.py`（批量插件 flags/覆盖记录统计）。

## 五、部署（2026-09-18）

| 项 | 值 |
| --- | --- |
| MO2 mod 目录 | `D:\Mod Organizer 2\starfield_mods\mods\Simple Immersive Looting\` |
| 文件 | `SimpleImmersiveLooting.esm`（**汉化版 2 876 B**）+ `SimpleImmersiveLooting - Main.ba2`（原件 1 791 B）+ `meta.ini`（含汉化说明） |
| modlist.txt | `+Simple Immersive Looting`（置顶，与 build-sas.ps1 同规则） |
| plugins.txt | `*SimpleImmersiveLooting.esm`（追加到末尾） |
| loadorder.txt | `SimpleImmersiveLooting.esm`（追加到末尾） |
| 备份 | 三个 profile 文件的 `.bak-sil`；原始 ESM 在 `tr/orig/`；汉化产物在 `tr/build/` |
| 脚本 | `tools/mo2-enable-sil.ps1`（幂等，可重复执行） |
| 临时文件 | 游戏 `Data\` 下核查用的副本已删除 |

★ **MO2 当时处于运行状态** —— 这三个 profile 文件在 MO2 退出时会被内存中的旧内容覆盖，
**必须先重启 MO2**（关闭 → 重新打开），新 mod 才会出现在列表里并处于启用状态。

## 六、复现命令

```powershell
# 身份 / 结构
python tools\re\sf_plugin_check.py <plugin.esm>
python tools\re\rec_scan.py      <plugin.esm> --sig PERK
python tools\re\vmad.py          <plugin.esm> 0x01000806
python tools\re\pexinfo.py       sil_script_lootingoptionsperk.pex --dump-all

# xEdit 解读（xDump 是免 GUI 的：输出在 stdout）
tools\vendor\xEdit\xDump64.exe -SF1 -D:"D:\SteamLibrary\steamapps\common\Starfield\Data" "SimpleImmersiveLooting.esm"

# 汉化
python tools\re\trtool.py extract  <orig.esm> -o sil_extract.tsv --stats
python tools\re\trtool.py apply    <orig.esm> --map tr\lang\sil_map.tsv -o <out.esm> --verbose
python tools\re\tr_verify.py       <orig.esm> <out.esm>

# 部署
& 'tools\mo2-enable-sil.ps1'
```

## 七、游戏内实测判据（待用户执行）

**前置**：重启 MO2 → 正常方式启动游戏（读档或新档均可）。

1. **杀死一个人类 NPC**（例：新亚特兰蒂斯附近/任何人类敌人），走到尸体旁：
   - 交互菜单应出现 **「扒取装备」**（原来是英文 Strip）；
   - 选择后：尸体卸下衣服/护甲/宇航服/武器，装备变成可拾取物；
2. **用 EM（电磁）武器击晕一个人类 NPC**（未死）：交互菜单应出现 **「转移」**
   （原版在昏迷 NPC 上没有这个选项 —— 这正是本 mod 的功能），可打开其背包；
3. 若**选项不出现**，按顺序排查：
   - 控制台 `help SIL_PERK` —— 应能列出 `SIL_PERK_LootingOptions`（记住了它的 FormID，插件索引见 MO2 右侧）；
   - 控制台 `player.addperk <FormID>` 手动加 perk 后再看（若这样有效 ⇒ 是 quest/脚本环节的问题）；
   - 查 `%USERPROFILE%\Documents\My Games\Starfield\Logs\Script\Papyrus.0.log`
     是否有 `SIL_Script_Manager` / `SIL_Script_LootingOptionsPerk` 的报错；
   - 把上述三样结果反馈，再决定下一步（对策已在手：改 fragment 挂法 / 用当前 CK 重建记录）。
4. **不回归红线**：其它 mod（`du_*` / `ase*` / `kinggath` / 本项目的 Always Scan）行为不受影响；
   GMST 覆盖只影响"死亡时装备掉落概率"（0.1 → 0），这是 mod 的既有设计。

## 八、Nexus Mods 上传包（2026-09-18）

用户要求：把本汉化打包成可上传 Nexus 的**完整包**（不是只含 ESM）。照 `tools/package-nexus.ps1`
（高亮 mod 那套）的模式新增 **`tools/package-sil.ps1`**：

| 项 | 值 |
| --- | --- |
| 产物 | **`dist\SimpleImmersiveLooting-zh-CN-1.0.zip`**，**4 267 B**，SHA256 `F7FEDF8DA08E3D1A2B7D4D383BB80F67EE2E21F7F8DB554922531DDAC8D7851E` |
| 包内（完整包） | `SimpleImmersiveLooting.esm`（汉化版 2 876 B，`3971667F…`）+ `SimpleImmersiveLooting - Main.ba2`（**原版脚本包 1 791 B，逐字节未动**，`3A17C46C…`）+ `README.txt`（3 054 B，`85E44F75…`）；包根即 Data 布局，**一步安装、无需先装原 mod** |
| 包内不含 | `meta.ini`（MO2 本地元数据，与分发包无关） |
| 反向可选模式 | `-TranslationOnly` ⇒ 只含汉化 ESM 的翻译包（输出文件名自动加 `-translation-only` 后缀，不与完整包互相覆盖），供「未获原作者再分发许可」时的退路 |
| 自检 | 必需文件存在 / zip 内容逐条列出 / 每文件 SHA256 / 包体积；README 里 `<PUT-YOUR-NEXUS-NAME-HERE>` 未替换时告警 |
| 上传文案 | `package/sil/nexus-description.md`（表单字段 + 英文 BBCode 正文 + 中文对照 + 上传前检查清单，含**再分发许可**提醒）；包内说明源 `package/sil/README.txt` |

**复现**：`& 'tools\package-sil.ps1'`（改完 README 作者名后需重跑；`dist/` 不入库）。
★ 上传前必做：替换 README 里的作者名占位符 → 重跑打包 → Files 里上传 zip。
★ 因完整包含原作者的 `.ba2`，发布前需确认 korodic 的再分发许可（页面权限说明或私信）。
