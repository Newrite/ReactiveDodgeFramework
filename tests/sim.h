#pragma once

#include "core/ReactiveCore.h"

#include <initializer_list>
#include <vector>

namespace sim
{
	using namespace rdf;

	// Heading helpers: 0 = north (forward), +90 deg = east (right).
	inline Vec2 Dir(float headingDeg, float magnitude = 1.0f)
	{
		return FromHeading(headingDeg * kDegToRad) * magnitude;
	}

	struct Sample
	{
		double      t{ 0 };
		float       dt{ 0 };
		Decision    decision;
		bool        running{ false };
		DodgePhase  phase{ DodgePhase::Idle };
		MotionCommand cmd;
		Vec2        pos;
		float       heading{ 0 };
		float       facing{ 0 };
		float       actionTime{ 0 };
		int         chain{ 0 };
		bool        completed{ false };
	};

	// Drives ReactiveCore like the game layer does: every decision is accepted, and the published
	// velocity is integrated with the same dt the hook would use.
	class Sim
	{
	public:
		explicit Sim(const Config& cfg = {}) :
			core(cfg) {}

		Sample Frame(float dt, Vec2 move = {}, std::initializer_list<CommandKind> presses = {}, EngineView engine = {})
		{
			FrameInput in;
			in.dt = dt;
			in.move = move;
			in.actorYaw = yaw;
			in.locomotionSpeed = carry;
			in.lockOn = lockOn;
			in.freeRotation = freeRotation;
			in.cameraIsYaw = cameraIsYaw;
			in.cameraYaw = cameraYaw;
			core.BeginFrame(in);
			for (auto k : presses) {
				core.Press(k, move);
			}

			Sample s;
			s.dt = dt;
			s.decision = core.Evaluate(engine);
			s.actionTime = core.ActionTime();  // time at which the arbiter decided
			core.Apply(s.decision, true);
			const auto step = core.Step();
			s.cmd = step.command;
			s.completed = step.completed;
			s.running = core.DodgeRunning();
			s.phase = core.Phase();
			s.chain = core.ChainCount();

			if (step.command.active) {
				pos.x += step.command.vx * dt;
				pos.y += step.command.vy * dt;
				yaw = WrapAngle(yaw + step.command.yawRate * dt);
			}
			s.t = core.Now();
			s.pos = pos;
			s.heading = core.Heading();
			s.facing = core.Facing();
			log.push_back(s);
			return s;
		}

		// Runs frames of `dt` until `seconds` of game time have passed.
		void Run(double seconds, float dt, Vec2 move = {})
		{
			const double end = core.Now() + seconds;
			while (core.Now() < end - 1e-9) {
				Frame(dt, move);
			}
		}

		ReactiveCore       core;
		Vec2               pos{};
		float              yaw{ 0.0f };
		float              carry{ 0.0f };
		bool               lockOn{ false };
		bool               freeRotation{ true };
		bool               cameraIsYaw{ false };
		float              cameraYaw{ 0.0f };
		std::vector<Sample> log;
	};

	// Power-of-two frame time: sums exactly, so window edges can be hit exactly.
	constexpr float kDt64 = 1.0f / 64.0f;

	inline Config ExactConfig()
	{
		Config c;
		c.timing = { 0.078125f, 0.296875f, 0.328125f, 0.125f };  // 5/64, 19/64, 21/64 -> 0.703125 total
		c.policy.attackFrom = 0.3125f;                              // 20/64
		c.policy.blockFrom = 0.3125f;
		c.policy.chainFrom = 0.375f;                                // 24/64
		c.policy.exitFrom = 0.46875f;                               // 30/64
		c.input.bufferWindow = 0.1875f;                             // 12/64
		c.input.debounce = 0.0625f;                                 // 4/64
		return c;
	}
}
