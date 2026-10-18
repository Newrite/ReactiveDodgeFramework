#include "ReactiveCore.h"

#include <algorithm>

namespace rdf
{
	ReactiveCore::ReactiveCore(const Config& cfg) :
		cfg_(cfg),
		buffer_(cfg.input.bufferWindow, cfg.input.debounce),
		arbiter_(cfg.policy),
		driver_(cfg) {}

	void ReactiveCore::BeginFrame(const FrameInput& frame)
	{
		frame_ = frame;
		clock_.Advance(frame.dt);
		buffer_.Expire(clock_.Now());
	}

	std::uint64_t ReactiveCore::Press(CommandKind kind, Vec2 dir)
	{
		return buffer_.Push(kind, clock_.Now(), dir);
	}

	bool ReactiveCore::MoveHeld() const
	{
		return Length(frame_.move) > cfg_.steering.deadzone;
	}

	Decision ReactiveCore::Evaluate(const EngineView& engine)
	{
		ArbiterState s;
		s.dodgeActive = action_.Running();
		s.exiting = action_.Exiting();
		s.actionTime = action_.Time();
		s.chainCount = action_.ChainCount();
		s.engine = engine.engine;
		s.attackState = engine.attackState;
		s.dodgeWindowOpen = engine.dodgeWindowOpen;
		s.staminaOk = engine.staminaOk;
		s.moveHeld = MoveHeld();
		return arbiter_.Arbitrate(s, buffer_, clock_.Now());
	}

	void ReactiveCore::StartDodge(const Decision& d, bool chain)
	{
		const double now = clock_.Now();
		buffer_.Consume(d.seq);
		buffer_.ConsumeKindUpTo(CommandKind::Dodge, now);
		if (!chain) {  // presses made before the dodge belong to the action that preceded it
			buffer_.ConsumeKindUpTo(CommandKind::Attack, now);
			buffer_.ConsumeKindUpTo(CommandKind::PowerAttack, now);
			buffer_.ConsumeKindUpTo(CommandKind::Block, now);
		}

		DriverStart start;
		start.chain = chain;
		start.facing = chain ? driver_.Facing() : frame_.actorYaw;
		start.moveHeld = MoveHeld();
		start.carrySpeed = chain ? driver_.Speed() : std::min(frame_.locomotionSpeed, cfg_.motion.maxCarrySpeed);
		if (Length(d.dir) > 1e-3f) {
			start.heading = HeadingOf(d.dir);
		} else if (chain) {
			start.heading = driver_.Heading();
		} else {
			start.heading = cfg_.motion.backstepWithoutInput ? WrapAngle(frame_.actorYaw + kPi) : frame_.actorYaw;
		}

		const int chainCount = chain ? action_.ChainCount() + 1 : 1;
		action_.Start(cfg_.timing, chainCount);
		driver_.Begin(start);
		startAngleDeg_ = AngleDelta(frame_.actorYaw, start.heading) * kRadToDeg;
	}

	void ReactiveCore::Apply(const Decision& d, bool accepted)
	{
		if (!accepted) {
			return;
		}
		const double now = clock_.Now();
		switch (d.kind) {
		case DecisionKind::None:
			break;
		case DecisionKind::Interrupt:
			action_.Interrupt();
			break;
		case DecisionKind::StartDodge:
			StartDodge(d, false);
			break;
		case DecisionKind::ChainDodge:
			StartDodge(d, true);
			break;
		case DecisionKind::Attack:
		case DecisionKind::PowerAttack:
			buffer_.Consume(d.seq);
			buffer_.ConsumeKindUpTo(CommandKind::Attack, now);
			buffer_.ConsumeKindUpTo(CommandKind::PowerAttack, now);
			action_.Reset();
			break;
		case DecisionKind::Block:
			buffer_.Consume(d.seq);
			buffer_.ConsumeKindUpTo(CommandKind::Block, now);
			action_.Reset();
			break;
		case DecisionKind::LocomotionExit:
			action_.RequestExit(cfg_.timing.exitBlend);
			driver_.BeginExit(action_.Time(), MoveHeld());
			break;
		}
	}

	DriverInput ReactiveCore::MakeDriverInput() const
	{
		DriverInput in;
		const float magnitude = std::min(Length(frame_.move), 1.0f);
		in.hasMove = magnitude > 0.0f;
		in.moveHeading = in.hasMove ? HeadingOf(frame_.move) : 0.0f;
		in.moveMagnitude = magnitude;
		in.lockOn = frame_.lockOn;
		in.lockOnYaw = frame_.lockOnYaw;
		return in;
	}

	StepResult ReactiveCore::Step()
	{
		StepResult result;
		if (!action_.Running() || !(frame_.dt > 0.0f)) {
			return result;
		}
		const float t0 = action_.Time();
		action_.Tick(frame_.dt);
		const float t1 = action_.Time();
		const DodgePhase phase = action_.Phase() == DodgePhase::Done ? DodgePhase::Recovery : action_.Phase();

		result.command = driver_.Step(t0, t1, frame_.dt, phase, MakeDriverInput());
		result.completed = action_.Phase() == DodgePhase::Done;
		return result;
	}

	void ReactiveCore::Reset()
	{
		buffer_.Clear();
		action_.Reset();
	}
}
