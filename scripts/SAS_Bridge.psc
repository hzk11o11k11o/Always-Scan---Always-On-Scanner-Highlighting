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
; ★ v2.4：本脚本现在还负责「面包屑引导路径」（见下面的 UpdateGuidePath）——
;   原版那条地上的线是扫描仪 HUD 画的，不举扫描仪就永远画不出来（docs/04 第八节），
;   所以改成自己沿「玩家 → 引导目标」铺一串地面光点。
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

; ---- v2.2：把脚本自己的状态回报给 DLL（DLL 打在主日志里，不用翻 Papyrus 日志）----
GlobalVariable Property GuideHb Auto Const Mandatory
	{ 心跳：SyncGuideSpell 每跑一次 +1。DLL 看它涨不涨就知道脚本活没活 }

GlobalVariable Property GuideState Auto Const Mandatory
	{ 引导法术状态：1=取不到法术形态 2=法术在但效果没生效(已尝试 Cast) 3=法术+效果都生效 }

; ---- v2.4：面包屑引导路径 ----
GlobalVariable Property SASOn Auto Const Mandatory
	{ DLL 写的 F8 开关状态（1/0）。0 = 连面包屑也一起收掉 }

GlobalVariable Property GuideMarkers Auto Const Mandatory
	{ 回报给 DLL 的「当前点亮了几个面包屑」：-1 = 开着但找不到引导目标 }

EffectShader Property ShaderPrimary Auto Const Mandatory
	{ SAS_HighlightFXS }

; ---- 内部状态（不是属性，不进存档）----
Bool  DrainBusy = False
Float DrainStartedAt = 0.0
Bool  LoadEventRegistered = False

; ============================================================================
; 任务引导路径（v2.1 新增）：把原版扫描仪画「地上那条线」用的法术加到玩家身上
; ----------------------------------------------------------------------------
; 调查结论（详见 docs/04-任务引导路径调研.md）：
;   原版 = 引擎在扫描期间给玩家 AddSpell 一个法术：
;       SPEL 0x0003CC96  SpellScannerGuide
;         └ MGEF 0x0003CC95  ScannerGuideEffect   ← 路径是这条 MGEF 自己画出来的
;   引擎是从 GMST/DObj `HandscannerGuideSpellDO`（DFOB 0x00112B0B）里取的这个法术，
;   全在 C++ 侧，Papyrus 库里没有任何脚本碰它。
;   本 MOD 不装备扫描仪 ⇒ 引擎永远不会加它 ⇒ 我们自己加。
;   ★ 路径的渲染是 magic effect 自己的事，**不需要扫描模式**，所以只要法术在身上，
;     只要存在「正在追踪的任务目标」就会画出来（没有追踪目标时自然什么都不画）。
;
; 与 F8 开关的关系：目前**无关**（F8 只管描边）。要联动需要让 DLL 输出一个状态 GLOB，
; 见 docs/04 的 TODO。
;
; ★ v2.2 的两次尝试（对应 docs/04 里「加上法术了但地上还是没线」的排查）：
;   ① 实测发现 `AddSpell` 之后 `HasMagicEffect` 为假 —— 这条 SPEL 的 ODTY=0
;      （是「Spell」不是「Ability」）、EFIT 的 duration 非 0，属于「发射即忘」型：
;      原版 `Spell.psc` 对这类法术的用法是 `Spell.Cast(施法者, 目标)`，
;      `AddSpell` 只会把它记进法术书、效果不会生效。所以补一次 Cast。
;   ② 加了心跳（GuideHb）与状态（GuideState）两个 GLOB，由 DLL 打到自己的日志里，
;      这样「脚本有没有在跑 / 法术装上了没有 / 效果生效了没有」三件事一次就能看清。
; ============================================================================
Bool  GuideApplied = False
Float GuideLastCastAt = 0.0

Int Function CfgGuideSpellFormID()
	{ 原版 SpellScannerGuide（含 ScannerGuideEffect） }
	Return 0x0003CC96
EndFunction

Int Function CfgGuideEffectFormID()
	{ ScannerGuideEffect：用来判断效果是否还活着 }
	Return 0x0003CC95
EndFunction

Function SyncGuideSpell()
	Actor p = Game.GetPlayer()
	if p == None
		return
	endif

	; 心跳：DLL 读这个值，涨 = 脚本在跑（不涨 = 脚本没绑上 / 计时器没起来）
	GuideHb.SetValueInt(GuideHb.GetValueInt() + 1)

	Spell s = Game.GetForm(CfgGuideSpellFormID()) as Spell
	if s == None
		GuideState.SetValueInt(1)
		return
	endif

	MagicEffect mgef = Game.GetForm(CfgGuideEffectFormID()) as MagicEffect
	Bool hasSpell = p.HasSpell(s)
	Bool hasEffect = False
	if mgef != None
		hasEffect = p.HasMagicEffect(mgef)
	endif

	if hasEffect
		GuideApplied = True
		GuideState.SetValueInt(3)
		return
	endif

	; ---- 到这里：法术没在玩家身上，或者效果没生效 ----
	if !hasSpell
		p.AddSpell(s, false)
		; Ability 型的话，AddSpell 完效果立刻就该在
		if mgef != None && p.HasMagicEffect(mgef)
			GuideApplied = True
			GuideState.SetValueInt(3)
			Debug.Trace("[SAS] guide: AddSpell 生效（Ability 型）")
			return
		endif
	endif

	; 法术在（或刚加完）但效果没生效 ⇒ 属于「发射即忘」型，必须真的 Cast 一次。
	; ★ 限频：Cast 是真的施法，别每 0.25 秒来一次。
	GuideApplied = True
	GuideState.SetValueInt(2)
	if (Utility.GetCurrentRealTime() - GuideLastCastAt) >= CfgReapplySecs()
		GuideLastCastAt = Utility.GetCurrentRealTime()
		s.Cast(p, p)
		Debug.Trace("[SAS] guide: 调用了 Spell.Cast(player, player)")
	endif
EndFunction

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

Float Function CfgReapplySecs()
	{ 引导法术「发射即忘」型时的重施间隔（秒）。小于效果时长即可保持不断 }
	Return 4.0
EndFunction

; ============================================================================
; 面包屑引导路径（v2.4 自己画，详见 docs/04 第七/八/九节）
; ----------------------------------------------------------------------------
; 为什么必须自己画：
;   原版地上那条线是**扫描仪 HUD（MonocleMenu）**画的。B 社自己的
;   ScanTempleScript.psc 就是 RegisterForMenuOpenCloseEvent("MonocleMenu")
;   + abOpening 来记「玩家是否举着扫描仪」（注释原话：this isn't going to work
;   for real - we'll need a way to check if you have the scanner up or not），
;   而 ScannerGuideEffect 只是把路径状态广播给那个 UI。
;   ⇒ 不举扫描仪 = 那个 HUD 根本不存在 = 线永远画不出来（不是没找对 API）。
;
; 这里的做法：沿「玩家 → 引导目标」铺一串**地面光点**。
;   1) 标注形态用原版 LIGH（默认 00012E5A GenPointFlare1，一个点光源）：
;      不用自造网格，放在地上就是一摊光 —— 和原版那条线一样是「发光的路标」。
;      想换观感只改 CfgGuideMarkerFormID()，不用动代码。
;   2) 贴地用 ObjectReference.MoveToNearestNavmeshLocation()：B 社自己的
;      TestNPCArenaScript.psc 就是把路径标记这样放上 navmesh 的，**不需要射线**。
;   3) 只在「间隔到点 且 玩家真的动了」时才重算（那是 navmesh 查询，别每帧跑）。
;   4) 每个标记 BlockActivation(true,true) —— 绝不抢玩家的交互（AGENTS.md 红线）。
;
; 局限（如实记录，不假装）：
;   Papyrus **没有**「玩家正在追踪哪一个目标」的查询接口，所以这里用的是
;   「所有 active quest 的当前阶段目标里、同一 cell 内最近的那个」。
;   要精确到「追踪中的那一个」需要 hooks/RE，留作后续。
; ============================================================================
ObjectReference[] GuideMarkerRefs
Bool  GuideArrayReady = False
ObjectReference GuideTarget = None
Float GuideTargetAt = 0.0
Float GuideLastUpdateAt = 0.0
Float GuideLastPX = 0.0
Float GuideLastPY = 0.0
Float GuideLastPZ = 0.0
Bool  ScannerUp = False

Int Function CfgGuideMarkerCount()
	{ 面包屑个数。8 个 x 5 米 ≈ 覆盖身前 40 米 }
	Return 8
EndFunction

Float Function CfgGuideSpacing()
	{ 相邻两个面包屑的间距（游戏单位）。5 米 x 3.4286 }
	Return 5.0 * 3.4286
EndFunction

Float Function CfgGuideStartDist()
	{ 第一个面包屑离玩家多远（游戏单位）。太近会糊在脚下 }
	Return 3.0 * 3.4286
EndFunction

Float Function CfgGuideHeight()
	{ 标记先放到玩家脚底上方这么高，再由 MoveToNearestNavmeshLocation 贴地 }
	Return 2.0 * 3.4286
EndFunction

Float Function CfgGuideMaxDist()
	{ 超过这个距离的引导目标就不画（游戏单位）。120 米 }
	Return 120.0 * 3.4286
EndFunction

Float Function CfgGuideInterval()
	{ 引导路径最短重算间隔（秒） }
	Return 0.5
EndFunction

Float Function CfgGuideMoveThreshold()
	{ 玩家相对上次重算移动超过这个距离才重算（游戏单位）。1.5 米 }
	Return 1.5 * 3.4286
EndFunction

Float Function CfgGuideTargetRefresh()
	{ 引导目标重新检索间隔（秒）。检索要遍历所有 active quest，别太频 }
	Return 2.0
EndFunction

Int Function CfgGuideMarkerFormID()
	{ 面包屑形态：原版 LIGH GenPointFlare1（点光源） }
	Return 0x00012E5A
EndFunction

Function EnsureGuideArray()
	If !GuideArrayReady
		GuideMarkerRefs = new ObjectReference[CfgGuideMarkerCount()]
		GuideArrayReady = True
	EndIf
EndFunction

Function HideGuideMarkers()
	If !GuideArrayReady
		Return
	EndIf
	Int i = 0
	While i < GuideMarkerRefs.Length
		ObjectReference m = GuideMarkerRefs[i]
		If m != None
			m.Disable()
		EndIf
		i = i + 1
	EndWhile
EndFunction

ObjectReference Function CreateGuideMarker(Actor p)
	Form f = Game.GetForm(CfgGuideMarkerFormID())
	If f == None
		Return None
	EndIf
	; abForcePersist=True / abInitiallyDisabled=True / abDeleteWhenAble=False
	ObjectReference m = p.PlaceAtMe(f, 1, True, True, False)
	If m == None
		Return None
	EndIf
	m.BlockActivation(True, True)
	Return m
EndFunction

; 引导目标：所有 active quest 的「当前阶段目标」里，同一 cell 内最近的那个
ObjectReference Function FindGuideTarget(Actor p, Float px, Float py, Float pz)
	ObjectReference best = None
	Float bestD = CfgGuideMaxDist()
	Cell myCell = p.GetParentCell()
	Quest[] qs = Game.GetPlayerActiveQuests()
	Int qi = 0
	While qi < qs.Length
		Quest q = qs[qi]
		If q != None
			ObjectReference[] ts = q.GetCurrentStageTargets()
			Int ti = 0
			While ti < ts.Length
				ObjectReference t = ts[ti]
				If t != None
					If !t.IsDeleted() && !t.IsDisabled() && t.GetParentCell() == myCell
						Float ddx = t.GetPositionX() - px
						Float ddy = t.GetPositionY() - py
						Float ddz = t.GetPositionZ() - pz
						Float d2 = ddx * ddx + ddy * ddy + ddz * ddz
						If d2 < bestD * bestD
							bestD = Math.sqrt(d2)
							best = t
						EndIf
					EndIf
				EndIf
				ti = ti + 1
			EndWhile
		EndIf
		qi = qi + 1
	EndWhile
	Return best
EndFunction

Function UpdateGuidePath()
	Actor p = Game.GetPlayer()
	If p == None
		Return
	EndIf
	EnsureGuideArray()

	; F8 关掉时，面包屑一起收掉
	If SASOn.GetValueInt() == 0
		HideGuideMarkers()
		GuideMarkers.SetValueInt(0)
		Return
	EndIf

	; 玩家自己举着扫描仪时原版会画，别画两条
	If ScannerUp
		HideGuideMarkers()
		GuideMarkers.SetValueInt(0)
		Return
	EndIf

	Float now = Utility.GetCurrentRealTime()
	If GuideLastUpdateAt > 0.0 && (now - GuideLastUpdateAt) < CfgGuideInterval()
		Return
	EndIf

	Float px = p.GetPositionX()
	Float py = p.GetPositionY()
	Float pz = p.GetPositionZ()

	; 玩家没怎么动 + 刚算过 ⇒ 什么都不用做
	If GuideLastUpdateAt > 0.0
		Float mvx = px - GuideLastPX
		Float mvy = py - GuideLastPY
		Float mvz = pz - GuideLastPZ
		Float thr = CfgGuideMoveThreshold()
		If (mvx * mvx + mvy * mvy + mvz * mvz) < thr * thr
			Return
		EndIf
	EndIf

	GuideLastUpdateAt = now
	GuideLastPX = px
	GuideLastPY = py
	GuideLastPZ = pz

	; 目标检索：缓存 2 秒，失效（被打掉/关掉/换 cell）立刻重查
	Bool needSearch = (GuideTarget == None) || ((now - GuideTargetAt) > CfgGuideTargetRefresh())
	If !needSearch
		If GuideTarget.IsDeleted() || GuideTarget.IsDisabled() || GuideTarget.GetParentCell() != p.GetParentCell()
			needSearch = True
		EndIf
	EndIf
	If needSearch
		GuideTarget = FindGuideTarget(p, px, py, pz)
		GuideTargetAt = now
	EndIf

	If GuideTarget == None
		HideGuideMarkers()
		GuideMarkers.SetValueInt(-1)
		Return
	EndIf

	Float dx = GuideTarget.GetPositionX() - px
	Float dy = GuideTarget.GetPositionY() - py
	Float dist = Math.sqrt(dx * dx + dy * dy)
	Float start = CfgGuideStartDist()
	Float spacing = CfgGuideSpacing()
	If dist <= (start + spacing)
		HideGuideMarkers()
		GuideMarkers.SetValueInt(0)
		Return
	EndIf

	Float ux = dx / dist
	Float uy = dy / dist
	Float reach = dist - spacing * 0.4
	Cell  myCell = p.GetParentCell()

	Int shown = 0
	Int i = 0
	While i < GuideMarkerRefs.Length
		Float d = start + spacing * i
		ObjectReference m = GuideMarkerRefs[i]
		If d > reach
			If m != None
				m.Disable()
			EndIf
		Else
			If m == None
				m = CreateGuideMarker(p)
				GuideMarkerRefs[i] = m
			EndIf
			If m != None
				If m.GetParentCell() != myCell
					m.MoveTo(p)
				EndIf
				m.Enable()
				m.SetPosition(px + ux * d, py + uy * d, pz + CfgGuideHeight())
				m.MoveToNearestNavmeshLocation()
				shown = shown + 1
			EndIf
		EndIf
		i = i + 1
	EndWhile
	GuideMarkers.SetValueInt(shown)
EndFunction

; ============================================================================
; 事件
; ============================================================================
Event OnInit()
	StartTimer(CfgInterval(), 1)
	RegisterForMenuOpenCloseEvent("MonocleMenu")
EndEvent

Event OnQuestInit()
	StartTimer(CfgInterval(), 1)
	RegisterLoadEvent()
	RegisterForMenuOpenCloseEvent("MonocleMenu")
EndEvent

; 原版扫描仪 HUD 的开关状态。B 社自己的脚本就是这么判断「有没有举着扫描仪」的
; （见 docs/04 8.1）。举着的时候由原版画线，我们收掉自己的面包屑，免得两条重叠。
Event OnMenuOpenCloseEvent(string asMenuName, bool abOpening)
	If asMenuName == "MonocleMenu"
		ScannerUp = abOpening
	EndIf
EndEvent

Event OnTimer(int aiTimerID)
	StartTimer(CfgInterval(), 1)
	DrainGuarded()
	SyncGuideSpell()
	UpdateGuidePath()
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
	; 面包屑：读档后位置全变了，强制重算一次并忘掉旧目标
	GuideTarget = None
	GuideTargetAt = 0.0
	GuideLastUpdateAt = 0.0
	ScannerUp = False
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
