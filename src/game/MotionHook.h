#pragma once

#include "core/MotionCommand.h"

#include <cstdint>

namespace rdf::game::motion
{
	// Replaces the single call to Character::ProcessMotionData inside
	// MovementTweenerAgentAnimationDriven::BuildOutput. Returns false (and installs nothing)
	// when the bytes at the call site are not the expected 1.5.97 call.
	bool Install();
	// MovementControllerNPC vfunc 7 (the controls-driven movement payload): applies the published
	// speed floor for the owner after a dodge.
	bool InstallSpeedFloor();

	// Main thread only. Stamps the owner/generation and publishes through the seqlock.
	void Publish(MotionCommand cmd, std::uint32_t ownerFormId);
	// Invalidates every earlier snapshot (load, new game, 3D reset) and publishes "inactive".
	void Reset();

	struct HookStats
	{
		std::uint32_t calls{ 0 };      // all invocations
		std::uint32_t overrides{ 0 };  // invocations that wrote our displacement
		std::uint32_t floorHits{ 0 };  // controls-driven payloads raised to the post-dodge speed floor
		std::uint32_t firstThread{ 0 };
		std::uint32_t lastThread{ 0 };
		float         lastLocalX{ 0 }, lastLocalY{ 0 }, lastYaw{ 0 };  // last override output
	};
	HookStats ReadStats();
}
