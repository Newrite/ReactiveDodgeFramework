#pragma once

#include "Settings.h"

namespace rdf::game
{
	// Structured dev log lines, only emitted when [Dev] bDevLog=1.
	template <class... Args>
	void DevLog(spdlog::format_string_t<Args...> fmt, Args&&... args)
	{
		if (Settings::Get().devLog) {
			logger::info(fmt, std::forward<Args>(args)...);
		}
	}
}
