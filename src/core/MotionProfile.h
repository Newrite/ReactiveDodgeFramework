#pragma once

#include "Config.h"
#include "Math.h"

#include <array>

namespace rdf
{
	// Speed curve of one dodge: smoothstep from the carry-in speed to a peak (startup), plateau
	// (active), smoothstep down to the exit speed (recovery). The peak is solved so the curve
	// integrates to the configured distance. All queries are analytic, so the distance covered
	// over any time step is exact regardless of the frame rate.
	class MotionProfile
	{
	public:
		void Plan(const Timing& timing, const MotionConfig& motion, float carrySpeed, float exitSpeed);

		// Change the speed the recovery ends at (only before a blend-out was requested).
		void RetargetExit(float exitSpeed);
		// Replace everything after `t` by a smoothstep from the current speed to `exitSpeed` over `blend`.
		void BlendOut(float t, float exitSpeed, float blend);

		float Speed(float t) const;
		float Distance(float t) const;  // cumulative distance travelled by time t
		float Total() const { return total_; }
		float Peak() const { return peak_; }

	private:
		struct Segment
		{
			float dur{ 0.0f };
			float v0{ 0.0f };
			float v1{ 0.0f };
		};
		static float SegSpeed(const Segment& s, float tau);
		static float SegDistance(const Segment& s, float tau);

		std::array<Segment, 3> segs_{};
		Segment                tail_{};
		float                  cut_{ -1.0f };  // >= 0 once a blend-out replaced the rest of the curve
		float                  total_{ 0.0f };
		float                  peak_{ 0.0f };
	};
}
