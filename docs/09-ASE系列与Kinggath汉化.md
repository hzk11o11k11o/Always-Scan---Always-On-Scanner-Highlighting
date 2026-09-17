# 09 · `ase3` / `aseveil` / `aseeverbright` / `kinggathcreations_spaceship` 汉化

> 目标：**中文**，名词与 B 社官方汉化一致。
> 日期：2026-09-17
> 对象（4 个 Creation，均落在 MO2 的 `overwrite\`）：
> - `ase3.esm`（远见者 / SEDA 任务线，内联字符串）
> - `aseveil.esm`（帷幕 / 创世纪组织任务线，内联字符串）
> - `aseeverbright.esm`（永辉 / 霜印任务线，内联字符串）
> - `kinggathcreations_spaceship.esm`（国王加思「飞船 + 瞭望塔」大型 mod，**本地化插件**）

---

## 一、结论速览

| mod | 载体 | 可翻译串 | 已译 | 状态 |
| --- | --- | --- | --- | --- |
| `ase3.esm` | 内联 | 242 | 233（96.3%） | **完成并部署** |
| `aseveil.esm` | 内联 | 222 | 216（97.3%） | **完成并部署** |
| `aseeverbright.esm` | 内联 | 388 | 347（89.4%） | **完成并部署** |
| `kinggathcreations_spaceship.esm` | **外部 `.strings`** | 8141 | 5294（65.0%） | **部分完成并部署**（UI/终端/物品全译，对白 3242/6006） |

三个 ASE 插件都用 `trtool.py extract → 词典 → apply` 原位汉化，`tr_verify.py` 均 `problems: 0`。
`kinggath` 用**新工具 `tr_locpack.py`** 生成 `_zhhans` 字符串包，`kg_sanity.py` 自检通过。

**唯一刻意不译的东西**：
- `ase3` / `aseveil` / `aseeverbright` 里**纯内部代号**（`SEDA`、`ASE3`、`ASE3AstOrbital`、
  `ASEHuman_Male_LeftEye2_Black`、`ASE2PlayerHome`、`EternalFaction`、`[BE - ASE3]`…）
  以及 `QUST QMDP` 的 `<Alias=...>` 占位符、伪造加密乱码串。
- `kinggath` 里未译的 **2847 行**全部是 `ilstrings`（NPC 对白），见第五节。

---

## 二、★ 关键发现：`kinggath` 是**本地化插件**，文本不在 ESM 里

`check_esm_header.py` 显示 `kinggathcreations_spaceship.esm` 的 flags = **`0x00000081`
（0x01 ESM + 0x80 Localized）**，而三个 ASE 插件都是 `0x00000001`。

- 本地化插件的玩家可见文本**不内联**，而是 4 字节字符串 ID，真正的文本在
  `Data\Strings\<插件名>_<语言>.{strings,dlstrings,ilstrings}`。
- 这些 strings **打包在 `kinggathcreations_spaceship - main.ba2` 里**（`STRINGS/` 目录），
  `trtool.py extract` 只能看到 429 条 `QUST NAM2`（CK 阶段备注，见下）。
- 该 mod 自带了 9 种语言，但 **`_zhhans` 与 `_it`/`_ja`/`_pl` 的字节数完全相同、
  内容与 `_en` 一字不差**（`ststrings.py dict` 得 0 对）⇒ 官方并未提供中文，而是英文副本。

### 为此新增的工具

| 工具 | 作用 |
| --- | --- |
| `tools/re/tr_locpack.py` | `dump`（三类 strings 合并导出 TSV）/ `stats` / `toextract`（转成 `tr_pipeline` 认的抽取格式）/ `tomap`（把 `map` 转回来）/ `apply`（生成 `_zhhans` 包） |
| `tools/re/kg_sanity.py` | 自检：**ID 集合必须与英文包完全一致**（不丢文本），且每条改动都含中文（无乱码） |

对 `tr_locpack.py` 的两处必要修正：
- **按原始字节保真**：未翻译的条目**原样写回原始字节**（早期实现用
  `decode(utf-8,'replace')` 再 `encode` 会把 Windows-1252 的 `\x92` 等字节变成
  `EF BF BD`，损坏原文）。现在 `parse_raw()` / `build_raw()` 全程走 bytes。
- `--loc zhhans --fallback en`：以英文包为底，只替换词典里有的 ID。

### `trtool.py` 白名单 = `*FULL + BOOK DESC + MESG DESC/ITXT + …`
`kinggath` 的 ESM 在本项目白名单下**只抽出 `QUST NAM2`（429 条）**。
按 `docs/07` 的离线实证，`NAM2` 是 CK 里的阶段备注（`STAGE 0` / `ON START` 一类），
**永不出现在 UI 里**。本轮**未译**（ESM 保持原样，零风险）；如需与 `du_*` 惯例一致，
后续可 `trtool.py extract → 译 → apply` 补上，属于纯可选项。

---

## 三、`kinggath` 的部署方式：**松散文件覆盖 BA2**

翻译后的三个文件放到 **`D:\Mod Organizer 2\starfield_mods\overwrite\Strings\`**：

```
kinggathcreations_spaceship_zhhans.strings      87421 B
kinggathcreations_spaceship_zhhans.dlstrings    38078 B
kinggathcreations_spaceship_zhhans.ilstrings  437342 B
```

依据：B 社引擎的资源查找是「松散文件优先于归档」，所以 `Data\Strings\` 下的松散
`_zhhans` 会盖住 BA2 里的英文副本。**这一点必须游戏内实测确认**（见第六节）；
若实测无效，退路是**重打包 main.ba2**（用自研 BA2 写入器替换 `STRINGS/` 下三个文件，
其余条目原样搬运）。

> 为什么不改 ESM：本地化插件的 ESM 里根本没有文本，改它没意义；
> 也不动 `.ba2` 的 957 MB 主体，避免整包重写风险。

---

## 四、三个 ASE 插件（内联字符串）

与 `du_*` / `above` / `mantis` 完全同一套流水线（`trtool.py` + 词典 + `tr_verify.py`）。

- master：`ase3` 依赖 `sfbgs003.esm`；`aseveil` / `aseeverbright` 只有 `Starfield.esm`。
- 全部为**未标本地化的非官方插件**，字符串内联、UTF-8 写回。
- 有**若干覆盖原版记录**（FormID 高字节 `00`），例如 `ase3` 的
  `PNDT 0005E452 Lantana III`、`aseeverbright` 的 `WRLD 0029DDD7 Gagarin Landing`、
  `PNDT 0005E513 Eridani V`… ⇒ 一律**按官方中文写回**（否则中文游戏里会退化回英文）。

### 名词统一（官方串为准）

| 英文 | 官方中文 | 备注 |
| --- | --- | --- |
| Lantana | **马缨丹星**（`Lantana III` = 马缨丹III） | |
| Al-Battani | 阿尔巴塔尼星 | |
| Jaffa | 迦法星 | |
| Andromas | 安德罗玛 | |
| Groombridge | 格龙布里奇星 | |
| Nesoi | 内索伊星 | |
| Indum | **天峥** | 直觉想不到 |
| Eridani | **波江座** | |
| Van Maanen | 范马南星 | |
| Vega | 织女星 | |
| Bradbury | 布拉德伯里星 | |
| Altair | 牵牛星 | |
| Piazzi | 皮亚兹星 | |
| Hawley | 霍雷星 | |
| Gagarin Landing | 加加林着陆点 | |
| Deepseeker | **深海探索者** | |
| Matteo Khatri | 马蒂奥·卡特里 | |
| Equinox / EquiNOX | **二分点** | |
| CombaTech | 搏战科技 | |
| Frontier | 开拓号 | |
| Pilgrim / Sentinel / Guardian / Warden | 朝圣者 / 哨兵 / 守护者 / 守卫长 | |
| Sari Dress | 纱丽连衣裙 | |

mod 自造词（官方表里没有）的统一译法：

| 英文 | 译法 | 说明 |
| --- | --- | --- |
| Watchtower（组织/站名） | **瞭望塔** | 全 mod 统一 |
| Deviant（对星裔的称呼） | **异常体** | 与「星裔」区分，是瞭望塔的用语 |
| Astralgate | **星界之门** | 「人造星界之门」= Deviant Countermeasure 05 |
| Armillary | **浑天仪** | 原版就是「浑天仪」 |
| Stardock | **星坞** | 空间站名 |
| Heimdall Station | 海姆达尔站 | |
| Cyberamp | **赛博增幅** | 与官方 `Neuroamp = 神经增幅器` 区分 |
| Skydrop | **空投** | 空投曳光弹 = Skydrop Flare |
| Salvage | 拆解（名词：打捞） | |
| Nova Galactic | 新星银河 | |
| Terrormorph | **骇变兽** | 官方串，勿再用「惊惧兽」 |
| SEDA | 保留 `SEDA` | 无解的企业缩写，全串保留 |
| Farseer | **远见者** | `ase3` 任务线名 |
| Everbright / Everfrost / Everstorm | **永辉 / 永冻 / 永暴** | 保持三词同构 |
| Frostmarked | **霜印** | 阵营 |
| Genesis (Initiative) | **创世纪（组织）** | `aseveil` 反派组织 |
| Conclave | **秘议会** | 创世纪的领导层 |
| Mendori / Kholen | 门多里 / 科伦 | 种族 / 异星生物 |
| The Veil | **帷幕** | `aseveil` 任务名 |
| Overseer | **监督者** | `aseeverbright` 的核心实体 |
| Prime Token | 至尊代币 | |

### 三个插件里刻意不译的
`SEDA` / `ASE3` / `ASE3AstOrbital` / `ASE2StationOrbital` / `ASEHuman_Male_*` /
`ASE2PlayerHome` / `EternalFaction` / `EllasContainer` / `TheosContainer` /
`[BE - ASE3]`（构建标签）/ `BOOK DESC` 的伪造加密乱码 / `<Alias=...>` 占位符。

---

## 五、`kinggath` 的翻译进度与续译方法

三类 strings 的构成：

| 类 | 总数 | 已译 | 内容 |
| --- | --- | --- | --- |
| `strings` | 1892 | **1794** | UI、终端正文、书籍/日志、任务目标、飞船部件、阵营 |
| `dlstrings` | 243 | **243** | 物品说明、perk 描述、教程（含超长的 Watchtower 玩法说明） |
| `ilstrings` | 6006 | **3242** | NPC 对白（**主要剩余工作量**） |

`map` 统计：`rows=8141 translated=5294 missing=2847（65.0%）`。
**剩余 2847 行全部是 `ilstrings` 对白**（`tr/out/kg_en.tsv` 第 5327 行起）。

### 续译一条龙（已就绪，可增量）

```powershell
$W = 'D:\workspace\starfield mod\always scan'
$OV = 'D:\Mod Organizer 2\starfield_mods\overwrite'
Set-Location $W

# 1) 继续写批次：tr/lang/batches/b16xx_kg_il_XX.tsv（en<TAB>zh）
#    ★ 每批写完先校验英文键能在抽取文件里逐字命中：
python tools/re/tr_check_keys.py tr/out/kg.extract.tsv tr/lang/batches/b16xx_kg_il_XX.tsv

# 2) 合并词典 + 生成映射 + 生成 zhhans 包
python tools/re/tr_pipeline.py merge (Get-ChildItem tr/lang/batches/*.tsv).FullName `
       tr/lang/official_seed.tsv -o tr/lang/dict.tsv
python tools/re/tr_pipeline.py merge tr/lang/dict.tsv tr/lang/official_seed.tsv `
       tr/loc/starfield_en_zh.tsv -o tr/lang/dict_full.tsv
python tools/re/tr_pipeline.py map tr/out/kg.extract.tsv --dict tr/lang/dict_full.tsv `
       -o tr/out/kg.map.tsv
python tools/re/tr_locpack.py tomap tr/out/kg.map.tsv -o tr/out/kg.locmap.tsv
python tools/re/tr_locpack.py apply 'tr/loc/kg/STRINGS' kinggathcreations_spaceship `
       --loc zhhans --fallback en --map tr/out/kg.locmap.tsv -o tr/build/kg

# 3) 自检 + 部署
python tools/re/kg_sanity.py
Copy-Item tr/build/kg/* "$OV\Strings\" -Force

# 4) 看还剩多少
python tools/re/tr_pipeline.py status --mods tr/out/kg.extract.tsv --dict tr/lang/dict_full.tsv
```

**对白的查看入口**：`tr/out/kg_en.tsv`（`kind<TAB>id<TAB>text`，按 id 排序，
`strings` 在前、`dlstrings` 次之、`ilstrings` 在后）；配套的
`tr/out/kg.extract.tsv` 是同一顺序、可直接喂 `tr_pipeline` 的抽取格式。

### 已踩到的坑

1. **`ba2list.py` 早期不解压**：`STRINGS/…` 条目是 **zlib 压缩**（`packed != unpacked`），
   早期只按 `packed` 长度原样写出 ⇒ 解析报 `subsection not found`。
   已修：`packed and packed != unpacked` 时 `zlib.decompress`。
2. **`ststrings.py` 后缀大小写**：`_en.DLSTRINGS`（大写）不被
   `suffix in ('.dlstrings', …)` 命中 ⇒ 被当成 NUL 结尾串解析，越界报错。
   已修：`suffix = path.suffix.lower()`。同时修 `rstrip(b'\x00')`
   （dlstrings/ilstrings 的 `u32 长度` 把结尾 NUL 也算进去了）。
3. **少数英文串含非 UTF-8 字节**：如 `AI Project Lead's Log` 的弯引号（Windows-1252 `\x92`）。
   直接手写这个键会错 ⇒ 这两条改用 `tr_mkbatch.py` **按抽取文件行号取键**
   （`tr/lang/spec/kg_special.txt`）。
4. **`full-width` 标点不是「中文」**：`kg_sanity.py` 初版只看 `U+4E00–9FFF`，
   把 `（…）` 误报为「不是中文」。已把 CJK 标点（`U+3000–303F`、`U+FF00–FFEF`）算进判据。

---

## 六、部署状态与游戏内验证清单

| 文件 | 位置 | MD5 |
| --- | --- | --- |
| `ase3.esm` | `overwrite\` | `68B450C0170089D7CD4DFBDE33174D72` |
| `aseveil.esm` | `overwrite\` | `ADC6357819D3E05A821166E20E4DF2F8` |
| `aseeverbright.esm` | `overwrite\` | `840884C78AFBAA2DB0C181056A09DA18` |
| `…_zhhans.strings/.dlstrings/.ilstrings` | `overwrite\Strings\` | 见第六节末（`kg_sanity.py` 自检 OK） |

原始文件备份：`tr/orig/{ase3,aseveil,aseeverbright,kinggathcreations_spaceship}.esm`；
`kinggath` 的原始 `.strings` 三件套在 `tr/loc/kg/STRINGS/`（从 BA2 抽出）。

四个 mod 在 MO2 profile `Default` 里**本来就已启用**，只需原位替换 / 加松散文件，不用改 profile。

### 游戏内验证清单

1. **关键未知项：松散 `Strings` 是否盖过 BA2**——进游戏，看国王加思 mod 的
   任意 UI（如"瞭望塔选项"玩法设置）或物品名是否中文。
   - 中文 ⇒ 方案成立，继续补对白即可。
   - 仍旧英文 ⇒ 松散文件未生效，需改为**重打包 `kinggathcreations_spaceship - main.ba2`**
     （`tr_locpack.py` 已有读写能力，只差一个 BA2 写入器）。
2. `ase3` / `aseveil` / `aseeverbright` 三者的任务名、目标、物品名应是中文，
   且 `<Alias=...>` 被正确替换成实际地点名。
3. **原版覆盖项**（`Lantana III` / `Gagarin Landing` / `Eridani V` 等）应为官方中文，
   不退回英文。
4. 老存档可读、不崩；出问题就用 `tr/orig/` 里的原件覆盖回去，并删掉
   `overwrite\Strings\` 下的松散文件。
