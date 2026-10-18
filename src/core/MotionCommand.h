#pragma once

#include <cstdint>

namespace rdf
{
	// Immutable snapshot the main thread publishes for the root-motion hook.
	struct MotionCommand
	{
		std::uint32_t active{ 0 };       // 1 = the hook owns this actor's displacement
		std::uint32_t ownerFormId{ 0 };  // actor the command is for
		std::uint32_t generation{ 0 };   // bumped on load/reset; stale snapshots are ignored
		float         vx{ 0.0f };        // world velocity, units/s (x east, y north)
		float         vy{ 0.0f };
		float         yawRate{ 0.0f };   // rad/s, clockwise positive (Skyrim angle.z convention)
		float         speed{ 0.0f };     // |v|, diagnostics only
	};
}
