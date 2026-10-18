#pragma once

#include <cmath>
#include <cstdint>

namespace rdf
{
	// Game-active timebase: only unpaused, already timescaled frame time accumulates.
	class Clock
	{
	public:
		void Advance(float dt)
		{
			if (!std::isfinite(dt) || dt <= 0.0f) {
				return;
			}
			now_ += static_cast<double>(dt);
			++frame_;
		}

		double        Now() const { return now_; }
		std::uint64_t Frame() const { return frame_; }

	private:
		double        now_{ 0.0 };
		std::uint64_t frame_{ 0 };
	};
}
