# 06 · Dark Universe 系列 Creation 汉化

> 对象：`du_xfire.esm` / `du_overtime.esm` / `du_outlaws_01.esm` / `du_retrograde.esm` / `du_takeover.esm`
> （Creations 平台下载，落在 MO2 的 `overwrite\`）
> 目标：**全中文**，名词与 B 社官方汉化一致。
> 日期：2026-09-17

---

## 一、结论速览

| mod | 记录数 | 可翻译串 | 唯一串 | 状态 |
| --- | --- | --- | --- | --- |
| `du_xfire.esm` | 259 | 646 | 89 | **完成并部署** |
| `du_takeover.esm` | 56 929 | 104 | 54 | **完成并部署** |
| `du_overtime.esm` | 793 | 3 017 | 554 | **完成并部署** |
| `du_retrograde.esm` | 85 323 | 5 819 | 2 417 | **完成并部署** |
| `du_outlaws_01.esm` | 4 421 | 5 027 | 3 750 | **进行中**（名称/书籍/任务目标/**全部短串**已完成；3 类长文本未译） |

五个 mod 都已写过 MO2 的 `overwrite\`（原位替换，原文件备份在 `tr/orig/`）。
`du_outlaws_01.esm` 当前是**短串完整版**：4 421 条记录里改动 2 881 条、
替换 3 753 条（74.7% 行 / 2 476 个唯一串），结构校验 `problems: 0`。
剩余 **1 274 个唯一串**全部是长文本（`QUST CNAM` 460 / `BOOK DESC` 402 /
`MESG DESC` 318）加 95 个刻意不译的占位串（71 个 `QUST FULL` 8 位十六进制
占位名、24 个纯 hull 代号如 `PX-15`）。

---

## 二、为什么不用 xEdit / xTranslator

常规做法是 xEdit 无头跑脚本导出文本再导回。本项目实测**不可行**：

- xEdit 必须先加载 `Starfield.esm`（1457 MB / 382 万条记录）并建引用缓存。
  首次（无缓存）一轮要 **15~20 分钟**，而且很脆：缓存建到一半被中断就白跑。
- 而本机命令包装器会把超过约 10 分钟的命令连带进程树一起杀掉，跑不完。

所以改为**纯 Python 自研流水线**（`tools/re/`），全流程秒级：

| 工具 | 作用 |
| --- | --- |
| `trtool.py` | `extract` 从 ESM 抽出全部"玩家可见字符串"；`apply` 把译文写回 ESM（重建子记录长度、重压缩、GRUP 尺寸重算） |
| `tr_verify.py` | 结构校验：记录/子记录**签名序列**必须与原文件完全一致，仅白名单子记录的内容可改 |
| `tr_pipeline.py` | 词典驱动：`seed`（官方对照）/ `merge` / `todo` / `map` / `status` |
| `str_scan.py` | 普查某插件里"哪些 (记录, 子记录) 承载文本"，用于建立白名单 |
| `ststrings.py` | 解析 Bethesda `.strings/.dlstrings/.ilstrings` |
| `ba2list.py` | 解析 Starfield `BTDX v2` BA2（BSArch 0.9c 打不开星空的归档） |
| `enc_probe.py` | 判断插件内联字符串是 UTF-8 还是单字节代码页 |
| `gen_poi_names.py` / `gen_outlaws_names.py` / `gen_outlaws_objectives.py` | 组合式名称、任务目标的批量生成 |
| `tokstat.py` / `split_names.py` | 词元统计，用来发现"组合式命名" |
| `xedit-scripts/tr_export.pas` | 保留的 xEdit 版导出脚本（慢，仅作对照/应急） |

> 2026-09-17 追加（做 `above and beyond` / `morelore_mantislegacy` 时写的通用小工具，
> 详见 `docs/07`）：`tr_official_hits.py`（mod 串逐条查官方逐字命中）、
> `tr_term_probe.py` / `tr_zh_probe.py`（按英文/中文子串查官方对照表）、
> `tr_sid_probe.py`（按字符串 ID 取官方 en/zh）、
> `tr_mkbatch.py`（按抽取文件行号生成词典批次，省掉手抄长英文段落的风险）。

### 写回 ESM 的三个硬坑（都已踩过并修好）

1. **子记录长度字段**：替换文本后必须重写子记录头里的 `u16 size`，否则记录
   解析立刻错位（症状：记录在改动处被"截断"）。第一版就栽在这。
2. **换行/制表符转义**：TSV 里 `\n` `\r` `\t` `\\` 都要转义，否则一条记录会被
   拆成多行。
3. **压缩记录**：带 `FLAG_COMPRESSED` 的记录写回时要 `zlib` 重新压缩并更新
   "解压后长度"字段。

### 编码判定（关键）

- 游戏自带的 `Starfield - Localization.ba2` 里 `strings/starfield_zhhans.*` 是
  **UTF-8**（直接 UTF-8 解码出正常中文）⇒ 引擎的字符串池是 UTF-8。
- 反过来 `du_*` 插件里的内联字符串是**单字节**的（如 `Siobh\xe1n` =
  Latin-1 的 á，不是 UTF-8 的 `\xc3\xa1`）⇒ Creation Kit 写盘用的是系统代码页。
- **本汉化统一写 UTF-8**（不带 BOM）：引擎按 UTF-8 读，中文才能显示。
  ⚠️ 这一点**只在游戏里才能最终验证**，是本方案唯一的未知项。
  （若实测乱码，改动点只有一处：`trtool.py` 的 `encode_text()`。）

---

## 三、名词统一的做法

关键一步：**从游戏本体抽出官方英中对照表**当词典。

```
tools/re/ba2list.py "<Starfield>/Data/Starfield - Localization.ba2" --grep zhhans --extract tr/loc
tools/re/ba2list.py "<Starfield>/Data/Starfield - Localization.ba2" --grep _en    --extract tr/loc
tools/re/ststrings.py dict tr/loc/strings/starfield_en.strings \
                              tr/loc/strings/starfield_zhhans.strings \
                              tr/loc/strings/starfield_en.dlstrings \
                              tr/loc/strings/starfield_zhhans.dlstrings \
                              tr/loc/strings/starfield_en.ilstrings \
                              tr/loc/strings/starfield_zhhans.ilstrings \
                       -o tr/loc/starfield_en_zh.tsv      # 184 732 对
```

- 与 mod 串**完全一致**的，直接采用官方译文（seed 命中 102 条）。
- 其余**人工翻译时一律查表对齐**。已确认的官方术语（节选，**与常见误译不同**）：

| 英文 | 官方中文 | 备注 |
| --- | --- | --- |
| Crimson Fleet | **深红舰队** | 不是"猩红舰队" |
| United Colonies / UC | 联合殖民地 / 联殖 | 简称"联殖" |
| Freestar Collective / Freestar | 自由星系联盟 / 星联 | |
| Ecliptic | 黄道佣兵团 | |
| Spacer | 太空劫匪 | |
| Va'ruun | 瓦鲁 | 不是"瓦鲁恩" |
| Trade Authority | 贸易管理局 | |
| GalBank | 银河银行 | |
| Trackers Alliance | 追踪者联盟 | |
| Constellation | 星宿座 | |
| Chunks | 块餐 | |
| Xenowarfare | 异兽作战部 | |
| Heatleech | 热蛭 | |
| Terrormorph | **骇变兽** | ⚠️ 2026-09-17 更正：此前本条记的“惊惧兽”是错的，官方串是骇变兽（见 `docs/07` 第三节） |
| LIST | 独立盟 | |
| The Clinic / The Den / Neon / Cydonia / Akila | 星际诊所号 / 巢穴站 / 霓虹城 / 赛多尼亚 / 阿基拉城 | |
| SysDef | 联殖防务 | |

---

## 四、各 mod 的文本形态与处理

### 4.1 可翻译子记录白名单

由 `str_scan.py` 普查后确定，写在 `trtool.py` 的 `WHITELIST`：

```
*    -> FULL                     （所有记录类型的显示名）
BOOK -> DESC                     （正文）
MESG -> DESC, ITXT               （正文、菜单按钮）
COBJ -> DESC                     （制作说明）
ARMO -> DESC                     （护甲说明）
QUST -> CNAM, NAM1, NAM2, NNAM, QMDP, QMDT, QMSU
FACT -> MNAM, FNAM
```

**刻意排除**（很重要）：
- `EDID` / `MODL` / `BFCB` / `VMAD` / `FLTR` / `ALID` … 内部标识与路径。
- **`BOOK` 的 `ENAM`**：在 `du_overtime` 里是正文
  （`[Data Slate #A-471 | Secure Playback]`），但在 `du_outlaws_01` 里是
  **8 位十六进制 ID**。翻译它会改掉真实标识符 ⇒ 一律不动。
- 纯数字/纯代号串（`QUST QMDP` 的 `<Alias=...>`、`GBFM FULL` 的 `PX-15`、
  `QUST FULL` 里的 8 位 hex 占位名）——不译。

### 4.2 组合式命名（省掉上千条重复翻译）

- `du_retrograde`：NPC 名 = `<帮派> <职能>`，79 个前缀 × 365 个后缀覆盖
  2 463 条 NPC 名（`tokens_du_prefix.tsv` + `tokens_du_suffix.tsv` +
  `tokens_du_base.tsv`，站点名走同一套前缀表）。`rg_poi_*` 任务名由
  `gen_poi_names.py` 还原成对应站点名（`rg_poi_penpt627` → `围栏站 PT-627`）。
- `du_outlaws_01`：NPC 名 = `<名/帮派> <姓|职能>`，书籍名 =
  `<人名/主题> <文档类型>`。`tokens_out_pre/last/roles/doctype/qual/ships/places.tsv`
  组合出 **985 条**（`gen_outlaws_names.py`），任务目标用模板 + 词典组合出
  **139 条**（`gen_outlaws_objectives.py`）。

### 4.3 词典文件（手工成果，务必保留）

```
tr/lang/dict.tsv              # 合并后的总词典（en -> zh），3 300+ 条
tr/lang/official_seed.tsv     # 官方对照自动命中的部分
tr/lang/batches/*.tsv          # 一批批人工翻译（可直接追加新批次）
tr/lang/tokens_*.tsv           # 组合式命名的词元表
tr/lang/todo/                  # 各 mod 的"还没译"清单
```

`du_outlaws_01` 的短串批次（新增）：

| 批次 | 内容 | 条数 |
| --- | --- | --- |
| `b33_outlaws_quest.tsv` | `QUST FULL` 任务名 | 441 |
| `b34_outlaws_msg_itxt.tsv` | `MESG ITXT` 讯息对话选项 | 142 |
| `b35_outlaws_acti.tsv` | `ACTI FULL` 世界物件名 | 253 |
| `b36_outlaws_armo.tsv` | `ARMO FULL` 护甲名 | 99 |
| `b37_outlaws_ships.tsv` | `GBFM FULL` 飞船显示名（纯 hull 代号保持原样） | 67 |
| `b38_outlaws_misc.tsv` | 零散短串（开发占位、短提示） | 2 |

`dict.tsv` 是 `merge(tr/lang/batches/*.tsv, tr/lang/official_seed.tsv)` 的产物，
**新增翻译只要往 `tr/lang/batches/` 加文件**，然后重跑 merge 即可。

---

## 五、构建 / 部署一条龙

```powershell
$W = 'D:\workspace\starfield mod\always scan'          # 工作区
$O = 'D:\Mod Organizer 2\starfield_mods\overwrite'     # MO2 的 overwrite

# 1) 抽取字符串（原文件在 $O，副本在 tr\data）
python tools/re/trtool.py extract "$O\du_xfire.esm" -o tr/out/du_xfire.tsv

# 2) 合并词典 + 生成映射（组合式命名的 mod 要带词元表）
python tools/re/tr_pipeline.py merge (Get-ChildItem tr/lang/batches/*.tsv).FullName `
       tr/lang/official_seed.tsv -o tr/lang/dict.tsv
python tools/re/tr_pipeline.py map tr/out/du_xfire.tsv --dict tr/lang/dict.tsv `
       -o tr/out/du_xfire.map.tsv

# 3) 生成汉化 ESM
python tools/re/trtool.py apply "$O\du_xfire.esm" --map tr/out/du_xfire.map.tsv `
       -o tr/build/du_xfire.esm

# 4) 结构校验（必须 problems: 0）
python tools/re/tr_verify.py "$O\du_xfire.esm" tr/build/du_xfire.esm

# 5) 部署（原文件已备份在 tr\orig\）
Copy-Item tr/build/du_xfire.esm "$O\du_xfire.esm" -Force
```

> `du_retrograde` 的 `map` 要多带 `--prefix tr/lang/tokens_prefix_all.tsv
> --suffix tr/lang/tokens_du_suffix.tsv`；
> `du_outlaws_01` 要带 `--prefix tr/lang/tokens_out_pre.tsv
> --suffix tr/lang/tokens_out_last.tsv`。

---

## 六、`du_outlaws_01` 剩余工作量（未完成部分）

`du_outlaws_01` 是五个里最大的。

**已完成（截至 2026-09-17）**：**全部短串**：

| 子记录 | 内容 | 条数 |
| --- | --- | --- |
| `NPC_ FULL` | NPC 名（词元组合） | 827 |
| `BOOK FULL` | 书籍/数据板名称 | 440 |
| `QUST NNAM` | 任务目标 | 137 |
| `QUST FULL` | 任务名 | 441 |
| `ACTI FULL` | 世界物件名 | 253 |
| `MESG ITXT` | 讯息对话选项 | 142 |
| `ARMO FULL` | 护甲名 | 99 |
| `GBFM FULL` | 飞船显示名（有文字成分的 67 条） | 67 |

加上官方命中的其余条目 ⇒ **3 753 条 / 74.7% 的行、2 476 / 3 750 个唯一串**，
已构建（`problems: 0`）并部署到 `overwrite\`。

**未完成**：只剩三类长文本 + 刻意不译的占位串：

| 子记录 | 条数 | 字符数 | 内容 |
| --- | --- | --- | --- |
| `BOOK DESC` | 402 | ~916 000 | 书籍/数据板正文（长篇小说式） |
| `QUST CNAM` | 460 | ~382 000 | 任务简报 |
| `MESG DESC` | 318 | ~175 000 | 讯息正文 |
| 不译占位 | 95 | — | `QUST FULL` 的 71 个 8 位十六进制占位名 + `GBFM FULL` 的 24 个纯 hull 代号（`PX-15`…） |

继续做法（工具已就绪）：

1. `tr/lang/todo/du_outlaws_01.tsv` 是当前完整的待译清单
   （按字符数排序，短的在前），可用 `--max-chars` 分批；
2. 把译文写进新的 `tr/lang/batches/bXX_*.tsv`；
3. 重跑第五节第 2~5 步（`du_outlaws_01` 那一条）即可。

⚠️ 两个已踩过的坑：

- `tr/lang/batches/b31_outlaws_books.tsv` 里有一行 `Whispers In The Grav\n\n`
  —— 原文结尾带两个换行，键值必须保留 `\n\n` 转义；
- **键必须与原文逐字符一致**：`ARMO` 里有 3 条原文拼的是 `Gravyard`（作者笔误，
  不是 `Graveyard`），照"正确拼写"写进词典会整条不命中；
- `trtool.py extract` 的 `decode_text()` 用 `PRINTABLE`（仅 ASCII）筛串 ⇒
  **对已汉化的 ESM 再 extract，只会导出"还是英文"的串**，正好可以用来反查
  "还有哪些没译"。

---

## 七、部署状态

MO2 profile `Default`（`D:\Mod Organizer 2\starfield_mods\profiles\Default`）里
五个 `du_*.esm` **本来就都已启用**（`plugins.txt` / `loadorder.txt` 都带 `*`），
所以汉化只需要**原位替换 `overwrite\` 里的文件**，不用改 profile。

| 文件 | 位置 |
| --- | --- |
| 原始文件备份 | `tr/orig/du_*.esm` |
| 抽出的字符串表 | `tr/out/du_*.tsv`、`tr/out/du_*.map.tsv` |
| 生成的汉化 ESM | `tr/build/du_*.esm` |
| 已部署（5 个） | `D:\Mod Organizer 2\starfield_mods\overwrite\du_{xfire,takeover,overtime,retrograde,outlaws_01}.esm` |

> 2026-09-17：`du_outlaws_01.esm` 短串完整版已部署
> （`tr/build/du_outlaws_01.esm` = `overwrite\du_outlaws_01.esm`，5 641 279 B，
> MD5 `3ACBB69E0A5BC5AE932305E12E527D51`；反查确认新译文已写入）。

### 游戏内验证清单

1. **文字是否正常显示**（最关键）：进游戏看任意一个 du 任务名/物品名。
   - 正常中文 ⇒ 编码判断正确，继续。
   - 乱码/方块 ⇒ 引擎按代码页读内联字符串，需要改成 `.strings` 本地化插件方案
     （见第八节）。
2. 任务日志里 `du_overtime` 的「维修：<Alias=PrimaryRef> at ...」应显示中文，
   且 `<Alias=...>` 占位符**原样**（被正确替换成实际地点名）。
3. 老存档可读、不崩；`du_retrograde` 的据点/船只名应是中文。
4. 若出现"某个 mod 加载失败" ⇒ 立刻用 `tr/orig/` 里的原件覆盖回去。

---

## 八、如果 UTF-8 内联字符串行不通（备用方案）

引擎对**未标本地化**插件的内联字符串若按代码页解释，中文无法内联。此时改走
游戏原生机制：

1. 把插件头 flags 置上 `0x80`（Localized）；
2. 每个可翻译子记录的内容替换为 4 字节字符串 ID（`.strings` / `.dlstrings` /
   `.ilstrings` 分别对应不同类型的字段）；
3. 同时提供 `Data\Strings\<插件名>_zhhans.strings`（UTF-8）与 `_en.strings`
   （原文），由 MO2 部署。

`trtool.py` 已经有完整的解析/重建能力，改造量集中在"字段字符串类型 → 用哪个
strings 文件"这一张表上。
