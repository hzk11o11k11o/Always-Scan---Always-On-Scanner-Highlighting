# 14 · 官方 Creations（"CC"）上传调研与提交材料包

> 日期：2026-09-19
> 用户要求：*"再打一个上传官方CC用的包，并告诉我怎么上传官方CC"*
> **结论：官方 Creations 不接受依赖 SFSE / DLL 的 mod —— 本 MOD 以现有形态无法在官方 CC 发布。**
> 产出：调研证据链（本文）+ `tools\package-cc.ps1` + `package\cc\*` +
> `dist\StarfieldAlwaysScan-CC-4.7.0.zip`（**定位 = 提交材料包**，非可上传包）。

---

## 1. 结论（三条硬约束，任何一条都足以挡下）

| # | 约束 | 依据 |
| --- | --- | --- |
| 1 | 官方 Creations 的内容形态 = **ESM 插件 + 游戏数据**（脚本 / BA2），CK 上传器不打包 DLL | §2.1 本机 CK 字符串实证；§2.3 平台现存 Creation 全部是 ESM+BA2 |
| 2 | Creations 面向 **PC + Xbox**（平台有 Xbox 过滤），Xbox 无法加载 SFSE/DLL ⇒ 依赖外部代码的 mod 不可能通过 | §2.3；平台 URL `?platforms=XB` |
| 3 | 本 MOD 功能 **95%+ 在 `SAS_AlwaysScan.dll` 内**（直接调引擎 HighlightManager / SetOutlineState）；**且不存在"纯原版版"路径** —— Papyrus 层无 outline API | §2.4（全量 `.psc` 实测） |

⇒ 去掉 DLL 后 `ESM + Papyrus` 只剩空壳（ESM = FormID 占位 + 通知 GLOB；脚本 = HUD 提示 + 老存档清理）。
**这不是打包方式问题，是平台政策与 MOD 构造的根本不兼容。**

---

## 2. 证据链

### 2.1 本机 Creation Kit 上传器（字符串实证）

从 `D:\SteamLibrary\steamapps\common\Starfield\CreationKit.exe`（**239 885 664 B**，Steam 版）
提取全部 UTF-16 字符串，上传相关文案如下（原文照抄）：

| 字符串 | 含义 |
| --- | --- |
| `Bethesda.net Login` | CK 内登录 Bethesda.net |
| `Your Bethesda.net account must be linked to your ... account in order to upload mods to Bethesda.net.` | 账号必须关联平台账号（如 Steam） |
| `Bethesda.net Legal Documents` | 首次上传要接受法律文件 |
| `Bethesda.net Mod Upload` / `Bethesda.net Upload Progress` | 上传对话框与进度窗口 |
| `No active plugin - pick an ESM to package and upload instead:` | **上传单位 = 一个 ESM 插件**（CK 自己打包） |
| `Select Data To Upload` / `Public Package Data` / `Selected Package Data` | 选择要打包进 Creation 的数据文件 |
| `Create New Creation` / `Edit Creation` / `Updating existing content on Bethesda.net...` | 新建 / 更新既有内容 |

⇒ 官方**没有"上传 zip"的流程**：一切发生在 CK 内，包由 CK 生成。
另：CK 对 DLL 没有本地拦截（没搜到相关验证字符串）——限制在**平台侧审核/政策**。

### 2.2 官方帮助文章（书面流程）

| 文章 | 内容 |
| --- | --- |
| Skyrim 版上传流程 <https://help.bethesda.net/app/answers/detail/a_id/36342> | CK → `File` → `Login to Bethesda.net` → `File` → `Upload Plugin and Archives to Bethesda.net` → `Create New Mod` → 填 **Title / Description / Category / Platform** → 自动上传 → 预览页。**与 2.1 的 Starfield CK 字符串一一对应**（同一套上传器） |
| Verified Creator 申请 <https://help.bethesda.net/app/answers/detail/a_id/63828> | 条件：作品集、Bethesda.net 账号开 MFA、关联 Xbox 账号、装最新 CK；录取后注册 Microsoft Partner Center（**付费**内容路径） |
| Modding Guidelines <https://help.bethesda.net/app/answers/detail/a_id/51731> | 只是"下架原因清单"（NSFW / 侵权 / 绕过付费 / 现实宗教政治等），**没有技术条款**，未提及 SFSE/DLL |

★ 诚实标注：`creations.bethesda.net` 与部分 help 页面是 **JS 动态站点**，静态抓取拿不到
完整条款原文；**没有取得"明文禁止脚本扩展器"的官方原句**。§1 的结论依据是
2.1~2.3 的机制证据 + 平台一贯要求（自包含、跨平台）。

### 2.3 平台现存 Creation 的形态（强旁证）

本机已订阅 / 已分析的官方 Creations（MO2 `overwrite\` 与 mods 目录）：

| Creation | 文件 |
| --- | --- |
| `sfta06` | `sfta06.esm` + `sfta06 - main.ba2` / `- textures.ba2` / `- voices_*.ba2` |
| `kinggathcreations_spaceship` | `.esm`（flags 0x81 本地化）+ `main.ba2`（文本在 BA2 的 STRINGS/） |
| DU 系列（`du_*`）、`ase*`、`above and beyond` 等 | 全部 `.esm` + ` - main.ba2` / ` - textures.ba2` |

⇒ **平台上现存的 Starfield Creation 全部是 ESM + BA2，没有一个含 DLL**；
命名规范 `<名>.esm` + `<名> - <类型>.ba2`（主/贴图/语音）。

### 2.4 "能不能做不依赖 SFSE 的版本" —— Papyrus 无 outline API（实测）

对游戏全量 Papyrus 源码（`Data\Scripts\Source\**\*.psc`）搜 `outline|highlight`（不区分大小写）：

```
命中：1 处，且为无关文件（QF_PlayerSkills_002C59E4.psc，技能片段）
```

⇒ **Papyrus 层调不到引擎的高亮（outline）系统** —— 这正是本 MOD 从 v1 起必须走 SFSE DLL 的
根本原因（`docs/03` 的既有结论再次得到验证）。因此"做个纯原版版传 CC"**目前无可行路径**。

---

## 3. 上传流程（若仍要走一遍：CK 内操作）

> 完整版（含红线与表单文案）在 `package\cc\CC-UPLOAD-GUIDE.md`，随包分发。

1. **准备**：Bethesda.net 账号（开 MFA、关联 Steam/Xbox）；确认上传资格
   （免费 Creation 的开放程度以平台当前政策为准；付费必须 Verified Creator）。
2. **文件就位**：把包内 `StarfieldAlwaysScan.esm`、`Scripts\...` 复制到
   `D:\SteamLibrary\steamapps\common\Starfield\Data\`（CK 读游戏目录，不走 MO2 虚拟目录）。
3. **CK 内**：启动 CK → `File → Login to Bethesda.net` → 接受 Legal Documents →
   加载 `Starfield.esm` + `StarfieldAlwaysScan.esm`（设为 Active File）→
   `File → Upload Plugin and Archives to Bethesda.net` → `Select Data To Upload` 只勾 `Scripts\...` →
   `Create New Creation`（Title/Description/Category=Gameplay/Platform=**仅 PC**）→ 加 `preview.jpg` → 上传。
4. **红线**：❌ 不要选 SFSE 目录/DLL（禁止外部代码，违规可致下架并影响账号）；
   ❌ 不要勾主机平台；❌ 描述里不要教玩家装 SFSE。

★ 预期结果：**因依赖外部工具（SFSE）被拒**。本流程用于"若你要试 / 要向 B 社询问"时的材料准备。

---

## 4. 产出物

| 文件 | 说明 |
| --- | --- |
| `tools/package-cc.ps1` | 一键打"提交材料包"；**刻意不含 SFSE**，并有"包内不得出现 SFSE/DLL/INI"的自检断言 |
| `package/cc/CC-UPLOAD-GUIDE.md` | 上传指南：结论（§0）→ 机制 → 步骤 → 红线 → 清单 → 备选路径 |
| `package/cc/creation-copy.txt` | CK 表单文案（英文；**如实**标注 SFSE 依赖） |
| `package/cc/preview.jpg` | 预览图素材：AI 概念图（太空站走廊 + 蓝/橙/绿/红描边物品），1200×756、252 378 B、已裁去工具水印；**上传前建议换成真实游戏截图** |
| `dist\StarfieldAlwaysScan-CC-4.7.0.zip` | **268 358 B**，SHA256 **`6BA2ACF74D4DF3A26D8BC9E0764C0FC01C186122460AA599D5D1912D01D7AC6C`**；包内 7 件：`StarfieldAlwaysScan.esm` 1 077 / `Scripts\SAS_Bridge.pex` 2 271 / `Scripts\Source\SAS\SAS_Bridge.psc` 5 254 / `preview.jpg` 252 378 / `README.txt` 13 022 / `CC-UPLOAD-GUIDE.md` 8 755 / `creation-copy.txt` 3 332 |

★ `dist/` 在 `.gitignore` 内（包可随时重建，不入库）。

---

## 5. 经验 / 教训

1. **CK 上传器的机制**（以后任何"要不要上 CC"的问题都先看这一条）：
   上传单位是 **ESM + 选中的数据**，由 CK 自动打包；没有"提交 zip"这条路。
2. **判断某个 MOD 能否上 CC 的标准**：它是否需要**任何外部代码/工具**（DLL、SFSE、启动器）。
   需要 ⇒ 不可能。ESM + 脚本 + 资产（纹理/网格/声音）⇒ 才谈得上可行。
3. **Papyrus 无 outline API**（本轮再次全量实测）—— 这条同时封死了"纯脚本版"这条路。
4. **踩坑：.NET 的 `Save('相对路径')` 用的是进程 cwd，不是 PowerShell 的 `cd` 之后的位置**
   （本轮 `preview_s.png` 一度被写到工作区根）。**凡 PowerShell 里调 .NET 文件 API，一律用绝对路径**。
5. 图片素材经 `System.Drawing` 缩放/转码：1536×1024 PNG（4.0 MB）→ 裁底部 56 px（去水印）→
   缩到 1200×756 → JPEG q88 = **252 KB**，包体积可控（总包 262 KB）。
