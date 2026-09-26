<#
sas-log-trend.ps1 —— SAS_AlwaysScan.log 趋势分析（帧数是否随时间下降）

思路：把日志里每条 `scan#N …` 行和它后面几行里的 `timing:` / `timing2:` /
`planet targets` / 3D复检 抽出来，按 scan 序号排成时间序列，输出「每采样点一次」
的表格，肉眼（或 Excel）看 avg/max 是否随会话推进单调变大。

用法：
  pwsh -NoProfile -File tools\sas-log-trend.ps1 -LogPath <日志> [-EveryN 10] [-Csv out.csv]

判读：
  · `scanAvg/scanMax` 随时间**持续变大** + `refs` 基本不变 ⇒ 真的在变慢；
  · `refs` 同时变大 ⇒ 只是场景里东西变多（外景走动），不算泄漏；
  · `loopAvg` 大 ⇒ 遍历（cell / 引用）变慢；`syncAvg` 大 ⇒ 引擎调用变慢；
    `shapeAvg` 大 ⇒ VirtualQuery 形状校验没命中缓存。
#>
param(
	[string]$LogPath = "D:\Mod Organizer 2\starfield_mods\mods\Starfield Always Scan (SFSE)\SAS_AlwaysScan.log",
	[int]$EveryN = 10,
	[string]$Csv = ""
)

$rxScan    = [regex]'^\[(?<t>[\d:.]+)\]\s+\[\d+\]\s+\[\w\]\s+scan#(?<n>\d+)\s+cell=(?<cell>\S+)\s+off=\S+\s+refs=(?<refs>\d+)\s+cells=(?<cells>\d+)\s+cand=(?<cand>\d+)\s+sel=(?<sel>\d+)\s+outline=(?<outl>\d+)/\S+\s+on=(?<on>\d+)\s+resync=(?<resync>\d+)'
$rxTiming  = [regex]'timing: scan avg=(?<avg>\d+)ms max=(?<max>\d+)ms ops=(?<ops>\d+) deferred=(?<def>\d+)'
$rxTiming2 = [regex]'timing2: shape avg=(?<sa>\d+)ms max=(?<sm>\d+)ms vq=(?<vq>[\d.]+)/scan \| loop avg=(?<la>\d+)ms max=(?<lm>\d+)ms \| sync avg=(?<ya>\d+)ms max=(?<ym>\d+)ms'
$rxResync  = [regex]'3D复检: probes=(?<p>\d+) reassert=(?<r>\d+)'
$rxPlanet  = [regex]'planet targets \(窗口内\): 未扫描=(?<un>\d+) 已扫描=(?<sc>\d+)'
$rxSession = [regex]'SAS_AlwaysScan v(?<v>[\d.]+) loading'

$lines = Get-Content -LiteralPath $LogPath
$rows  = New-Object System.Collections.ArrayList
$names = @{}

function New-Row([string]$when) {
	[ordered]@{
		time = $when; scan = 0; refs = 0; cand = 0; sel = 0; outl = 0; resync = 0
		scanAvg = 0; scanMax = 0; ops = 0; deferred = 0
		shapeAvg = 0; loopAvg = 0; loopMax = 0; syncAvg = 0
		probes = 0; reassert = 0; un = 0; sc = 0
	}
}

$cur = $null
for ($i = 0; $i -lt $lines.Count; ++$i) {
	$l = $lines[$i]

	if ($rxSession.IsMatch($l)) {
		# 一次新会话：换行，输出上一段的尾部，重置计数
		if ($null -ne $cur) { $null = $rows.Add([PSCustomObject]$cur) }
		$cur = New-Row "== v$($rxSession.Match($l).Groups['v'].Value) =="
		continue
	}
	if ($null -eq $cur) { $cur = New-Row "" }

	$m = $rxScan.Match($l)
	if ($m.Success) {
		# 每条 scan# 行 = 一个采样点：先落上一行，再开新行
		$null = $rows.Add([PSCustomObject]$cur)
		$cur = New-Row $m.Groups['t'].Value
		$cur.scan = [int]$m.Groups['n'].Value
		$cur.refs = [int]$m.Groups['refs'].Value
		$cur.cand = [int]$m.Groups['cand'].Value
		$cur.sel = [int]$m.Groups['sel'].Value
		$cur.outl = [int]$m.Groups['outl'].Value
		$cur.resync = [int]$m.Groups['resync'].Value
		continue
	}
	if ($rxTiming.IsMatch($l)) {
		$mm = $rxTiming.Match($l)
		$cur.scanAvg = [int]$mm.Groups['avg'].Value
		$cur.scanMax = [int]$mm.Groups['max'].Value
		$cur.ops = [int]$mm.Groups['ops'].Value
		$cur.deferred = [int]$mm.Groups['def'].Value
		continue
	}
	if ($rxTiming2.IsMatch($l)) {
		$mm = $rxTiming2.Match($l)
		$cur.shapeAvg = [int]$mm.Groups['sa'].Value
		$cur.loopAvg = [int]$mm.Groups['la'].Value
		$cur.loopMax = [int]$mm.Groups['lm'].Value
		$cur.syncAvg = [int]$mm.Groups['ya'].Value
		continue
	}
	$mm = $rxResync.Match($l)
	if ($mm.Success) { $cur.probes = [int]$mm.Groups['p'].Value; $cur.reassert = [int]$mm.Groups['r'].Value; continue }
	$mm = $rxPlanet.Match($l)
	if ($mm.Success) { $cur.un = [int]$mm.Groups['un'].Value; $cur.sc = [int]$mm.Groups['sc'].Value; continue }
}
if ($null -ne $cur) { $null = $rows.Add([PSCustomObject]$cur) }

# 采样（跳过会话分隔行）：每 EveryN 条打一行
$samples = @($rows | Where-Object { $_.scan -gt 0 })
"采样点总数: $($samples.Count)（每 $EveryN 条显示 1 条）"
"SAS_AlwaysScan.log 趋势分析：" | Write-Host
$samples |
	Where-Object { ($_.scan % $EveryN) -eq 0 } |
	Format-Table time, scan, refs, cand, sel, outl, scanAvg, scanMax, shapeAvg, loopAvg, loopMax, syncAvg, probes, reassert, un, sc -AutoSize |
	Out-String -Width 240

if ($Csv) {
	$rows | Export-Csv -LiteralPath $Csv -NoTypeInformation -Encoding UTF8
	"CSV -> $Csv"
}
