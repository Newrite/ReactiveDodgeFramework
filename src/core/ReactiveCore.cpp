#include "ReactiveCore.h"

#include <algorithm>
#include <cmath>

namespace rdf
{
	ReactiveCore::ReactiveCore(const Config& cfg) :
		cfg_(cfg),
		buffer_(cfg.input.bufferWindow, cfg.input.debounce),
		arbiter_(cfg.policy),
		driver_(cfg) {}

	bool ReactiveCore::Reconfigure(const Config& cfg)
	{
		if (action_.Running()) {
			return false;
		}
		cfg_ = cfg;
		buffer_.SetTiming(cfg.input.bufferWindow, cfg.input.debounce);
		arbiter_ = ActionArbiter(cfg.policy);
		driver_ = MotionDriver(cfg);
		return true;
	}

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
		// While momentum runs out into an attack/block the dodge is over as far as decisions go:
		// a dodge press is judged against the attack, not chained.
		s.dodgeActive = action_.Running() && !intoAction_;
		s.exiting = action_.Exiting();
		s.actionTime = action_.Time();
		s.chainCount = action_.ChainCount();
		s.engine = engine.engine;
		s.attackState = engine.attackState;
		s.dodgeWindowOpen = engine.dodgeWindowOpen;
		s.staminaOk = engine.staminaOk;
		s.annotated = windows_.Annotated();
		s.moveHeld = MoveHeld();
		return arbiter_.Arbitrate(s, buffer_, clock_.Now());
	}

	bool ReactiveCore::ExitWindowOpen() const
	{
		if (!action_.Running()) {
			return true;
		}
		ArbiterState s;
		s.dodgeActive = !intoAction_;
		s.exiting = action_.Exiting();
		s.actionTime = action_.Time();
		s.annotated = windows_.Annotated();
		return arbiter_.WindowsFor(s).exit;
	}

	float ReactiveCore::StartHeading(const Decision& d, bool chain) const
	{
		if (Length(d.dir) > 1e-3f) {
			return HeadingOf(d.dir);
		}
		if (chain) {
			return driver_.Heading();
		}
		return cfg_.motion.backstepWithoutInput ? WrapAngle(frame_.actorYaw + kPi) : frame_.actorYaw;
	}

	float ReactiveCore::PreviewStartAngleDeg(const Decision& d) const
	{
		return AngleDelta(frame_.actorYaw, StartHeading(d, d.kind == DecisionKind::ChainDodge)) * kRadToDeg;
	}

	bool ReactiveCore::PreviewKeepsFacing(const Decision& d) const
	{
		return rdf::KeepsFacing(cfg_.steering, frame_.lockOn, frame_.freeRotation, PreviewStartAngleDeg(d));
	}

	bool ReactiveCore::PreviewNeutral(const Decision& d) const
	{
		return d.kind == DecisionKind::StartDodge && Length(d.dir) <= 1e-3f;  // a chain without input keeps its heading
	}

	void ReactiveCore::Consume(const Decision& d)
	{
		const double now = clock_.Now();
		buffer_.Consume(d.seq);
		const auto kind = d.kind == DecisionKind::Block ? CommandKind::Block : CommandKind::Attack;
		buffer_.ConsumeKindUpTo(kind, now);
		if (kind == CommandKind::Attack) {
			buffer_.ConsumeKindUpTo(CommandKind::PowerAttack, now);
		}
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
		start.facing = frame_.actorYaw;
		start.moveHeld = MoveHeld();
		start.carrySpeed = chain ? driver_.Speed() : std::min(frame_.locomotionSpeed, cfg_.motion.maxCarrySpeed);
		start.heading = StartHeading(d, chain);
		start.keepFacing = PreviewKeepsFacing(d);
		start.holdFacing = cfg_.steering.holdFacing && !frame_.lockOn && !frame_.cameraIsYaw;
		// The directional clip is picked by the nearest 45-degree step (the game layer's ClipIndex).
		const float startAngle = AngleDelta(frame_.actorYaw, start.heading);
		start.clipAngle = std::round(startAngle / (kPi / 4.0f)) * (kPi / 4.0f);
		start.warp = !frame_.cameraIsYaw;

		windows_.Reset();  // the new clip announces its own windows
		intoAction_ = false;
		const int chainCount = chain ? action_.ChainCount() + 1 : 1;
		action_.Start(cfg_.timing, chainCount);
		driver_.Begin(start);
		startAngleDeg_ = AngleDelta(frame_.actorYaw, start.heading) * kRadToDeg;
	}

	bool ReactiveCore::Invulnerable(float from, float to) const
	{
		if (!action_.Running()) {
			return false;
		}
		if (windows_.Annotated()) {
			return windows_.Invulnerable();
		}
		return action_.Time() >= from && action_.Time() < to;
	}

	bool ReactiveCore::PerfectWindow(float from, float to) const
	{
		if (!action_.Running()) {
			return false;
		}
		if (const auto& clip = windows_.Perfect()) {
			return *clip;
		}
		return action_.Time() >= from && action_.Time() <= to;
	}

	void ReactiveCore::OnWindowEvent(WindowEvent e)
	{
		if (action_.Running()) {
			windows_.On(e);
		}
	}

	void ReactiveCore::Apply(const Decision& d, bool accepted)
	{
		if (!accepted) {
			return;
		}
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
		case DecisionKind::Block:
			// The action owns the actor from now on; the dodge only lets its momentum run out.
			Consume(d);
			action_.RequestExit(cfg_.timing.actionExitBlend);
			driver_.BeginStop(action_.Time(), cfg_.timing.actionExitBlend);
			intoAction_ = true;
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
		in.actorYaw = frame_.actorYaw;
		in.cameraYaw = frame_.cameraYaw;
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
		windows_.Reset();
		intoAction_ = false;
	}
}
