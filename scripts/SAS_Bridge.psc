Scriptname SAS_Bridge extends Quest

; ============================================================================
; Starfield Always Scan —— 「视觉桥」
;
; 这个脚本只做一件事：把 SFSE 插件（SAS_AlwaysScan.dll）塞进 FormList 里的引用
; 逐个 Play / Stop 掉。**扫描、过滤、挑目标、开关状态全在原生插件里**。
;
; 通道协议（与 plugin/src/AlwaysScan.cpp 顶部注释一一对应）：
;   SAS_OpList    (FLST) ← DLL 追加「该亮」的引用
;   SAS_OpCursor  (GLOB) → 本脚本发布「已处理到第几个」
;   SAS_StopList  (FLST) ← DLL 追加「该灭」的引用（只在关掉开关时用）
;   SAS_StopCursor(GLOB) → 同上
;   SAS_Epoch     (GLOB) → 读档时 +1，DLL 看到变化就整批重置
;   DLL 只在 cursor == size（**相等**判定）时才清表。
;
; ★★ 三条「不要再踩」的硬约束（全部来自上一代项目的实测崩溃/卡死现场）★★
;
; ① **游标必须早于 Play/Stop 推进**。
;    `EffectShader.Play` 会把 Papyrus 栈挂起（实测单条 22~280ms，VM 忙时到秒级），
;    而 Papyrus 的事件**不排队**：OnTimer 每 0.25 秒又会起一个新栈。
;    如果进度只活在「函数返回值」里，新栈读到旧值 → 从同一条重读 → 同一批物件
;    被反复 Play → 栈数无上限 → `VM is frozen` → 主线程停 10 秒。
;    所以循环里每次现读游标、**取走一条立刻写回**，然后才 Play。
;
; ② **单实例守卫**（DrainBusy）：同一时刻只允许一个 Drain 在跑，
;    彻底掐断「Play 挂起期间又叠新栈」这条路。
;    另留**逃生门**（CfgStallSecs）：上一轮卡死超过 5 秒就强行接管 ——
;    心跳必须能自愈（曾经因为「OnTimer 里 StartTimer 排在重活之后」永久哑掉）。
;
; ③ **先 re-arm 再干活**：OnTimer 第一件事就是排下一次计时器。
;
; 另外：配置项全部写成**函数**而不是 Auto 属性 —— Auto 属性会被存档持久化，
; 改 pex 里的默认值对老存档无效（上一代踩过这个坑）。
;
; ============================================================================
; ★ v3.1：任务引导线功能**整体移除**（自制那条 + 「原版法术」那条，都不要了）
; ----------------------------------------------------------------------------
; 自制那条（v2.4 ~ v3.0）：曾经沿「玩家 → 引导目标」铺一串地面光点（最后是
; 20 颗密排珠子的「世界锚定滚动光带」）。多轮实测证明它又麻烦又不好看：
;   · 形态换成带网格的 MSTT 才看得见（LIGH 是纯光源，白天没有可见网格）；
;   · SetPosition / MoveToNearestNavmeshLocation 在 Starfield 里是**延迟函数**
;     （GameScript::DelayFunctor），摆完立刻读坐标拿到的是旧值 ⇒ 珠子飘在半空；
;   · 一帧里重摆整条链 = 周期性硬卡顿，改成滚动光带也只是「勉强能看」。
; 原版那条（v2.1 ~ v2.3 的 SyncGuideSpell）：把原版 SpellScannerGuide 法术挂在
; 玩家身上。v2.3 已查明**路径的渲染在扫描仪 HUD（MonocleMenu）侧** —— 不举扫描仪
; 时那个 HUD 根本不存在，法术挂在身上也不会有地上的线（引擎侧门控；MGEF 本身
; 没有任何 CTDA 条件，见 docs/04 第七节）。既然这条路必然要求举着扫描仪
; （那就该由原版自己画），留着它只会每 4 秒空施一次法 ⇒ 一并删除。
;
; ★ 现在本脚本只做一件事：Drain() —— 把 DLL 的 Play / Stop 信箱排空
;   （只服务 HighlightMode=0 的旧方案）。
;   引导线相关的 GLOB（SAS_GuideHb/GuideState/On/GuideMarkers）、脚本属性与
;   ESM 记录（0x807~0x80B）全部移除。
; 全过程与结论见 docs/04 第十二 ~ 十五节。
; ============================================================================

FormList Property OpList Auto Const Mandatory
	{ 点亮信箱：DLL 往这里追加「该亮」的引用 }

GlobalVariable Property OpCursor Auto Const Mandatory
	{ 本脚本已处理到 OpList 的第几个（下标） }

FormList Property StopList Auto Const Mandatory
	{ 熄灭信箱：只在玩家关掉开关时 DLL 才往这里追加 }

GlobalVariable Property StopCursor Auto Const Mandatory

GlobalVariable Property Epoch Auto Const Mandatory
	{ 读档计数器：变化 = 让 DLL 全部重放 }

EffectShader Property ShaderPrimary Auto Const Mandatory
	{ SAS_HighlightFXS（只服务 HighlightMode=0 的旧方案） }

; ---- 内部状态（不是属性，不进存档）----
Bool  DrainBusy = False
Float DrainStartedAt = 0.0
Bool  LoadEventRegistered = False

; ============================================================================
; 可调参数（函数形式，随时改随时生效）
; ============================================================================
Float Function CfgInterval()
	Return 0.25
EndFunction

Int Function CfgMaxPerDrain()
	{ 单次心跳最多处理几条（Play 是**连续**占用主线程的，3 条最坏 ≈ 840ms） }
	Return 3
EndFunction

Float Function CfgGlowDurationSecs()
	{ 每次 Play 的时长。必须与 DLL 的 GlowDurationSecs 一致 } 
	Return 90.0
EndFunction

Float Function CfgStallSecs()
	Return 5.0
EndFunction

; ============================================================================
; ★ v3.1：老存档遗留珠子的一次性清理
; ----------------------------------------------------------------------------
; v2.4 ~ v3.0 的面包屑标记是用 `PlaceAtMe(abForcePersist=True, abDeleteWhenAble=False)`
; 建的 —— 它们是**跟着存档走的常驻引用**。功能删掉之后，老存档里那些珠子会
; 一直躺在地上（脚本不再管它们了）。
;
; 所以这里保留两个**老变量（名字必须一模一样，Papyrus 是按变量名从存档恢复的）**：
;   GuideMarkerRefs / GuideArrayReady —— 只当「这个存档有没有留下珠子」的标记用。
; 判定：老版本的 EnsureGuideArray 建完池子会把 GuideArrayReady 置 True；
;       新存档里它永远是 False（没有任何代码会把它置 True）⇒ 这条路径自动跳过。
; 清理完成（或玩家还没进世界时先跳过）后把它置 False，只执行一次。
; ============================================================================
ObjectReference[] GuideMarkerRefs
Bool  GuideArrayReady = False

Function CleanupLegacyGuideBeads()
	If !GuideArrayReady
		Return
	EndIf
	; 玩家还没进世界（载入中）时先不动手，下一轮再说
	Actor p = Game.GetPlayer()
	If p == None || p.GetParentCell() == None
		Return
	EndIf
	; 先落守卫：哪怕后面出什么岔子也不会重复执行
	GuideArrayReady = False
	Int n = GuideMarkerRefs.Length
	Int i = 0
	While i < n
		ObjectReference old = GuideMarkerRefs[i]
		If old != None
			old.Disable()
			old.Delete()
		EndIf
		i = i + 1
	EndWhile
	GuideMarkerRefs = new ObjectReference[0]
	Debug.Trace("[SAS] legacy: 已清理旧版面包屑标记 " + n + " 个")
EndFunction

; ============================================================================
; 事件
; ============================================================================
Event OnInit()
	StartTimer(CfgInterval(), 1)
EndEvent

Event OnQuestInit()
	StartTimer(CfgInterval(), 1)
	RegisterLoadEvent()
EndEvent

Event OnTimer(int aiTimerID)
	StartTimer(CfgInterval(), 1)
	CleanupLegacyGuideBeads()
	DrainGuarded()
EndEvent

; 读档自愈：Quest 自身没有 OnPlayerLoadGame，必须用远事件订阅（上一代验证过）
Function RegisterLoadEvent()
	if LoadEventRegistered
		return
	endif
	Actor p = Game.GetPlayer()
	if p != None
		RegisterForRemoteEvent(p, "OnPlayerLoadGame")
		LoadEventRegistered = True
	endif
EndFunction

Event Actor.OnPlayerLoadGame(Actor akSender)
	Epoch.SetValueInt(Epoch.GetValueInt() + 1)
	OpCursor.SetValueInt(0)
	StopCursor.SetValueInt(0)
	LoadEventRegistered = False
	RegisterLoadEvent()
EndEvent

; ============================================================================
; 排空
; ============================================================================
Function DrainGuarded()
	Float now = Utility.GetCurrentRealTime()
	if DrainBusy
		if (now - DrainStartedAt) < CfgStallSecs()
			return
		endif
		; 逃生门：上一轮挂了太久，强行接管
	endif

	DrainBusy = True
	DrainStartedAt = now
	Drain()
	DrainBusy = False
EndFunction

Function Drain()
	Int budget = CfgMaxPerDrain()
	Int used = 0
	Int i = 0
	Int size = 0
	Int cur = 0
	ObjectReference r = None

	; ---- 1) 熄灭通道（只有关掉开关时才会有内容）----
	size = StopList.GetSize()
	cur = StopCursor.GetValueInt()
	while cur < size && used < budget
		r = StopList.GetAt(cur) as ObjectReference
		cur = cur + 1
		StopCursor.SetValueInt(cur)
		if r != None
			ShaderPrimary.Stop(r)
		endif
		used = used + 1
	endWhile

	; ---- 2) 点亮通道 ----
	size = OpList.GetSize()
	cur = OpCursor.GetValueInt()
	while cur < size && used < budget
		r = OpList.GetAt(cur) as ObjectReference
		cur = cur + 1
		OpCursor.SetValueInt(cur)
		if r != None
			ShaderPrimary.Play(r, CfgGlowDurationSecs())
		endif
		used = used + 1
	endWhile
EndFunction
