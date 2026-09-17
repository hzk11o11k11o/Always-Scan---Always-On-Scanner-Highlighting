# 07 · `above and beyond.esm` / `morelore_mantislegacy.esm` 汉化

> 目标：**全中文**，名词与 B 社官方汉化一致。
> 日期：2026-09-17
> 对象：
> - `above and beyond.esm`（Creations 下载，落在 MO2 `overwrite\`；UC 先锋队任务板扩展）
> - `morelore_mantislegacy.esm`（MO2 mod 目录 `More Lore - Legacy of the Mantis`；给原版祈祷者巢穴补传说文档）

---

## 一、结论速览

| mod | 记录数 | 可翻译串 | 唯一串 | 改动 | 状态 |
| --- | --- | --- | --- | --- | --- |
| `above and beyond.esm` | 112 | 430 | 164 | 373 条（59 记录） | **完成并部署** |
| `morelore_mantislegacy.esm` | 172 | 102 | 102 | 102 条（40 记录） | **完成并部署** |

两个文件都用 `trtool.py extract → 词典 → apply` 原位汉化，`tr_verify.py` 均 `problems: 0`。

**唯一刻意不译的东西**：`above and beyond` 的 57 条 `QUST QMDP` **别名占位符**（`<Alias=TargetLocation>` 等）。
它们是引擎替换用的占位文本，改了就废。反查确认：汉化后 `above` 重新 `extract` 只剩这 57 条；
`mantis` 重新 `extract` **0 条**（即 100% 无英文残留）。

---

## 二、这两个 mod 的文本形态

两者都是**未标本地化（无 `0x80` flag）的非官方插件**，字符串**内联**在记录里
（`enc_probe.py` 报 `high-byte text subrecords: 0`），和 `du_*` 系列一样，所以沿用
`docs/06` 那套纯 Python 流水线即可，编码仍统一写 **UTF-8（无 BOM）**。

可翻译子记录（沿用 `trtool.py` 的 `WHITELIST`，本次两个 mod 没有需要新增的类型）：

```
*    -> FULL      （所有记录类型的显示名）
BOOK -> DESC      （正文）
MESG -> DESC, ITXT
QUST -> CNAM, NAM1, NAM2, NNAM, QMDP, QMDT, QMSU
```

本批出现的承载文本类型：`BOOK FULL/DESC`、`QUST FULL/CNAM/NAM2/NNAM/QMSU/QMDT/QMDP`、
`MESG FULL/DESC/ITXT`、`NPC_ FULL`、`ACTI FULL`、`CELL FULL`、`LCTN FULL`、`WRLD FULL`、
`TMLM FULL`、`CONT FULL`、`MISC FULL`、`KYWD FULL`。

### ★ 两件重要的事（和 `du_*` 不同）

1. **两个插件大量「覆盖原版记录」**（FormID 高字节 = `00`）。例如：
   - `above`：`WRLD 0001251B New Atlantis` / `WRLD 00244CAA Paradiso` /
     `WRLD 002C6534 Ka'Zaal Sulfur Mine` / `CELL 00012998 Markers`
   - `mantis`：`QUST 001CD001`（原版 **MS04**）/ `CELL 00293F1E Lair of the Mantis` /
     `CELL 0027E165 Scientific Wing` / `LCTN 0001295A New Atlantis` /
     `TMLM 001434F5 Security Computer` / `TMLM 001434F6 Doriane's Computer`
   Creation Kit 保存非本地化插件时会把**本地化字符串 ID 烙成英文明文** ⇒
   中文游戏里这些名字会退化回英文。**所以这些覆盖项要按官方中文写回去**（见第三节）。

2. **`NAM2` 不是给玩家看的**（离线实证）：直接读 `Starfield.esm` 的
   `QUST 001CD001`（`esmrec.py --formid 0x1CD001`），它的 `FULL` 是 **4 字节字符串 ID**
   （`0x0002D8D4` → `Mantis` / 祈祷者），而 `NAM2` 是**内联 ASCII**（`STAGE 0` / `ON START`）。
   ⇒ `FULL`/`CNAM`/`NNAM` 是本地化的玩家可见文本，`NAM2` 只是 CK 里的阶段备注。
   本项目仍按既有惯例把 `NAM2` 一并译了（与已部署的 5 个 `du_*` 一致，且它永不出现在 UI 里）。

---

## 三、名词统一（从游戏本体抽官方对照）

做法同 `docs/06`：先把官方英中对照表 `tr/loc/starfield_en_zh.tsv` 当词典，逐条对齐。
本次新用到的官方术语（**几个和直觉不一样的**）：

| 英文 | 官方中文 | 备注 |
| --- | --- | --- |
| Mantis | **祈祷者** | 不是“螳螂”！`Lair of the Mantis` = 祈祷者的巢穴 |
| Terrormorph | **骇变兽** | ⚠️ `docs/06` 里记的“惊惧兽”是**错的**，以官方串为准 |
| Spacer / Pirate | 太空劫匪 / 海盗 | |
| UC Vanguard | 联殖先锋队 | 简称“先锋队” |
| United Colonies / UC | 联合殖民地 / 联殖 | |
| Ecliptic | 黄道佣兵团 | |
| Va'ruun Zealot | 瓦鲁狂徒 | |
| MAST | **三头同盟** | |
| Aegis | **宙斯盾** | |
| Ryujin | 龙神集团 | |
| The Well | **深井区** | 新亚特兰蒂斯下层区 |
| New Atlantis / Paradiso | 新亚特兰蒂斯城 / 天堂乐园 | |
| Cydonia / Gagarin / New Homestead | 赛多尼亚 / 加加林星 / 新家园 | |
| Sol / Mars / Luna / Rhea | 太阳系 / 火星 / 月球 / 土卫五 | |
| Lantana IV / Bessel II / Hyla I | 马缨丹IV / 贝塞尔II / 海拉I | |
| Denebola I-b | 五帝座一I-b | |
| The Almagest | **至高** | 站名，直译很容易错 |
| Ka'Zaal | 肯扎尔 | `Ka'Zaal Sulfur Mine` = 肯扎尔星硫磺矿井 |
| Chunks | 块餐 | |
| Settled Systems / Colony War | 定居星系群 / 殖民地大战 | |
| Livvey / Doriane / Leon Voclain | 利维 / 多里安 / 利昂·沃克莱恩 | |
| Commander Tuala | 图阿拉指挥官 | |
| Security Computer | 安保电脑 | |

**`mantis` 的 6 条 `QUST CNAM` 里有 4 条就是原版 MS04 的日志文本**，官方串里能直接查到
（带尾部 `\x00`），已**照抄官方译文**；`NNAM` 里 5 条同样照抄官方。

mod 自造词（官方表里没有）按 B 社风格意译：

| 英文 | 译法 | 说明 |
| --- | --- | --- |
| The Unwanted（帮派） | 弃民 | |
| Dustwell / Dustwell Station | 尘井 / 尘井站 | |
| Long Reckoning（船） | 长清算号 | |
| Tellurian Drift（货船） | 大地漂流号 | |
| Halvorsen's Reach | 哈尔沃森聚居地 | |
| Hand of Ash（瓦鲁护卫舰） | 灰烬之手号 | |
| Sonder | 桑德 | ⚠️ 全游戏 strings 里**查无此名**（只有船名 `The Sonder` = 过客号）⇒ 作者自造，音译 |
| Boost / Echo（Scraphead 社交网） | 点赞 / 回响 | `Scraphead`/用户名/频道名保留英文 |
| The Legacy of the Mantis | 祈祷者的遗产 | mod 把原版 MS04 从「祈祷者」改了名 |

---

## 四、`<header ...>` 标记（`mantis` 的论坛体书籍）

`mantis` 的多篇书籍正文长得像社交平台帖子，用 `<header leftText=... rightText=...>` 包住发帖人。
实测：**官方 strings 里 `<header` 出现 0 次**（官方书籍只用 `<font>/<i>/<b>/<u>/<p>`），
所以这多半是作者自定的标记。处理原则：**标记连属性值（用户名）原样保留**，只译正文与
`+174 Boosts | 51 Echos` 这类计数。这样即使引擎把它当纯文本渲染，也不会更糟。

---

## 五、一条龙命令（可复现）

```powershell
$W = 'D:\workspace\starfield mod\always scan'
$OV = 'D:\Mod Organizer 2\starfield_mods\overwrite'
$MM = 'D:\Mod Organizer 2\starfield_mods\mods\More Lore - Legacy of the Mantis'
Set-Location $W

# 1) 抽串（原始文件已备份在 tr\orig\）
python tools/re/trtool.py extract "$OV\above and beyond.esm" -o tr/out/above.tsv --stats
python tools/re/trtool.py extract "$MM\morelore_mantislegacy.esm" -o tr/out/mantis.tsv --stats

# 2) 查官方命中（术语对齐用）
python tools/re/tr_official_hits.py tr/out/above.tsv tr/out/mantis.tsv   # 官方串里逐字命中的
python tools/re/tr_term_probe.py Mantis Vanguard ...                     # 按英文子串查
python tools/re/tr_zh_probe.py 遗产 抢劫                                  # 按中文子串查

# 3) 人工译文写在 tr/lang/spec_*.txt（「行号 <TAB> 中文」），生成词典批次
python tools/re/tr_mkbatch.py tr/out/above.tsv  tr/lang/spec_above.txt  tr/lang/batches/b40_above.tsv
python tools/re/tr_mkbatch.py tr/out/mantis.tsv tr/lang/spec_mantis.txt tr/lang/batches/b41_mantis.tsv

# 4) 合并总词典 + 生成映射
python tools/re/tr_pipeline.py merge (Get-ChildItem tr/lang/batches/*.tsv).FullName `
       tr/lang/official_seed.tsv -o tr/lang/dict.tsv
python tools/re/tr_pipeline.py map tr/out/above.tsv  --dict tr/lang/dict.tsv -o tr/out/above.map.tsv
python tools/re/tr_pipeline.py map tr/out/mantis.tsv --dict tr/lang/dict.tsv -o tr/out/mantis.map.tsv

# 5) 生成 + 校验
python tools/re/trtool.py apply "$OV\above and beyond.esm" --map tr/out/above.map.tsv -o 'tr/build/above and beyond.esm'
python tools/re/trtool.py apply "$MM\morelore_mantislegacy.esm" --map tr/out/mantis.map.tsv -o tr/build/morelore_mantislegacy.esm
python tools/re/tr_verify.py "$OV\above and beyond.esm" 'tr/build/above and beyond.esm'      # 期望 problems: 0
python tools/re/tr_verify.py "$MM\morelore_mantislegacy.esm" tr/build/morelore_mantislegacy.esm

# 6) 部署（原位替换）
Copy-Item 'tr/build/above and beyond.esm' "$OV\above and beyond.esm" -Force
Copy-Item tr/build/morelore_mantislegacy.esm "$MM\morelore_mantislegacy.esm" -Force
```

### 本次新增的通用小工具（`tools/re/tr_*.py`）

| 工具 | 作用 |
| --- | --- |
| `tr_official_hits.py` | 把 mod 串逐条丢进官方对照表，列出**逐字命中**的（含尾部 NUL 变体） |
| `tr_term_probe.py` | 按**英文子串**在官方对照表里查条目（定术语用） |
| `tr_zh_probe.py` | 按**中文子串**反查官方对照表（确认某个中文说法官方到底怎么用） |
| `tr_sid_probe.py` | 按**字符串 ID** 取官方 en/zh（覆盖原版记录时确认准确官方译名，如 MS04） |
| `tr_mkbatch.py` | 按「抽取文件行号 → 译文」生成词典批次，**避免手抄长英文段落出错** |

> `tr_mkbatch.py` 的两个细节：① 行号按抽取文件的**物理行号**（`#` 表头算第 1 行）；
> ② 行号写错会直接报错（`spec 行号 N ... 没有对应数据行`），不会静默漏译。

---

## 六、踩坑 / 经验

1. **`QUST QMDT` 是标签、`QUST QMDP` 是取值**：`QMDT`（Target System/Planet/Location…）
   要译；`QMDP` **大部分**是 `<Alias=...>` 占位符不能译，但**也有真实地名**
   （`Sol`/`Mars`/`Luna`/`Gagarin`/`Hyla I`/`Bessel II`/`The Almagest`/`Dez Okafor`…）。
   第一轮我把 `QMDP` 整类跳过了，结果漏掉 10 条地名 —— 判据就是 `map` 的 `missing` 数，
   **一定要逐条看 missing 到底是什么**，别默认"都是占位符"。
2. **覆盖原版记录必须查官方译名**：`WRLD/CELL/LCTN/TMLM/QUST` 的 FormID 高字节是 `00`
   ⇒ 是覆盖项，用 `tr_sid_probe.py` / `tr_official_hits.py` 取官方中文写回去，否则中文游戏里会露英文。
3. **`docs/06` 的「Terrormorph = 惊惧兽」是错的**，官方串是 **骇变兽**。
   （教训：术语表要以 `tr/loc/starfield_en_zh.tsv` 为准，不要凭印象。）
4. **不要手抄长英文原文**：10 段 A4 长的书籍正文 + `<header>` 标记，手抄一个字符就整条漏译。
   用 `tr_mkbatch.py` 按行号取键，彻底消掉这一类错误。
5. `BOOK FULL` **不加书名号**：官方风格并不统一
   （`《块餐速食店员工指南》` vs `先锋队莫亚拉的数据板`/`黄道佣兵团笔记`），
   且本项目 `du_*` 已有 3 300 条词典**一条都没用 `《》`** ⇒ 保持一致，一律不加。

---

## 七、部署状态

| 文件 | 位置 | MD5 |
| --- | --- | --- |
| `above and beyond.esm` | `D:\Mod Organizer 2\starfield_mods\overwrite\` | `EDD66C757921C3652D0194E94925CAEE` |
| `morelore_mantislegacy.esm` | `...\mods\More Lore - Legacy of the Mantis\` | `351AAC74EBD7D01E0B02DE00EF396486` |

两者在 MO2 profile `Default` 里**本来就已启用**（`plugins.txt` 带 `*`，
`loadorder.txt` 也在），所以只需原位替换文件，不用改 profile。
原始文件备份：`tr/orig/above and beyond.esm`、`tr/orig/morelore_mantislegacy.esm`。

### 游戏内验证清单

1. 进游戏打开**先锋队任务板** → 任务名/简报/目标应是中文，且 `<Alias=...>` 占位符被正确
   替换成实际地名（不是原样显示）。
2. 去**五帝座一I-b 的祈祷者巢穴**捡文档 → 论坛体帖子的正文是中文，
   行首 `<header leftText='Scraphead PING' ...>` **原样保留**（英文，这是预期）。
3. **原版任务名**该显示 `祈祷者的遗产`（mod 覆盖了 MS04 的名字）；
   世界观地名（新亚特兰蒂斯城 / 天堂乐园 / 肯扎尔星硫磺矿井）不应退化回英文。
4. 存档可读、不崩；出问题就用 `tr/orig/` 里的原件覆盖回去。
