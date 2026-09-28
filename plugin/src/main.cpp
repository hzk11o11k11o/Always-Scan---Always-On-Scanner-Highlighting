#include "PCH.h"

#include "AlwaysScan.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>

#include "REX/CONVERT.h"  // REX::UTF16_TO_UTF8（日志里打印真实路径）

// ★ 2026-09-27：`GetPrivateProfileIntW`（读 INI 的 `LogMaxMB`）。PCH.h 里已定义
//   NOMINMAX，不会有 min/max 宏冲突（AlwaysScan.cpp 也是这么拿的）。
#include <Windows.h>

#include <spdlog/details/file_helper.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

// 注意：SFSE_PLUGIN_VERSION 由 commonlibsf.plugin 规则生成的 commonlibsf-plugin.cpp
// 提供（配置见 plugin/xmake.lua 的 add_rules 参数），这里不要再定义一份，
// 否则会报 LNK2005 重复定义。

namespace
{
	std::atomic_bool g_installed{ false };

	// 日志名（落到 <esm 同级目录>\SAS_AlwaysScan.log；该目录不可写时回退到
	// SFSE 默认的 SFSE\Logs\<name>.log）。与下面 SFSE::Init 共用同一个常量，
	// 避免两处写死不一致。
	constexpr const char* kLogName = "SAS_AlwaysScan";

	// 日志文件上限 / 开关（★ 2026-09-27 起 **INI 可配**，见下面 LogMaxBytesFromIni）：
	//   开启时：写新一行若会超过上限就把旧内容整体清空，保证日志占用恒定 ≈ 上限
	//   （不是滚动保留旧文件）。
	// ★ 需求（AGENTS.md，2026-09-27 订正 R14）：「N 网公开版日志**完全关闭**，本机开发
	//   版本日志最大 10M，把这个做成配置写进 ini 文件吧（**配置项 0 关闭，大于 0 开启**），
	//   免得还得改代码」。
	//   ⇒ 键 = `[General] LogMaxMB`：**0 = 完全关闭**（不建文件、不写一个字节）；
	//      **1 ~ 1024 = 开启**，上限 N MiB。
	//   内置默认 **0（关闭）**（= 公开版口径：INI 缺失 / 手动安装也不产生日志）；
	//   本机部署的 `SAS_AlwaysScan.ini` 里显式写 `LogMaxMB=10`。
	constexpr int kLogMaxMbMax     = 1024;

	// 「单文件封顶」文件 sink。commonlibsf 默认建的是 basic_file_sink
	// （纯追加、永不清理），长时间游玩日志会无限变大；本 sink 在写入前检查
	// 累计字节数，超限就截断重开（file_helper::reopen(true)），从空文件重新累积。
	class SizeLimitedFileSink final : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		SizeLimitedFileSink(const std::filesystem::path& a_path, std::size_t a_maxBytes) :
			_maxBytes(a_maxBytes)
		{
#ifdef SPDLOG_WCHAR_FILENAMES
			_file.open(a_path.wstring(), false);  // 追加打开：保留上次运行的内容
#else
			_file.open(a_path.string(), false);
#endif
			_bytesWritten = _file.size();
		}

	protected:
		void sink_it_(const spdlog::details::log_msg& a_msg) override
		{
			spdlog::memory_buf_t formatted;
			formatter_->format(a_msg, formatted);

			// 先判断后写：最多超出「单条消息」的字节数，之后从 0 重算。
			if (_bytesWritten + formatted.size() > _maxBytes) {
				_file.reopen(true);
				_bytesWritten = 0;
			}

			_file.write(formatted);
			_bytesWritten += formatted.size();
		}

		void flush_() override
		{
			_file.flush();
		}

	private:
		spdlog::details::file_helper _file;
		std::size_t                   _maxBytes;
		std::size_t                   _bytesWritten{};
	};

	// 本插件 DLL 所在目录（MO2 下 = mod 目录里的 SFSE\Plugins\；
	// 手动安装 = <游戏>\Data\SFSE\Plugins\）。
	// 用当前模块的 ImageBase 反查（REX::W32::GetCurrentModule 就是 &__ImageBase），
	// 精确到本 DLL —— 不依赖 SFSE 接口，也不怕同时加载了多个插件。
	std::filesystem::path PluginDir()
	{
		wchar_t buf[1024]{};
		const std::uint32_t n = REX::W32::GetModuleFileNameW(
			REX::W32::GetCurrentModule(), buf, static_cast<std::uint32_t>(std::size(buf)));
		if (n == 0 || n >= std::size(buf)) {
			return {};
		}
		return std::filesystem::path{ buf, buf + n }.parent_path();
	}

	// esm 所在目录 = <游戏>\Data（本 DLL 在 Data\SFSE\Plugins\ 下，上溯两级）。
	//
	// ★ 需求（AGENTS.md）：日志文件生成在**和 esm 同级目录**里 ⇒ 就写这里。
	//   MO2 下 usvfs 只把「写 mod 目录里**已存在**的文件」重定向回 mod 目录，
	//   全新的文件会被丢进 overwrite —— 所以部署脚本（tools/build-sas.ps1）会
	//   在 mod 根预置一个空的 SAS_AlwaysScan.log，日志才会真落在 esm 旁边；
	//   没预置（比如手动安装）时，日志仍写在 Data 根（内容一样能查到）。
	std::filesystem::path EsmDir()
	{
		const auto dllDir = PluginDir();
		if (dllDir.empty()) {
			return {};
		}
		return dllDir.parent_path().parent_path();
	}

	// 路径转 UTF-8（spdlog 的文本都是 UTF-8；非 ASCII 路径在中文系统上
	// 直接 .string() 会变 GBK 字节 —— 日志里要能对得上真实路径）。
	std::string ToUtf8(const std::filesystem::path& a_path)
	{
		std::string s;
		REX::UTF16_TO_UTF8(a_path.wstring(), s);
		return s;
	}

	// ★ 2026-09-27（订正 R14）：日志上限 / 开关 = INI `[General] LogMaxMB`（MiB）。
	//   路径规则与 AlwaysScan.cpp 的配置解析**完全一致**（两处必须同规则，
	//   否则会出现「INI 读到了、日志上限没读到」的鬼故事）：
	//     ① 优先「和 esm 同级」（MO2 下 = mod 目录根）的 SAS_AlwaysScan.ini；
	//     ② 不存在 ⇒ 回退 DLL 旁边的老位置（v4.16 及以前）。
	//   值：缺键 / 读不到 / 0 ⇒ **0 = 完全关闭**；负数按 0；>1024 ⇒ 1024。
	//   返回 0 表示关闭（调用方不建文件 sink）；否则 = 字节上限。
	//   用宽字符 API（游戏可能在中文路径下）。
	std::size_t LogMaxBytesFromIni()
	{
		std::filesystem::path ini;
		if (const auto dir = EsmDir(); !dir.empty()) {
			auto p = dir / "SAS_AlwaysScan.ini";
			if (std::filesystem::exists(p)) {
				ini = std::move(p);
			}
		}
		if (ini.empty()) {
			if (const auto dir = PluginDir(); !dir.empty()) {
				auto p = dir / "SAS_AlwaysScan.ini";
				if (std::filesystem::exists(p)) {
					ini = std::move(p);
				}
			}
		}
		int mb = 0;
		if (!ini.empty()) {
			mb = static_cast<int>(::GetPrivateProfileIntW(L"General", L"LogMaxMB", 0, ini.wstring().c_str()));
		}
		if (mb < 0) {
			mb = 0;  // 负数按「关闭」处理（有效域 = 0 ~ 1024）
		}
		if (mb > kLogMaxMbMax) {
			mb = kLogMaxMbMax;
		}
		return static_cast<std::size_t>(mb) * 1024 * 1024;
	}

	// 接管日志：建「限长文件 sink」并把默认 logger 的 sink 全换成它。
	// 必须在 SFSE::Init 之后调用。
	//
	// ★ 需求（AGENTS.md）：日志写在**和 esm 同级**的目录（<游戏>\Data\，MO2 下
	//   = mod 目录根）；那里建不出文件时回退 SFSE 默认日志目录
	//   （Documents\My Games\Starfield\SFSE\Logs\）—— 保证任何时候都有日志可查。
	//
	// ★ 调用方传了 `.log = false`（不建 commonlibsf 默认的 Documents 日志），
	//   所以这里要自己把 level / flush_on / pattern 一并设好（值与 commonlibsf
	//   InitLog 的默认一致：debug 构建 Debug、发布 Info，写一行 flush 一次）。
	void ApplyLogSizeLimit()
	{
		auto* logger = spdlog::default_logger_raw();
		if (!logger) {
			return;
		}

		// ★ 2026-09-27（订正 R14）：日志上限 / 开关从 INI 读（`[General] LogMaxMB`，MiB）——
		//   建 sink 前只解析一次（之后改 INI 要重启游戏才生效，与其它配置一致）。
		const std::size_t maxBytes = LogMaxBytesFromIni();
		if (maxBytes == 0) {
			// `LogMaxMB=0` ⇒ **完全关闭日志**（公开版默认口径）：不建任何文件、清掉
			// 全部 sink 并把 level 置 off —— 之后所有日志调用零输出（连 MSVC 调试
			// 输出也不留）、开销降到一个 level 比较。这也是「完全关闭」的兑现方式：
			// 不产生日志文件 = 玩家侧零痕迹。
			logger->sinks().clear();
			logger->set_level(spdlog::level::off);
			return;
		}

		std::filesystem::path fileName{ kLogName };
		fileName += ".log";

		std::filesystem::path usedDir;
		std::shared_ptr<spdlog::sinks::sink> fileSink;
		if (const auto dir = EsmDir(); !dir.empty()) {
			try {
				fileSink = std::make_shared<SizeLimitedFileSink>(dir / fileName, maxBytes);
				usedDir = dir;
			} catch (const std::exception& e) {
				REX::WARN("esm 同级目录里建日志失败（{}）—— 回退到 SFSE 默认日志目录", e.what());
			}
		}
		if (!fileSink) {
			if (const auto dir = SFSE::log::log_directory()) {
				try {
					fileSink = std::make_shared<SizeLimitedFileSink>(*dir / fileName, maxBytes);
					usedDir = *dir;
				} catch (const std::exception&) {
				}
			}
		}
		if (!fileSink) {
			REX::WARN("日志文件建不出来（继续用 MSVC 调试输出；不影响任何功能）");
			return;
		}

		// clear() 会把 spdlog 自带的 stdout sink 换掉（GUI 进程里 stdout 本来也没用）。
		logger->sinks().clear();
		logger->sinks().push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
		logger->sinks().push_back(fileSink);

		// level / flush_on / pattern：值都对齐 commonlibsf InitLog 的默认。
#ifdef NDEBUG
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
#else
		logger->set_level(spdlog::level::debug);
		logger->flush_on(spdlog::level::debug);
#endif
		spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
		REX::INFO("日志文件：{}（上限 {} MiB，写满清空重来；★ INI `[General] LogMaxMB` 可调"
				  " —— 0 = 完全关闭（N 网公开版默认），本机开发 10，改完重启游戏生效）",
			ToUtf8(usedDir / fileName), maxBytes / (1024 * 1024));
	}

	// 插件加载时可能过早（SFSE 的任务系统还没起来），所以在
	// PostPostLoad / PostDataLoad 消息里再兜底试一次。
	void TryInstall()
	{
		if (g_installed.load()) {
			return;
		}
		if (SAS::Install()) {
			g_installed.store(true);
		}
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostPostLoad:
		case SFSE::MessagingInterface::kPostDataLoad:
			TryInstall();
			break;
		default:
			break;
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	// ★ `.log = false`：不要 commonlibsf 默认那个 Documents\My Games\Starfield\SFSE\Logs\
	//   文件 sink（需求要求日志在「和 esm 同级目录」，由 ApplyLogSizeLimit() 自己建）。
	SFSE::Init(a_sfse, { .log = false, .logName = kLogName });
	ApplyLogSizeLimit();

	// ★ 2026-09-27 订正 R2（公开版仍是 2.0，build 号按用户要求不提升）：
	//   ① 引擎状态表树头 RVA 0x5949CE0 → **0x5F39CE0**（手算 rip 相对地址时少看一位，R1）；
	//   ② 「青」记忆不再短路主判据 / 资源链（只有「绿」单向短路，R1）；
	//   ③ ★ **按物种（base）扩散**：任一实例被权威确认「已扫描」⇒ 同 species / 同资源
	//      的**所有**实例一起变绿（引擎知识库本来就是这一级）—— 治用户实测的
	//      「有些植物扫描后还是青，打开扫描仪再关闭才变绿」（R2）。
	//   这三条一起治「扫描后没变色」—— 启动日志里认这个括号即可确认跑的是订正版。
	// ★★★ 2026-09-27 深夜 订正 R5（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **「已扫描」记忆按存档隔离** —— 治用户实测的「未扫描星球资源直接显示绿色，
	//   特别是重新读档（未扫描时的存档）后问题非常严重」（docs/38）：
	//   · 读档边界 = 引擎自己的 `TESLoadGameEvent`（快速旅行 / 进门不算）；
	//   · 每次读档：记忆作用域 +1（只有「本存档内当场见证」的绿才算数）+ 清空按物种
	//     扩散表 + 用资源链（0x1597A50）复核并丢掉「本存档里必然没扫描」的条目；
	//   · 落盘记忆仍跨会话保留（本会话**第一次**读档时有效 = 重开游戏继续玩同一存档）；
	//   · 另外：**按物种（base）扩散只在同一颗星球（worldspace）内生效** ——
	//     v5.1.2 的全局扩散会让「别的星球上没扫过的同种资源」也变绿。
	// ★★★ 2026-09-27 订正 R6（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **「读档边界」改为证据驱动** —— 治用户实测的「传送切换地图后，已扫描物品
	//   变成青色，开启扫描仪再关闭又变回绿色」：
	//   · 在 R5 里引擎的 `TESLoadGameEvent` 被当作「读档」的无条件边界 —— 而实测
	//     它会**在部分传送 / 换世界空间时也发**（用户那次「传送」触发了读档 #2）；
	//   · R6：事件只当**候选**（且复核延后 4s 到世界稳定后做）—— 资源链复核
	//     **一个 base 都证伪不了** ⇒ 判定「传送 / 读同一存档」⇒ 记忆保持有效
	//     （作用域不推进、物种表不清、条目不丢）；有证伪才是真「读回更早的存档」；
	//   · 另外：复核时**跳过「只见证于别的世界空间」的 base**（跨世界空间查必然
	//     假证伪）+ 链「已扫描」的 base 会被「重见证」（恢复作用域）。
	//   回退：INI `FloraLoadBoundaryEvidence=0`（回到 R5 行为）。
	// ★★★ 2026-09-27 订正 R7（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **读档边界改用「游戏时间指纹」**（`Calendar::gameDaysPassed`）—— R6 的链证据
	//   口径实测**仍然误作废**（用户 2026-09-27 10:15 那一局：载入画面关闭 + 4s 的复核
	//   把 9 个 base 判成「本存档没扫描」并删掉 132 条记忆，而 10s 后引擎自己拿着扫描仪
	//   把**同一批** base 画成绿、80s 后资源链对同一个资源也答「已扫描」⇒ 「没扫描」
	//   这个否定答案在载入刚结束时**没有区分度**，见 docs/40）：
	//   · 每条记忆自带**学习时刻**（落盘行第 3 字段 = 游戏时间，天）；
	//   · 读档时把「本存档的游戏时间」当**锚点**：学习时刻 ≤ 锚点 ⇒ 记忆有效；
	//     > 锚点 ⇒ 属于更新的时间线 ⇒ 不在作用域（★ **不删条目**，换更新的存档就回来）；
	//   · 传送 / 继续同一存档 ⇒ 锚点几乎不动 ⇒ **什么都不作废**（本次修复的核心）；
	//   · 读回更早的存档 ⇒ 只有「未来」的条目失效，其余照常有效（比 R5 的
	//     「一律作废、植物靠举扫描仪重学」精准得多）。
	//   回退：INI `FloraSaveFingerprint=0`（回到 R6 链证据路径）；读不到游戏时间时也会自动回退。
	// ★★★ 2026-09-27 订正 R8（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **治「传送后已扫描物品变青」的剩余那一半**（R7 之后用户第三次报同一现象）——
	//   R7 的读档边界已经不再误作废（那一局 `作废=0 条`），但**按物种扩散被星球口径挡住** +
	//   **引用级记忆命中不了**（外景临时引用每次会话换 FormID），于是：
	//   · 在星球 A 本会话亲手扫过的物种，星图快速旅行到 B 后**引擎自己**（一举扫描仪）
	//     把同 species 实例画成绿 4/5 ⇒ 引擎的物种知识**跨星球生效**，我们却涂成青色；
	//   · 载入 / 传送后同一个物种的实例换了 FormID ⇒ 落盘记忆（按引用）命中不到。
	//   R8 两处改动（都在 `AlwaysScan.cpp`）：
	//     ① `FloraBaseKnown()` **默认不再按星球硬拒绝**（`FloraSpeciesPlanetScope` 默认 0 =
	//        引擎口径）；作用域仍由**存档边界**管（物种表会话级不落盘 + 读档按游戏时间锚点剪枝）；
	//     ② 新增 `SeedFloraBaseFromRefMemory()`：把**作用域内**的落盘绿条目按 base **播种**进
	//        物种表（启动载入后 + 每次读档剪枝后各一次）⇒ 同 species 的实例**不必等举扫描仪**
	//        直接是绿的。
	//   回退：INI `FloraSpeciesPlanetScope=1`（回到 R5 的严格口径，只为对照）。
	// ★★★ 2026-09-27 订正 R9（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **植物「已扫描」改为直读引擎自己的扫描进度表 + 默认抛弃自建记忆**（用户指令：
	//   「我们还是走植物也直接读游戏本身物件状态的方式，抛弃自己记忆的方法」）。
	//   · 新增 `QueryFloraScanProgressDirect/Cached`：复刻引擎给「已扫描」植物写 state 4/5
	//     时用的那条**纯查询**（`PlayerKnowledge` 物种槽 `percent`；`0x1307180(ref)` 取知识 ID +
	//     两级只读哈希 `0x24105D0`/`0x23467B0`，命中读 `[元素+0x20]`）⇒ **任何时刻、任何
	//     引用**（含外景运行时临时引用）都能问「这个物种在这个存档里扫过没」；
	//     `percent == 100` ⇒ 已扫描（绿）；结果按 **base** 缓存（物种级，引擎口径）。
	//   · 新增 `FloraUseMemory`（默认 **0 = 抛弃记忆**）：按引用记忆 / 按物种扩散 /
	//     落盘播种 / 「沿用旧结论」全部**不参与判定** —— 用户报的「所有星球资源固定
	//     显示已扫描绿」的主嫌就是 R8 的「跨星球放行 + 播种」把物种表大面积填充。
	//   · 保留的判据全部是「引擎自己的数据」：进度直读 + 状态表只读探针（4/5）+
	//     `GetOutlineState` + 资源链（LVLI）；全程签名 / 形状 / SEH 三重保护，任一环
	//     走不通自动回退（不影响高亮本身）。
	//   回退：INI `FloraEngineProgress=0`（关新判据）/ `FloraUseMemory=1`（回 v5.1.8 记忆口径）。
	// ★★★ 2026-09-27 订正 R10（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **R9 的新判据实际上一次都没跑起来** —— 用户报告「已扫描物件还是扫描前青色，而且这次
	//   开关扫描仪都没自我修复」。日志铁证（用户 2026-09-27 12:57~12:59 那一局）：
	//   · 启动行 `flora progress: key 类型常量异常（0）-> 引擎扫描进度直读 DISABLED`
	//     + 统计行 `引擎进度直读(★v5.2): 问=0 … ready=0` ⇒ ⓠ 判据全程零查询；
	//   · 于是植物只剩「举着扫描仪时才成立的证据」（⓪.5 状态表探针 / ① GetOutlineState，
	//     实测 `未扫描=489` 压倒性多数）⇒ 放下扫描仪就回到青色、`FloraUseMemory=0` 又
	//     不记录引擎画过的绿 ⇒ 用户看到的「开关扫描仪也不自愈」。
	//   **根因（★ 离线已证）**：那个「key 类型 word」（`0x130A270` 里 `movzx r8d, word [rip+disp]`）
	//   位于 `.data` 的**未初始化段**（目标 RVA `0x61E1D94` 超出 `.data` 的 raw size ⇒
	//   文件里没有它，运行时才被写入）⇒ **启动时读它必然是 0**；R9 把「读到 0」当异常把
	//   整条判据禁用了。引擎自己也是**每次查询现读**那条指令。
	//   修法（`AlwaysScan.cpp`）：① 解析阶段只**记住 word 的地址**（签名 / 可读性校验不变），
	//   不再因值为 0 而禁用；② `QueryFloraScanProgressDirect` 里**每次查询现读**该 word
	//   构造 key（与引擎同一时刻取值，最忠实）；③ 新增诊断 `stage`（停在哪一层）+
	//   `keyType`（现读值）+ `flora progress probe:` 前 8 条 + 统计行
	//   `keyType=0x…(现读) keyZero=` —— 下一局若有问题，一次日志就能定位到具体那一层。
	// ★★★ 2026-09-27 订正 R11（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   同上现象再次复现（用户报告：「再次出现已扫描物件还是扫描前青色的现象，而且这次开关
	//   扫描仪都没自我修复……新高亮的物体容易出现这个问题」）。R10 的「现读」确实生效了
	//   （`ready=1`，不再是 DISABLED），但日志出现新证据（用户 2026-09-27 13:10~13:12 那一局）：
	//   · 前 8 条 probe **全部** `stage=3 -> 0x81 组件拿不到`（key1/key2 都是 0、keyType 未读到）；
	//   · 统计行 `问=0 满=0 未满=0 失败=63 兜底K1=（无）` ⇒ 整条判据**仍然零成功查询**；
	//   · 而同一批引用的引擎状态问答正常（`问=273 已扫描=34 未知=41`）、引擎自己画绿正常
	//     （`manager occupancy: 4=415 5=57`）⇒ 是**我们复刻的链路缺了一段**。
	//   **根因（★ 离线反汇编实证，本轮核心）**：引擎取 key1（物种/资源 ID）有**两级**：
	//   ① 组件主路径 = `GetComponent([ref+0xC8], 0x81)` → `[comp+0x28]`；
	//   ② ★ 兜底 = `0x910690(out, ctx, ref, 0)` —— 在「组件缺失 / `[comp+0x28]==0`」时
	//     从 cell / 世界空间 / 全局把 key1 找出来（`0x7BCBD0` 的 `0x7BCC63` 分支）。
	//   **实测这些植物 ref 根本没有 0x81 组件**（stage=3 100%）——「0x81 组件」不在植物 ref 上
	//   （组件存在位图都没有这一位），**引擎画绿走的就是兜底 ②**；R10 只复刻了 ① ⇒ 判据
	//   一次都没成功过。★ 旁证：`0x2C5C90`（GetOutlineState 的组件检查）只查「存在位图」、
	//   `0x347170` 还要遍历链表 —— 两者对 0x2A 组件都正常 ⇒ 容器解读没错，是 0x81 真没有。
	//   **修法（`AlwaysScan.cpp`，一处）**：`QueryFloraScanProgressDirect` 的 key1 获取改成
	//   **「先组件、后兜底」**（与引擎 0x7BCBD0 的顺序一致）—— 组件拿到非 0 key1 就用它；
	//   否则调 `0x910690` 兜底（全 SEH + 签名校验；`k1fb=` 在 ready 行可见）。新增诊断：
	//   `k1来源=组件/兜底`（probe 行）+ 统计行 `兜底K1=`。兜底函数签名不符时**只关兜底、
	//   不禁用整条判据**（R10 的教训：禁整条判据是危险动作）。
	//   ★ 副作用面已被历史验证：我们自 v4.25 起就在调 `0x1306E80`（GetOutlineState），它对
	//   「有 0x2A 组件」的引用**必然**调 `0x7BCBD0` → 组件失败时即本兜底；那些版本从未出现
	//   崩溃 / 表增长 / 卡顿回归 ⇒ 直接调用不引入新副作用面。
	//   回退：INI `FloraEngineProgress=0`（关新判据）。
	// ★★★ 2026-09-27 订正 R12（公开版仍是 2.0、DLL build 仍是 5.1.0；**只改诊断、不改行为**）：
	//   用户指令：「肉眼检测似乎没什么问题了，你再结合日志看看有没有遗漏的地方」。
	//   本轮审查 R11 实测日志（2026-09-27 13:33~13:45 两局）的发现：
	//   · ★ 好消息（R11 生效的硬证据）：`兜底K1=104` **恰好等于** `问(35) + 失败(69) = 104`
	//     ⇒ 每一次真实查询都成功走兜底拿到 key1 —— key1 获取已 100% 工作；
	//     另外 `满=35`（直读查出 percent=100）说明链路在**部分**目标上已经跑通。
	//   · 问题 ①：两局共 16 条 `flora progress probe:` **全是 stage=8**
	//     （「桶内偏移 / 二级基址形状不对」）—— 这一档混合了三种完全不同的情况：
	//     subOff 读不到 / 二级基址不可读 / **cap==0（空表 = 这个物种没扫过，正常）**；
	//     ⇒ 无法从日志区分「没扫过（行为正确）」与「结构真的不对（漏绿隐患）」。
	//   · 问题 ②：probe 失败行 `key1=0x0 key2=0x0` 是**假的**（失败分支没回填诊断值）
	//     —— 实际 key1 非零（否则 stage 会是 4），这行会把排障方向带偏。
	//   · 问题 ③：判绿行（`flora scan: ref=… 判据 = …`）在两局日志里**一条都没有** ——
	//     8 条额度被会话开头的「判青最终行」吃光 ⇒ R11 文档里的核心验收点
	//     （`判据 = 引擎扫描进度直读：percent=100/100`）**结构上不可观测**（即使判据在干活）。
	//   · 问题 ④：`冲突=`（直读与引擎状态表/GetOutlineState 相矛盾的计数）定义了但从不自增。
	//   修法（全部在 `AlwaysScan.cpp`，纯诊断 / 可观测性；**行为一个字节都没动**）：
	//   ① `stage` 8 细分成 12(桶内 subOff 读不到)/13(基址不可读)/14(**空表=没扫过**)/15(arr·cap 形状坏)，
	//      probe 行随之附 `桶=/subOff=/cap=/arr=`（下一局一眼定性）；
	//   ② 失败路径**照实回填** key1/key2（probe 行不再是假 0）；
	//   ③ probe 除「前 8 条」外，**每个 stage 的首条**与**首条成功**都保证打出（样例必可见）；
	//   ④ **判绿行独立额度**（`floraScanGreenProbes`，不再与判青最终行抢 8 条额度）；
	//   ⑤ `冲突=` 实现：直读答「未满」而引擎状态表 4/5 或 `GetOutlineState==2` 时计数 + 首条 WARN。
	//   ★ 版本号纪律：公开版仍 **2.0**、DLL 内部 build 仍 **5.1.0**（本轮未动版本号）。
	// ★★★ 2026-09-27 订正 R13（公开版仍是 2.0、DLL build 仍是 5.1.0；**只改诊断、不改行为**）：
	//   承接上一轮（用户在 R12 部署后实测报「肉眼检测似乎没什么问题了」→ 再结合日志复查）。
	//   审查 R12 实测日志（2026-09-27 13:59~14:08，部署目录）的发现：
	//   · ★ R12 验收点**全部达成**：`订正 R12` 启动行 / probe 失败行 `key1=0x3F5A1` 非 0 /
	//     `stage=14`（空表 = 没扫过）出现在未扫描物种上 / `stage=12/13/15` 零出现（无结构异常）/
	//     判绿行 `判据 = 引擎扫描进度直读：percent=100/100` 出现 8 条 / `冲突=0` /
	//     `兜底K1=197 == 问(59)+失败(138)` / `日志文件：…（上限 10 MiB…）` 正确。
	//   · 问题 ①（本轮修）：**判绿行 8 条额度在 14:03:16~14:03:27 被 3 个 base 用光** ⇒
	//     之后 8 次「扫描 → 放下扫描仪」（14:03:42 ~ 14:07:22）**全程没有任何判绿日志** ——
	//     下一次「某物种扫描后不绿」的第一现场仍然不可观测。
	//   · 问题 ②（本轮修）：**缓存命中的判绿行打印 `key1=0x0 key2=0x0`** —— 直读进度缓存
	//     只存了 `{progress, atMs, ok}`，命中时不回填 key1/key2。与 R10（`keyType=0x0`）、
	//     R11（失败行 `key1=0x0`）是**同一个坑的第三次**：只带回部分字段 = 日志会说谎。
	//   修法（全部在 `AlwaysScan.cpp`，纯诊断；**行为一个字节都没动**）：
	//   ① 判绿行 / 链判决行额度升级为「前 N 条 ∨ **每个 base 首条**」（会话硬上限 64）——
	//      每个新物种的首条必留痕，日志量 ≈ 前 8 条 + 一局遇到的 base 数；
	//   ② `FloraProgressRec` 加 key1/key2（缓存命中时回填）⇒ 判绿行的 key1/key2 不再假 0；
	//   ③ 链判决行（「为什么青」的第一现场）同样按 base 首条 —— 对症用户主诉的「已扫描却青」。
	//   ★ 版本号纪律：公开版仍 **2.0**、DLL 内部 build 仍 **5.1.0**（本轮未动版本号）。
	// ★★★ 2026-09-27 订正 R14（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   用户新需求（AGENTS.md）：「N 网公开版日志**完全关闭**、本机开发版本日志最大 10M，
	//   做成配置写进 ini（**配置项 0 关闭，大于 0 开启**），免得还得改代码」。
	//   改动（`main.cpp`，一处语义升级）：`[General] LogMaxMB` 从「钳制 1~1024 的上限」
	//   升级为「**开关 + 上限**」—— **0 = 完全关闭**（不建文件、清空全部 sink + level
	//   置 off，日志调用零输出零文件）；**>0 = 开启**，上限 N MiB（钳制到 1024）。
	//   内置默认 1 → **0（关闭）** = 公开版口径（INI 缺失 / 手动安装也不产生日志）；
	//   本机部署 INI 显式写 `LogMaxMB=10`（开发排障口径，行为不变）。
	// ★★★ 2026-09-28（第二轮 · 用户指令；公开版仍 2.0.1、DLL 内部 build 仍 5.1.0）：
	//   **配色修订 ——「门 / 电脑 / 按钮」白 #FFFFFF → 品红 #FF3BD4**
	//   （`颜色分类.md` 同步；只改颜色值 —— 通道数 14、槽位分配、其余类别一字未动；
	//   选色推导见 docs/50：313° 落在「紫 269° → 红 0°」空档正中，与最近的紫 / 红
	//   各拉开 ~44° / ~47°，离其余类别全部 ≥82°）。
	// ★★★ 2026-09-28（第三轮 · 用户指令；公开版仍 2.0.1、DLL 内部 build 仍 5.1.0）：
	//   **「电脑 / 按钮等所有可互动物品」退回 2.0** —— 品红 #FF3BD4 → **原版绿**
	//   （state 6 → 4 + 颜色不覆盖；`kChannelColorDef[kDevice]` 0xFF3BD4 → 0x27C684）；
	//   **「门」仍是品红 #FF3BD4**；黄 = 开锁器 / 信用币 不变 —— 完整记录见 docs/51。
	// ★★★ 2026-09-28（第四轮 · 用户实测反馈修复；公开版仍 2.0.1、DLL 内部 build 仍 5.1.0）：
	//   **「信用条」没有变黄** —— 真根因：**它不是 MISC，而是 FLOR**
	//   （`Loot_CredStick_Small/Common/Rare` = 0x3CC32C / 0x3CC325 / 0x3CC32A，
	//    zhhans 名 = 信用条；而 `Credits` 0xF 的 zhhans 名是「信用币」）。
	//   之前它们掉进「植物 / 星球目标」分支 ⇒ 走已扫描 / 未扫描判据、画青色脉冲。
	//   ⇒ `ClassifyBase()` 的 kFLOR 分支先按 FormID 认这 3 条并归「黄组」#FFE100。
	//   证据链（zhhans .strings → FULL 反查 → 运行期 flora 日志）见 docs/53。
	REX::INFO("SAS_AlwaysScan v5.1.0 loading（订正 R14：日志**完全关闭**开关（N 网公开版口径）—— `[General] LogMaxMB` 升级为「开关 + 上限」：**0 = 完全关闭**（不建文件、清空全部 sink + level off，零输出零文件；★ 内置默认也是 0）、**>0 = 开启**（上限 N MiB，钳制 1024）；本机部署 INI 显式写 10；R13 的诊断可观测性（判绿行/链判决行「每 base 首条」+ 缓存回填 key1/key2）原样保留；★ 除日志开关外行为零改动；★ 2026-09-28 第二轮配色修订：门 / 电脑 / 按钮 = 品红 #FF3BD4（白 #FFFFFF 起不再使用，见 docs/50）；★★ 第三轮配色修订：电脑 / 按钮等可互动物品**退回 2.0** = 原版绿 #27C684（state 6 → 4 + 不覆盖、通道默认色同步），门仍品红 #FF3BD4；黄 = 开锁器 / 信用币不变（见 docs/51）；★★★ 第四轮（行为修复）：**「信用条」= FLOR**（`Loot_CredStick_Small/Common/Rare` 0x3CC32C/0x3CC325/0x3CC32A）—— 之前被当成「植物 / 星球目标」（青色脉冲；被 `Credits` 的中文名「信用币」误导过一轮），现在归入黄组 #FFE100（见 docs/53））(SFSE build {})",
		SFSE::GetSFSEVersion());

	if (auto* messaging = SFSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	} else {
		REX::WARN("messaging interface unavailable");
	}

	TryInstall();
	return true;
}
