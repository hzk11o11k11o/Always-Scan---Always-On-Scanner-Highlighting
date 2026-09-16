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
; ★ v3.0：面包屑改成「世界锚定的滚动光带」（20 颗密排珠子，每 tick 最多动 3 颗），
;   修掉「只看得到一颗 / 很强卡顿 / 不随玩家连续移动」。两条根因：
;   ① SetPosition 等在 Starfield 里是**延迟函数**（DelayFunctor），
;      调用后立刻读坐标是旧值 ⇒ v2.8 的球飘在半空；
;   ② 一帧里把整条链重摆一遍（8 颗 x 3 次引擎调用，含 8 次 navmesh 查询）+ 1.5 米移动节流。
;   详见下面「面包屑引导路径」小节与 PlaceGuideBead 的注释。
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
;   1) 形态用原版 MSTT（★ v2.8 起默认 00098106 GlowBall10x10，一个 0.1 米的
;      **发光球网格** Effects\Ambient\GlowBall10x10.nif，再 SetScale 放大）：
;      v2.4~v2.7 用的 LIGH 0x00012E5A 是**纯光源、没有网格**，白天室外看不见
;      （这是 v2.7 实测「markers 有数但什么都没有」的根因）。想换观感只改
;      CfgGuideMarkerFormID()（候选都列在那个函数里），不用动代码。
;   2) 贴地用 ObjectReference.MoveToNearestNavmeshLocation()：B 社自己的
;      TestNPCArenaScript.psc 就是把路径标记这样放上 navmesh 的，**不需要射线**。
;   3) 每个标记 BlockActivation(true,true) + SetMotionType(Keyframed) —— 不抢交互、
;      也不会被物理引擎拖走（MSTT 默认是 Dynamic，会滚）。
;
; ★★ v3.0：从「每 0.5 秒把 8 颗球整体重摆一遍」改成「世界锚定的滚动光带」★★
;   实测（v2.8 用户反馈）的三个毛病：
;     ① 有时候只看得到一颗  ② 卡顿感非常强  ③ 不是随玩家连续移动的（一格一格跳）
;   根因（两条，都是离线/引擎侧可证的）：
;     A. **SetPosition / MoveToNearestNavmeshLocation / Enable / Disable / SetScale
;        在 Starfield 里都是「延迟函数」**（RE::GameScript::DelayFunctor：
;        kSetPosition=6 / kMoveToNearestNavmeshLoc=26 / kEnable=3 / kDisable=4 /
;        kSetScale=11）—— 它们被塞进渲染安全队列、等 3D 就绪后由引擎执行，
;        **调用之后立刻读坐标拿到的是旧值**。v2.8 那句
;        「贴地之后再读坐标 +0.25 米」读到的正是吸附**之前**的 Z（玩家头顶 2 米）
;        ⇒ 球飘在半空（用户截图里那颗就是），而且一帧里 8 颗全在飘。
;     B. **一帧里把整条链重摆一遍**（8 颗 x 3 次引擎调用，其中 8 次是 navmesh 查询）
;        + 「移动 1.5 米才重算」⇒ 每走 1.5 米卡一下、球一次跳 1.5 米。
;   新做法（每 tick 只动 ≤ CfgGuideBudget() 颗，判断全是纯数学）：
;     · 珠子**世界锚定**：铺好后它们不动，玩家往前走时只有「最靠后那颗」被回收
;       去队尾（每前进一个间距发生一次）⇒ 走动时是光带从脚边流过，不是整条线跟着跳；
;     · 重铺（方向变了 / 横漂 > 2 米）也是**分批**做的（3 颗/次），所以永远不会有
;       「一帧铺满整条链」的抖动；
;     · 链长 = min(20 颗 x 0.55 米, 目标距离 - 余量)，目标近时自动压密，永不越过目标；
;     · 越过目标的珠子直接 SetPosition 到地下（比 Disable/Enable 便宜，也不来回切 3D）。
;
; 局限（如实记录，不假装）：
;   Papyrus **没有**「玩家正在追踪哪一个目标」的查询接口，所以这里用的是
;   「所有 active quest 的当前阶段目标里、同一 cell 内最近的那个」。
;   要精确到「追踪中的那一个」需要 hooks/RE，留作后续。
; ============================================================================
; ---- ★ v3.0「滚动光带」状态 ----
;   几何完全由下面这几个标量决定，逐帧只用**纯数学**判断该动哪几颗珠子：
;     GuideLX/LY      —— 铺路原点（世界坐标，= 铺路那一刻的玩家位置）
;     GuideLUX/LUY    —— 铺路方向（XY 单位向量）
;     GuideSpacingEff —— 本轮有效间距（目标近时自动压密，链尾永不越过目标）
;     GuideHead       —— 环头：珠子池里「当前离玩家最近」那颗的下标
;   槽位 k（0 = 最近、N-1 = 最远）离玩家的距离 = start + k*spacing - adv，
;   其中 adv = 玩家沿铺路方向前进的距离。
;   珠子**世界锚定**：铺好后玩家走动它们不动，只是「环头那颗被回收去队尾」
;   （每前进一个间距才回收一颗）⇒ 走动时是光带从脚边流过，而不是整条线跟着跳。
ObjectReference[] GuideMarkerRefs
Bool[]  GuideBeadParked
Float[] GuideBeadZ
Bool  GuideArrayReady = False
Int   GuideMarkerFormVer = 0
Int   GuidePoolVer = 0
Int   GuideHead = 0
Bool  GuideLayValid = False
Float GuideLX = 0.0
Float GuideLY = 0.0
Float GuideLUX = 0.0
Float GuideLUY = 0.0
Float GuideSpacingEff = 0.0
Int   GuideRelayNext = -1
Float GuideLastRelayAt = 0.0
Float GuideMovedTick = 0.0
Float GuideMovedTotal = 0.0
Cell  GuideLastCell = None
ObjectReference GuideTarget = None
Float GuideTargetAt = 0.0
Float GuideLastUpdateAt = 0.0
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

Int Function CfgGuidePoolVer()
	{ ★ 珠子池版本号。改了池子大小 / 排布方式就必须 +1 —— 脚本变量是跟着存档
	  走的，老存档里握着旧尺寸的数组，不重建就是一团乱（v2.8 那个「形态迁移」坑） }
	Return 30
EndFunction

Int Function CfgGuideBeadCount()
	{ 光带里的珠子总数。20 颗 x 0.55 米 ≈ 11 米的连续光带 }
	Return 20
EndFunction

Float Function CfgGuideSpacingBase()
	{ 相邻两颗珠子的间距（游戏单位）。0.55 米：配 0.4 米的球基本首尾相接 }
	Return 0.55 * 3.4286
EndFunction

Float Function CfgGuideStartDist()
	{ 第一颗珠子离玩家多远（游戏单位）。0.9 米（太近会在脚底下闪） }
	Return 0.9 * 3.4286
EndFunction

Float Function CfgGuideEndMargin()
	{ 链尾离目标留的余量（游戏单位）：光带不许铺过目标 }
	Return 0.5 * 3.4286
EndFunction

Float Function CfgGuideMinGap()
	{ 环头那颗离玩家这么近就把它回收去队尾（游戏单位）。0.7 米 }
	Return 0.7 * 3.4286
EndFunction

Int Function CfgGuideBudget()
	{ ★ 单次心跳最多动几颗珠子 —— 「不卡顿」的关键：
	  绝不把整条链在同一帧里铺满（v2.8 是 8 颗 x 3 次引擎调用一起打、
	  还夹着 8 次 navmesh 查询 ⇒ 周期性硬卡顿） }
	Return 3
EndFunction

Float Function CfgGuideTurnCos()
	{ 铺路方向与当前方向夹角超过这个 cos 就重铺（cos(8°) ≈ 0.990） }
	Return 0.990
EndFunction

Float Function CfgGuideLatTol()
	{ 玩家相对铺路直线的横向漂移超过这个值就重铺（游戏单位）。2 米 }
	Return 2.0 * 3.4286
EndFunction

Float Function CfgGuideRelayCooldown()
	{ 两次重铺之间至少隔这么久（秒）—— 防止走曲线时一直重铺 }
	Return 1.5
EndFunction

Float Function CfgGuideParkDepth()
	{ 「停用」一颗珠子 = 把它挪到地下这么深（游戏单位）。
	  为什么不调 Disable()：它同样是延迟函数，而且来回切更贵；
	  直接 SetPosition 到地下代价一样、还免了 3D 反复附着 }
	Return 4000.0
EndFunction

Float Function CfgGuideMaxDist()
	{ 超过这个距离的引导目标就不画（游戏单位）。120 米 }
	Return 120.0 * 3.4286
EndFunction

Float Function CfgGuideInterval()
	{ 引导路径最短重算间隔（秒）= 心跳间隔。判断逻辑是纯数学，
	  真正调引擎的次数由 CfgGuideBudget() 限死 }
	Return 0.25
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
	{ 珠子缩放。网格本身只有 0.1 米，4 倍 = 0.4 米的发光球
	  （配 0.55 米间距 ⇒ 基本首尾相接，看着是一条连续光带而不是一串点） }
	Return 4.0
EndFunction

Float Function CfgGuideMarkerLift()
	{ 放置时用的「离地高度」（游戏单位）。★ 它只当一次初始基准用：
	  紧接着的 MoveToNearestNavmeshLocation() 会把珠子吸附到 navmesh 上。
	  **绝不能**写成「吸附之后再读坐标来抬高」—— 见 PlaceGuideBead 的注释：
	  在 Starfield 里 SetPosition / MoveToNearestNavmeshLocation 都是延迟函数，
	  SetPosition 之后立刻 GetPositionZ() 读到的还是旧值。 }
	Return 0.2 * 3.4286
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

; ★ 池子每次重建都要满足两个条件：形态没换、池子版本没换。
;   （脚本变量跟着存档走 —— 老存档里握着旧尺寸/旧形态的数组，不重建就是一团乱。）
Bool Function EnsureGuideArray(Actor p)
	If GuideArrayReady && GuidePoolVer == CfgGuidePoolVer() && GuideMarkerFormVer == CfgGuideMarkerFormID()
		Return True
	EndIf
	; 玩家还没进世界（载入中 / 没有 cell）⇒ 这时候 PlaceAtMe 不可靠，下一轮再说
	If p.GetParentCell() == None
		Return False
	EndIf
	If GuidePoolVer != CfgGuidePoolVer() || GuideMarkerFormVer != CfgGuideMarkerFormID()
		DestroyGuideMarkers()
	EndIf
	GuidePoolVer = CfgGuidePoolVer()
	GuideMarkerFormVer = CfgGuideMarkerFormID()
	GuideLayValid = False
	GuideRelayNext = -1
	GuideHead = 0
	GuideArrayReady = True
	Int n = CfgGuideBeadCount()
	GuideMarkerRefs = new ObjectReference[n]
	GuideBeadParked = new Bool[n]
	GuideBeadZ = new Float[n]
	Int i = 0
	While i < n
		GuideBeadParked[i] = True
		GuideMarkerRefs[i] = CreateGuideMarker(p)
		i = i + 1
	EndWhile
	Debug.Trace("[SAS] guide: 光带池重建 poolVer=" + CfgGuidePoolVer() + " form=" + FormHex(Game.GetForm(GuideMarkerFormVer)) + " n=" + n)
	Return True
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
	GuideArrayReady = False
	GuideMarkers.SetValueInt(0)
EndFunction

; F8 关掉 / 玩家自己举着扫描仪时：整条光带收起来。
; （这是低频操作，Disable 的延迟函数代价可以接受）
Function HideGuideMarkers()
	If !GuideArrayReady
		Return
	EndIf
	Int i = 0
	While i < GuideMarkerRefs.Length
		ObjectReference m = GuideMarkerRefs[i]
		If m != None
			If !GuideBeadParked[i]
				m.Disable()
				GuideBeadParked[i] = True
			EndIf
		EndIf
		i = i + 1
	EndWhile
	; ★ 收起来之后铺路状态就作废了：下次重新亮起来必须**整条重铺**
	;   （不然珠子是 Disabled 的，而 conveyor 只会偶尔回收几颗 ⇒ 一片空白）
	GuideLayValid = False
	GuideRelayNext = -1
	GuideHead = 0
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
	Return m
EndFunction

; ============================================================================
; ★ v3.0：把一颗珠子放到「离玩家 alongPlayer 单位」的位置上（沿铺路方向）
; ----------------------------------------------------------------------------
; alongPlayer > cap（链尾上限）⇒ 判定为「该停用」：把它挪到地下 ParkDepth 深，
; 而不是调 Disable()（延迟函数、来回切更贵）。
;
; ★★ 只调一次 SetPosition，而且**绝不在 SetPosition 之后读坐标 ★★
;   在 Starfield 里这些都是**延迟函数**（GameScript::DelayFunctor 的
;   kSetPosition=6 / kMoveToNearestNavmeshLoc=26 / kEnable=3 / kDisable=4 /
;   kSetScale=11）：它们被塞进渲染安全队列，由引擎在 3D 就绪后执行，
;   调用后立刻 GetPositionZ() 拿到的是**旧值**。
;   v2.8 那颗飘在半空的球就是这么来的：
;     SetPosition(x, y, pz+2m) → MoveToNearestNavmeshLocation() → 读回坐标(还是 pz+2m)
;     → 再加 0.25 米 ⇒ 球落到「玩家头顶 2.25 米」而不是地上。
; ============================================================================
Function PlaceGuideBead(Actor p, Int idx, Float alongPlayer, Float cap, Float pz)
	If idx < 0 || idx >= GuideMarkerRefs.Length
		Return
	EndIf
	ObjectReference m = GuideMarkerRefs[idx]
	If m == None
		Return
	EndIf
	Float px = p.GetPositionX()
	Float py = p.GetPositionY()
	GuideBeadZ[idx] = pz
	GuideMovedTick = GuideMovedTick + 1.0
	If alongPlayer > cap
		; ---- 停用位：挪到地下（不调 Disable/Enable）----
		m.SetPosition(px + GuideLUX * alongPlayer, py + GuideLUY * alongPlayer, pz - CfgGuideParkDepth())
		GuideBeadParked[idx] = True
		Return
	EndIf
	If m.GetParentCell() != p.GetParentCell()
		m.MoveTo(p)
	EndIf
	If GuideBeadParked[idx]
		m.Enable()
		GuideBeadParked[idx] = False
	EndIf
	m.SetPosition(px + GuideLUX * alongPlayer, py + GuideLUY * alongPlayer, pz + CfgGuideMarkerLift())
	m.MoveToNearestNavmeshLocation()
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
	If !EnsureGuideArray(p)
		Return
	EndIf

	; F8 关掉时，光带一起收掉
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
	GuideLastUpdateAt = now
	GuideMovedTick = 0.0

	Float px = p.GetPositionX()
	Float py = p.GetPositionY()
	Float pz = p.GetPositionZ()

	; ★ v3.0：不再用「玩家移动 1.5 米才重算」那种节流 ——
	;   那正是「一格一格跳、不跟着玩家连续移动」的根源。
	;   现在每 0.25 秒都过一遍，但判断全是纯数学，
	;   真正调引擎的次数被 CfgGuideBudget() 限死在「每 tick 最多 3 颗」。
	Cell myCell = p.GetParentCell()
	If myCell != GuideLastCell
		; 换 cell（含换世界空间）⇒ 旧光带整条作废，下一轮重铺
		GuideLastCell = myCell
		GuideLayValid = False
		GuideRelayNext = -1
		GuideHead = 0
	EndIf

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
	If dist <= (start + CfgGuideMinGap())
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
	Float cap = dist - CfgGuideEndMargin()   ; 链尾离玩家的距离上限：光带不许越过目标
	Int   n = CfgGuideBeadCount()

	; ---- ① 要不要重铺（没铺过 / 方向变了 / 玩家横着漂太远）----
	Bool needLay = False
	If !GuideLayValid
		needLay = True
	ElseIf (now - GuideLastRelayAt) < CfgGuideRelayCooldown()
		needLay = False
	ElseIf (GuideLUX * ux + GuideLUY * uy) < CfgGuideTurnCos()
		needLay = True
	Else
		Float adv0 = (px - GuideLX) * GuideLUX + (py - GuideLY) * GuideLUY
		; ★ 玩家倒退 / 绕回去时珠子会被留在身后 ⇒ 也要重铺（否则光带一直晾在原地）
		If (0.0 - adv0) > CfgGuideLatTol()
			needLay = True
		EndIf
		Float lx = (px - GuideLX) - GuideLUX * adv0
		Float ly = (py - GuideLY) - GuideLUY * adv0
		If (lx * lx + ly * ly) > (CfgGuideLatTol() * CfgGuideLatTol())
			needLay = True
		EndIf
	EndIf

	If needLay
		GuideLX = px
		GuideLY = py
		GuideLUX = ux
		GuideLUY = uy
		GuideHead = 0
		GuideRelayNext = 0
		GuideLastRelayAt = now
		GuideLayValid = True
		; 有效间距：整条链不许超过「目标距离 - 余量」⇒ 目标近时自动压密成一小段
		Float sp = Math.Min(CfgGuideSpacingBase(), (cap - start) / n)
		GuideSpacingEff = Math.Max(sp, 0.08 * 3.4286)
	EndIf

	Float adv = (px - GuideLX) * GuideLUX + (py - GuideLY) * GuideLUY
	Int budget = CfgGuideBudget()

	If GuideRelayNext >= 0
		; ---- ② 重铺：每 tick 只铺 budget 颗（永远不会有「一帧铺满整条链」的卡顿）----
		While budget > 0 && GuideRelayNext < n
			PlaceGuideBead(p, GuideRelayNext, start + GuideRelayNext * GuideSpacingEff - adv, cap, pz)
			GuideRelayNext = GuideRelayNext + 1
			budget = budget - 1
		EndWhile
		If GuideRelayNext >= n
			GuideRelayNext = -1
		EndIf
	Else
		; ---- ③ 环头回收：最近那颗已经贴到玩家身上了 ⇒ 挪去队尾 ----
		;   这是「走动时唯一会发生的引擎动作」，频率 = 玩家速度 / 间距（走 3 m/s ≈ 每秒 5 颗）
		While budget > 0 && (start - adv) < CfgGuideMinGap()
			PlaceGuideBead(p, GuideHead, start + (n - 1) * GuideSpacingEff - adv, cap, pz)
			GuideHead = (GuideHead + 1) % n
			adv = adv - GuideSpacingEff    ; 队首少了一颗 ⇒ 等价于 adv 减一个间距
			budget = budget - 1
		EndWhile
		; ---- ④ 链尾修剪：越过目标的珠子挪到地下（玩家越走越近时才发生）----
		Int k = n - 1
		Bool trimmed = False
		While budget > 0 && k >= 0 && !trimmed
			Int idx = (GuideHead + k) % n
			If (start + k * GuideSpacingEff - adv) > cap
				If !GuideBeadParked[idx]
					PlaceGuideBead(p, idx, start + k * GuideSpacingEff - adv, cap, pz)
					trimmed = True
					budget = budget - 1
				EndIf
			Else
				k = -1
			EndIf
			k = k - 1
		EndWhile
	EndIf

	; ---- ⑤ 可见珠子数（纯数学，不调引擎）----
	Int vis = 0
	Int vi = 0
	While vi < n
		Float dv = start + vi * GuideSpacingEff - adv
		If dv > 0.0 && dv <= cap
			vis = vis + 1
		EndIf
		vi = vi + 1
	EndWhile
	GuideMarkers.SetValueInt(vis)
	GuideMovedTotal = GuideMovedTotal + GuideMovedTick

	; ---- 诊断（每 5 秒一屏）----
	;   看这几个数就能定位：relay/head/sp/adv/vis/moved 与 m0 的 cell/坐标。
	If CfgGuideDebug() && ((now - GuidePaintPosDiagAt) >= 5.0)
		GuidePaintPosDiagAt = now
		ObjectReference m0 = GuideMarkerRefs[GuideHead]
		If m0 != None
			Debug.Trace("[SAS] guide/paint: target=" + FormHex(GuideTarget) + " proxy=" + GuideTargetIsProxy + " d=" + Math.Floor(dist / 3.4286) + "m lay=" + GuideLayValid + " relay=" + GuideRelayNext + " head=" + GuideHead + " sp=" + GuideSpacingEff + " adv=" + Math.Floor(adv / 3.4286) + "m vis=" + vis + "/" + n + " moved=" + GuideMovedTick + "(累计" + GuideMovedTotal + ") | m0=" + FormHex(m0) + " cell=" + FormHex(m0.GetParentCell()) + " on=" + m0.IsEnabled() + " pos=" + Math.Floor(m0.GetPositionX()) + "," + Math.Floor(m0.GetPositionY()) + "," + Math.Floor(m0.GetPositionZ()) + " ppos=" + Math.Floor(px) + "," + Math.Floor(py) + "," + Math.Floor(pz) + " pcell=" + FormHex(myCell))
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
	; 光带：读档后位置全变了 ⇒ 忘掉目标和整条铺路状态，下一轮从零重铺
	GuideTarget = None
	GuideTargetAt = 0.0
	GuideLastUpdateAt = 0.0
	ScannerUp = False
	GuideLayValid = False
	GuideRelayNext = -1
	GuideHead = 0
	GuideLastCell = None
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
