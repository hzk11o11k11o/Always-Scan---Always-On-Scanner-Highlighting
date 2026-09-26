#include "PCH.h"

#include "AlwaysScan.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>

#include "REX/CONVERT.h"  // REX::UTF16_TO_UTF8（日志里打印真实路径）

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

	// 日志文件上限 1 MiB：写新一行时若会超过就把旧内容整体清空，
	// 保证日志占用恒定 ~1 MiB（不是滚动保留旧文件）。
	constexpr std::size_t kLogMaxBytes = 1024 * 1024;

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

		std::filesystem::path fileName{ kLogName };
		fileName += ".log";

		std::filesystem::path usedDir;
		std::shared_ptr<spdlog::sinks::sink> fileSink;
		if (const auto dir = EsmDir(); !dir.empty()) {
			try {
				fileSink = std::make_shared<SizeLimitedFileSink>(dir / fileName, kLogMaxBytes);
				usedDir = dir;
			} catch (const std::exception& e) {
				REX::WARN("esm 同级目录里建日志失败（{}）—— 回退到 SFSE 默认日志目录", e.what());
			}
		}
		if (!fileSink) {
			if (const auto dir = SFSE::log::log_directory()) {
				try {
					fileSink = std::make_shared<SizeLimitedFileSink>(*dir / fileName, kLogMaxBytes);
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
		REX::INFO("日志文件：{}（上限 {} KiB，写满清空重来）", ToUtf8(usedDir / fileName), kLogMaxBytes / 1024);
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

	REX::INFO("SAS_AlwaysScan v4.30.0 loading (SFSE build {})", SFSE::GetSFSEVersion());

	if (auto* messaging = SFSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	} else {
		REX::WARN("messaging interface unavailable");
	}

	TryInstall();
	return true;
}
