# 上传到官方 Creations（Creation Club / "CC"）—— 结论与操作指南

> 适用：Starfield Always Scan（SFSE）v4.7.0
> 结论日期：2026-09-19
> 相关脚本：`tools\package-cc.ps1`  →  产物 `dist\StarfieldAlwaysScan-CC-<version>.zip`

---

## 0. 先说结论（**务必先读**）

**官方 Creations 平台不接受依赖 SFSE / DLL 的 mod，本 MOD 以现有形态无法在官方 CC 发布。**

原因（三条，任何一条都足以挡下）：

1. **平台形态**：官方 Creations 的内容只能是 **ESM 插件 + 游戏数据**（脚本、BA2 归档等），
   官方 CK 的上传器只打包「插件 + 选中数据」；平台上现存的 Starfield Creations
   （含你已订阅的 `du_*`、`kinggathcreations_spaceship`、`sfta06` 等）**全部是 ESM + BA2，没有一个含 DLL**。
2. **跨平台要求**：Creations 面向 PC + Xbox（`creations.bethesda.net` 有 Xbox 平台过滤）。
   Xbox 无法加载 SFSE/DLL ⇒ 依赖外部代码的 Creation 不可能通过。
3. **本 MOD 的构造**：本 MOD 的功能 **95% 以上在 `SAS_AlwaysScan.dll` 里**（它直接调用引擎内部的
   HighlightManager / SetOutlineState 函数）。DLL 一旦去掉，`ESM + Papyrus` 只剩一个空壳
   （ESM 里是 FormID 占位记录 + 一个通知用 GLOB；脚本只负责 HUD 提示与老存档清理）。
   并且**不存在"纯原版复刻"路径**：游戏 Papyrus 脚本层没有描边（outline）API
   —— 实测全量游戏 `.psc` 源码，`outline / highlight` 关键字命中 0 处，
   调不到引擎的高亮管理器，这是本 MOD 必须用 SFSE 的根本原因（v1 起已验证）。

⇒ **本包（`StarfieldAlwaysScan-CC-*.zip`）的定位 = "提交材料包"**：
把 CK 上传流程里会用到的文件与文案预先备好。若你仍想试验提交（例如想向
Bethesda 确认政策、或将来政策松动），照下面步骤即可；**日常发布请继续使用 Nexus 包**
（`dist\StarfieldAlwaysScan-4.7.0.zip`）。

> ★ 诚实提醒：把"没有 DLL 的壳"提交上去，即使侥幸通过，玩家装了也**不会有任何功能**。
> 本 MOD 与官方 CC **在当前平台政策下本质不兼容**，这不是打包方式的问题。

---

## 1. 官方上传通道是什么样（已核实的机制）

官方 Creations **没有"上传一个 zip"这样的流程**。上传完全发生在
**Creation Kit** 内（你机器上已装：`D:\SteamLibrary\steamapps\common\Starfield\CreationKit.exe`）：

| 环节 | 说明 | 证据 |
| --- | --- | --- |
| 登录 | CK 内登录 **Bethesda.net** 账号 | CK 界面字符串 `Bethesda.net Login`（本机 `CreationKit.exe` 提取） |
| 账号关联 | Bethesda.net 账号须**关联平台账号**（如 Steam） | `Your Bethesda.net account must be linked to your ... account in order to upload mods to Bethesda.net` |
| 法律文件 | 首次上传前要接受 `Bethesda.net Legal Documents` | 同上 |
| 打包单位 | **一个 ESM 插件 + 选中的数据文件**（CK 自动打包上传） | `No active plugin - pick an ESM to package and upload instead:`、`Select Data To Upload`、`Public Package Data` |
| 新建/更新 | 新建走 `Create New Creation`；后续更新走 `Edit Creation` / `Updating existing content on Bethesda.net` | 同上 |
| 上传 | 进度条窗口 `Bethesda.net Upload Progress` | 同上 |
| 元数据 | 标题 / 描述 / 分类 / 平台 + 预览媒体（CK 可 `Add Image File`） | CK 字符串 + B 社官方帮助文章（见下） |

官方帮助文章（同一套流程的书面版，以 Skyrim 版为例，Starfield CK 沿用同一上传器）：
<https://help.bethesda.net/app/answers/detail/a_id/36342>
（步骤原文：CK → `File` → `Login to Bethesda.net` → `File` → `Upload Plugin and Archives to Bethesda.net`
→ `Create New Mod` → 填 Title / Description / Category / Platform → 自动上传 → 完成为预览页。）

**上传资格**：免费 Creation 的上传权限以平台当前政策为准（Bethesda 曾分阶段开放；
付费内容必须加入 **Verified Creator Program**：需作品集、Bethesda.net 账号开 MFA、关联 Xbox 账号，
入口 <https://creations.bethesda.net/en/creators/bethesdagamestudios>）。

---

## 2. 上传前准备（如果仍要试）

1. **Bethesda.net 账号**：开启 MFA、关联 Steam/Xbox 账号。
2. **确认上传资格**：登录 <https://creations.bethesda.net> 看是否能进 Creator 页；
   若要求申请，按 §1 末的入口提交。
3. **文件就位**：把本包内容按 Data 布局放到能被 CK 看到的位置
   （CK 读的是游戏目录的 `Data\`，**不是 MO2 的虚拟目录**——因为 CK 不在 MO2 里跑）：
   ```
   复制到  D:\SteamLibrary\steamapps\common\Starfield\Data\
     StarfieldAlwaysScan.esm
     Scripts\SAS_Bridge.pex
     Scripts\Source\SAS\SAS_Bridge.psc
   ```
   （`SFSE\` 目录**不要复制也不要上传**，见 §5 红线。）
4. **预览图**：`preview.jpg`（本包自带，为 AI 生成的概念图；**建议上传前换成真实游戏内截图**，
   截图内容建议：星球表面/室内，物品上有蓝色、容器橙色描边，热键提示 `Always Scan: ON` 在 HUD 上）。

---

## 3. CK 内上传步骤（照做即可）

1. 启动 **Creation Kit**（Steam 库 → Starfield 工具）。
2. 菜单 **File → Login to Bethesda.net**，用 Bethesda.net 账号登录（弹窗标题 `Bethesda.net Login`）。
   首次登录会要求接受 `Bethesda.net Legal Documents`。
3. 加载插件：**File → Data**，勾 `Starfield.esm` 与 `StarfieldAlwaysScan.esm`，
   把 `StarfieldAlwaysScan.esm` 设为 **Active File**，确认加载。
   （若你不加载任何插件直接上传，CK 会提示 `No active plugin - pick an ESM to package and upload instead:`，
   也可以在那里直接选 ESM。）
4. 菜单 **File → Upload Plugin and Archives to Bethesda.net**
   （对话框标题 `Bethesda.net Mod Upload`；不同 CK 版本菜单措辞可能略有差异，寻找 Bethesda.net 字样）。
5. **Select Data To Upload**：只勾选本 MOD 的数据文件（`Scripts\...`）。**不要**勾任何与 SFSE 相关的内容。
6. **Create New Creation**，填写四个字段（文案见同目录 `creation-copy.txt`）：
   - **Title**：`Always Scan (SFSE build - submission package)`
   - **Description**：把 `creation-copy.txt` 里的描述整段粘贴
   - **Category**：`Gameplay`（若可多选：`Items and Objects`）
   - **Platform**：`PC`（**不要**勾 Xbox/主机 —— 本 MOD 主机上不可能运行，勾了只会更难通过）
   - 媒体：`Add Image File` 添加 `preview.jpg`
7. 点上传，等 `Bethesda.net Upload Progress` 走完；完成后 CK 会给出预览页链接。
8. 之后在 <https://creations.bethesda.net> 你的资料页查看状态；修改用 **Edit Creation** 重传。

> ★ 提交后进入审核。**预期结果：因"依赖外部工具（SFSE）"被拒**。
> 本指南的目的不是绕过这一点（也做不到），而是让"如果你要试/要问"时不至于连材料都没准备。

---

## 4. 表单文案

见同目录 **`creation-copy.txt`**（英文纯文本，可直接复制粘贴）。

---

## 5. 红线（**不要做**）

- ❌ **不要**把 `SFSE\Plugins\*.dll` / `SAS_AlwaysScan.ini` 选进 `Select Data To Upload`。
  平台禁止外部代码；违规则可能被**下架并影响账号**。这也是本包默认**不含任何 SFSE 文件**的原因。
- ❌ **不要**在描述里教玩家"装 SFSE 才能用"——描述须自包含。
- ❌ **不要**勾主机平台。

---

## 6. 本包内容清单

| 文件 | 用途 |
| --- | --- |
| `StarfieldAlwaysScan.esm` | CK 上传的插件主体（1 077 B） |
| `Scripts\SAS_Bridge.pex` | Papyrus 编译产物（Creations 允许的资产） |
| `Scripts\Source\SAS\SAS_Bridge.psc` | 同名源码（随包提供，供审核查阅） |
| `preview.jpg` | 预览图素材（AI 概念图，上传前建议换成真实截图） |
| `README.txt` | 英文说明（Nexus 包复用版；含 SFSE 依赖说明） |
| `CC-UPLOAD-GUIDE.md` | 本文件 |
| `creation-copy.txt` | 表单文案 |

---

## 7. 备选路径（推荐）

| 方案 | 说明 |
| --- | --- |
| **Nexus Mods（现行渠道）** | `dist\StarfieldAlwaysScan-4.7.0.zip`，发布文案在 `package\nexus-description.md`。这是 SFSE 类 MOD 的**唯一合规分发渠道**。 |
| 做"纯原版版"上传 CC | **目前无可行路径**：Papyrus 层无 outline API（§0 第 3 条）。若未来 B 社开放相关脚本 API，再评估。 |
| 与 B 社确认政策 | 若你想正式问一次，可用 `creation-copy.txt` 的描述 + 本指南 §0 的三条理由发给 Bethesda 支持（<https://help.bethesda.net>）。 |
