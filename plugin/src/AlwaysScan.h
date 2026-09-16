#pragma once

namespace SAS
{
	// 注册主循环回调（SFSE 永久任务，每帧在主线程调用一次）。
	// 返回 false 表示任务接口还没就绪，调用方可以稍后重试。
	bool Install();
}
