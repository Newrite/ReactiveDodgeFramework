#pragma once

#include "core/InputBuffer.h"
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

	// Drops presses made while the game is paused or in a text-input menu.
	void DiscardQueued();

	// Analog move input (x right, y forward) rotated into world space by the camera yaw. Length 0..1.
	Vec2  WorldMoveInput();
	// The block ("Left Attack/Block") button is down right now.
	bool  BlockHeld();
	float CameraYaw();

	template <class F>
	void Drain(F&& handler)
	{
		Press p;
		while (detail::Pop(p)) {
			handler(p);
		}
	}
}
