#pragma once

#include "core/ActionArbiter.h"

namespace rdf::game::engine
{
	struct Snapshot
	{
		EngineBlock  block{ EngineBlock::None };
		std::uint8_t attackState{ 0 };
		float        stamina{ 0.0f };
	};

	// Read-only queries on the player; the first matching block wins, strongest first.
	Snapshot Read(RE::PlayerCharacter* player);
}
