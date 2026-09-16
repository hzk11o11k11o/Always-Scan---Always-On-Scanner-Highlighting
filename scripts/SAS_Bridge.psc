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
; ★ v2.8：光点形态从「纯光源 LIGH」换成「带发光网格的 MSTT GlowBall10x10」——
;   v2.7 实测 markers 有数但玩家看不到，根因是 LIGH 记录**没有 MODL（网格）**，
;   白天室外就是一摊看不见的光。详见 CfgGuideMarkerFormID() 的注释。
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

FormList Property DoorBases Auto Const Mandatory
	{ ★ v2.6：master 里**所有** DOOR 基础记录（构建时离线打包，339 项）。
	  用途见下面的 FindGuideDoor —— 目标在别的 cell 时，把面包屑指向「通往它的门」。
	  为什么能这么用：`ObjectReference.FindAllReferencesOfType(Form akObjectOrList, float)`
	  的注释原文是 "objects in the given list" ⇒ 接受 FormList。 }

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
;   1) 标注形态用原版 MSTT（★ v2.8 起默认 00098106 GlowBall10x10，一个 0.1 米的
;      **发光球网格** Effects\Ambient\GlowBall10x10.nif，再 SetScale 放大）：
;      v2.4~v2.7 用的 LIGH 0x00012E5A 是**纯光源、没有网格**，白天室外看不见
;      （这是 v2.7 实测「markers 有数但什么都没有」的根因）。想换观感只改
;      CfgGuideMarkerFormID()（候选都列在那个函数里），不用动代码。
;   2) 贴地用 ObjectReference.MoveToNearestNavmeshLocation()：B 社自己的
;      TestNPCArenaScript.psc 就是把路径标记这样放上 navmesh 的，**不需要射线**；
;      贴完再抬 CfgGuideMarkerLift()（球心在原点，不抬就一半埋地里）。
;   3) 只在「间隔到点 且 玩家真的动了」时才重算（那是 navmesh 查询，别每帧跑）。
;   4) 每个标记 BlockActivation(true,true) + SetMotionType(Keyframed) —— 不抢交互、
;      也不会被物理引擎拖走（MSTT 默认是 Dynamic，会滚）。
;
; 局限（如实记录，不假装）：
;   Papyrus **没有**「玩家正在追踪哪一个目标」的查询接口，所以这里用的是
;   「所有 active quest 的当前阶段目标里、同一 cell 内最近的那个」。
;   要精确到「追踪中的那一个」需要 hooks/RE，留作后续。
; ============================================================================
ObjectReference[] GuideMarkerRefs
Bool  GuideArrayReady = False
Int   GuideMarkerFormVer = 0
ObjectReference GuideTarget = None
Float GuideTargetAt = 0.0
Float GuideLastUpdateAt = 0.0
Float GuideLastPX = 0.0
Float GuideLastPY = 0.0
Float GuideLastPZ = 0.0
Bool  ScannerUp = False

; ★ v2.5：诊断（定位「guideMarkers 恒为 -1 = 找不到目标」）——
;   脚本把「看到了哪些任务 / 目标在哪 / 为什么被跳过」写进 Papyrus 日志，
;   不再靠猜。定位完成后把 CfgGuideDebug() 改成 False 即可（日志会安静下来）。
ObjectReference GuideOtherCellTarget = None
Float GuideOtherCellDist = 0.0
Float GuideDiagAt = 0.0
Float GuidePaintDiagAt = 0.0
Float GuideDoorDiagAt = 0.0
; ★ v2.8：绘制诊断单独一个时间戳。以前和 GuidePaintDiagAt 共用，
;   而「目标在别的 cell ⇒ 指向门」那行会先把它刷掉 ⇒ 同一轮里的绘制行永远打不出来
;   （v2.7 实测里就看不到任何 markers 的位置信息）。
Float GuidePaintPosDiagAt = 0.0
Bool  GuideTargetIsProxy = False

Int Function CfgGuideMarkerCount()
	{ 面包屑个数。8 个 x 3 米 ≈ 覆盖身前 24 米 }
	Return 8
EndFunction

Float Function CfgGuideSpacing()
	{ 相邻两个面包屑的间距（游戏单位）。★ v2.7：5m → 3m
	  （代理目标是门时距离往往只有几米，5m 间距会把整段路遮没） }
	Return 3.0 * 3.4286
EndFunction

Float Function CfgGuideStartDist()
	{ 第一个面包屑离玩家多远（游戏单位）。★ v2.7：3m → 1.5m }
	Return 1.5 * 3.4286
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
	{ 面包屑形态。
	  ★ v2.8：从「LIGH 0x00012E5A GenPointFlare1」换成
	    「MSTT 0x00098106 GlowBall10x10」（网格 Effects\Ambient\GlowBall10x10.nif）。
	  为什么（v2.7 实测）：markers 有数（2/3/8）但玩家什么都看不到。
	  把两个形态的原始记录摊开对比就明白了（tools/re/esmrec.py --formid）：
	    LIGH 0x00012E5A 只有 EDID/OBND/ODTY/FLLD/DAT2/FLBD/FLRD/FLGD/LLLD/FLAD/FVLD，
	    **没有 MODL（网格）** —— 它是纯光源，白天室外几乎不可见（夜里才看得见地上被照亮）。
	    MSTT 0x00098106 有 MODL = 一个 0.1 米的发光球（自带自发光材质）⇒ 白天也看得见。
	  其它候选（都带可见网格，改这一行即可换）：
	    0x00098105 GlowCube10x10（方块）
	    0x0009811C GlowDisc10x10（扁平圆盘，贴地最像原版那种地上小点）
	    0x0001760F GlowLightCone36（36 单位的光锥，很大，别用）
	  旧值（已废，留档）：0x00012E5A LIGH GenPointFlare1 }
	Return 0x00098106
EndFunction

Float Function CfgGuideMarkerScale()
	{ 面包屑缩放。网格本身只有 0.1 米，3 倍 = 0.3 米的发光球
	  （第一人称在几米外就是一串清清楚楚的光点） }
	Return 3.0
EndFunction

Float Function CfgGuideMarkerLift()
	{ 贴地之后再抬高这么多（游戏单位）。球的球心在原点 ⇒ 不抬就有一半埋在地面里 }
	Return 0.25 * 3.4286
EndFunction

Float Function CfgGuideDoorRadius()
	{ ★ v2.6：找「通往目标 cell 的门」时在玩家周围搜多大（游戏单位）。60 米 }
	Return 60.0 * 3.4286
EndFunction

Int Function CfgLoadDoorKeywordFormID()
	{ ★ v2.7：`IsLoadDoor` 关键字（KYWD 0x002CF614）。
	  离线证据（tools/xedit-scripts/dump_ref_paths.pas 跑出来的 XTEL 结构）：
	  The Rock 里的储物柜门 / 冰柜门和「真正的传送门」是同一个 DOOR 大类的
	  不同 base，唯一区别就是 load door 的 base 带这个关键字
	  （AK_Ext_Bld_WallA_DoorC_Load_02 的 KWDA = [AK_Door_Swap_key, IsLoadDoor]）。
	  所以找到门之后用 HasKeyword 再筛一道，就能把「储物柜门」全排除掉。 }
	Return 0x002CF614
EndFunction

; ★ v2.5 诊断开关。True 时把「目标检索」的每一步写进 Papyrus 日志
; （<我的文档>\My Games\Starfield\Logs\Script\Papyrus.0.log，搜 "[SAS]"）。
; 它每 5 秒最多打一屏，跑几分钟足够定位；定位完改回 False。
Bool Function CfgGuideDebug()
	Return True
EndFunction

String Function FormHex(Form f)
	{ FormID 的十六进制文本（None 直接给 "None"），只为日志好看 }
	If f == None
		Return "None"
	EndIf
	Return Utility.IntToHex(f.GetFormID())
EndFunction

Bool Function SameSpace(Cell a, Cell b)
	{ 两个引用的坐标是否在同一个坐标系里（能不能算距离 / 画面包屑）。
	  同一个 cell ⇒ 能；
	  两个都是 exterior cell ⇒ 共享 worldspace 坐标（跨 cell 也能）；
	  其余（跨 interior cell）⇒ 不能，只能靠原版图标。 }
	If a == None || b == None
		Return False
	EndIf
	If a == b
		Return True
	EndIf
	Return !a.IsInterior() && !b.IsInterior()
EndFunction

Function EnsureGuideArray()
	If !GuideArrayReady
		GuideMarkerRefs = new ObjectReference[CfgGuideMarkerCount()]
		GuideArrayReady = True
	EndIf
	; ★ v2.8：形态换过就得把老标记全销毁重建。
	;   脚本变量（含 GuideMarkerRefs）是**跟着存档走的** —— 用户读档后数组里还握着
	;   v2.7 那些「看不见的 LIGH」引用，而绘制循环只在 m == None 时才新建
	;   ⇒ 换了 CfgGuideMarkerFormID() 也会一直用旧形态（就是「改了却没变化」的坑）。
	If GuideMarkerFormVer != CfgGuideMarkerFormID()
		DestroyGuideMarkers()
		GuideMarkerFormVer = CfgGuideMarkerFormID()
		Debug.Trace("[SAS] guide: 面包屑形态变更 -> 旧标记全部销毁重建（form=" + FormHex(Game.GetForm(CfgGuideMarkerFormID())) + "）")
	EndIf
EndFunction

Function DestroyGuideMarkers()
	If !GuideArrayReady
		Return
	EndIf
	Int i = 0
	While i < GuideMarkerRefs.Length
		ObjectReference old = GuideMarkerRefs[i]
		If old != None
			old.Disable()
			old.Delete()
			GuideMarkerRefs[i] = None
		EndIf
		i = i + 1
	EndWhile
	GuideMarkers.SetValueInt(0)
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
	; ★ v2.8：MSTT 是「可移动静态」——默认是 Dynamic，会被物理引擎接管
	;   （落体 / 被玩家踢飞 / 顺坡滚走）。Keyframed = 只认脚本摆的位置，位置才稳得住。
	;   注意常量要写成 `m.Motion_Keyframed`（它是 ObjectReference 上的属性）——
	;   直接写 `Motion_Keyframed` 编译报 "variable Motion_Keyframed is undefined"。
	m.SetMotionType(m.Motion_Keyframed, True)
	; 形态本身只有 0.1 米，放大到看得见
	m.SetScale(CfgGuideMarkerScale())
	If CfgGuideDebug()
		Debug.Trace("[SAS] guide: 造出标记 " + FormHex(m) + " form=" + FormHex(f) + " scale=" + CfgGuideMarkerScale())
	EndIf
	Return m
EndFunction

; 引导目标：所有 active quest 的「当前阶段目标」里，同一 cell 内最近的那个。
; ★ v2.5 起：
;   - **优先取「玩家在任务菜单里追踪的那个任务」（Quest.IsActive()）**的目标
;     —— 它的注释原文就是 "Is this quest 'active' (tracked by the player)?"；
;   - 找不到同 cell 的时，把「最近的跨 cell 目标」记进 GuideOtherCellTarget，
;     调用方据此区分「根本没有目标」和「目标在别的 cell」；
;   - 诊断（CfgGuideDebug）把每一步都写进 Papyrus 日志。
ObjectReference Function FindGuideTarget(Actor p, Float px, Float py, Float pz)
	ObjectReference best = None
	ObjectReference bestAny = None
	ObjectReference bestTracked = None
	Float bestD = CfgGuideMaxDist()
	Float bestAnyD = 1000000.0
	Float bestTrackedD = CfgGuideMaxDist()
	Cell myCell = p.GetParentCell()
	Quest[] qs = Game.GetPlayerActiveQuests()
	Bool dbg = CfgGuideDebug() && ((Utility.GetCurrentRealTime() - GuideDiagAt) > 5.0)
	If dbg
		GuideDiagAt = Utility.GetCurrentRealTime()
		Debug.Trace("[SAS] guide/diag: activeQuests=" + qs.Length + " myCell=" + FormHex(myCell))
	EndIf
	Int qi = 0
	While qi < qs.Length
		Quest q = qs[qi]
		If q != None
			ObjectReference[] ts = q.GetCurrentStageTargets()
			Bool tracked = q.IsActive()
			If dbg && (ts.Length > 0 || tracked)
				Debug.Trace("[SAS] guide/diag:  q=" + FormHex(q) + " stage=" + q.GetStage() + " targets=" + ts.Length + " tracked=" + tracked)
			EndIf
			Int ti = 0
			While ti < ts.Length
				ObjectReference t = ts[ti]
				If t != None
					Float dx = t.GetPositionX() - px
					Float dy = t.GetPositionY() - py
					Float dz = t.GetPositionZ() - pz
					Float d = Math.sqrt(dx * dx + dy * dy + dz * dz)
					Cell tc = t.GetParentCell()
					Bool same = SameSpace(tc, myCell)
					Bool dead = t.IsDeleted() || t.IsDisabled()
					If dbg
						Debug.Trace("[SAS] guide/diag:    t=" + FormHex(t) + " d=" + Math.Floor(d / 3.4286) + "m same=" + same + " dead=" + dead + " cell=" + FormHex(tc))
					EndIf
					If !dead
						If d < bestAnyD
							bestAnyD = d
							bestAny = t
						EndIf
						If same
							If tracked && d < bestTrackedD
								bestTrackedD = d
								bestTracked = t
							EndIf
							If d < bestD
								bestD = d
								best = t
							EndIf
						EndIf
					EndIf
				EndIf
				ti = ti + 1
			EndWhile
		EndIf
		qi = qi + 1
	EndWhile

	If dbg
		Debug.Trace("[SAS] guide/diag:  -> sameCell=" + FormHex(best) + " trackedSame=" + FormHex(bestTracked) + " anyCell=" + FormHex(bestAny) + " anyD=" + Math.Floor(bestAnyD / 3.4286) + "m")
	EndIf

	If bestTracked != None
		GuideOtherCellTarget = None
		GuideOtherCellDist = 0.0
		Return bestTracked
	EndIf
	If best != None
		GuideOtherCellTarget = None
		GuideOtherCellDist = 0.0
		Return best
	EndIf
	If bestAny != None
		GuideOtherCellTarget = bestAny
		GuideOtherCellDist = bestAnyD
	Else
		GuideOtherCellTarget = None
		GuideOtherCellDist = 0.0
	EndIf
	Return None
EndFunction

; ============================================================================
; ★ v2.6：目标在别的 cell ⇒ 指向「通往它的门」
; ----------------------------------------------------------------------------
; 为什么需要：Papyrus 拿到的目标引用如果不在玩家所在的坐标空间（实测最常见的是
; **飞船内部** —— 主线 MQ305「一大步」的目标「在你的飞船上建造天体仪」就是
; `FF01E9CB @ cell FF01D4DE`、d=605m，两个 interior cell 的局部坐标根本不可比），
; 那光靠坐标一个点都画不出来。
;
; 兜底逻辑（分级，越靠前越准）：
;   ① 门的目标 cell == 任务目标的 cell      ⇒ 精确（比如飞船的舱门）
;   ② 任意「通往 exterior 的门」           ⇒ 方向对（先出门；出门后玩家换了 cell，
;      下一轮重新算，于是自然形成「多跳指引」）
;
; 用 FindAllReferencesOfType + 离线打包的 SAS_DoorBases（master 里全部 339 个
; DOOR base）实现 —— Papyrus **没有**「遍历 cell 引用」的 API，这是唯一能拿到门的办法。
; ============================================================================
ObjectReference Function FindGuideDoor(Actor p, Float px, Float py, Float pz, Cell targetCell)
	ObjectReference best = None
	ObjectReference bestOut = None
	ObjectReference bestLoad = None
	Float bestD = 0.0
	Float bestOutD = 0.0
	Float bestLoadD = 0.0
	If targetCell == None || DoorBases == None
		Return None
	EndIf
	Cell myCell = p.GetParentCell()
	Bool iAmIndoor = (myCell != None) && myCell.IsInterior()
	Float radius = CfgGuideDoorRadius()

	Bool dbg = CfgGuideDebug() && ((Utility.GetCurrentRealTime() - GuideDoorDiagAt) >= 5.0)
	If dbg
		GuideDoorDiagAt = Utility.GetCurrentRealTime()
		Debug.Trace("[SAS] door/diag: myCell=" + FormHex(myCell) + " indoor=" + iAmIndoor + " target=" + FormHex(targetCell) + " pRef=" + FormHex(targetCell.GetParentRef()) + " r=" + Math.Floor(radius / 3.4286) + "m doorsList=" + FormHex(DoorBases))
	EndIf

	; ★ ① 先试「上一层引用」：目标 cell 若是飞船/载具内部，`Cell.GetParentRef()` 就是
	;   它在世界里的那个引用（B 社注释原文：the ship if this is a ship interior cell）。
	;   玩家与它在同一个坐标空间时（比如都在阿基拉城的室外），直接指过去最准。
	ObjectReference pr = targetCell.GetParentRef()
	If pr != None && SameSpace(pr.GetParentCell(), myCell)
		Return pr
	EndIf

	; ★ v2.7：找门改成「三个 API 依次降级」（v2.6 只有一个，而它什么都没找到 ——
	;   下面这些日志就是用来一次性钉死「到底哪个 API 好使」的）：
	;     api1 = ObjectReference.FindAllReferencesOfType(FormList, r)（最理想：一次拿全部）
	;     api2 = Game.FindClosestReferenceOfAnyTypeInListFromRef(FormList, ref, r)
	;            （签名就是 FormList，一定能接受列表；只给最近的一个）
	;     api3 = ObjectReference.FindAllReferencesWithKeyword(Keyword, r)
	Int api = 0
	ObjectReference[] doors = p.FindAllReferencesOfType(DoorBases, radius)
	If doors.Length > 0
		api = 1
	EndIf
	If dbg
		Debug.Trace("[SAS] door/diag:  api1 FindAllReferencesOfType(List,r) -> " + doors.Length)
	EndIf
	If api == 0
		ObjectReference one = Game.FindClosestReferenceOfAnyTypeInListFromRef(DoorBases, p, radius)
		If one != None
			doors = new ObjectReference[1]
			doors[0] = one
			api = 2
		EndIf
		If dbg
			Debug.Trace("[SAS] door/diag:  api2 FindClosestRefOfAnyTypeInListFromRef -> " + FormHex(one))
		EndIf
	EndIf
	Keyword lkw = Game.GetForm(CfgLoadDoorKeywordFormID()) as Keyword
	If api == 0 && lkw != None
		ObjectReference[] kwDoors = p.FindAllReferencesWithKeyword(lkw, radius)
		If kwDoors.Length > 0
			doors = kwDoors
			api = 3
		EndIf
		If dbg
			Debug.Trace("[SAS] door/diag:  api3 FindAllReferencesWithKeyword(IsLoadDoor,r) -> " + kwDoors.Length)
		EndIf
	EndIf

	; ★ ② 优先级：通往目标 cell 的门 > 通往室外的门（玩家在室内时）> 最近的 load door
	;   - 前两条依赖 `GetTeleportCell()`；
	;   - 最后一条不依赖它（用 IsLoadDoor 关键字认定），是「Papyrus 门系统跟我们想的不一样」时的
	;     硬兜底：在室内指向一扇能出去的门总是比什么都不画强。
	Int i = 0
	While i < doors.Length
		ObjectReference d = doors[i]
		If d != None
			Float dx = d.GetPositionX() - px
			Float dy = d.GetPositionY() - py
			Float dz = d.GetPositionZ() - pz
			Float dist = Math.sqrt(dx * dx + dy * dy + dz * dz)
			Cell tc = d.GetTeleportCell()
			Bool isLoad = (lkw != None) && d.HasKeyword(lkw)
			If dbg && i < 8
				Debug.Trace("[SAS] door/diag:    d=" + FormHex(d) + " base=" + FormHex(d.GetBaseObject()) + " load=" + isLoad + " tc=" + FormHex(tc) + " tcell=" + FormHex(d.GetTransitionCell()) + " dist=" + Math.Floor(dist / 3.4286) + "m")
			EndIf
			If isLoad
				If bestLoad == None || dist < bestLoadD
					bestLoad = d
					bestLoadD = dist
				EndIf
			EndIf
			If tc != None
				If tc == targetCell
					If best == None || dist < bestD
						best = d
						bestD = dist
					EndIf
				ElseIf !tc.IsInterior() && iAmIndoor
					If bestOut == None || dist < bestOutD
						bestOut = d
						bestOutD = dist
					EndIf
				EndIf
			EndIf
		EndIf
		i = i + 1
	EndWhile
	If dbg
		Debug.Trace("[SAS] door/diag:  -> api=" + api + " doors=" + doors.Length + " best=" + FormHex(best) + " bestOut=" + FormHex(bestOut) + " bestLoad=" + FormHex(bestLoad))
	EndIf
	If best != None
		Return best
	EndIf
	If bestOut != None
		Return bestOut
	EndIf
	Return bestLoad
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

	; 目标检索：缓存 2 秒，失效（被打掉/关掉/换坐标系）立刻重查
	Bool needSearch = (GuideTarget == None) || ((now - GuideTargetAt) > CfgGuideTargetRefresh())
	If !needSearch
		If GuideTarget.IsDeleted() || GuideTarget.IsDisabled() || !SameSpace(GuideTarget.GetParentCell(), p.GetParentCell())
			needSearch = True
		EndIf
	EndIf
	If needSearch
		GuideTarget = FindGuideTarget(p, px, py, pz)
		GuideTargetAt = now
		GuideTargetIsProxy = False
	EndIf

	; ★ v2.6：目标在别的 cell ⇒ 退而指向「通往它的门」（见 FindGuideDoor 的说明）
	; 注意：变量名不能叫 door —— "Door" 是 Papyrus 已知的类型名，编译器直接拒绝。
	If GuideTarget == None && GuideOtherCellTarget != None && DoorBases != None
		ObjectReference dr = FindGuideDoor(p, px, py, pz, GuideOtherCellTarget.GetParentCell())
		If dr != None
			GuideTarget = dr
			GuideTargetAt = now
			GuideTargetIsProxy = True
			If CfgGuideDebug() && ((now - GuidePaintDiagAt) >= 5.0)
				GuidePaintDiagAt = now
				Debug.Trace("[SAS] guide: 目标在别的 cell ⇒ 指向门 " + FormHex(dr) + " -> " + FormHex(dr.GetTeleportCell()))
			EndIf
		EndIf
	EndIf

	If GuideTarget == None
		HideGuideMarkers()
		If GuideOtherCellTarget != None
			; 找到了目标，但它在别的 cell 且找不到能指的门
			GuideMarkers.SetValueInt(-2)
		Else
			GuideMarkers.SetValueInt(-1)
		EndIf
		If CfgGuideDebug() && ((now - GuidePaintDiagAt) >= 5.0)
			GuidePaintDiagAt = now
			Debug.Trace("[SAS] guide: 没有同 cell 的目标（markers=" + GuideMarkers.GetValueInt() + " otherCell=" + FormHex(GuideOtherCellTarget) + " otherD=" + Math.Floor(GuideOtherCellDist / 3.4286) + "m）")
		EndIf
		Return
	EndIf

	Float dx = GuideTarget.GetPositionX() - px
	Float dy = GuideTarget.GetPositionY() - py
	Float dist = Math.sqrt(dx * dx + dy * dy)
	Float start = CfgGuideStartDist()
	Float spacing = CfgGuideSpacing()
	; ★ v2.7：v2.6 这里用的是 start+spacing（8 米），而代理目标（门）常在 5~6 米内
	;   ⇒ 刚找到门就被「太近」挡掉、地上依然看不到东西。改成 start + 半个间距（3 米）。
	If dist <= (start + spacing * 0.5)
		HideGuideMarkers()
		GuideMarkers.SetValueInt(0)
		If CfgGuideDebug() && ((now - GuidePaintDiagAt) >= 5.0)
			GuidePaintDiagAt = now
			Debug.Trace("[SAS] guide: 目标太近（" + Math.Floor(dist / 3.4286) + "m），不画")
		EndIf
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
				If m == None && CfgGuideDebug() && ((now - GuidePaintDiagAt) >= 5.0)
					GuidePaintDiagAt = now
					Debug.Trace("[SAS] guide: PlaceAtMe 失败，形态 " + FormHex(Game.GetForm(CfgGuideMarkerFormID())) + " 造不出标记")
				EndIf
			EndIf
			If m != None
				If m.GetParentCell() != myCell
					m.MoveTo(p)
				EndIf
				m.Enable()
				m.SetPosition(px + ux * d, py + uy * d, pz + CfgGuideHeight())
				; 贴地：移到「它附近 navmesh 上最近的位置」（不需要射线，B 社自己的脚本就这么用）
				m.MoveToNearestNavmeshLocation()
				; ★ v2.8：贴地后抬一点 —— 球心在原点，不抬就有一半埋在地面里
				m.SetPosition(m.GetPositionX(), m.GetPositionY(), m.GetPositionZ() + CfgGuideMarkerLift())
				shown = shown + 1
			EndIf
		EndIf
		i = i + 1
		EndWhile
		GuideMarkers.SetValueInt(shown)
		; ★ v2.8：绘制诊断改用**自己**的时间戳（以前和「指向门」那行共用，
		;   结果同一轮里那行先刷掉了计时器 ⇒ 这里永远打不出来）。
		;   打的是「标记 0 到底在哪 / 在哪个 cell / 开没开 / 缩放多少」，
		;   下次实测一眼就能分清「没造出来」「造在别的 cell」「造在眼前但看不见」。
		If CfgGuideDebug() && ((now - GuidePaintPosDiagAt) >= 5.0)
			GuidePaintPosDiagAt = now
			ObjectReference m0 = None
			If GuideMarkerRefs.Length > 0
				m0 = GuideMarkerRefs[0]
			EndIf
			If m0 != None
				Debug.Trace("[SAS] guide/paint: target=" + FormHex(GuideTarget) + " isProxy=" + GuideTargetIsProxy + " d=" + Math.Floor(dist / 3.4286) + "m markers=" + shown + "/" + GuideMarkerRefs.Length + " | m0=" + FormHex(m0) + " cell=" + FormHex(m0.GetParentCell()) + " on=" + m0.IsEnabled() + " scale=" + m0.GetScale() + " m0pos=" + Math.Floor(m0.GetPositionX()) + "," + Math.Floor(m0.GetPositionY()) + "," + Math.Floor(m0.GetPositionZ()) + " ppos=" + Math.Floor(px) + "," + Math.Floor(py) + "," + Math.Floor(pz) + " pcell=" + FormHex(myCell))
			Else
				Debug.Trace("[SAS] guide/paint: target=" + FormHex(GuideTarget) + " markersonly=" + shown + " m0=None")
			EndIf
		EndIf
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
