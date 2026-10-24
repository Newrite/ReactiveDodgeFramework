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
	//   RDF_ExitBlend.<run>,<block>,<attack> (or one for all)  its own fExitBlend / fActionExitBlend / fAttackExitBlend
	//   RDF_ArmStance.<share>                                  its own fArmStance (fArmStanceFirstPerson)
	//   RDF_Steer.<startup>,<active>,<recovery>[,<maxDeviation>]  its own [Steering] fRate* (deg/s) and fMaxDeviation
	//   RDF_Warp.<degrees>                                     its own fOrientationWarpMax
	//   RDF_RunSpeed.<units per second>                        its own [Motion] fRunSpeed
	//   RDF_YawRate.<face>[,<startup>]                         its own fFaceYawRate (and fStartupYawRate), deg/s
	//   RDF_Commit.<seconds>                                   its own fDirectionCommitTime
	// The graph splits an annotation at its first '.', so the numbers arrive as the event's payload.
	struct ClipMotion
	{
		std::optional<float>                distance;
		std::optional<std::array<float, 3>> phases;  // startup, active, recovery
	};

	// How the dodge drives the actor besides its path (the clip's RDF_Warp / RunSpeed / YawRate / Commit): each one replaces
	// the ini's value when set.
	struct ClipDriving
	{
		std::optional<float> warpMax;         // deg
		std::optional<float> runSpeed;        // units/s
		std::optional<float> faceYawRate;     // deg/s
		std::optional<float> startupYawRate;  // deg/s
		std::optional<float> commitTime;      // s
	};

	struct ClipSteering
	{
		std::array<float, 3> rates{};  // deg/s in startup, active, recovery
		std::optional<float> maxDeviation;  // deg
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
		inline constexpr float kMinBlend = 0.02f;  // the ini's ranges ([Timing] f*ExitBlend)
		inline constexpr float kMaxBlend = 1.0f;
		inline constexpr float kMaxRate = 10000.0f;
		inline constexpr float kMaxDeviation = 180.0f;
		inline constexpr float kMaxWarp = 90.0f;     // the ini ranges
		inline constexpr float kMaxRunSpeed = 1500.0f;
		inline constexpr float kMaxCommit = 0.5f;

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

		// Clip-local seconds at speed 1; delivered on the first frame, then divided by playback speed.
		inline std::optional<float> ParseWindowStart(std::string_view text)
		{
			const auto value = Number(text);
			return value && text.empty() && *value >= 0.0f && *value <= kMaxLength ? value : std::nullopt;
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

		// Up to N comma-separated numbers: how many were read, or nullopt when malformed.
		template <std::size_t N>
		std::optional<std::size_t> List(std::string_view text, std::array<float, N>& out)
		{
			std::size_t n = 0;
			while (n < N) {
				const auto x = Number(text);
				if (!x) {
					return std::nullopt;
				}
				out[n++] = *x;
				if (text.empty()) {
					return n;
				}
				if (text.front() != ',') {
					return std::nullopt;
				}
				text.remove_prefix(1);
			}
			return std::nullopt;  // more than N
		}

		// "<all>" or "<run>,<block>,<attack>" (attack may be 0: the attack's own root motion at once).
		inline std::optional<std::array<float, 3>> ParseExitBlends(std::string_view text)
		{
			std::array<float, 3> v{};
			const auto           n = List(text, v);
			if (n == 1u) {
				v[1] = v[2] = v[0];
			} else if (n != 3u) {
				return std::nullopt;
			}
			if (v[0] < kMinBlend || v[0] > kMaxBlend || v[1] < kMinBlend || v[1] > kMaxBlend || v[2] < 0.0f || v[2] > kMaxBlend) {
				return std::nullopt;
			}
			return v;
		}

		inline std::optional<ClipSteering> ParseSteering(std::string_view text)
		{
			std::array<float, 4> v{};
			const auto           n = List(text, v);
			if (n != 3u && n != 4u) {
				return std::nullopt;
			}
			ClipSteering s;
			for (std::size_t i = 0; i < 3; ++i) {
				if (v[i] < 0.0f || v[i] > kMaxRate) {
					return std::nullopt;
				}
				s.rates[i] = v[i];
			}
			if (n == 4u) {
				if (v[3] < 0.0f || v[3] > kMaxDeviation) {
					return std::nullopt;
				}
				s.maxDeviation = v[3];
			}
			return s;
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
		inline std::optional<float> ParseArmStance(std::string_view text) { return ParseInRange(text, 0.0f, 1.0f); }
		inline std::optional<float> ParseWarp(std::string_view text) { return ParseInRange(text, 0.0f, kMaxWarp); }
		inline std::optional<float> ParseRunSpeed(std::string_view text) { return ParseInRange(text, 0.0f, kMaxRunSpeed); }
		inline std::optional<float> ParseCommit(std::string_view text) { return ParseInRange(text, 0.0f, kMaxCommit); }

		// "<face>" or "<face>,<startup>"
		inline std::optional<ClipDriving> ParseYawRates(std::string_view text)
		{
			std::array<float, 2> v{};
			const auto           n = List(text, v);
			if (!n || v[0] < 0.0f || v[0] > kMaxRate || (*n == 2 && (v[1] < 0.0f || v[1] > kMaxRate))) {
				return std::nullopt;
			}
			ClipDriving d;
			d.faceYawRate = v[0];
			if (*n == 2) {
				d.startupYawRate = v[1];
			}
			return d;
		}
	}
}
