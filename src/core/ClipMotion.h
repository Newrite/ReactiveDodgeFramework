#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <string_view>

namespace rdf
{
	// How the clip playing a dodge says it moves (ANIMATION_CONTRACT.md 5), from annotations at its first frame:
	//   RDF_Motion.<distance>[,<startup>,<active>,<recovery>]  the distance it covers and its phases (clip seconds at speed 1)
	//   RDF_ClipLength.<seconds>                               its real length (a clip still streaming in reports 0)
	//   RDF_Speed.<multiplier>                                 its own [Graph] fAnimSpeed
	// The graph splits an annotation at its first '.', so the numbers arrive as the event's payload.
	struct ClipMotion
	{
		std::optional<float>                distance;
		std::optional<std::array<float, 3>> phases;  // startup, active, recovery
	};

	namespace clip_motion
	{
		inline constexpr float kMaxDistance = 5000.0f;
		inline constexpr float kMaxPhase = 5.0f;
		inline constexpr float kMinTotal = 0.05f;
		inline constexpr float kMinLength = 0.05f;
		inline constexpr float kMaxLength = 10.0f;
		inline constexpr float kMinSpeed = 0.1f;  // fAnimSpeed's own range
		inline constexpr float kMaxSpeed = 5.0f;

		inline std::optional<float> Number(std::string_view& text)
		{
			while (!text.empty() && text.front() == ' ') {
				text.remove_prefix(1);
			}
			float      v = 0.0f;
			const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
			if (ec != std::errc() || !(v == v)) {
				return std::nullopt;
			}
			text.remove_prefix(static_cast<std::size_t>(end - text.data()));
			while (!text.empty() && text.front() == ' ') {
				text.remove_prefix(1);
			}
			return v;
		}

		// "<distance>" or "<distance>,<startup>,<active>,<recovery>"; nullopt when malformed or out of range.
		inline std::optional<ClipMotion> ParseMotion(std::string_view text)
		{
			std::array<float, 4> v{};
			std::size_t          n = 0;
			while (n < v.size()) {
				const auto x = Number(text);
				if (!x) {
					return std::nullopt;
				}
				v[n++] = *x;
				if (text.empty()) {
					break;
				}
				if (text.front() != ',') {
					return std::nullopt;
				}
				text.remove_prefix(1);
			}
			if (!text.empty() || (n != 1 && n != 4) || v[0] < 0.0f || v[0] > kMaxDistance) {
				return std::nullopt;
			}
			ClipMotion m;
			m.distance = v[0];
			if (n == 4) {
				const std::array<float, 3> phases{ v[1], v[2], v[3] };
				for (const float p : phases) {
					if (p < 0.0f || p > kMaxPhase) {
						return std::nullopt;
					}
				}
				if (phases[0] + phases[1] + phases[2] < kMinTotal) {
					return std::nullopt;
				}
				m.phases = phases;
			}
			return m;
		}

		inline std::optional<float> ParseInRange(std::string_view text, float lo, float hi)
		{
			const auto v = Number(text);
			if (!v || !text.empty() || *v < lo || *v > hi) {
				return std::nullopt;
			}
			return v;
		}

		inline std::optional<float> ParseLength(std::string_view text) { return ParseInRange(text, kMinLength, kMaxLength); }
		inline std::optional<float> ParseSpeed(std::string_view text) { return ParseInRange(text, kMinSpeed, kMaxSpeed); }
	}
}
