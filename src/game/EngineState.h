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
	// rotateInPlaceRun (rad/s) of the player's current movement type, the factor the engine applies to
	// lookInputVec.x to turn the player (MovementHandlerAgentPlayerControls 0x1406FBA80). Read through
	// ActorState vfunc 8 (PlayerCharacter's 0x1416640E8 slot 8 -> Character 0x1406082B0, which fills
	// the 11 movement-type floats plus 16 more bytes at +0x28). 0 if it fails.
	float LookTurnRate(RE::PlayerCharacter* player);
	// What the engine's StaggeredStopHandler does (0x140721280: actorState2 bit 13 = staggered).
	void EndStagger(RE::PlayerCharacter* player);
}
