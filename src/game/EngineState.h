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
	// `ownDodge`: our dodge is starting or running, so the stagger state the graph is still blending out
	// of (IsStaggering) does not count; only the actor's own stagger flag does.
	Snapshot Read(RE::PlayerCharacter* player, bool ownDodge);
	// What the engine's StaggeredStopHandler does (0x140721280: actorState2 bit 13 = staggered).
	void EndStagger(RE::PlayerCharacter* player);
}
