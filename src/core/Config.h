#pragma once

#include <cmath>
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
		float         exitFrom{ 0.34f };
		int           maxChain{ 3 };  // 0 = unlimited
		std::uint32_t attackStateMask{ attack_state::Bit(attack_state::kFollowThrough) | attack_state::Bit(attack_state::kNextAttack) |
			                           attack_state::Bit(attack_state::kBowDrawn) | attack_state::Bit(attack_state::kBowReleased) |
			                           attack_state::Bit(attack_state::kBowFollowThrough) };

		bool cancelInRecovery{ true };  // an attack framework's recovery (MCO / BFCO) lets a dodge cancel the attack

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
		float exitBlend{ 0.25f };        // blend-out length when a dodge is cancelled into locomotion
		float actionExitBlend{ 0.15f };  // momentum carried into a block cancel, decaying to zero
		// ... and into an attack cancel. 0 = the attack's own root motion takes over at once: attack frameworks pick
		// moving / sprint variants from the graph's Speed, and the dodge's momentum would make them do that.
		float attackExitBlend{ 0.0f };
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
		float           faceMotionMaxAngle{ 100.0f };  // Auto + free rotation: wider dodges keep facing (backsteps)
		// How far input may bend a running dodge. 180 = anywhere (default), 0 = the start direction is final.
		float           maxDeviation{ 180.0f };  // deg the trajectory may leave its start heading
		float           inputCone{ 180.0f };     // deg: input farther than this from the current heading is ignored
		// Dodges that keep facing (camera / Vanilla-style TDM): hold the start yaw instead of letting the
		// camera turn the actor. A locked target and first person keep owning the yaw.
		bool            holdFacing{ false };
		// The first commitTime seconds of a dodge re-aim it at the live input instead of bending it, so a
		// press made a frame before the stick / keys arrived still goes where the player meant. 0 = off.
		float           commitTime{ 0.06f };
		// Directional (keep-facing) clips come in 45-degree steps; the body turns by the rest of the angle,
		// at most this much, so the clip runs along the real path, and turns back during the recovery.
		// 0 = off. Not in first person (the yaw is the view).
		float           orientationWarpMax{ 22.5f };
		OrientationMode orientation{ OrientationMode::Auto };
	};

	// True when the actor keeps its facing (lock-on strafing) instead of turning to the trajectory.
	// Auto: turn to face the trajectory only when the actor may rotate freely (TDM 360 movement)
	// and no target is locked; otherwise the camera or the lock-on owns the facing.
	// Even with free rotation, a dodge more than faceMotionMaxAngle away from the facing keeps it
	// and plays a backward clip: a 180-degree snap-turn before a backstep reads as a spin.
	// Decided once per dodge from the angle between the facing and the start heading.
	inline bool KeepsFacing(const SteeringConfig& cfg, bool lockOn, bool freeRotation, float startAngleDeg)
	{
		switch (cfg.orientation) {
		case OrientationMode::KeepFacing: return true;
		case OrientationMode::FaceMotion: return false;
		default: return lockOn || !freeRotation || std::fabs(startAngleDeg) > cfg.faceMotionMaxAngle;
		}
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
