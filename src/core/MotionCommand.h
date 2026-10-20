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
		// The engine's controls-driven speed may not drop below this (units/s). Published from the
		// hand-over into running on, so it bridges the engine's re-acceleration from its first frame.
		float speedFloor{ 0.0f };
		// First person: the camera's yaw is the actor's, and while the motion is animation driven the
		// engine's own mouse turn (MovementHandlerAgentPlayerControls, lookInputVec.x * rotateInPlaceRun)
		// is not applied. The hook adds lookInputVec.x * lookYawRate per second. 0 = off.
		float lookYawRate{ 0.0f };
	};
}
