#pragma once

#include "ActionArbiter.h"
#include "ClipMotion.h"
#include "Clock.h"
#include "Config.h"
#include "DodgeAction.h"
#include "InputBuffer.h"
#include "MotionCommand.h"
#include "MotionDriver.h"

namespace rdf
{
	struct FrameInput
	{
		float dt{ 0.0f };
		Vec2  move{};             // world-space move input, length 0..1 (analog)
		float actorYaw{ 0.0f };   // actor heading now
		float locomotionSpeed{ 0.0f };  // measured planar speed, carried into a fresh dodge
		bool  lockOn{ false };
		bool  freeRotation{ true };  // something lets the actor face any direction (TDM 360 mode)
		bool  cameraIsYaw{ false };  // first person: the actor's yaw is the view, never hold it
		float cameraYaw{ 0.0f };     // where the camera looks: a held / warped facing is handed back to it
	};

	struct EngineView
	{
		EngineBlock  engine{ EngineBlock::None };
		std::uint8_t attackState{ 0 };
		bool         dodgeWindowOpen{ false };
		bool         attackRecovery{ false };
		std::optional<bool> attackClipCancel;
		bool         cancelAnyTime{ false };
		bool         dodgeForbidden{ false };
		bool         staminaOk{ true };
	};

	struct StepResult
	{
		MotionCommand command{};
		bool          completed{ false };  // the dodge reached its natural end during this step
	};

	// Engine-free orchestration of one frame:
	//   BeginFrame -> Press* -> Evaluate -> (caller performs side effects) -> Apply -> Step
	// Evaluate does not mutate the buffer; Apply consumes commands only for accepted decisions,
	// so a refused side effect leaves the press buffered until it expires.
	class ReactiveCore
	{
	public:
		explicit ReactiveCore(const Config& cfg);
		// New tuning (an in-game menu edit). Only between dodges: a running one keeps the config it began
		// with. Returns false (nothing changed) while a dodge runs; the caller retries.
		bool          Reconfigure(const Config& cfg);

		void          BeginFrame(const FrameInput& frame);
		std::uint64_t Press(CommandKind kind, Vec2 dir);
		void          PromoteAttack() { buffer_.PromoteAttack(clock_.Now()); }
		// Called every frame the button behind `kind` is held (a held block outlasts the buffer window).
		void          KeepAlive(CommandKind kind) { buffer_.KeepAlive(kind, clock_.Now()); }

		Decision   Evaluate(const EngineView& engine);
		void       Apply(const Decision& decision, bool accepted);
		StepResult Step();
		void       Reset();

		bool       DodgeRunning() const { return action_.Running(); }
		DodgePhase Phase() const { return action_.Phase(); }
		float      ActionTime() const { return action_.Time(); }
		int        ChainCount() const { return action_.ChainCount(); }
		bool       Exiting() const { return action_.Exiting(); }
		bool       CancelledIntoAction() const { return intoAction_; }
		// The current dodge may be left for locomotion (and so for a jump): its exit window is open.
		bool       ExitWindowOpen() const;
		bool       BlockWindowOpen() const;  // the running dodge may be cancelled into a block now
		float      Heading() const { return driver_.Heading(); }
		float      Facing() const { return driver_.Facing(); }
		float      Speed() const { return driver_.Speed(); }
		// What StartDodge would do, without doing it: lets the caller ask the graph first.
		float PreviewStartAngleDeg(const Decision& d) const;
		bool  PreviewKeepsFacing(const Decision& d) const;  // what a dodge started by `d` would do
		// `d` starts a fresh dodge pressed without move input (its heading comes from bBackstepWithoutInput).
		bool  PreviewNeutral(const Decision& d) const;
		bool  KeepsFacing() const { return driver_.KeepsFacing(); }  // the current dodge
		bool  HoldsFacing() const { return driver_.HoldsFacing(); }  // ... and holds its start yaw
		bool  OwnsYaw() const { return driver_.OwnsYaw(); }          // the dodge turns the actor itself
		float WarpOffsetDeg() const { return driver_.WarpOffset() * kRadToDeg; }
		// Marks the command behind an attack/block decision consumed (after a deferred success).
		void  Consume(const Decision& d);
		// Heading of the current dodge relative to the actor's yaw when it began, degrees, +right.
		float      StartAngleDeg() const { return startAngleDeg_; }
		// An annotated clip's own i-frame window, else [from, to) of the ini timers.
		bool       Invulnerable(float from, float to) const;
		// The clip's own perfect window (RDF_PerfectStart / End) if it has one, else [from, to] of the ini.
		bool       PerfectWindow(float from, float to) const;
		// RDF_* annotation events of the clip playing the current dodge.
		void       OnWindowEvent(WindowEvent e);
		// RDF_Motion of the clip playing the current dodge: its distance and phases replace the ini's. Taken only while
		// the dodge is still in its startup and not exiting (the annotation sits at the clip's first frame); false when
		// it was ignored. A clip without phases keeps the current ones and changes the distance only.
		bool       OnClipMotion(const ClipMotion& motion);
		// RDF_ExitBlend: the current dodge's blend-outs into running, a block and an attack. False once it is exiting.
		bool       OnClipExitBlends(const std::array<float, 3>& blends);
		// RDF_Steer: the current dodge's steering rates (and max deviation).
		bool       OnClipSteering(const ClipSteering& steering);
		// The current dodge's phases and blend-outs: the ini's, or the clip's own.
		const Timing& DodgeTiming() const { return timing_; }
		// The jump may cut the dodge: the clip's RDF_JumpOpen once it fired, else the exit window.
		bool       JumpWindowOpen() const { return !action_.Running() || windows_.JumpOpen() || ExitWindowOpen(); }
		bool       ClipAnnotated() const { return windows_.Annotated().has_value(); }
		double     Now() const { return clock_.Now(); }
		const InputBuffer& Buffer() const { return buffer_; }

	private:
		void     StartDodge(const Decision& d, bool chain);
		float    StartHeading(const Decision& d, bool chain) const;
		DriverInput MakeDriverInput() const;
		bool     MoveHeld() const;

		Config        cfg_;
		Clock         clock_;
		InputBuffer   buffer_;
		ActionArbiter arbiter_;
		DodgeAction   action_;
		MotionDriver  driver_;
		FrameInput    frame_{};
		float         startAngleDeg_{ 0.0f };
		Timing        timing_;    // the current dodge's phases and blend-outs (the ini's until the clip says otherwise)
		bool          intoAction_{ false };  // the current dodge was cancelled into an attack/block
		WindowTracker windows_;
	};
}
