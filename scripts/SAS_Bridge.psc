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
	DrainGuarded()
	SyncGuideSpell()
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
