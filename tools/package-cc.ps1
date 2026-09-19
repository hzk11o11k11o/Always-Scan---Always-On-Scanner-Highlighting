# ============================================================================
#  Starfield Always Scan (SFSE) - 官方 Creations（"CC"）提交材料包
#
#  官方 Creations 没有"上传 zip"流程：上传发生在 Creation Kit 内
#  （File -> Login to Bethesda.net -> Upload ... -> 勾选要打包的数据）。
#  所以本脚本产出的是"提交材料包"——把 CK 上传流程要用到的文件与文案
#  预先摆好：
#
#      dist\StarfieldAlwaysScan-CC-<version>.zip
#        ├─ StarfieldAlwaysScan.esm            ← CK 上传的插件主体
#        ├─ Scripts\SAS_Bridge.pex             ← Papyrus 资产（允许）
#        ├─ Scripts\Source\SAS\SAS_Bridge.psc  ← 源码
#        ├─ preview.jpg                        ← 预览图素材（package\cc\）
#        ├─ README.txt                         ← 英文说明（package\README.txt）
#        ├─ CC-UPLOAD-GUIDE.md                 ← 上传指南（含结论与红线）
#        └─ creation-copy.txt                  ← 表单文案
#
#  ★ 本包**刻意不含** SFSE\Plugins 下的任何文件：
#    官方 Creations 禁止外部代码（DLL），上传它可能被下架并影响账号。
#    完整（含 SFSE）的分发包请用 tools\package-nexus.ps1。
#
#  ★ 先读 package\cc\CC-UPLOAD-GUIDE.md §0：
#    本 MOD 功能核心在 DLL 内，**以现有形态预期无法通过官方审核**；
#    本包用于"若你仍要提交/向 Bethesda 询问"时的材料准备。
#
#  用法（必须 pwsh 7）：
#      & '.\tools\package-cc.ps1'
#      & '.\tools\package-cc.ps1' -Version 4.8.0 -OutDir D:\some\where
#
#  自检：必需文件缺失直接 throw；包内容逐条列清单 + SHA256，便于对账。
# ============================================================================
param(
    [string]$Version = '',            # 默认从部署目录的 meta.ini 读 version=
    [string]$OutDir  = '',            # 默认 <项目根>\dist
    [string]$ModRoot = 'D:\Mod Organizer 2\starfield_mods\mods\Starfield Always Scan (SFSE)'
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
Write-Host "=== Packaging Starfield Always Scan v$Version for Bethesda Creations (submission package) ===" -ForegroundColor Cyan
Write-Host "     注意：本包不含 SFSE/DLL —— 上传前请阅读包内 CC-UPLOAD-GUIDE.md 的结论" -ForegroundColor Yellow

# ------------------------------------------------- 1. 收集文件（来源 -> 包内路径）
# 来自 MO2 部署目录（MOD 本体）
$fromMod = [ordered]@{
    'StarfieldAlwaysScan.esm'           = 'StarfieldAlwaysScan.esm'
    'Scripts\SAS_Bridge.pex'            = 'Scripts\SAS_Bridge.pex'
    'Scripts\Source\SAS\SAS_Bridge.psc' = 'Scripts\Source\SAS\SAS_Bridge.psc'
}
# 来自 package\cc\（CC 专用材料）
$fromCc = [ordered]@{
    'CC-UPLOAD-GUIDE.md' = 'CC-UPLOAD-GUIDE.md'
    'creation-copy.txt'  = 'creation-copy.txt'
    'preview.jpg'        = 'preview.jpg'          # 可选（缺则 WARN）
}

foreach ($rel in $fromMod.Keys) {
    $src = Join-Path $ModRoot $rel
    if (-not (Test-Path -LiteralPath $src)) { throw "提交包缺少必需文件: $src" }
}
foreach ($rel in $fromCc.Keys) {
    $src = Join-Path (Join-Path $root 'package\cc') $rel
    if (-not (Test-Path -LiteralPath $src)) {
        if ($rel -eq 'preview.jpg') { Write-Host "      WARN: 缺少 package\cc\preview.jpg（预览图素材），包内将没有它" -ForegroundColor Yellow }
        else { throw "提交包缺少必需文件: $src" }
    }
}

# 包内 README（复用 Nexus 版；不存在只警告）
$readmeSrc = Join-Path $root 'package\README.txt'
$hasReadme = Test-Path -LiteralPath $readmeSrc
if (-not $hasReadme) { Write-Host "      WARN: 缺少 package\README.txt，包内将没有英文说明" -ForegroundColor Yellow }

# 版本号一致性提醒
if (Test-Path -LiteralPath $metaPath) {
    $mv = (Select-String -LiteralPath $metaPath -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()
    if ($mv -ne $Version) { Write-Host "      WARN: meta.ini version=$mv 与本次打包 version=$Version 不一致" -ForegroundColor Yellow }
}

# ------------------------------------------------------------- 2. 组装 staging
$staging = Join-Path $env:TEMP 'sas-cc-package'
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

function Stage-One([string]$src, [string]$rel) {
    if (-not (Test-Path -LiteralPath $src)) { return }
    $dst = Join-Path $staging $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

foreach ($k in $fromMod.Keys) { Stage-One (Join-Path $ModRoot $k) $fromMod[$k] }
foreach ($k in $fromCc.Keys)  { Stage-One (Join-Path (Join-Path $root 'package\cc') $k) $fromCc[$k] }
if ($hasReadme) { Stage-One $readmeSrc 'README.txt' }

# ------------------------------------------------------------ 3. 打 zip（7z）
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipPath = Join-Path $OutDir "StarfieldAlwaysScan-CC-$Version.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

Push-Location $staging
try {
    & $sevenZip a -tzip -mx=9 -y $zipPath '*' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z 打包失败 (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

# ------------------------------------------- 4. 自检：不能含 SFSE，清单 + 哈希
$listing = & $sevenZip l -ba $zipPath
$bad = $listing | Where-Object { $_ -match 'SFSE|\.dll|\.ini' }
if ($bad) { throw "自检失败：提交包内出现 SFSE/DLL/INI 文件：`n$($bad -join "`n")" }

Write-Host ''
Write-Host '--- zip contents ---' -ForegroundColor Yellow
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
Write-Host '    上传流程见包内 CC-UPLOAD-GUIDE.md（在 Creation Kit 内登录并上传，不是传本 zip）' -ForegroundColor DarkGray
