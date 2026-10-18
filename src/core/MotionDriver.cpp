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
		facing_ = facingAtStart_ = WrapAngle(s.facing);
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

	float MotionDriver::FacingTarget(const DriverInput& in) const
	{
		const bool keep = cfg_.steering.orientation == OrientationMode::KeepFacing ||
		                  (cfg_.steering.orientation == OrientationMode::Auto && in.lockOn);
		if (!keep) {
			return steering_.Heading();
		}
		return in.lockOn ? in.lockOnYaw : facingAtStart_;
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

		const float newFacing = MoveToward(facing_, FacingTarget(in), cfg_.steering.faceYawRate * kDegToRad * dt);
		const float yawRate = dt > 0.0f ? AngleDelta(facing_, newFacing) / dt : 0.0f;
		facing_ = newFacing;

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
