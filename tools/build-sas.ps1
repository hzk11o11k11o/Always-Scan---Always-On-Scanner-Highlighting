# ============================================================================
#  Starfield Always Scan (SFSE) - 一键构建
#   1) xEdit 无头生成 StarfieldAlwaysScan.esm（记录表见 build_sas.pas 顶部注释）
#   2) Papyrus 编译 SAS_Bridge.psc
#   3) xmake 构建 SAS_AlwaysScan.dll
#   4) 部署到 MO2 mod 目录
#   5) 更新 MO2 profile（启用本 mod）
#
#  ★ 必须用 pwsh 跑（PowerShell 7）。本文件是 UTF-8 无 BOM，
#    Windows PowerShell 5.1 会按 GBK 解码，中文路径会变成乱码。
#      pwsh -File tools\build-sas.ps1
#
#  常用组合：
#    pwsh -File tools\build-sas.ps1 -SkipPluginBuild -SkipDllBuild   # 只重编脚本
#    pwsh -File tools\build-sas.ps1 -SkipPluginBuild -SkipPapyrusCompile  # 只重编 DLL
# ============================================================================
param(
    [switch]$SkipPluginBuild,
    [switch]$SkipPapyrusCompile,
    [switch]$SkipDllBuild,
    [switch]$SkipDeploy,
    [switch]$SkipProfile
)

$ErrorActionPreference = 'Stop'

$root         = Split-Path -Parent $PSScriptRoot          # 项目根
$xeditExe     = Join-Path $root 'tools\vendor\xEdit\xSFEdit64.exe'
$runXedit     = Join-Path $root 'tools\run-xedit.ps1'
$buildScript  = Join-Path $root 'tools\xedit-scripts\build_sas.pas'
$pluginDir    = Join-Path $root 'plugin'
$tmp          = 'C:\Users\huangzhe\AppData\Local\Temp\kilo\sf-alwaysscan'
$tmpOut       = Join-Path $tmp 'out'
$tmpPex       = Join-Path $tmp 'pex'
$tmpSrc       = Join-Path $tmp 'src'
$dataDir      = 'D:\SteamLibrary\steamapps\common\Starfield\Data'
$papyrusCmp   = 'D:\SteamLibrary\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe'
$papyrusFlags = Join-Path $dataDir 'Scripts\Source\Base\Starfield_Papyrus_Flags.flg'
$papyrusInc   = Join-Path $dataDir 'Scripts\Source\Base'
$pluginName   = 'StarfieldAlwaysScan.esm'
$bridgeScript = 'SAS_Bridge'
$dllName      = 'SAS_AlwaysScan'
$dllPath      = Join-Path $pluginDir "build\windows\x64\releasedbg\$dllName.dll"
$pdbPath      = Join-Path $pluginDir "build\windows\x64\releasedbg\$dllName.pdb"
$modsDir      = 'D:\Mod Organizer 2\starfield_mods\mods'
$modName      = 'Starfield Always Scan (SFSE)'

Write-Host '=== Always Scan (SFSE) Build Pipeline ===' -ForegroundColor Cyan
$sw = [System.Diagnostics.Stopwatch]::StartNew()

New-Item -ItemType Directory -Force -Path $tmpOut, $tmpPex, $tmpSrc | Out-Null

# ------------------------------------------------- 0. vendored header offsets
# ★ commonlibsf 原版有两个真实的偏移错误，本项目已就地修正（见 docs/99）：
#     BGSListForm::arrayOfForms  0x30 -> 0x38
#     TESGlobal::value           0x40 -> 0x48
#   依据：TESForm 的数据在 0x31 结束、按 8 字节对齐补到 0x38；派生类的第一个成员
#   若含指针（对齐 8）就只能从 0x38 开始，绝不可能落在尾部填充里。
#   如果这两处被「重新解包 commonlibsf」覆盖回去，DLL 会往错误偏移写数据
#   （把 FLST 的表单头写坏）—— 而且是运行期才炸。所以这里做**构建期硬校验**。
Write-Host '[0/5] Verifying vendored commonlibsf header offsets...' -ForegroundColor Yellow
$hf = @(
    @{ Path = (Join-Path $root 'tools\commonlibsf-main\include\RE\B\BGSListForm.h'); Marker = 'offsetof(BGSListForm, arrayOfForms) == 0x38' },
    @{ Path = (Join-Path $root 'tools\commonlibsf-main\include\RE\T\TESGlobal.h');      Marker = 'offsetof(TESGlobal, value) == 0x48' }
)
foreach ($h in $hf) {
    if (-not (Test-Path -LiteralPath $h.Path)) { throw "vendored header missing: $($h.Path)" }
    if (-not (Select-String -LiteralPath $h.Path -SimpleMatch $h.Marker -Quiet)) {
        throw @"
vendored header offset fix is missing: $($h.Path)
  expected marker: $($h.Marker)
  commonlibsf 原版把 BGSListForm::arrayOfForms 写成 0x30、TESGlobal::value 写成 0x40，
  正确值是 0x38 / 0x48（TESForm 的数据在 0x31 结束、按 8 字节对齐补到 0x38）。
  请按 docs/99-当前项目进度.md 里的说明把这两处偏移改回来再构建。
"@
    }
    Write-Host ("      OK  {0}" -f (Split-Path -Leaf $h.Path)) -ForegroundColor DarkGray
}


# ---------------------------------------------------------------- 1. plugin
if (-not $SkipPluginBuild) {
    Write-Host '[1/5] Building plugin via headless xEdit (slow, ~4 min)...' -ForegroundColor Yellow

    # ★ .pas 的两条硬约束（两条都真的浪费过一整轮 5 分钟构建）：
    #   ① 必须纯 ASCII —— xEdit 按 ANSI(GBK) 读脚本，UTF-8 中文注释会变乱码，
    #      字节撞进语法 → "Error in unit ... : = expected but '(' found"。
    #   ② **不能出现花括号** —— Pascal 的 { } 注释在**第一个** '}' 处结束，
    #      注释里写一个 '}' 就会把后半句当代码解析 →
    #      "Declaration expected but '...' found"。本脚本一律用 // 注释。
    $raw = [System.IO.File]::ReadAllBytes($buildScript)
    $badAscii = @()
    $badBrace = @()
    $lineNo = 1
    foreach ($b in $raw) {
        if ($b -eq 10) { $lineNo++ }
        elseif ($b -gt 127) { $badAscii += $lineNo }
        elseif ($b -eq 0x7B -or $b -eq 0x7D) { $badBrace += $lineNo }
    }
    if ($badAscii.Count -gt 0) {
        $uniq = ($badAscii | Select-Object -Unique | Sort-Object) -join ', '
        throw "build_sas.pas contains non-ASCII bytes on line(s): $uniq (xEdit requires pure ASCII)"
    }
    if ($badBrace.Count -gt 0) {
        $uniq = ($badBrace | Select-Object -Unique | Sort-Object) -join ', '
        throw "build_sas.pas contains brace characters on line(s): $uniq (Pascal brace comments truncate at the first brace; use // instead)"
    }

    foreach ($p in @((Join-Path $dataDir $pluginName), (Join-Path $tmpOut $pluginName), (Join-Path $tmpOut 'h_99_done.txt'))) {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    }

    & $runXedit -Exe $xeditExe -ScriptPath $buildScript `
        -DoneFile (Join-Path $tmpOut 'h_99_done.txt') -IdleCloseSec 60 -TimeoutSec 2400 | Select-Object -Last 3

    if (-not (Test-Path -LiteralPath (Join-Path $tmpOut $pluginName))) {
        throw "Plugin build failed: $pluginName not produced (see $tmpOut\h_*.txt)"
    }
    Write-Host '      plugin built.' -ForegroundColor Green

    # FormID map 自检：必须与 plugin/src/AlwaysScan.cpp 的 kLocal* 常量一致
    $mapFile = Join-Path $tmpOut 'h_99_done.txt'
    if (Test-Path -LiteralPath $mapFile) {
        $expect = @{
            '0x800' = 'SAS_HighlightFXS'
            '0x801' = 'SAS_OpList'
            '0x802' = 'SAS_OpCursor'
            '0x803' = 'SAS_StopList'
            '0x804' = 'SAS_StopCursor'
            '0x805' = 'SAS_Epoch'
            '0x806' = 'SAS_AlwaysScanQuest'
            '0x807' = 'SAS_Notify'
        }
        $lines = Get-Content -LiteralPath $mapFile
        foreach ($id in $expect.Keys | Sort-Object) {
            $hit = $lines | Where-Object { $_ -match [regex]::Escape($id) }
            if (-not $hit) {
                throw "FormID map check failed: $id ($($expect[$id])) not found in h_99_done.txt"
            }
            Write-Host ("      {0}  {1}" -f $id, $hit.Trim()) -ForegroundColor DarkGray
        }
        # 顺序错位是最致命的（DLL 会往错误的表单写数据），这里再核对一次低 24 位
        foreach ($line in $lines) {
            if ($line -match '^\s*(0x[0-9A-F]{3})\s+(\S+)\s+([0-9A-F]{6})$') {
                $want = [Convert]::ToInt32($Matches[1].Substring(2), 16)
                $got  = [Convert]::ToInt32($Matches[3], 16)
                if ($want -ne $got) {
                    throw "FormID map MISMATCH: $($Matches[1]) $($Matches[2]) -> 0x$($Matches[3]) (expected 0x$($Matches[1].Substring(2)))"
                }
            }
        }
        Write-Host '      FormID map verified.' -ForegroundColor Green
    }
} else {
    Write-Host '[1/5] Plugin build skipped.' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- 2. papyrus
if (-not $SkipPapyrusCompile) {
    Write-Host '[2/5] Compiling Papyrus bridge...' -ForegroundColor Yellow
    Copy-Item -LiteralPath (Join-Path $root "scripts\$bridgeScript.psc") -Destination (Join-Path $tmpSrc "$bridgeScript.psc") -Force
    $stalePex = Join-Path $tmpPex "$bridgeScript.pex"
    if (Test-Path -LiteralPath $stalePex) { Remove-Item -LiteralPath $stalePex -Force }

    & $papyrusCmp (Join-Path $tmpSrc "$bridgeScript.psc") "-f=$papyrusFlags" "-i=$papyrusInc" "-o=$tmpPex" | Out-Host
    if (-not (Test-Path -LiteralPath $stalePex)) { throw 'Papyrus compilation failed' }
    Write-Host ("      papyrus compiled ({0} B)." -f (Get-Item -LiteralPath $stalePex).Length) -ForegroundColor Green
} else {
    Write-Host '[2/5] Papyrus compile skipped.' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- 3. dll
if (-not $SkipDllBuild) {
    Write-Host '[3/5] Building SFSE plugin (xmake)...' -ForegroundColor Yellow
    Push-Location $pluginDir
    try {
        xmake f -y -p windows -a x64 -m releasedbg --vs=2022 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "xmake config failed (exit $LASTEXITCODE)" }
        xmake build $dllName | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "xmake build failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    if (-not (Test-Path -LiteralPath $dllPath)) { throw "DLL not built: $dllPath" }
    Write-Host ("      dll built ({0} B)." -f (Get-Item -LiteralPath $dllPath).Length) -ForegroundColor Green
} else {
    Write-Host '[3/5] DLL build skipped.' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- 4. deploy
if (-not $SkipDeploy) {
    Write-Host '[4/5] Deploying to MO2...' -ForegroundColor Yellow
    $modRoot = Join-Path $modsDir $modName
    # ★ 注意：**不要**对 $modRoot 做递归删除。
    #   一是这一步在安全删除包装器下会失败（genie-trash 拒绝整目录），
    #   二是本 mod 的产物集合是固定的（esm / pex / dll / pdb / ini / meta.ini），
    #   下面全是 Copy-Item -Force 覆盖写，本来就不需要先清空。
    #   真要清空的话手删即可（或只删 $pluginsDir 里的 dll/pdb）。
    New-Item -ItemType Directory -Force -Path $modRoot | Out-Null

    $pluginsDir   = Join-Path $modRoot 'SFSE\Plugins'
    $scriptsDir   = Join-Path $modRoot 'Scripts'
    $scriptSrcDir = Join-Path $scriptsDir 'Source\SAS'
    New-Item -ItemType Directory -Force -Path $pluginsDir, $scriptSrcDir | Out-Null

    Copy-Item -LiteralPath (Join-Path $tmpOut $pluginName) -Destination (Join-Path $modRoot $pluginName) -Force
    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $pluginsDir "$dllName.dll") -Force
    if (Test-Path -LiteralPath $pdbPath) {
        Copy-Item -LiteralPath $pdbPath -Destination (Join-Path $pluginsDir "$dllName.pdb") -Force
    }
    # 配置 INI：★ 2026-09-25 起放在 **esm 同级**（mod 目录根）—— 用户要求
    #   「配置文件要放在和 esm 文件同级目录里」（AGENTS.md，与日志同一条规则）。
    #   DLL 优先读那里，读不到才回退 SFSE\Plugins\（老位置，带 WARN）。
    #   **用户已经改过的版本不要覆盖**（否则每次构建都会把热键/半径重置回默认值）。
    $iniSrc = Join-Path $root 'resources\SAS_AlwaysScan.ini'
    $iniDst = Join-Path $modRoot 'SAS_AlwaysScan.ini'
    $iniOld = Join-Path $pluginsDir 'SAS_AlwaysScan.ini'
    if (Test-Path -LiteralPath $iniOld) {
        # 老位置还留着（v4.16 及以前部署的）：新位置没有就**搬过去**（保住用户改过的
        # 值）；新位置已有则把老的改名存档，免得两处各一份、改错文件。
        if (-not (Test-Path -LiteralPath $iniDst)) {
            Move-Item -LiteralPath $iniOld -Destination $iniDst -Force
            Write-Host '      config ini moved next to the esm (mod root).' -ForegroundColor DarkGray
        } else {
            Move-Item -LiteralPath $iniOld -Destination "$iniOld.moved-to-esm-dir" -Force
            Write-Host '      old SFSE\Plugins ini renamed to *.moved-to-esm-dir' -ForegroundColor DarkGray
        }
    }
    if ((Test-Path -LiteralPath $iniSrc) -and -not (Test-Path -LiteralPath $iniDst)) {
        Copy-Item -LiteralPath $iniSrc -Destination $iniDst -Force
        Write-Host '      config ini installed (default).' -ForegroundColor DarkGray
    } elseif (Test-Path -LiteralPath $iniDst) {
        Write-Host '      config ini kept (already exists, not overwritten).' -ForegroundColor DarkGray
        # 但如果新版 INI 里有旧版没有的键（新增配置项），提示一下 —— 否则用户永远
        # 不会知道新功能有开关存在（DLL 用的是内置默认值，行为没问题）。
        if (Test-Path -LiteralPath $iniSrc) {
            $newKeys = (Select-String -LiteralPath $iniSrc -Pattern '^([A-Za-z][A-Za-z0-9_]*)\s*=' |
                        ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -Unique
            $oldKeys = (Select-String -LiteralPath $iniDst -Pattern '^([A-Za-z][A-Za-z0-9_]*)\s*=' |
                        ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -Unique
            $missing = $newKeys | Where-Object { $oldKeys -notcontains $_ }
            if ($missing) {
                Write-Host ("      WARN: deployed ini lacks new key(s): {0}" -f ($missing -join ', ')) -ForegroundColor Yellow
                $newIni = "$iniDst.new"
                Copy-Item -LiteralPath $iniSrc -Destination $newIni -Force
                Write-Host ("      new default written to {0} (diff it, then merge what you want)" -f (Split-Path -Leaf $newIni)) -ForegroundColor Yellow
            }
        }
    }
    if (Test-Path -LiteralPath (Join-Path $tmpPex "$bridgeScript.pex")) {
        Copy-Item -LiteralPath (Join-Path $tmpPex "$bridgeScript.pex") -Destination (Join-Path $scriptsDir "$bridgeScript.pex") -Force
    }
    Copy-Item -LiteralPath (Join-Path $root "scripts\$bridgeScript.psc") -Destination (Join-Path $scriptSrcDir "$bridgeScript.psc") -Force

    # ★ v4.16：DLL 把日志写在「和 esm 同级」的目录（= 虚拟 Data 根，MO2 下就是
    #   mod 目录根）。MO2 的 usvfs 只把「写 mod 目录里**已存在**的文件」重定向回
    #   mod 目录，新建的文件会落到 overwrite —— 所以这里预置一个空的
    #   SAS_AlwaysScan.log 把写入"钉"回 esm 旁边。已存在则保留（日志内容不能丢）。
    $logPath = Join-Path $modRoot "$dllName.log"
    if (-not (Test-Path -LiteralPath $logPath)) {
        New-Item -ItemType File -Path $logPath -Force | Out-Null
        Write-Host "      empty $dllName.log pre-seeded (usvfs 会把日志写回 esm 旁边)" -ForegroundColor DarkGray
    }

    # ★ 2026-09-25（v4.17 / 发布版 1.6）：
    #   · modid = N 网 mod 页 ID（18268）—— **不填 0**：MO2 的「Newest Version」
    #     靠它去查 N 网，modid=0 时那一列永远是空的（用户反馈「读不到版本」）。
    #   · version = **N 网公开版号**（1.x），不是 DLL 内部版本：
    #     用户要求 MO2 里显示与 N 网页面上的 Version 字段一致。
    # ★ 2026-09-25（v4.18 / 发布版 1.7）：修复「分色不生效」——
    #   配色覆盖必须写在任何 HighlightManager 创建之前（见 AlwaysScan.cpp 的
    #   WriteColorOverrides 注释：0x17D47B0 对已存在的管理器不再刷新颜色）。
    # ★ 2026-09-25（v4.19 / 发布版 1.7.1）：高区分度配色 ——
    #   ① 订正配色块地址（老代码写的 0x591E088 是 float 常量区，写错表）；
    #   ② 默认给**所有类别**写覆盖色（红/品红/亮绿/黄/紫/橙/青/白…），
    #      不再依赖原生状态色（原生 0/1/2/3 全是蓝色系，肉眼分不开）。
    # ★ 2026-09-25（v4.20 / 发布版 1.7.2）：覆盖太深 → 半透明 ——
    #   门 / 武器 / 防具 默认 40% 不透明（INI `AlphaXxx`，0 = 保留引擎原值），
    #   让物品本身材质透出来（用户反馈「完全盖过材质」）。
    # ★ 2026-09-26（v4.22 / 发布版 1.7.4）：① state 7/8 归还原版扫描仪（星球上的
    #   矿石 / 气体 / 液体 / 植物 / 动物靠它们区分「扫描前 / 扫描后」，不再覆盖）⇒
    #   「资源」改用 state 3（紫）；② 「植物」类别默认关（FLOR 里含矿脉 / 气泉 / 液池）；
    #   ③ 修「资源判据自检被样本不在内存卡死 ⇒ 资源静默归杂项（和杂物同色）」。
    # ★ 2026-09-26（v4.23 / 发布版 1.7.5）：① 修「资源」判据**真根因** ——
    #   BSTArray 布局读反了（data 实际在 +0x08，旧代码读 +0x00 ⇒ 任何 MISC 都判不出
    #   资源，全程静默归杂项）+ 偏移自适应 + `关键词数组标定` 日志；
    #   ② 星球扫描目标（植物 / 矿脉 / 气泉 / 液池）**重新默认开**：state 7 +
    #   颜色不覆盖（用原版青色脉冲轮廓），并新增 `YieldWhileScanning=1`
    #   （举着扫描仪时 MOD 整段让位 ⇒ 原版的「扫描前 / 扫描后」区别不再被盖）。
    # ★ 2026-09-26（v4.24 / 发布版 1.7.6）：**state 4/5 归还引擎** —— 用户实测
    #   「扫描后没有变成原版扫描后的绿色」的真根因：引擎的扫描求值函数（0x159ED90）
    #   把「**已经扫描过**」的星球目标（FLOR → produceItem → MISC → BGSResource(IRES)
    #   →「资源已扫描」命中 ⇒ `add edx,4`）写进 **state 4（远）/ 5（近）**，
    #   而这两个槽位的原生色就是**绿色 #27C684**；v4.19~v4.23 把它们覆盖成
    #   「设备 青」/「弹药救援 亮绿」⇒ 扫描后永远看不到那个绿。
    #   修法：4/5 颜色一个字节都不写（设备 / 弹药救援 继续用这两个槽位，颜色 = 原版绿）;
    #   另新增诊断 `ManagerOccupancyProbe`（举扫描仪 1.5s 后打 11 个管理器元素数）。
    # ★ 2026-09-26（v4.25 / 发布版 1.7.7）：**放下扫描仪之后也区分「扫没扫过」** ——
    #   用户实测「矿石/气体/液体/植物 收起扫描后还是扫描前的青色」。MOD 现在直接调
    #   引擎自己的判据函数（RVA 0x1597A50「该资源是否已进勘测数据」），照抄引擎那条链
    #   （FLOR+0x260 produceItem → 首条目 MISC → MISC+0x238 资源数组 → BGSResource(IRES)）：
    #     已扫描 ⇒ StateFloraScanned（默认 5，原生绿 #27C684）、未扫描 ⇒ StateFlora（7，青）。
    #   新键：FloraScannedByResource / StateFloraScanned / FloraScanProbeMax；
    #   让位保护改成按类别（OutlineEntry::cat）、状态变化即时生效（不再等重申时刻）。
    # ★ 2026-09-26（v4.26 / 发布版仍 1.7.7）：v4.25 的判据**实测第一步就读不到**
    #   （produce=0x0，连 Starfield.esm 原版矿脉也一样）⇒ 改成**两条证据来源（取或）**：
    #     ① 引擎亲手画过的颜色（让位期间读一眼引擎写的 outline 状态：4/5 = 已扫描绿）；
    #     ② 资源链判据 + **自适应偏移**（先试 +0x260/+0x258，再窗口 0x180~0x380 找
    #        能把整条链走通的指针；标定写会话变量 + 日志）；
    #   链走不通时打前 3 个 base 的**指针窗口取证**（供下一轮定位偏移）。
    # ★ 2026-09-26（v4.27 / v4.28 / v4.29 / 发布版仍 1.7.7）：判据上界订正（IRES 0x9F
    #   被 0x60 上界误杀）→ 主判据换成 `GetOutlineState(ref)`（= 原生 IsScanned，
    #   植物不再「没扫就绿」）→ 放下扫描仪后的「恢复提速」（ResyncBoostMs/Budget=2500/192
    #   + 状态变化优先换色）。
    # ★ 2026-09-26（v4.30 / 发布版 1.7.8）：**state 0/1 归还引擎** —— 用户实测
    #   「扫描中的 NPC 全部变成只有赏金人物才会出现的颜色」：引擎的逐引用求值函数
    #   （0x159ED90）对**通用引用（含活人 NPC）**写 **state 0（远）/ 1（近）**，
    #   而 v4.19~v4.29 把 0/1 覆盖成「武器 红 / 服饰 品红」⇒ 举着扫描仪时所有行人
    #   跟着变色。修法：0/1 一个字节都不写；「武器 / 服饰」改挂 9 / 10、
    #   「容器 / 尸体 / 门」搬去 1 / 0 并用原版色 ⇒ 覆盖槽位从 7 个降到 5 个
    #   （2/3/6/9/10）。证据链见 docs/28。
    # ★ 2026-09-26（v4.31 / 发布版 1.7.9）：**配色重排（容器橙 / 门白恢复）+
    #   植物「低概率变回青色」修复** —— 用户实测两条：
    #   ① 「上次改动把容器 / 尸体 / 门的颜色也变了」⇒ 容器 / 尸体 = 9（覆盖橙）、
    #      门 = 6（覆盖白，6 是引擎唯一不写的槽位）；代价 = 服饰（→1）/ 笔记（→0）
    #      改成不覆盖（可借槽位只有 2/3/6/9/10 五个）；
    #   ② 「已扫描植物低概率变回青色，开扫描仪再关又变绿」⇒ ⓪ 单向学习表
    #      （base 级，「已扫描」只写 true、本会话不重问）+ 重问拿不到权威答案时
    #      **沿用旧结论**。证据链 / 取舍表见 docs/29。
    Set-Content -LiteralPath (Join-Path $modRoot 'meta.ini') -Value "[General]`nmodid=18268`nversion=1.7.9`ncomment=Always-on scanner highlighting (SFSE)" -Encoding UTF8

    Get-ChildItem -LiteralPath $modRoot -Recurse -File | ForEach-Object {
        Write-Host ("  {0}  ({1} bytes)" -f $_.FullName.Substring($modRoot.Length + 1), $_.Length)
    }

    # xEdit 的 AddNewFileName 会在游戏 Data 目录留一个空文件，必须清掉
    $dataCopy = Join-Path $dataDir $pluginName
    if (Test-Path -LiteralPath $dataCopy) { Remove-Item -LiteralPath $dataCopy -Force }
    if (Test-Path -LiteralPath $dataCopy) {
        Write-Host '      WARN: could not remove Data copy (game running?)' -ForegroundColor Yellow
    } else {
        Write-Host '      Data dir clean.' -ForegroundColor Green
    }
} else {
    Write-Host '[4/5] Deploy skipped.' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- 5. MO2 profile
if (-not $SkipProfile) {
    Write-Host '[5/5] Updating MO2 profile...' -ForegroundColor Yellow
    $profileDir = 'D:\Mod Organizer 2\starfield_mods\profiles\Default'
    if (-not (Test-Path -LiteralPath $profileDir)) { throw "MO2 profile not found: $profileDir" }

    # --- modlist.txt：启用本 mod（置顶） ---
    $modlistPath = Join-Path $profileDir 'modlist.txt'
    $lines = @(Get-Content -LiteralPath $modlistPath)
    if (-not (Test-Path -LiteralPath "$modlistPath.bak-sas")) { Copy-Item -LiteralPath $modlistPath -Destination "$modlistPath.bak-sas" -Force }
    $lines = $lines | Where-Object { $_ -ne "+$modName" -and $_ -ne "-$modName" }
    $firstMod = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^[+\-*]') { $firstMod = $i; break }
    }
    if ($firstMod -lt 0) { $lines += "+$modName" }
    else { $lines = $lines[0..($firstMod - 1)] + @("+$modName") + $lines[$firstMod..($lines.Count - 1)] }
    Set-Content -LiteralPath $modlistPath -Value $lines -Encoding UTF8

    # --- plugins.txt / loadorder.txt：确保本插件被激活 ---
    foreach ($file in @('plugins.txt', 'loadorder.txt')) {
        $path = Join-Path $profileDir $file
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if (-not (Test-Path -LiteralPath "$path.bak-sas")) { Copy-Item -LiteralPath $path -Destination "$path.bak-sas" -Force }
        $pl = @(Get-Content -LiteralPath $path)
        $pl = $pl | Where-Object { $_ -ne "*$pluginName" -and $_ -ne $pluginName }
        if ($file -eq 'plugins.txt') { $pl += "*$pluginName" } else { $pl += $pluginName }
        Set-Content -LiteralPath $path -Value $pl -Encoding UTF8
    }

    Write-Host "      enabled '+$modName', plugin=$pluginName" -ForegroundColor Green

    # 上一代项目那个 mod 功能重叠（也往 ref 上打 EFSH），两个一起开会双倍占 VM。
    $oldMod = 'Starfield Highlight Items (SFSE)'
    $oldLine = @(Get-Content -LiteralPath $modlistPath) | Where-Object { $_ -eq "+$oldMod" }
    if ($oldLine) {
        Write-Host "      WARN: '$oldMod' 仍然启用 —— 与本 mod 功能重叠，建议在 MO2 里禁用它。" -ForegroundColor Yellow
    }
    Write-Host '      NOTE: if Mod Organizer 2 is running, restart it (or it will overwrite these files).' -ForegroundColor Yellow
} else {
    Write-Host '[5/5] MO2 profile update skipped.' -ForegroundColor DarkGray
}

Write-Host ("=== Done in {0:N1}s ===" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Cyan
