# ============================================================================
#  Simple Immersive Looting - Chinese Translation: Nexus Mods 打包
#
#  把 MO2 部署目录里的汉化版插件整理成一个可以直接上传到 Nexus 的 zip：
#
#      dist\SimpleImmersiveLooting-zh-CN-<version>.zip
#        ├─ SimpleImmersiveLooting.esm          ← 汉化版插件（Data 根）
#        ├─ SimpleImmersiveLooting - Main.ba2   ← 原版脚本包（未改动）
#        └─ README.txt                          （取自 package\sil\README.txt）
#
#  默认打「**完整包**」：一步安装、无需先装原 mod。
#  注意：完整包会**再分发原作者的 .ba2** —— 发布前请确认原作者许可
#  （Nexus 页面权限 / 私信同意）。如只想发翻译文件，用 -TranslationOnly。
#  包内**不含** meta.ini（MO2 本地元数据，与分发包无关）。
#
#  用法（不要用 `pwsh -File`，命令包装器会拦；直接在当前会话跑）：
#      & 'tools\package-sil.ps1'
#      & 'tools\package-sil.ps1' -Version 1.1
#      & 'tools\package-sil.ps1' -TranslationOnly      # 只含汉化 ESM 的翻译包
#      & 'tools\package-sil.ps1' -OutDir D:\some\where
#
#  自检：必需文件存在 + zip 内容逐条列出 + 每个文件 SHA256 + 包体积。
# ============================================================================
param(
    [string]$Version = '',            # 默认从部署目录的 meta.ini 读 version=
    [string]$OutDir  = '',            # 默认 <项目根>\dist
    [string]$ModRoot = 'D:\Mod Organizer 2\starfield_mods\mods\Simple Immersive Looting',
    [string]$NamePrefix = 'SimpleImmersiveLooting-zh-CN',
    [switch]$TranslationOnly  # 只打「翻译包」（仅汉化 ESM，不含原 .ba2）
)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

# ---------------------------------------------------------------- 0. 检查输入
if (-not (Test-Path -LiteralPath $ModRoot)) { throw "部署目录不存在: $ModRoot （先跑 tools\mo2-enable-sil.ps1 或用 -ModRoot 指定）" }
if (-not (Test-Path -LiteralPath $sevenZip)) { throw "找不到 7-Zip: $sevenZip" }

$metaPath = Join-Path $ModRoot 'meta.ini'
if (-not $Version) {
    if (-not (Test-Path -LiteralPath $metaPath)) { throw "缺少 $metaPath，无法确定版本号（可用 -Version 指定）" }
    $m = Select-String -LiteralPath $metaPath -Pattern '^version=(.+)$'
    if (-not $m) { throw "meta.ini 里没有 version= （可用 -Version 指定）" }
    $Version = $m.Matches[0].Groups[1].Value.Trim()
}
Write-Host "=== Packaging Simple Immersive Looting (zh-CN) v$Version for Nexus ===" -ForegroundColor Cyan

# ------------------------------------------------- 1. 收集文件（相对 Data 根）
$payload = [ordered]@{
    'SimpleImmersiveLooting.esm' = $true
}
if (-not $TranslationOnly) {
    $payload['SimpleImmersiveLooting - Main.ba2'] = $true
    Write-Host '      模式: 完整包（含原版 .ba2）—— 发布前请确认原作者许可' -ForegroundColor Yellow
} else {
    Write-Host '      模式: 翻译包（仅汉化 ESM，需先装原 mod）' -ForegroundColor DarkGray
}
foreach ($rel in $payload.Keys) {
    $src = Join-Path $ModRoot $rel
    if (-not (Test-Path -LiteralPath $src)) { throw "分发包缺少必需文件: $src" }
}

$readmeSrc = Join-Path $root 'package\sil\README.txt'
if (Test-Path -LiteralPath $readmeSrc) { $payload['README.txt'] = $true }
else { Write-Host "      WARN: 缺少 package\sil\README.txt，包内将没有说明文件" -ForegroundColor Yellow }

# 占位符检查：README 里的作者名是否还没改
if (Test-Path -LiteralPath $readmeSrc) {
    if (Select-String -LiteralPath $readmeSrc -Pattern 'PUT-YOUR-NEXUS-NAME-HERE' -Quiet) {
        Write-Host '      WARN: package\sil\README.txt 里还有 <PUT-YOUR-NEXUS-NAME-HERE> 占位符' -ForegroundColor Yellow
        Write-Host '            上传前请改成自己的 Nexus 用户名并重新打包。' -ForegroundColor Yellow
    }
}

# ------------------------------------------------------------- 2. 组装 staging
$staging = Join-Path $env:TEMP 'sil-nexus-package'
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

foreach ($rel in $payload.Keys) {
    if ($rel -eq 'README.txt') { $src = $readmeSrc } else { $src = Join-Path $ModRoot $rel }
    $dst = Join-Path $staging $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

# ------------------------------------------------------------ 3. 打 zip（7z）
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$suffix = if ($TranslationOnly) { '-translation-only' } else { '' }
$zipPath = Join-Path $OutDir "$NamePrefix-$Version$suffix.zip"
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
Write-Host '    正文/字段见 package\sil\nexus-description.md' -ForegroundColor DarkGray
