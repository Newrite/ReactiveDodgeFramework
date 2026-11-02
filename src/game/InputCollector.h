#pragma once

#include "core/InputBuffer.h"
#include <optional>
#include "core/Math.h"

namespace rdf::game::input
{
	struct Press
	{
		CommandKind kind{ CommandKind::Dodge };
		bool        promote{ false };  // attack held long enough: upgrade the pending attack to a power attack
	};

	// Registers the BSInputDeviceManager sink (kDataLoaded).
	void Install();

	// Main thread: returns queued presses, oldest first, and clears the queue.
	template <class F>
	void Drain(F&& handler);

	namespace detail
	{
		bool Pop(Press& out);
	}

	// Gameplay input is blocked by pause, console and dialogue (dialogue does not pause).
	bool IsBlocked();
	// Drops queued gameplay presses.
	void DiscardQueued();

	// Analog move input (x right, y forward) rotated into world space by the camera yaw. Length 0..1.
	// `cameraYaw`: interpret the stick against this camera yaw instead of the current one.
	Vec2  WorldMoveInput(std::optional<float> cameraYaw = std::nullopt);
	// The block ("Left Attack/Block") button is down right now.
	bool  BlockHeld();
	float CameraYaw();

	template <class F>
	void Drain(F&& handler)
	{
		if (IsBlocked()) {
			DiscardQueued();
			return;
		}
		Press p;
		while (detail::Pop(p)) {
			handler(p);
		}
	}
}
