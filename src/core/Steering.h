#pragma once

#include "Config.h"
#include "DodgeAction.h"
#include "Math.h"

namespace rdf
{
	// Heading of the dodge trajectory. Turns toward the desired heading by the shortest arc,
	// capped by a phase-dependent angular velocity scaled by stick strength. Without input the
	// heading is kept, so a reversal is never instantaneous and a release freezes the path.
	class Steering
	{
	public:
		explicit Steering(const SteeringConfig& cfg = {}) :
			cfg_(cfg) {}

		void  Reset(float heading) { heading_ = WrapAngle(heading); }
		float Heading() const { return heading_; }

		// Stick strength in [0,1]: 0 inside the deadzone, then ((m - dz) / (1 - dz)) ^ exponent.
		float Strength(float magnitude) const;
		float MaxRate(DodgePhase phase) const;  // rad/s

		// `hasDesired` false = no input: heading unchanged. Returns the new heading.
		float Update(bool hasDesired, float desiredHeading, float magnitude, float dt, DodgePhase phase);

	private:
		SteeringConfig cfg_;
		float          heading_{ 0.0f };
	};
}
