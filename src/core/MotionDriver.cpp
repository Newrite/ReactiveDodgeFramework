#include "MotionDriver.h"

#include <algorithm>
#include <cmath>

namespace rdf
{
	MotionDriver::MotionDriver(const Config& cfg) :
		cfg_(cfg), steering_(cfg.steering) {}

	void MotionDriver::Begin(const DriverStart& s)
	{
		if (s.chain) {
			latchedHeading_ = WrapAngle(s.heading);
			latched_ = true;
		} else {
			steering_.Reset(s.heading);
			latched_ = false;
		}
		facing_ = WrapAngle(s.facing);
		origin_ = WrapAngle(s.heading);
		keepFacing_ = s.keepFacing;
		holdFacing_ = s.keepFacing && s.holdFacing;
		runSpeed_ = cfg_.motion.runSpeed;
		faceYawRate_ = cfg_.steering.faceYawRate;
		startupYawRate_ = cfg_.steering.startupYawRate;
		commitTime_ = cfg_.steering.commitTime;
		warpMax_ = cfg_.steering.orientationWarpMax;
		warpWanted_ = s.keepFacing && s.warp;
		warp_ = warpWanted_ && warpMax_ > 0.0f;
		distance_ = cfg_.motion.distance;
		clipAngle_ = s.clipAngle;
		warpOffset_ = 0.0f;
		exitFrom_ = -1.0f;
		exitBlend_ = 0.0f;
		heldFacing_ = facing_;
		timing_ = cfg_.timing;
		steering_.ResetRates(cfg_.steering);
		maxDeviation_ = cfg_.steering.maxDeviation;
		carrySpeed_ = s.carrySpeed;
		exitSpeed_ = ExitSpeed(s.moveHeld);
		profile_.Plan(timing_, cfg_.motion, carrySpeed_, exitSpeed_);
		speed_ = profile_.Speed(0.0f);
		retargeted_ = false;
		exiting_ = false;
	}

	bool MotionDriver::ResolveSteering(const DriverInput& in, float& desired, float& magnitude)
	{
		const auto& st = cfg_.steering;
		if (in.hasMove && steering_.Strength(in.moveMagnitude) > 0.0f &&
			std::fabs(AngleDelta(steering_.Heading(), in.moveHeading)) <= st.inputCone * kDegToRad + 1e-5f) {
			latched_ = false;
			// Bend toward the input, but never past maxDeviation from where this dodge set off.
			const float limit = maxDeviation_ * kDegToRad;
			const float off = std::clamp(AngleDelta(origin_, in.moveHeading), -limit, limit);
			desired = WrapAngle(origin_ + off);
			magnitude = in.moveMagnitude;
			return true;
		}
		if (latched_) {
			if (std::fabs(AngleDelta(steering_.Heading(), latchedHeading_)) < 1e-4f) {
				latched_ = false;
				return false;
			}
			desired = latchedHeading_;
			magnitude = 1.0f;
			return true;
		}
		return false;
	}

	// Full warp through startup and active, eased back to none over the recovery, so the body ends the
	// dodge facing the way it started (the camera / lock-on takes over from there).
	float MotionDriver::WarpWeight(float t) const
	{
		const float from = timing_.startup + timing_.active;
		float       w = 1.0f;
		if (t > from) {
			w = timing_.recovery > 0.0f ? 1.0f - Smoothstep(std::clamp((t - from) / timing_.recovery, 0.0f, 1.0f)) : 0.0f;
		}
		// Cut short (into running, an attack or a block): unwind within the hand-over instead.
		if (exitFrom_ >= 0.0f) {
			const float u = exitBlend_ > 0.0f ? std::clamp((t - exitFrom_) / exitBlend_, 0.0f, 1.0f) : 1.0f;
			w = std::min(w, 1.0f - Smoothstep(u));
		}
		return w;
	}

	float MotionDriver::YawRate(DodgePhase phase) const
	{
		return (phase == DodgePhase::Startup ? startupYawRate_ : faceYawRate_) * kDegToRad;
	}

	MotionCommand MotionDriver::Step(float t0, float t1, float dt, DodgePhase phase, const DriverInput& in)
	{
		const float recoveryStart = timing_.startup + timing_.active;
		if (!retargeted_ && !exiting_ && t1 >= recoveryStart) {
			profile_.RetargetExit(ExitSpeed(in.hasMove));
			retargeted_ = true;
		}

		// Commit window: the direction is still being chosen, so the live input re-aims the dodge outright.
		const bool committing = t0 < commitTime_ && in.hasMove && steering_.Strength(in.moveMagnitude) > 0.0f;
		if (committing) {
			steering_.Reset(in.moveHeading);
			origin_ = steering_.Heading();
			latched_ = false;
		} else {
			float desired = 0.0f;
			float magnitude = 0.0f;
			const bool has = ResolveSteering(in, desired, magnitude);
			steering_.Update(has, desired, magnitude, dt, phase);
		}

		const float distance = profile_.Distance(t1) - profile_.Distance(t0);
		const float v = dt > 0.0f ? distance / dt : 0.0f;
		speed_ = profile_.Speed(t1);

		// Closed loop on the real yaw: whatever else turned the actor since the last step is
		// corrected here instead of accumulating as a drift between our facing and the actor.
		// When keeping facing, the camera / lock-on owns the yaw and the clip carries the direction.
		const float actual = WrapAngle(in.actorYaw);
		float       yawRate = 0.0f;
		facing_ = actual;
		warpOffset_ = 0.0f;
		const float weight = WarpWeight(t1);
		if (warp_) {
			const float limit = warpMax_ * kDegToRad;
			float       rest = AngleDelta(WrapAngle(heldFacing_ + clipAngle_), steering_.Heading());
			rest = std::fabs(rest) < 1e-4f ? 0.0f : rest;  // the clip already runs along the path
			warpOffset_ = std::clamp(rest, -limit, limit) * weight;
		}
		if (OwnsYaw() && dt > 0.0f) {
			// A kept facing belongs to the camera (or, locked on, to the target the camera looks at): while
			// the dodge eases out it turns toward the camera, so the camera / TDM finds the actor already
			// facing its way instead of snapping it there on the next attack.
			const float held = WrapAngle(heldFacing_ + warpOffset_);
			const float target = keepFacing_ ? WrapAngle(held + AngleDelta(held, in.cameraYaw) * (1.0f - weight)) : steering_.Heading();
			facing_ = MoveToward(actual, target, YawRate(phase) * dt);
			yawRate = AngleDelta(actual, facing_) / dt;
		}

		const Vec2 dir = FromHeading(steering_.Heading());
		MotionCommand cmd;
		cmd.active = 1;
		cmd.vx = dir.x * v;
		cmd.vy = dir.y * v;
		cmd.yawRate = yawRate;
		cmd.speed = v;
		return cmd;
	}

	void MotionDriver::BeginStop(float t, float blend, bool toRun)
	{
		profile_.BlendOut(t, ExitSpeed(toRun), blend);
		exiting_ = true;
		if (exitFrom_ < 0.0f) {
			exitFrom_ = t;
			exitBlend_ = blend;
		}
	}

	void MotionDriver::Replan(const Timing& timing, float distance)
	{
		if (exiting_) {
			return;
		}
		timing_.startup = timing.startup;
		timing_.active = timing.active;
		timing_.recovery = timing.recovery;
		distance_ = distance;
		auto motion = cfg_.motion;
		motion.distance = distance_;
		profile_.Plan(timing_, motion, carrySpeed_, exitSpeed_);
		retargeted_ = false;  // the recovery may start later now: the exit speed is decided there again
	}

	void MotionDriver::SetSteering(const ClipSteering& steering)
	{
		steering_.SetRates(steering.rates[0], steering.rates[1], steering.rates[2]);
		if (steering.maxDeviation) {
			maxDeviation_ = *steering.maxDeviation;
		}
	}

	void MotionDriver::SetDriving(const ClipDriving& d)
	{
		if (d.warpMax) {
			warpMax_ = *d.warpMax;
			warp_ = warpWanted_ && warpMax_ > 0.0f;
		}
		if (d.faceYawRate) {
			faceYawRate_ = *d.faceYawRate;
		}
		if (d.startupYawRate) {
			startupYawRate_ = *d.startupYawRate;
		}
		if (d.commitTime) {
			commitTime_ = *d.commitTime;
		}
		if (d.runSpeed) {
			runSpeed_ = *d.runSpeed;
			// A dodge planned to hand over into running ends at the new speed; one planned to stop is unchanged. Not once
			// it is exiting: the blend-out already runs toward its own speed.
			if (!exiting_ && exitSpeed_ > 0.0f) {
				exitSpeed_ = runSpeed_;
				Replan(timing_, distance_);
			}
		}
	}

	void MotionDriver::BeginExit(float t, bool moveHeld, float blend)
	{
		profile_.BlendOut(t, ExitSpeed(moveHeld), blend);
		exiting_ = true;
		if (exitFrom_ < 0.0f) {
			exitFrom_ = t;
			exitBlend_ = blend;
		}
	}
}
