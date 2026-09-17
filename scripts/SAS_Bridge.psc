Scriptname SAS_Bridge extends Quest

; ============================================================================
; Starfield Always Scan —— 桥脚本
;
; ★ v4.0 起这个脚本只剩两件事，而且**都与「高亮」无关**：
;     ① 老存档遗留的「面包屑珠子」一次性清理
;        （v2.4~v3.0 的自制引导线用 PlaceAtMe(abForcePersist=True) 建的常驻引用，
;          跟着存档走，功能删掉之后得有人负责把它们收回）；
;     ② 按 DLL 写进 `SAS_Notify` (GLOB) 的标记弹一条 HUD 提示（开 / 关）。
;
;   ★ 高亮**完全在 DLL 里**（直接驱动引擎原生 outline，和原版手持扫描仪同一套渲染）。
;     旧方案的 EFSH / OpList / StopList / 游标 / Epoch 全部删除 —— 那套「DLL 找目标 →
;     FormList 信箱 → 脚本 Play/Stop」的桥已经不存在了。
;
;   ★ 为什么这个脚本还留着（而不是把 mod 变成纯 DLL）：
;     老存档里本 quest 的脚本实例是**按 FormID 归档**的。一旦 quest 的 FormID 位移，
;     实例就对不上，`GuideArrayReady` 读回来永远是 False ⇒ 上面第 ① 件事不会再跑
;     ⇒ 老存档地上的珠子永久残留。
;     所以 ESM 里 0x800..0x806 那几条旧记录**保持创建顺序不动**（现在只是 FormID 占位），
;     新记录一律追加在最后（0x807 SAS_Notify）。详见 tools/xedit-scripts/build_sas.pas。
;
;   ★ 等老存档都不需要之后，本脚本 + 整个 ESM 都可以删掉（那时本 mod 变成纯 DLL，
;     构建也不再需要 xEdit）。
;
;   三条「不要再踩」的硬约束（来自上一代项目的实测崩溃/卡死现场）：
;     ① 变量名不能乱改 —— Papyrus 是按**变量名**从存档恢复的；
;     ② 配置项写成函数而不是 Auto 属性（Auto 属性会被存档持久化，改默认值对老存档无效）；
;     ③ OnTimer 第一件事就是 re-arm（否则计时器断了就永久哑掉）。
; ============================================================================

GlobalVariable Property NotifyFlag Auto Const Mandatory
	{ 0x807 SAS_Notify：DLL 写（1 = 刚开 / 2 = 刚关），本脚本读完后立刻归零 }

; ---- ★ 老变量：名字必须与 v2.4~v3.0 时**一模一样** ----
;   现在只当「这个存档有没有留下珠子」的标记用（没有任何代码会再把它置 True）。
ObjectReference[] GuideMarkerRefs
Bool  GuideArrayReady = False

; ============================================================================
; 可调参数（函数形式，随时改随时生效）
; ============================================================================
Float Function CfgInterval()
	{ 轮询间隔（秒）。0.5 秒足够跟上热键，开销可以忽略 }
	Return 0.5
EndFunction

; ============================================================================
; 事件
; ============================================================================
Event OnInit()
	StartTimer(CfgInterval(), 1)
EndEvent

Event OnQuestInit()
	StartTimer(CfgInterval(), 1)
EndEvent

Event OnTimer(int aiTimerID)
	StartTimer(CfgInterval(), 1)
	CleanupLegacyGuideBeads()
	PollNotify()
EndEvent

; ============================================================================
; ① 老存档遗留珠子的一次性清理
; ----------------------------------------------------------------------------
; 判定：老版本的 EnsureGuideArray 建完池子会把 GuideArrayReady 置 True；
;       新存档里它永远是 False（没有任何代码会把它置 True）⇒ 这条路径自动跳过。
; 清理完成（或玩家还没进世界）后把它置 False，保证只执行一次。
; ============================================================================
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
; ② 热键提示
; ----------------------------------------------------------------------------
; DLL 把 1 / 2 写进 GLOB；这里读到就弹一条提示并**立刻归零**（避免重复弹）。
; 之所以绕这一圈：commonlibsf 的 RE::DebugNotification 没移植（REL::ID = 0），
; 而 VM dispatch 要碰 BSTThreadScrapFunction 那一层，风险与收益不成比例。
; ============================================================================
Function PollNotify()
	If NotifyFlag == None
		Return
	EndIf
	Int v = NotifyFlag.GetValueInt()
	If v == 0
		Return
	EndIf
	NotifyFlag.SetValueInt(0)
	If v == 1
		Debug.Notification("Always Scan: ON")
	ElseIf v == 2
		Debug.Notification("Always Scan: OFF")
	EndIf
EndFunction
