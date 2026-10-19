#pragma once

#include <cstdint>

namespace rdf
{
	enum class OrientationMode : std::uint8_t
	{
		Auto,        // KeepFacing while locked on, FaceMotion otherwise
		FaceMotion,  // actor yaw follows the trajectory heading
		KeepFacing,  // actor yaw follows the lock-on target (or stays put)
	};

	// Raw values of RE::ATTACK_STATE_ENUM (the game layer static_asserts the correspondence).
	namespace attack_state
	{
		constexpr std::uint8_t kNone = 0, kDraw = 1, kSwing = 2, kHit = 3, kNextAttack = 4, kFollowThrough = 5,
		                       kBash = 6, kBowDraw = 8, kBowAttached = 9, kBowDrawn = 10, kBowReleasing = 11,
		                       kBowReleased = 12, kBowNextAttack = 13, kBowFollowThrough = 14, kFire = 15,
		                       kFiring = 16, kFired = 17;
		constexpr std::uint32_t Bit(std::uint8_t s) { return 1u << s; }
	}

	// Which commands may cancel which part of a dodge, and from when (seconds since dodge start).
	struct ActionPolicy
	{
		float         attackFrom{ 0.30f };
		float         chainFrom{ 0.34f };
		float         blockFrom{ 0.30f };
		float         exitFrom{ 0.45f };
		int           maxChain{ 3 };  // 0 = unlimited
		std::uint32_t attackStateMask{ attack_state::Bit(attack_state::kFollowThrough) | attack_state::Bit(attack_state::kNextAttack) |
			                           attack_state::Bit(attack_state::kBowDrawn) | attack_state::Bit(attack_state::kBowReleased) |
			                           attack_state::Bit(attack_state::kBowFollowThrough) };

		bool DodgeAllowedFromAttackState(std::uint8_t state) const
		{
			return state == attack_state::kNone || (state < 32 && (attackStateMask & attack_state::Bit(state)) != 0);
		}
	};

	struct Timing
	{
		float startup{ 0.08f };
		float active{ 0.30f };
		float recovery{ 0.32f };
		float exitBlend{ 0.15f };  // blend-out length when a dodge is cancelled into locomotion
		float Total() const { return startup + active + recovery; }
		// Playback multiplier that makes a clip of `clipLength` seconds (at speed 1) end with the recovery.
		float PlaybackSpeed(float clipLength, float multiplier) const
		{
			return Total() > 0.0f ? multiplier * clipLength / Total() : multiplier;
		}
	};

	struct MotionConfig
	{
		float distance{ 300.0f };       // units travelled by an uncancelled dodge
		float runSpeed{ 300.0f };       // exit speed when move input is held
		float maxCarrySpeed{ 450.0f };  // cap on the carry-in speed
		bool  backstepWithoutInput{ true };  // no direction held: dodge backwards, otherwise forwards
	};

	struct SteeringConfig
	{
		float           rateStartup{ 900.0f };  // deg/s
		float           rateActive{ 260.0f };
		float           rateRecovery{ 600.0f };
		float           deadzone{ 0.2f };
		float           responseExponent{ 1.0f };  // strength = ((m - dz) / (1 - dz)) ^ exponent
		float           faceYawRate{ 720.0f };     // deg/s: actor yaw follows its target at most this fast
		float           startupYawRate{ 3600.0f };  // deg/s during Startup: effectively a snap-turn
		OrientationMode orientation{ OrientationMode::Auto };
	};

	// True when the actor keeps its facing (lock-on strafing) instead of turning to the trajectory.
	inline bool KeepsFacing(const SteeringConfig& cfg, bool lockOn)
	{
		return cfg.orientation == OrientationMode::KeepFacing || (cfg.orientation == OrientationMode::Auto && lockOn);
	}

	struct InputConfig
	{
		float bufferWindow{ 0.20f };
		float debounce{ 0.06f };
	};

	struct Config
	{
		InputConfig    input;
		Timing         timing;
		ActionPolicy   policy;
		MotionConfig   motion;
		SteeringConfig steering;
	};
}
