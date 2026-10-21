#pragma once

#include "Config.h"
#include "DodgeAction.h"
#include "MotionCommand.h"
#include "MotionProfile.h"
#include "Steering.h"

namespace rdf
{
	struct DriverInput
	{
		bool  hasMove{ false };  // analog move input present this frame
		float moveHeading{ 0.0f };   // world heading of the input
		float moveMagnitude{ 0.0f };  // 0..1
		float actorYaw{ 0.0f };   // the actor's real yaw this frame (other mods may rotate it too)
	};

	struct DriverStart
	{
		float heading{ 0.0f };       // initial trajectory heading (ignored when `chain`)
		float facing{ 0.0f };        // actor yaw now
		float carrySpeed{ 0.0f };    // speed at the moment the dodge begins
		bool  moveHeld{ false };     // decides the exit speed
		bool  chain{ false };        // continue the previous trajectory and turn toward `heading`
		bool  keepFacing{ false };   // the camera / lock-on owns the yaw for this dodge
		bool  holdFacing{ false };   // keepFacing, but the actor holds `facing` instead (SteeringConfig::holdFacing)
		bool  warp{ false };         // keepFacing: turn the body so the directional clip runs along the path
		float clipAngle{ 0.0f };     // rad, direction of the chosen clip relative to `facing` (45-degree steps)
	};

	// Composes Steering x MotionProfile into a world velocity and a yaw rate.
	class MotionDriver
	{
	public:
		explicit MotionDriver(const Config& cfg);

		void Begin(const DriverStart& start);
		// Advance from action time t0 to t1 (dt of wall game time). Returns the command to publish;
		// `velocity` is the average over the step so integrating it with the same dt is exact.
		MotionCommand Step(float t0, float t1, float dt, DodgePhase phase, const DriverInput& in);
		void          BeginExit(float t, bool moveHeld);
		// Cancel into an action: decelerate to rest over `blend` instead of stopping dead.
		void          BeginStop(float t, float blend);

		float Speed() const { return speed_; }
		float Heading() const { return steering_.Heading(); }
		float Facing() const { return facing_; }
		bool  KeepsFacing() const { return keepFacing_; }
		bool  HoldsFacing() const { return holdFacing_; }
		// The dodge commands the actor's yaw (and so takes it from TDM): face motion, hold facing or warp.
		bool  OwnsYaw() const { return !keepFacing_ || holdFacing_ || warp_; }
		float WarpOffset() const { return warpOffset_; }  // rad, body turn applied this step (diagnostics)
		float Peak() const { return profile_.Peak(); }

	private:
		float ExitSpeed(bool moveHeld) const { return moveHeld ? cfg_.motion.runSpeed : 0.0f; }
		float YawRate(DodgePhase phase) const;
		float WarpWeight(float t) const;
		bool  ResolveSteering(const DriverInput& in, float& desired, float& magnitude);

		Config        cfg_;
		Steering      steering_;
		MotionProfile profile_;
		float         facing_{ 0.0f };
		float         speed_{ 0.0f };
		float         latchedHeading_{ 0.0f };
		float         origin_{ 0.0f };      // start heading of this dodge: maxDeviation is measured from it
		float         heldFacing_{ 0.0f };
		bool          latched_{ false };
		bool          retargeted_{ false };
		bool          exiting_{ false };
		bool          keepFacing_{ false };
		bool          holdFacing_{ false };
		bool          warp_{ false };
		float         clipAngle_{ 0.0f };
		float         warpOffset_{ 0.0f };
		float         exitFrom_{ -1.0f };   // action time the exit / stop began (-1 = none)
		float         exitBlend_{ 0.0f };
	};
}
