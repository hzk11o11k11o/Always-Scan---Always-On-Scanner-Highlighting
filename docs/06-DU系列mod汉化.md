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
| `du_outlaws_01.esm` | 4 421 | 5 027 | 3 750 | **完成**（短串 + 三类长文本 `MESG DESC` / `BOOK DESC` / `QUST CNAM` 全部译完；只剩 95 个刻意不译的占位串） |

五个 mod 都已写过 MO2 的 `overwrite\`（原位替换，原文件备份在 `tr/orig/`）。
`du_outlaws_01.esm` 当前是**全量汉化完整版**（2026-09-17 完成 `QUST CNAM`）：4 421 条记录里
改动 3 154 条、替换 **4 932** 条（98.1% 行），结构校验 `problems: 0`。
剩余 **95 行**全部是刻意不译的串：71 个 `QUST FULL` 8 位十六进制占位名
加 24 个纯 hull 代号（`PX-15`…）。

> **术语更正（2026-09-17）**：本文档早期把 `GalBank` 记作「银河银行」、`SysDef` 记作
> 「联殖防务」，**都与官方串不符** —— 以 `tr/loc/starfield_en_zh.tsv` 为准，
> 官方分别是 **`GalBank` = 盖尔银行**（`GalBank Statement` → 盖尔银行声明）、
> **`SysDef` = 星防队**（`UC SysDef Mission Board` → 联殖星防队任务板）。
> 已把 `tr/lang/batches/*.tsv` 里的两种旧译**全量替换**（60 个批次文件），
> 并重建 + 重新部署了**三个受影响的 mod**（`du_outlaws_01` / `du_overtime` / `du_retrograde`，
> 明细与体积逐字节核对见第七节）。

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
| `tr_slice.py` | 按 `(recsig, subsig)` 从抽取文件里切出一类长文本，并**保留抽取文件行号**（长文本分批的唯一入口） |
| `tr_plan.py` | 把切出的长文本按字符数切成 N 批，输出 `row_from`/`row_to`（可直接喂 `read_file` 取正文） |
| `tr_check_specs.py` | **spec 行号防御性校验**：逐条核对行号确实属于本类 `(recsig, subsig)`，防「抄错行号静默错配」 |
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
| GalBank | **盖尔银行** | 2026-09-17 更正：早期误记作「银河银行」 |
| Trackers Alliance | 追踪者联盟 | |
| Constellation | 星宿座 | |
| Chunks | 块餐 | |
| Xenowarfare | 异兽作战部 | |
| Heatleech | 热蛭 | |
| Terrormorph | **骇变兽** | ⚠️ 2026-09-17 更正：此前本条记的“惊惧兽”是错的，官方串是骇变兽（见 `docs/07` 第三节） |
| LIST | 独立盟 | |
| The Clinic / The Den / Neon / Cydonia / Akila | 星际诊所号 / 巢穴站 / 霓虹城 / 赛多尼亚 / 阿基拉城 | |
| SysDef | **星防队**（`UC SysDef` = 联殖星防队） | 2026-09-17 更正：早期误记作「联殖防务」 |

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
| `b50`~`b70_outlaws_msg_desc_*.tsv` | `MESG DESC` 讯息正文（21 批，按字符数由短到长切分） | 318 |
| `b71`~`b150_outlaws_book_desc_*.tsv` | `BOOK DESC` 书籍/数据板正文（80 批，约 1 万字符/批、由短到长切分，`book_desc_01`~`80`） | 403 |

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

**全部完成（截至 2026-09-17）**：短串 + 三类长文本（`MESG DESC` / `BOOK DESC` / `QUST CNAM`）：

| 子记录 | 条数 | 字符数 | 内容 |
| --- | --- | --- | --- |
| ~~`QUST CNAM`~~ | ~~460~~ | ~~~378 869~~ | ✅ 已完成（批次 `b151`~`b1620`，**24 批 462 条** ~378 869 字符，2026-09-17） |
| ~~`MESG DESC`~~ | ~~318~~ | ~~~175 000~~ | ✅ 已完成（批次 `b50`~`b70`，318 条 ~175 000 字符） |
| ~~`BOOK DESC`~~ | ~~401~~ | ~~~916 000~~ | ✅ 已完成（批次 `b71`~`b150`，80 批 **403** 条 ~916 000 字符） |
| 不译占位 | 95 | — | `QUST FULL` 的 71 个 8 位十六进制占位名 + `GBFM FULL` 的 24 个纯 hull 代号（`PX-15`…） |

继续做法（工具已就绪）：

> **长文本分批的标准流程**（`MESG DESC` 就是照这个跑完的）：
>
> ```powershell
> # 1) 切出一类长文本，带抽取文件行号，按字符数由短到长排序
> python tools/re/tr_slice.py tr/out/du_outlaws_01.tsv --sig MESG --sub DESC `
>        -o tr/lang/todo/outlaws/MESG_DESC.tsv
>
> # 2) 每批写一个 spec（<抽取文件行号>\t<译文>），由它生成词典批次
> #    行号来自第 1 步的第 1 列，英文原文由工具回填 ⇒ 不用手抄长段落、也不可能抄错键
> python tools/re/tr_mkbatch.py tr/out/du_outlaws_01.tsv `
>        tr/lang/spec/mesg_desc_01.txt tr/lang/batches/b50_outlaws_msg_desc_01.tsv
> ```
>
> 要点：**一篇长文一个批次**（第 21 批之后按约 1 万字符切）；译文里的换行要写成
> `\n`（`apply` 会 `unesc`），键由 `tr_mkbatch.py` 从抽取文件里逐字符取，**不要手抄**。

`QUST CNAM` 的实际做法（**新增两个通用小工具**：`tools/re/tr_plan.py` 按字符数分批、
`tools/re/tr_check_specs.py` 校验 spec 行号类别）：

1. `python tools/re/tr_slice.py tr/out/du_outlaws_01.tsv --sig QUST --sub CNAM -o tr/lang/todo/outlaws/QUST_CNAM.tsv`
   ⇒ **462 行 / 378 869 字符**（去重后的唯一串）；
2. `python tools/re/tr_plan.py tr/lang/todo/outlaws/QUST_CNAM.tsv --target 16000 -o tr/lang/todo/outlaws/QUST_CNAM.plan.tsv`
   ⇒ 按约 16 000 字符/批切成 **24 批**（计划里的 `row_from`/`row_to` 可直接喂 `read_file`）；
3. 每批写一个 spec `tr/lang/spec/qust_cnam_NN.txt`（`<抽取文件行号>\t<译文>`，
   换行写成 `\n`），再用 `tr_mkbatch.py` 生成批次 `b151`~`b1620_outlaws_qust_cnam_NN.tsv`；
4. **每批跑完后执行防御性校验**（把第七节那条坑自动化）：
   ```
   python tools/re/tr_check_specs.py tr/out/du_outlaws_01.tsv QUST CNAM ^
          (Get-ChildItem tr/lang/spec/qust_cnam_*.txt).FullName
   ```
   ⇒ 必须 `bad=0`，全部写完时 `checked lines=462 unique=462`；
5. 重跑第五节第 2~5 步（`du_outlaws_01` 那一条）。

**结果**：`dict.tsv` 4 291 → **4 751** 条；`map` 5 027 行 → 译出 **4 932**（98.1%），
未译 **95**（全部是刻意不译的占位串）；`apply`（从 `tr/orig` 整份重建）替换 4 932 条、
未应用 0；`tr_verify.py` = `problems: 0`；对构建产物再 `extract` 只剩 **95 条**、
**`QUST CNAM` 0 条残留**。

⚠️ 三个已踩过的坑：

- `tr/lang/batches/b31_outlaws_books.tsv` 里有一行 `Whispers In The Grav\n\n`
  —— 原文结尾带两个换行，键值必须保留 `\n\n` 转义；
- **键必须与原文逐字符一致**：`ARMO` 里有 3 条原文拼的是 `Gravyard`（作者笔误，
  不是 `Graveyard`），照"正确拼写"写进词典会整条不命中；
- `trtool.py extract` 的 `decode_text()` 用 `PRINTABLE`（仅 ASCII）筛串 ⇒
  **对已汉化的 ESM 再 extract，只会导出"还是英文"的串**，正好可以用来反查
  "还有哪些没译"。
- **★ `tr_mkbatch.py` 只按行号取键，抄错行号不会报错，只会静默错配** —— 做 `BOOK DESC`
  时 `book_desc_11.txt` 里把 `1258` 误抄成 `1207`，结果是：一条 `BOOK FULL`
  （`Mutineer Division Travel Records`）被替换成了那篇长文的译文，而该长文保持英文。
  **防御性做法（每次跑完都要做）**：遍历所有 spec 行号，核对抽取文件里该行的
  `(recsig, subsig)` 与本类是否一致，例如：
  ```powershell
  python -c "import io,glob;ls=io.open('tr/out/du_outlaws_01.tsv',encoding='utf-8').read().splitlines();bad=[];[bad.append((f,l)) for f in glob.glob('tr/lang/spec/book_desc_*.txt') for l in io.open(f,encoding='utf-8').read().splitlines() if l and not l.startswith('#') and ls[int(l.split(chr(9))[0])-1].split(chr(9))[2]!='DESC'];print(bad)"
  ```
  另：**一旦发现误配，必须从 `tr/orig/` 的原始文件整份重建**（而不是继续往已汉化的
  `overwrite\` 上增量 `apply`）——否则那条被错写的记录里已经没有英文键，`apply` 再也
  匹配不上，错误会永久留在部署文件里。

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

> 2026-09-17（最新）：`du_outlaws_01.esm` **全量汉化版**（短串 + `MESG DESC` + `BOOK DESC`
> + `QUST CNAM`）已部署 —— `tr/build/du_outlaws_01.esm` = `overwrite\du_outlaws_01.esm`，
> **5 563 835 B**，MD5 **`74FB0E81B2FB9E2AB52DD12A1F184E5E`**；从 `tr/orig/du_outlaws_01.esm`
> 整份重建，替换 **4 932** 条 / 改动 3 154 条记录、`problems: 0`；反查 `extract` 只剩 **95 条**
> 占位串，**三类长文本均 0 条残留**。
> （上一版：`QUST CNAM` 未译，5 575 493 B，MD5 `191ABBB93FFD4A61F3E6CBA8AEB3805E`。
> 本次 MD5 变了是因为**同时把全批次词典里的 `银河银行`→`盖尔银行`、`联殖防务`→`星防队`
> 统一成官方译名后重跑**；`du_overtime` / `du_retrograde` 也已按同样方式重建部署，
> 见下一节。）

> 2026-09-17（术语统一后的另外两个）：`overwrite\du_overtime.esm`（1 531 177 B，
> MD5 `5745AD2FAEE3F60D3755EEAD7CD41D96`）、`overwrite\du_retrograde.esm`（28 462 996 B，
> MD5 `68D80E2EBC0EAF73C05B04964B44FA16`），均从 `tr/orig` 整份重建、`problems: 0`。

### 术语统一：三个受影响 mod 全部重建并重新部署（2026-09-17）

| mod | 重建结果 | MD5 | 体积变化 |
| --- | --- | --- | --- |
| `du_outlaws_01.esm` | 替换 4 932 条 / `problems: 0` | `74FB0E81B2FB9E2AB52DD12A1F184E5E` | 5 575 493 → **5 563 835** |
| `du_overtime.esm` | 替换 2 558 条 / `problems: 0` | `5745AD2FAEE3F60D3755EEAD7CD41D96` | 1 531 161 → **1 531 177**（**+16**） |
| `du_retrograde.esm` | 替换 5 519 条 / `problems: 0` | `68D80E2EBC0EAF73C05B04964B44FA16` | 28 463 002 → **28 462 996**（**−6**） |

体积差可以**逐字节解释**，说明重建没有夹带任何意外改动：

- `du_retrograde` **−6 B** = 2 条 `联殖防务`（4 汉字 = 12 B）→ `星防队`（3 汉字 = 9 B），每条 −3 B；
- `du_overtime` **+16 B** = 6 条术语替换（`盖尔银行` 比 `银河银行` 短 1 B，共 −6 B）
  与 **4 条 `BOOK ENAM` 回退为英文**（每条 +4 B，共 +16 B）相抵后的净值。
  ⚠️ 这 4 条回退是**刻意且符合现行政策**的：`BOOK ENAM` 已从 `trtool.py` 白名单移除
  （`du_outlaws_01` 的 `ENAM` 是 8 位十六进制 ID，翻译等于改真实标识符），
  而旧 `du_overtime.esm` 是**白名单收紧之前**构建的，所以那 4 条此前是中文。
  由于 `extract` 已不再输出 `ENAM`，**残留串计数两者都是 460 条不变**
  （`du_retrograde` 前后都是 301 条）。

其余 mod（`above and beyond` / `morelore_mantislegacy` / `du_takeover` / `du_xfire`）的 map 里
**本来就没有**这两个术语，无需重建：`grep '银河银行|联殖防务' tr/out/*.map.tsv` ⇒ **0 命中**。
重建前的旧 ESM 备份在 `out/backup-manual/`（不入库）。

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
