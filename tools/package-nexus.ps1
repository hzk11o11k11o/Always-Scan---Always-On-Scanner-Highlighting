# ============================================================================
#  Starfield Always Scan (SFSE) - Nexus Mods 打包
#
#  把 MO2 部署目录（= 玩家看到的最终产物）整理成一个可以直接上传到
#  Nexus Mods 的 zip：
#
#      dist\StarfieldAlwaysScan-<version>.zip
#        ├─ StarfieldAlwaysScan.esm             ← Data 根
#        ├─ SAS_AlwaysScan.ini                  ← 和 esm 同级（配置）
#        ├─ Scripts\SAS_Bridge.pex
#        ├─ Scripts\Source\SAS\SAS_Bridge.psc   （源码，运行期不需要）
#        ├─ SFSE\Plugins\SAS_AlwaysScan.dll
#        └─ README.txt                          （取自 package\README.txt）
#
#  zip 内**不含**：
#    · meta.ini        —— MO2 的本地元数据，与分发包无关
#    · *.pdb           —— 9 MB 调试符号，玩家用不到（-IncludePdb 可带上，
#                         用于让玩家给你可符号化的崩溃转储）
#
#  用法（必须 pwsh 7）：
#      pwsh -File tools\package-nexus.ps1
#      pwsh -File tools\package-nexus.ps1 -Version 4.2.0 -IncludePdb
#      pwsh -File tools\package-nexus.ps1 -OutDir D:\some\where
#
#  自检（发现不对劲直接 throw，不让坏包流出去）：
#    · 五个必需文件全部存在（缺任何一个都报错）；
#    · zip 内的路径结构逐条列出并核对；
#    · 打印每个文件的 SHA256 + 包体积，便于与日志/文档对账。
# ============================================================================
param(
    [string]$Version = '',            # 默认从部署目录的 meta.ini 读 version=
    [string]$OutDir  = '',            # 默认 <项目根>\dist
    [string]$ModRoot = 'D:\Mod Organizer 2\starfield_mods\mods\Starfield Always Scan (SFSE)',
    [switch]$IncludePdb
)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

# ---------------------------------------------------------------- 0. 检查输入
if (-not (Test-Path -LiteralPath $ModRoot)) { throw "部署目录不存在: $ModRoot （先跑 build-sas.ps1）" }
if (-not (Test-Path -LiteralPath $sevenZip)) { throw "找不到 7-Zip: $sevenZip" }

$metaPath = Join-Path $ModRoot 'meta.ini'
if (-not $Version) {
    if (-not (Test-Path -LiteralPath $metaPath)) { throw "缺少 $metaPath，无法确定版本号（可用 -Version 指定）" }
    $m = Select-String -LiteralPath $metaPath -Pattern '^version=(.+)$'
    if (-not $m) { throw "meta.ini 里没有 version= （可用 -Version 指定）" }
    $Version = $m.Matches[0].Groups[1].Value.Trim()
}
Write-Host "=== Packaging Starfield Always Scan (SFSE) v$Version for Nexus ===" -ForegroundColor Cyan

# ------------------------------------------------- 1. 收集文件（相对 Data 根）
$payload = [ordered]@{
    'StarfieldAlwaysScan.esm'                      = $true
    'SAS_AlwaysScan.ini'                           = $true   # ★ 2026-09-25：配置改到 esm 同级
    'Scripts\SAS_Bridge.pex'                       = $true
    'Scripts\Source\SAS\SAS_Bridge.psc'            = $true
    'SFSE\Plugins\SAS_AlwaysScan.dll'              = $true
}
if ($IncludePdb) { $payload['SFSE\Plugins\SAS_AlwaysScan.pdb'] = $true }

# ★ v4.33：单向学习表 —— 包内放一个**空文件**占位（MO2 的 usvfs 只把「写已存在的
#   文件」重定向回 mod 目录 ⇒ 预置空文件后，运行期学到的记录才会落在 mod 目录里、
#   而不是丢进 overwrite）。**不能**复制部署目录里那份：它可能已经含玩家自己的
#   学习记录（运行期数据，不该进分发包）。
$emptyPayload = @('SAS_AlwaysScan.flora-learn.txt')
foreach ($rel in $emptyPayload) { $payload[$rel] = $false }

foreach ($rel in $payload.Keys.Clone()) {
    if ($emptyPayload -contains $rel) { continue }
    $src = Join-Path $ModRoot $rel
    if (-not (Test-Path -LiteralPath $src)) { throw "分发包缺少必需文件: $src" }
}

# 版本号一致性提醒：部署目录 meta.ini 与显式 -Version 不一致时警告（不算错）
if ((Test-Path -LiteralPath $metaPath)) {
    $mv = (Select-String -LiteralPath $metaPath -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()
    if ($mv -ne $Version) { Write-Host "      WARN: meta.ini version=$mv 与本次打包 version=$Version 不一致" -ForegroundColor Yellow }
}

# 包内 README（如果存在就带上；不存在只警告）
$readmeSrc = Join-Path $root 'package\README.txt'
if (Test-Path -LiteralPath $readmeSrc) { $payload['README.txt'] = $true }
else { Write-Host "      WARN: 缺少 package\README.txt，包内将没有说明文件" -ForegroundColor Yellow }

# ------------------------------------------------------------- 2. 组装 staging
$staging = Join-Path $env:TEMP 'sas-nexus-package'
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

foreach ($rel in $payload.Keys) {
    $dst = Join-Path $staging $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    if ($rel -eq 'README.txt') {
        Copy-Item -LiteralPath $readmeSrc -Destination $dst -Force
    } elseif ($emptyPayload -contains $rel) {
        New-Item -ItemType File -Path $dst -Force | Out-Null   # 空占位（见上）
    } else {
        Copy-Item -LiteralPath (Join-Path $ModRoot $rel) -Destination $dst -Force
    }
}

# ★ 2026-09-27：包内 INI 的**日志开关 / 上限**以 resources 模板为准（= N 网公开版口径：
#   **0 = 完全关闭**，一个字节都不写；见 AGENTS.md 与 docs/48）——
#   部署目录那份是**本机开发**口径（LogMaxMB=10），不能直接进包。只改这一个键；其余内容
#   照旧（含用户对部署 INI 的其它自定义）。模板值就是单一事实源（当前 = 0）。
$tplIni    = Join-Path $root 'resources\SAS_AlwaysScan.ini'
$stagedIni = Join-Path $staging 'SAS_AlwaysScan.ini'
if ((Test-Path -LiteralPath $tplIni) -and (Test-Path -LiteralPath $stagedIni)) {
    $pubMb = 1
    $mm = Select-String -LiteralPath $tplIni -Pattern '^LogMaxMB=(\d+)'
    if ($mm) { $pubMb = [int]$mm.Matches[0].Groups[1].Value }
    $utf8    = New-Object System.Text.UTF8Encoding($false)   # 与模板一致：无 BOM
    $iniText = [System.IO.File]::ReadAllText($stagedIni, $utf8)
    $iniNew  = [regex]::Replace($iniText, '(?m)^LogMaxMB=\d+', "LogMaxMB=$pubMb")
    if ($iniNew -ne $iniText) {
        [System.IO.File]::WriteAllText($stagedIni, $iniNew, $utf8)
        Write-Host ("      ini LogMaxMB set to {0} (public default; dev copy keeps its own value)" -f $pubMb) -ForegroundColor DarkGray
    }
}

# ------------------------------------------------------------ 3. 打 zip（7z）
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipPath = Join-Path $OutDir "StarfieldAlwaysScan-$Version.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

Push-Location $staging
try {
    & $sevenZip a -tzip -mx=9 -y $zipPath '*' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z 打包失败 (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

# ------------------------------------------------------------ 4. 自检 + 清单
Write-Host ''
Write-Host '--- zip contents ---' -ForegroundColor Yellow
$listing = & $sevenZip l -ba $zipPath
$listing | ForEach-Object { Write-Host "  $_" }

Write-Host ''
Write-Host '--- SHA256 ---' -ForegroundColor Yellow
Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName | ForEach-Object {
    $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    Write-Host ("  {0}  {1}" -f $h, $_.FullName.Substring($staging.Length + 1))
}
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
$zipSize = (Get-Item -LiteralPath $zipPath).Length
Write-Host ''
Write-Host ("=== Done: {0}  ({1:N0} bytes) ===" -f $zipPath, $zipSize) -ForegroundColor Green
Write-Host ("    SHA256: {0}" -f $zipHash) -ForegroundColor Green
Write-Host '    上传到 Nexus：Mod page -> Files -> Upload file（用这个 zip 作为主文件）' -ForegroundColor DarkGray
