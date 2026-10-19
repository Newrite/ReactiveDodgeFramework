#pragma once

#include "core/ActionArbiter.h"

namespace rdf::game::engine
{
	struct Snapshot
	{
		EngineBlock  block{ EngineBlock::None };
		std::uint8_t attackState{ 0 };
		float        resource{ 0.0f };  // current value of the configured cost resource
	};

	// Read-only queries on the player; the first matching block wins, strongest first.
	Snapshot Read(RE::PlayerCharacter* player);
}
