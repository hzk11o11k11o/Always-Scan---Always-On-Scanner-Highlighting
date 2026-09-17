# 05 · 第三方 mod 兼容性核查：`du_overtime.esm`（Dark Universe: Overtime）

> 问题（用户）：检查 `du_overtime.esm` 是否兼容当前游戏版本（1.16.244.0）。
>
> **结论：兼容。** 插件装载、归档格式、引用完整性、运行时四条线全部通过；
> 唯一"异常"（Papyrus 属性报错）是 mod 自身 ESM↔.pex 的漂移，
> **原版与 BGS 官方 Creation 上同样出现同类报错**，不构成版本不兼容。
>
> 核查日期：2026-09-17。全部结论都有本地证据（文件字节 / 日志 / 工具输出），无"应该没事"式推断。

---

## 一、被检对象的身份与版本

| 项 | 值 | 来源 |
| --- | --- | --- |
| Creation 名称 | Dark Universe: Overtime（Missionboard Expansion） | `ContentCatalog.txt`（TM_8b405fea-…） |
| 版本 | **1.1.3**（平台构建时间戳 = **2025-08-20**） | `ContentCatalog.txt` → `"Version": "1755701422.1.1.3"` |
| 下载时间 | **2026-09-16 20:50 本地**（平台时间戳 1789598644） | 同上 + 文件时间 |
| 文件 | `du_overtime.esm` 1,545,647 B + `du_overtime - main.ba2` 2,680,401 B | `D:\Mod Organizer 2\starfield_mods\overwrite\` |
| 尺寸校验 | 1,545,647 + 2,680,401 = **4,226,048 = 平台清单 `FilesSize`** | ⇒ 两个文件与平台 v1.1.3 清单**严格一致** |
| AchievementSafe | **false** | `ContentCatalog.txt`（用它就不能拿成就，除非另装成就解锁 mod） |

⇒ 它是**从 Creations 平台当天新下载的、平台当前提供的版本**（不是从别处淘来的老包）。

安装位置在 MO2 的 `overwrite\`（Creations 下载走 MO2 VFS 时必然落这里），
`plugins.txt` 里 `*du_overtime.esm` 已启用。

## 二、静态结构核查（直接读字节）

工具：`tools/re/check_esm_header.py`、`esm_walk.py`、`fv_compare.py`、`fv_hist.py`，
归档用 `tools/vendor/xEdit/BSArch64.exe`。

| 检查项 | 结果 | 判读 |
| --- | --- | --- |
| TES4 头 | `HEDR 0.96`，formVersion=576，flags=`0x00000101` | `0x01`=ESM、`0x100`=Light ⇒ **轻插件**（游戏日志里它就是 `FE00xxxx` 号段） |
| master 列表 | 只有 `Starfield.esm`（已在，隐式加载 0 号） | 无缺失前置，无多 master 冲突 |
| 记录总数 | 793（16 个 GRUP：QUST 309 / ACTI 135 / MESG 111 / FLST 69 / MISC 45 / GLOB 25 / NPC_ 26 / SMQN 18 / GBFM 16 / KYWD 12 / COBJ 10 / 其余 17） | 单一 master 内的"纯内容包" |
| ★ 记录类型 | **793 条 FormID 高字节全部 = 01 ⇒ 全是新增记录，0 条覆盖原版记录** | 结构上**不可能**污染/替换原版数据；冲突面 = 0 |
| 记录 form version | 全部 **576**（当前 CK 写 581/582） | 方向是"更旧"。原版自己仍在加载 **552** 的 `Constellation.esm`（隐式 master，每局必载）⇒ 引擎对旧 form version 是向后兼容的；反过来（比游戏更新）才会崩 |
| 归档 | BTDX / **Starfield v2** / GNRL，BSArch 可解析，64 个文件 | 与原版 `Starfield - Animations.ba2` 同为 v2；脚本、网格齐全（`scripts\ccs_*.pex`、`meshes\ccs\*.nif`） |
| 无 DLL | 只有 ESM + BA2 | 不受 SFSE / Address Library 版本联动影响 |

## 三、引用完整性（当前 Starfield.esm 为准）

工具：`tools/re/ref_check.py`（新增）——mmap 单遍递归建 `Starfield.esm` 的
formID→签名索引（index 0 全量），再逐条解压扫描本 mod 记录数据里的全部 4 字节候选引用。

| 项 | 数量 |
| --- | --- |
| 扫描记录 | 793 |
| 非文本候选引用 | 38,014（候选值 ∈ `0x1000..0xFFFFFF`，且非纯 ASCII） |
| "在 master 中找不到"的候选 | 全部落在**噪声**类别：脚本名/文件路径字符串片段（`nif\0`/`tem\0`/`DEFAULT`…）、CTDA 条件字节、VMAD 类型码、NAVM/GBFM 二进制 |

抽样人工验证（`tools/re/field_stats.py`、`dump_one_record.py`）：

```
ACTI 的 PNAM：本 mod 135 条全是 0x00334CCC
             原版 1463 条里 1347 条也是 0x00334CCC      ⇒ PNAM 是固定枚举，不是 FormID
```

⇒ **未发现任何指向原版记录的失效引用**（换言之：没有"游戏更新删了某条记录导致引用悬空"的情况）。

## 四、运行时证据（本机日志）

| 证据 | 内容 |
| --- | --- |
| 游戏装载 | `Logs\Script\Papyrus.*.log`（9/17 的 4 次会话）里 DUO 的 quest/script 全部以 `FE00xxxx` 实例化（如 `duo_artifact_local_qst02g (FE005A39)`） |
| 脚本齐全 | 51 条 `Cannot open store for class` 里 **没有一条**属于 `duo_*`/`ccs_*`（都是原版遗留的 `TestElectrifiedWaterTriggerScript` 之类） |
| 崩溃 | 安装（9/16 20:50）后**没有新转储**：最近的 `Starfield_09-16-09-55.dmp` 是 17:55 的，属本项目自己 v1 的旧 bug（见 `docs/02`） |
| 归档 | BSArch 能完整读，游戏侧无归档相关报错 |

### 唯一的"异常"：Papyrus 属性报错（判定为噪声）

4 份日志里 DUO 相关共 36 error + 18 warning，全部是同一类：

```
error:  Property TreasureMapQuestCount on script dou_artifact_ground_quest
        attached to duo_artifact_local_qst02g (FE005A39) cannot be bound
        because … is not the right type
error:  Property Boss on script dou_artifact_space_boardingrename_quest …
warning: Property SpawnedNests on script duo_missioninfestation_lists …
        cannot be initialized because the script no longer contains that property
The type of property SpawnedNests on object duo_citydelivery_qst
        does not match the passed-in type at creation, property skipped.
```

**为什么不算版本不兼容**：

1. 这类报错是 **ESM 里的 VMAD ↔ BA2 里的 .pex 自身的类型/存在性不匹配**，
   而这两个文件是**同一次平台下载的同一份清单**（尺寸校验见第一节）⇒ 是作者
   v1.1.3 自带的"脚本已改、VMAD 没重生成"漂移，与游戏版本无关；
2. **原版与 BGS 官方内容有同类报错**（同一份日志里）：
   - 原版：`Property StopFollowConditions on script DefaultFollowerToggleQuestScript
     attached to FFCydoniaZ05 (000D6927) cannot be bound …`
   - BGS 官方 Creation：`Property MQ305Delay on script SFBGS00D:SQ_StarbornContainerScript …`
   ⇒ 在当前 1.16.244 运行时里，这类"属性没绑上"的日志本来就是普遍现象；
3. 它不影响脚本类装载（无 `Cannot open store`）也不影响 quest 实例化。

**实际含义（如实记）**：DUO 里少数任务类型（宝图/交付/虫巢相关）的个别属性没绑上，
遇到这些任务时可能有异常表现；这属于 mod 自身质量，不属于"版本不兼容"。

## 五、结论与建议

| 判定 | 依据 |
| --- | --- |
| **兼容**（可继续启用） | 单 master 且存在、0 覆盖记录、BA2 v2、引用无悬空、脚本可装载、安装后无崩溃、平台当前版本 |

建议：

1. **保持现状**即可，不用降级/卸载；加载顺序也不用动（轻插件，序号变化只影响运行时 FormID）；
2. **最终确认**（游戏内，30 秒）：找一块任务板（Mission Board），看是否刷出 DUO 的任务类型
   （空间战 / 运输 / 虫巢 / 制造 / 劫持）—— 这是"内容是否真的在跑"的唯一权威判据；
3. 若之后发现该 mod 的任务异常，先看 `Papyrus.0.log` 里是否还是上面那几行属性报错，
   那是 mod 自身问题，别往游戏版本上找；
4. `AchievementSafe=false`：用它的存档不会给成就（原版机制，与本项目无关）。

## 六、本次新增的核查工具（`tools/re/`）

| 工具 | 用途 |
| --- | --- |
| `check_esm_header.py` | 打印插件 TES4 头（签名/长度/flags/formVersion）+ HEDR + master 列表 + 顶层分组 |
| `esm_walk.py` | 全量遍历记录（自动 zlib 解压）→ TSV（sig/formID/flags/formVersion/EDID）+ 统计 |
| `fv_compare.py` / `fv_hist.py` | form version 对比 / 全文件分层采样直方图（判断"落后/超前于游戏"） |
| `ref_check.py` | **引用完整性**：master 建索引 + 目标插件全候选引用扫描，报"master 中不存在"的候选 |
| `dump_one_record.py` | 打印单条记录的原始子记录（核对某字段到底是不是 FormID） |
| `field_stats.py` | 某类记录里某子记录的取值分布（可标 `--master` 判断存在性） |

> 复用提示：这 6 个脚本对**任何**第三方 ESM/ESP/BA2 都适用，
> 以后装了新 mod 想快速判断"会不会污染原版 / 是不是老 form version / 引用是否悬空"，
> 按本文第二节、第三节的命令跑一遍即可。
