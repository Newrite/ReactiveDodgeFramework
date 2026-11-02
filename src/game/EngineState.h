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

	// Read-only queries on an actor (player or NPC); the first matching block wins, strongest first.
	// `ownDodge`: our dodge is starting or running, so the stagger state the graph is still blending out
	// of (IsStaggering) does not count; only the actor's own stagger flag does.
	Snapshot Read(RE::Actor* actor, bool ownDodge);
	// rotateInPlaceRun (rad/s) of the player's current movement type, the factor the engine applies to
	// lookInputVec.x to turn the player (MovementHandlerAgentPlayerControls 0x1406FBA80). Read through
	// ActorState vfunc 8 (PlayerCharacter's 0x1416640E8 slot 8 -> Character 0x1406082B0, which fills
	// the 11 movement-type floats plus 16 more bytes at +0x28). 0 if it fails.
	float LookTurnRate(RE::PlayerCharacter* player);
	// What the engine's StaggeredStopHandler does (0x140721280: actorState2 bit 13 = staggered).
	void EndStagger(RE::Actor* actor);
	// The game's left button casts with this actor instead of attacking or blocking -- the engine's own condition, so a
	// mod's block never gets overruled by it: a spell in the left hand (any magic item but an enchantment) AND no block
	// wanted. AttackBlockHandler::ProcessButton (0x1407096B0) hands the press to that hand's magic caster on
	// `IsEquippedSpell(player, kLeftHand) && (actorState2 & 0x100) == 0` -- PlayerCharacter::IsEquippedSpell
	// (0x140632230) = `selectedSpells[kLeftHand]` (Actor+0x1C0, `Actor::selectedSpells` in CommonLib) with a spell type
	// other than kEnchantment -- and returns before any attack, block or power-attack handling, so a spell in that hand
	// also wins over a bow or a crossbow in the right one.
	bool LeftButtonCasts(RE::Actor* actor);
}
