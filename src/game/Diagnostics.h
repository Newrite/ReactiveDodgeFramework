#pragma once

#include "Settings.h"

namespace rdf::game
{
	// Structured dev log lines, only emitted when [Dev] bDevLog=1.
	template <class... Args>
	void DevLog(spdlog::format_string_t<Args...> format, Args&&... args)
	{
		if (Settings::Get().devLog) {
			logger::info(format, std::forward<Args>(args)...);
		}
	}
}
