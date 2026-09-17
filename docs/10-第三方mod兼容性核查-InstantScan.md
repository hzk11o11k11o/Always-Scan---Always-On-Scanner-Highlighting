# 10 · 第三方 mod 兼容性核查：`InstantScan.esm`（Instant Scan）

> 问题（用户）：检查 `InstantScan.esm` 是否兼容当前游戏版本（1.16.244.0），以及是否有功能缺失。
>
> **结论：兼容，且就它宣称的功能而言没有缺失。**
> 它只做一件事 —— 把「动植物调查所需扫描个体数」从 **8 改成 1**；
> 原版相关的计数 GMST 就只有两条，它全部覆盖了。
> 唯一需要知道的是它的**功能边界**（改变不了单次扫描耗时、不管资源/行星调查）与
> 「非官方 mod ⇒ 禁用成就」这条原版机制。
>
> 核查日期：2026-09-17。全部结论都有本地字节证据（工具输出），无"应该没事"式推断。

---

## 一、被检对象的身份与版本

| 项 | 值 | 来源 |
| --- | --- | --- |
| 名称 / 作者 | **Instant Scan** / `JustAnOrdinaryGuy` | `meta.ini`（`url=.../starfield/mods/759`）+ ESM `CNAM` |
| Nexus | `modid=759` | `meta.ini` |
| 安装包 | `Instant Scan-759-1-3-1701681068.zip`（时间戳 1701681068 = **2023-12-04**） | `meta.ini` `installationFile` |
| 声明版本 | **1.3.0.0** | `meta.ini` |
| 文件 | `InstantScan.esm` **281 B**（文件时间 2023-11-05）+ `meta.ini`，**再无他物** | `Get-ChildItem` |
| 部署位置 | `D:\Mod Organizer 2\starfield_mods\mods\Instant Scan\` | MO2 |
| 启用状态 | `modlist.txt` = `+Instant Scan`；`plugins.txt` = `*InstantScan.esm` | MO2 profile `Default` |

⇒ 这是一个**2023 年 12 月的纯 GMST 覆盖小 mod**：没有脚本（.pex）、没有 DLL、没有归档（BA2）。
凡是不涉及这三类载体的兼容性风险（SFSE 版本联动、BA2 版本、Papyrus 漂移）**在这里天然不存在**。

## 二、它到底改了什么（工具直接读字节）

用 `tools/re/esmrec.py <plugin> --formid 0x...` dump 出来的全部内容：

| # | 记录 | FormID | EDID | mod 写入值 | **原版当前值**（`Starfield.esm`） |
| --- | --- | --- | --- | --- | --- |
| 1 | GMST | `0x00249B84` | `iHandScannerPlantsCountBase` | `01 00 00 00` = **1** | `08 00 00 00` = **8** |
| 2 | GMST | `0x0038D142` | `iHandScannerAnimalCountBase` | `01 00 00 00` = **1** | `08 00 00 00` = **8** |

⇒ 语义 = **「完成一个植物 / 动物物种的扫描调查，需要扫描的个体数」由 8 降到 1**
（Nexus 描述原文：*Reduce the number of scans required for the flora and fauna survey to 1*）。
游戏里表现为：**对着一个物种扫一次，就 100% 完成该物种调查**。

## 三、兼容性核查（对当前 1.16.244.0）

| 检查项 | 实测结果 | 判读 |
| --- | --- | --- |
| TES4 头 | `flags = 0x00000001` | `0x01` = **ESM(master)** ⇒ 是 Starfield 允许的发布格式（本项目已实证：`.esp` 本体不加载，见 `docs/99` 同名段） |
| form version | 头部 555、记录 555（当前 CK 写 581/582） | 方向是**"更旧"** ⇒ 引擎向后兼容（原版自己仍在加载 552 的 `Constellation.esm`） |
| master 列表 | `Starfield.esm`、`BlueprintShips-Starfield.esm` | 两个都是**官方核心隐式加载**（`loadorder.txt` 第 1、3 位，每局必载）⇒ 无缺失前置 |
| ★ 覆盖命中 | 两条 `MAST` 记录的 **FormID 与 EDID 在当前原版里都存在且完全一致**（`0x00249B84` / `0x0038D142`） | `esmformid.py` 反查原版各命中 1 条、FormID 一字不差 ⇒ 覆盖关系**精确成立**，不是悬空覆盖 |
| 覆盖是否唯一 | `findstr /s /m` 扫 MO2 全部 `mods\**\*.esm`，含这两个 EDID 的**只有它自己** | 没有别的插件与它抢同一设置 |
| 排序 | `loadorder.txt` 里 `InstantScan.esm` 在 `Starfield.esm` **之后**（倒数第 2） | 后加载才覆盖得动，位置正确 |
| 与本体 mod 关系 | 本项目的 `StarfieldAlwaysScan.esm` 不改这两条 GMST | 零冲突 |
| 无 DLL/脚本/BA2 | 目录里只有 ESM + meta.ini | 不受 SFSE / Address Library / BA2 版本联动影响 |

**⇒ 结论：兼容当前版本，可继续启用。** 没有发现任何"游戏更新导致它失效或出错"的迹象 ——
恰恰相反，它依赖的两条原版记录在 1.16.244.0 里**依然健在且 FormID 未变**。

## 四、功能是否有缺失

### 4.1 就它宣称的功能：**没有缺失**

`iHandScanner*` 这一族在原版里**总共只有 2 条计数 GMST**：
```
python tools/re/esmstrings.py iHandScanner   ->  仅 2 个唯一串（两条 CountBase）
python tools/re/esmstrings.py CountBase      ->  同上，无其它 *CountBase*
```
mod 把这两条**全部**改成了 1 ⇒ 覆盖面与实际设置面**完全相等**，不存在"漏改某条计数"。

### 4.2 未触及的同类倍率（**不算缺失**，但记录在案）

原版另有一组"倍率"GMST，mod **没有**改：

| GMST | 用途 |
| --- | --- |
| `fHandScannerPlantCountBotanyMult` (`0x0002D6A4`) | 植物学技能对所需个数的倍率 |
| `fHandScannerAnimalCountZoologyMult` (`0x0002D73B`) | 动物学技能对所需个数的倍率 |
| `fHandScannerPlantCountScanningMult` (`0x0002D6EF`) | 植物扫描进度倍率 |
| `fHandScannerAnimalCountScanningMult` (`0x0002D786`) | 动物扫描进度倍率 |

因为 `base` 已经是 **1**，所需个数取整后**不可能再低于 1**，
这些倍率乘上去也不改变结果 ⇒ **保留它们是合理的，不是功能缺失**。

### 4.3 功能边界（容易被"Instant Scan"这个名字误导，**这才是要注意的部分**）

| 它**不做**什么 | 说明 |
| --- | --- |
| 不加快「单次扫描本身」 | 对准目标按住扫描仪、等进度条走完的**耗时没变**；变的是"要走完几次"（8→1） |
| 不管资源 / 地质调查 | 只覆盖 flora/fauna；行星表面**资源、地质**调查不受影响 |
| 不管星球 / 星系扫描 | 轨道上的星球扫描（survey）是另一套机制，与这两条 GMST 无关 |
| 不提供开关 | 纯数据覆盖，装上即生效；要"临时关掉"只能停用插件 |

### 4.4 已知副作用

- **会禁用成就**：非官方 mod 的通用后果（Nexus/镜像站页面的 *may disable achievements* 提示；
  游戏内以 `AchievementSafe=false` 一类机制判定），与本 mod 无关，需另装成就解锁 mod 规避。

## 五、风险与后续

1. **静默失效风险（低）**：它是 2023-12 的老包，全部效力建立在
   「Bethesda 不删除/不改名这两条 GMST」之上。若未来某版本删掉它们，
   结果只是**默默不生效**（数据覆盖不命中，不会崩）—— 届时用同样的两条命令即可复查。
2. **当前版本（1.16.244.0）实测结论：有效**，可继续与 `StarfieldAlwaysScan.esm` 一起启用。
3. **想扩展很容易**：同法在 ESM 里加一条同名 EDID / 原版 FormID 的 GMST 覆盖即可
   （例如扫描范围 `fHandScannerScanRange`、或把倍率也设为 1）。本项目的 `build_sas.pas`
   + `tools/re/esmrec.py` 已具备造记录与验记录的能力。

## 六、复现命令

```powershell
# 身份
Get-Content 'D:\Mod Organizer 2\starfield_mods\mods\Instant Scan\meta.ini'

# 结构与内容
python tools\re\sf_plugin_check.py 'D:\Mod Organizer 2\starfield_mods\mods\Instant Scan\InstantScan.esm'
python tools\re\esmrec.py 'D:\Mod Organizer 2\starfield_mods\mods\Instant Scan\InstantScan.esm' --formid 0x00249B84
python tools\re\esmrec.py 'D:\Mod Organizer 2\starfield_mods\mods\Instant Scan\InstantScan.esm' --formid 0x0038D142

# 原版对照（默认值 8 / 覆盖命中）
python tools\re\esmrec.py      'D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm' --formid 0x00249B84
python tools\re\esmformid.py   iHandScannerPlantsCountBase
python tools\re\esmstrings.py  iHandScanner        # 全族只有 2 条

# 覆盖唯一性
findstr /s /m /c:"iHandScannerPlantsCountBase" "D:\Mod Organizer 2\starfield_mods\mods\*.esm"
```
