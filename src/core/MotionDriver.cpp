#include "MotionDriver.h"

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
		profile_.Plan(cfg_.timing, cfg_.motion, s.carrySpeed, ExitSpeed(s.moveHeld));
		speed_ = profile_.Speed(0.0f);
		retargeted_ = false;
		exiting_ = false;
	}

	bool MotionDriver::ResolveSteering(const DriverInput& in, float& desired, float& magnitude)
	{
		if (in.hasMove && steering_.Strength(in.moveMagnitude) > 0.0f) {
			latched_ = false;
			desired = in.moveHeading;
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

	float MotionDriver::YawRate(DodgePhase phase) const
	{
		return (phase == DodgePhase::Startup ? cfg_.steering.startupYawRate : cfg_.steering.faceYawRate) * kDegToRad;
	}

	MotionCommand MotionDriver::Step(float t0, float t1, float dt, DodgePhase phase, const DriverInput& in)
	{
		const float recoveryStart = cfg_.timing.startup + cfg_.timing.active;
		if (!retargeted_ && !exiting_ && t1 >= recoveryStart) {
			profile_.RetargetExit(ExitSpeed(in.hasMove));
			retargeted_ = true;
		}

		float desired = 0.0f;
		float magnitude = 0.0f;
		const bool has = ResolveSteering(in, desired, magnitude);
		steering_.Update(has, desired, magnitude, dt, phase);

		const float distance = profile_.Distance(t1) - profile_.Distance(t0);
		const float v = dt > 0.0f ? distance / dt : 0.0f;
		speed_ = profile_.Speed(t1);

		// Closed loop on the real yaw: whatever else turned the actor since the last step is
		// corrected here instead of accumulating as a drift between our facing and the actor.
		// When keeping facing, the camera / lock-on owns the yaw and the clip carries the direction.
		const float actual = WrapAngle(in.actorYaw);
		float       yawRate = 0.0f;
		facing_ = actual;
		if (!KeepsFacing(cfg_.steering, in.lockOn, in.freeRotation) && dt > 0.0f) {
			facing_ = MoveToward(actual, steering_.Heading(), YawRate(phase) * dt);
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

	void MotionDriver::BeginExit(float t, bool moveHeld)
	{
		profile_.BlendOut(t, ExitSpeed(moveHeld), cfg_.timing.exitBlend);
		exiting_ = true;
	}
}
