#include "Steering.h"

#include <cmath>

namespace rdf
{
	float Steering::Strength(float magnitude) const
	{
		if (magnitude <= cfg_.deadzone) {
			return 0.0f;
		}
		const float s = Clamp01((magnitude - cfg_.deadzone) / (1.0f - cfg_.deadzone));
		return std::pow(s, cfg_.responseExponent);
	}

	float Steering::MaxRate(DodgePhase phase) const
	{
		switch (phase) {
		case DodgePhase::Startup:
			return cfg_.rateStartup * kDegToRad;
		case DodgePhase::Active:
			return cfg_.rateActive * kDegToRad;
		default:
			return cfg_.rateRecovery * kDegToRad;
		}
	}

	float Steering::Update(bool hasDesired, float desiredHeading, float magnitude, float dt, DodgePhase phase)
	{
		const float strength = hasDesired ? Strength(magnitude) : 0.0f;
		if (strength > 0.0f && dt > 0.0f) {
			heading_ = MoveToward(heading_, desiredHeading, MaxRate(phase) * strength * dt);
		}
		return heading_;
	}
}
