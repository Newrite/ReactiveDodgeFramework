#include "MotionProfile.h"

#include <algorithm>

namespace rdf
{
	float MotionProfile::SegSpeed(const Segment& s, float tau)
	{
		if (s.dur <= 0.0f) {
			return s.v1;
		}
		return s.v0 + (s.v1 - s.v0) * Smoothstep(tau / s.dur);
	}

	float MotionProfile::SegDistance(const Segment& s, float tau)
	{
		if (s.dur <= 0.0f) {
			return 0.0f;
		}
		tau = std::clamp(tau, 0.0f, s.dur);
		return s.v0 * tau + (s.v1 - s.v0) * s.dur * SmoothstepIntegral(tau / s.dur);
	}

	void MotionProfile::Plan(const Timing& timing, const MotionConfig& motion, float carrySpeed, float exitSpeed)
	{
		const float v0 = std::max(carrySpeed, 0.0f);
		const float weight = 0.5f * timing.startup + timing.active + 0.5f * timing.recovery;
		const float numerator = motion.distance - 0.5f * timing.startup * v0 - 0.5f * timing.recovery * exitSpeed;
		peak_ = weight > 0.0f ? std::max(numerator / weight, 0.0f) : 0.0f;

		segs_ = { Segment{ timing.startup, v0, peak_ }, Segment{ timing.active, peak_, peak_ },
			Segment{ timing.recovery, peak_, exitSpeed } };
		tail_ = {};
		cut_ = -1.0f;
		total_ = timing.Total();
	}

	void MotionProfile::RetargetExit(float exitSpeed)
	{
		if (cut_ < 0.0f) {
			segs_[2].v1 = exitSpeed;
		}
	}

	void MotionProfile::BlendOut(float t, float exitSpeed, float blend)
	{
		if (cut_ >= 0.0f) {
			return;
		}
		t = std::clamp(t, 0.0f, total_);
		tail_ = Segment{ blend, Speed(t), exitSpeed };
		cut_ = t;
		total_ = t + blend;
	}

	float MotionProfile::Speed(float t) const
	{
		if (t < 0.0f || t >= total_) {
			return t < 0.0f ? segs_[0].v0 : (cut_ >= 0.0f ? tail_.v1 : segs_[2].v1);
		}
		if (cut_ >= 0.0f && t >= cut_) {
			return SegSpeed(tail_, t - cut_);
		}
		for (const auto& s : segs_) {
			if (t < s.dur) {
				return SegSpeed(s, t);
			}
			t -= s.dur;
		}
		return segs_[2].v1;
	}

	float MotionProfile::Distance(float t) const
	{
		t = std::clamp(t, 0.0f, total_);
		const float original = cut_ >= 0.0f ? std::min(t, cut_) : t;

		float sum = 0.0f;
		float rest = original;
		for (const auto& s : segs_) {
			sum += SegDistance(s, rest);
			rest -= s.dur;
			if (rest <= 0.0f) {
				break;
			}
		}
		if (cut_ >= 0.0f && t > cut_) {
			sum += SegDistance(tail_, t - cut_);
		}
		return sum;
	}
}
