#include "UpdateHook.h"

#include "Diagnostics.h"
#include "Effects.h"
#include "EngineState.h"
#include "GraphCoordinator.h"
#include "InputCollector.h"
#include "JumpGate.h"
#include "LockOn.h"
#include "MotionHook.h"
#include "Settings.h"
#include "core/ReactiveCore.h"

#include <memory>

// wingdi.h defines GetObject as a macro, which breaks BGSDefaultObjectManager::GetObject.
#undef GetObject

namespace rdf::game::update
{
	namespace
	{
		// Owns one ReactiveCore and translates between it and the engine. Everything here runs on the
		// main thread inside PlayerCharacter::Update.
		class Controller
		{
		public:
			void Configure() { core_ = std::make_unique<ReactiveCore>(Settings::Get().core); }
			void Reset();
			void Tick(RE::PlayerCharacter* player, float dt);

		private:
			struct Retry
			{
				Decision decision;
				bool     pending{ false };
			};

			void       Refresh3D(RE::PlayerCharacter* player);
			void       MeasureMotion(RE::PlayerCharacter* player, float dt);
			FrameInput BuildFrame(RE::PlayerCharacter* player, float dt);
			void       DrainInput(const FrameInput& frame);
			void       HandleGraphEvents(RE::PlayerCharacter* player);
			void       Execute(RE::PlayerCharacter* player, const Decision& d);
			graph::DodgeEntry EntryFor(bool chain) const;
			void       TryStart(RE::PlayerCharacter* player, const Decision& d);
			void       CancelInto(RE::PlayerCharacter* player, const Decision& d);
			void       RetryDeferred(RE::PlayerCharacter* player);
			bool       Perform(RE::PlayerCharacter* player, const Decision& d);
			void       RollbackStart(RE::PlayerCharacter* player);
			void       EndByGraph(const char* why);
			void       AbandonGraph(RE::PlayerCharacter* player, const char* why);
			void       ReleaseOrphanedState(RE::PlayerCharacter* player, float dt);
			void       StepAndPublish(RE::PlayerCharacter* player, float dt);
			void       OnStopped();
			void       Trace(RE::PlayerCharacter* player);
			void       LogRejection(const Decision& d);
			void       MaintainBlock(RE::PlayerCharacter* player);
			void       TracePose(RE::PlayerCharacter* player) const;
			void       LogCost(RE::PlayerCharacter* player, float current) const;
			static int ClipIndex(float angleDeg, bool keepFacing);

			std::unique_ptr<ReactiveCore> core_;

			// graph handshake
			bool awaitingEnter_{ false };
			int  enterFrames_{ 0 };
			bool exitSent_{ true };
			bool wasRunning_{ false };
			int  prevDepth_{ 0 };
			Retry retry_;

			// bookkeeping
			RE::NiAVObject* last3D_{ nullptr };
			float           charged_{ 0.0f };
			RejectReason    lastReject_{ RejectReason::None };
			CommandKind     lastRejectCommand_{ CommandKind::Dodge };
			float           orphanTime_{ 0.0f };
			float           staggerAge_{ 0.0f };
			float           startCameraYaw_{ 0.0f };   // camera yaw when the current dodge started
			bool            freeRotation_{ false };    // TDM 360, sampled while no dodge runs
			const char*     modeName_{ "" };       // seconds the player has been staggered
			bool            escapeStagger_{ false };   // this tick a dodge may cut the stagger short  // graph in a dodge state we do not own, seconds
			int             tailFrames_{ 0 };   // dev trace keeps running this many ticks after a dodge
			bool            inStateB_{ true };  // dodge state used last; the first dodge goes to A
			bool            moveHeld_{ false };
			bool            blockHolding_{ false };  // a block we started out of a dodge, kept while held
			int             blockAge_{ 0 };          // ticks since that block started
			float           carry_{ 0.0f };     // post-dodge speed floor for the controls-driven engine, u/s

			// measured motion (previous frame's displacement, seen at the start of this tick)
			bool  havePos_{ false };
			Vec2  lastPos_{};
			float measuredSpeed_{ 0.0f };
			float measuredDistance_{ 0.0f };
			float measuredHeading_{ 0.0f };

			// what we asked the hook for: this tick's request, and last tick's (which is what the
			// displacement measured at the start of this tick should match)
			float requestedDistance_{ 0.0f };
			float requestedHeading_{ 0.0f };
			float prevRequestedDistance_{ 0.0f };
			float prevRequestedHeading_{ 0.0f };
		};

		Controller g_controller;

		Vec2 PlanarPosition(const RE::PlayerCharacter* p)
		{
			const auto pos = p->GetPosition();
			return { pos.x, pos.y };
		}

		bool PlayAction(RE::PlayerCharacter* player, RE::DEFAULT_OBJECT object)
		{
			auto* manager = RE::BGSDefaultObjectManager::GetSingleton();
			auto* action = manager ? manager->GetObject<RE::BGSAction>(object) : nullptr;
			if (!action) {
				return false;
			}
			const std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
			data->source = RE::NiPointer<RE::TESObjectREFR>(player);
			data->action = action;
			return data->Process();
		}

		// Raised block without an action record: wantBlocking + the vanilla graph event.
		constexpr int kFirstStepClip = 8;  // RDF_DodgeClip 0..7 rolls, 8..15 steps (ANIMATION_CONTRACT.md)

		// The arm layer holds the stance of what is drawn. iRightHandType / iLeftHandType report the
		// equipped item even while it is sheathed, so by default a sheathed weapon or spell keeps no stance
		// (bArmStanceWhenSheathed opts in); fists have none (the graph's left-hand "none" entry is a one-hander pose).
		bool HasStanceToKeep(RE::PlayerCharacter* player)
		{
			if (!player->AsActorState()->IsWeaponDrawn() && !Settings::Get().armStanceWhenSheathed) {
				return false;
			}
			std::int32_t right = 0;
			player->GetGraphVariableInt("iRightHandType"sv, right);
			return right != 0;
		}

		bool IsBlocking(RE::PlayerCharacter* player)
		{
			bool blocking = false;
			player->GetGraphVariableBool("IsBlocking"sv, blocking);
			return blocking;
		}

		bool StartBlock(RE::PlayerCharacter* player)
		{
			player->AsActorState()->actorState2.wantBlocking = 1;
			if (player->NotifyAnimationGraph("blockStart"sv)) {
				return true;
			}
			player->AsActorState()->actorState2.wantBlocking = 0;
			return false;
		}

		CommandKind CommandOf(DecisionKind k)
		{
			switch (k) {
			case DecisionKind::PowerAttack: return CommandKind::PowerAttack;
			case DecisionKind::Block: return CommandKind::Block;
			default: return CommandKind::Attack;
			}
		}
	}

	void Controller::Reset()
	{
		if (core_) {
			core_->Reset();
		}
		motion::Reset();
		graph::ClearEvents();
		lockon::ReleaseYaw();
		effects::SetInvulnerable(false);
		awaitingEnter_ = false;
		exitSent_ = true;
		wasRunning_ = false;
		carry_ = 0.0f;
		blockHolding_ = false;
		prevDepth_ = 0;
		retry_ = {};
		havePos_ = false;
		measuredSpeed_ = 0.0f;
		last3D_ = nullptr;
		charged_ = 0.0f;
	}

	void Controller::Tick(RE::PlayerCharacter* player, float dt)
	{
		auto* ui = RE::UI::GetSingleton();
		if (!core_ || !(dt > 0.0f) || !ui || ui->GameIsPaused()) {
			input::DiscardQueued();
			return;
		}

		Refresh3D(player);
		if (graph::TrackActiveGraph(player) && (core_->DodgeRunning() || awaitingEnter_)) {
			AbandonGraph(player, "active graph switched (camera)");
		}
		MeasureMotion(player, dt);
		const FrameInput frame = BuildFrame(player, dt);
		core_->BeginFrame(frame);
		DrainInput(frame);
		if (input::BlockHeld()) {
			core_->KeepAlive(CommandKind::Block);
		}
		MaintainBlock(player);
		HandleGraphEvents(player);
		ReleaseOrphanedState(player, dt);
		RetryDeferred(player);

		const bool           ownDodge = core_->DodgeRunning() || awaitingEnter_;
		const auto           state = engine::Read(player, ownDodge);
		const auto&          settings = Settings::Get();
		EngineView           view;
		view.engine = state.block;
		staggerAge_ = state.block == EngineBlock::Staggered ? staggerAge_ + dt : 0.0f;
		// [Stagger] bDodgeFromStagger: a dodge may cut the stagger short once fDodgeDelay has passed.
		escapeStagger_ = !ownDodge && state.block == EngineBlock::Staggered && settings.dodgeFromStagger &&
		                 staggerAge_ >= settings.staggerDodgeDelay;
		if (escapeStagger_) {
			view.engine = EngineBlock::None;
		}
		view.attackState = state.attackState;
		view.dodgeWindowOpen = graph::DodgeWindowOpen();
		view.staminaOk = !settings.costResource || state.resource >= settings.minimum;
		const Decision decision = core_->Evaluate(view);
		if (decision.reject == RejectReason::StaminaLow) {
			LogCost(player, state.resource);
		}
		Execute(player, decision);
		jump::SetLocked(settings.blockJumpDuringDodge &&
		                (awaitingEnter_ || (core_->DodgeRunning() && !core_->ExitWindowOpen())));

		StepAndPublish(player, dt);
		Trace(player);
		prevRequestedDistance_ = requestedDistance_;
		prevRequestedHeading_ = requestedHeading_;
	}

	void Controller::Refresh3D(RE::PlayerCharacter* player)
	{
		auto* current = player->Get3D();
		if (current == last3D_) {
			return;
		}
		last3D_ = current;
		graph::RegisterSink(player);
		if (core_->DodgeRunning() || awaitingEnter_) {
			AbandonGraph(player, "3D reloaded");
		}
	}

	void Controller::MeasureMotion(RE::PlayerCharacter* player, float dt)
	{
		const Vec2 pos = PlanarPosition(player);
		if (havePos_) {
			const Vec2  delta{ pos.x - lastPos_.x, pos.y - lastPos_.y };
			const float distance = Length(delta);
			if (distance < 2000.0f) {  // larger jumps are teleports / cell changes
				measuredDistance_ = distance;
				measuredSpeed_ += (distance / dt - measuredSpeed_) * 0.5f;
				if (distance > 0.5f) {
					measuredHeading_ = HeadingOf(delta);
				}
			} else {
				measuredDistance_ = 0.0f;
				measuredSpeed_ = 0.0f;
			}
		}
		lastPos_ = pos;
		havePos_ = true;
	}

	FrameInput Controller::BuildFrame(RE::PlayerCharacter* player, float dt)
	{
		FrameInput f;
		f.dt = dt;
		f.actorYaw = player->data.angle.z;
		f.locomotionSpeed = measuredSpeed_;
		f.lockOn = lockon::LockedOn(player);
		// [Steering] bInputFromStartCamera: while a dodge runs (not locked on) the stick is read against
		// the camera yaw it started with, so the camera cannot steer it.
		const bool frozenBasis = Settings::Get().inputFromStartCamera && core_->DodgeRunning() && !f.lockOn;
		f.move = input::WorldMoveInput(frozenBasis ? std::optional<float>(startCameraYaw_) : std::nullopt);
		// In first person the camera owns the yaw: the dodge never turns the player there.
		const auto* camera = RE::PlayerCamera::GetSingleton();
		const bool  firstPerson = camera && camera->IsInFirstPerson();
		// TDM's reported mode is only meaningful outside our dodge: its movement-input flag comes from the
		// controls-driven movement handler, which does not run while the dodge drives the motion, so in
		// "Vanilla style" it reports 360 mid-dodge. A chain keeps the mode the sequence started with.
		if (!core_->DodgeRunning()) {
			freeRotation_ = lockon::FreeRotation();
			modeName_ = lockon::ModeName();
		}
		f.freeRotation = freeRotation_ && !firstPerson;
		f.cameraIsYaw = firstPerson;
		f.cameraYaw = input::CameraYaw();
		moveHeld_ = Length(f.move) > Settings::Get().core.steering.deadzone;
		return f;
	}

	void Controller::DrainInput(const FrameInput& frame)
	{
		input::Drain([&](const input::Press& press) {
			if (press.promote) {
				core_->PromoteAttack();
				return;
			}
			const bool recorded = core_->Press(press.kind, frame.move) != 0;
			DevLog("press kind={} {}", static_cast<int>(press.kind), recorded ? "buffered" : "debounced");
		});
	}

	void Controller::HandleGraphEvents(RE::PlayerCharacter* player)
	{
		graph::DrainClipWindows([this](WindowEvent e) {
			core_->OnWindowEvent(e);
			DevLog("clip window event {} at t={:.3f}", static_cast<int>(e), core_->ActionTime());
		});
		const bool entered = graph::TakeEntered();
		const int  depth = graph::Depth();
		const int  previousDepth = prevDepth_;
		prevDepth_ = depth;
		if (!core_->DodgeRunning()) {
			awaitingEnter_ = false;
			return;
		}

		if (entered) {
			awaitingEnter_ = false;  // any Entered completes the start/chain handshake
			DevLog("graph entered dodge (depth {}, graph var {})", depth, graph::GraphIsDodging(player));
		} else if (awaitingEnter_) {
			if (++enterFrames_ > Settings::Get().enterTimeoutFrames) {
				RollbackStart(player);
			}
			return;
		}
		if (previousDepth > 0 && depth == 0) {
			// The exit notify fires when the out-transition starts; the graph stays animation-driven
			// for the whole out-blend. A hand-over we (or the clip end) began keeps driving the motion
			// through it, otherwise the clip's own end-of-roll motion (zero) would plant the actor.
			if (!core_->Exiting()) {
				EndByGraph("graph left every dodge state");
				return;
			}
			DevLog("{}", "graph left the dodge state; finishing the hand-over blend");
		}
	}

	void Controller::RollbackStart(RE::PlayerCharacter* player)
	{
		logger::warn("graph did not report {} within {} frames, rolling back (is the behavior patch installed?)",
			Settings::Get().graph.entered, Settings::Get().enterTimeoutFrames);
		effects::RefundCost(player, charged_);
		charged_ = 0.0f;
		awaitingEnter_ = false;
		exitSent_ = true;
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
	}

	void Controller::EndByGraph(const char* why)
	{
		DevLog("dodge ended by graph: {}", why);
		exitSent_ = true;  // the graph is already heading back to locomotion
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
	}

	// The player has a first- and a third-person graph and our start event reached both, so after a
	// camera switch the newly active one is still in a dodge state (bAnimationDriven, no clip-end
	// exit) that we no longer drive. Send the exit so every graph goes back to locomotion.
	void Controller::AbandonGraph(RE::PlayerCharacter* player, const char* why)
	{
		graph::SendExit(player);
		if (awaitingEnter_) {
			effects::RefundCost(player, charged_);
			charged_ = 0.0f;
			awaitingEnter_ = false;
		}
		if (core_->DodgeRunning()) {
			EndByGraph(why);
		} else {
			DevLog("graph abandoned before entering: {}", why);
			exitSent_ = true;
		}
	}

	// Safety net: the active graph reports a dodge state while we own no dodge (a switch we did not
	// see, a load mid-dodge). Our states have no exit of their own, so without this the player would
	// stand animation-driven until the next dodge or jump. The grace period covers the out-blend.
	void Controller::ReleaseOrphanedState(RE::PlayerCharacter* player, float dt)
	{
		if (core_->DodgeRunning() || awaitingEnter_ || !graph::GraphIsDodging(player)) {
			orphanTime_ = 0.0f;
			return;
		}
		orphanTime_ += dt;
		if (orphanTime_ >= 0.6f) {
			logger::info("graph stuck in a dodge state we do not own; sending {}", Settings::Get().graph.exit);
			graph::SendExit(player);
			orphanTime_ = 0.0f;  // retry after another grace period if it did not take
		}
	}

	int Controller::ClipIndex(float angleDeg, bool keepFacing)
	{
		if (!keepFacing) {
			return 0;  // the character turns to face the trajectory: always the forward roll
		}
		const int octant = static_cast<int>(std::lround(angleDeg / 45.0f));
		const int index = ((octant % 8) + 8) % 8;  // F, FR, R, BR, B, BL, L, FL
		return index + (Settings::Get().sidestepWhenKeepFacing ? 8 : 0);
	}

	void Controller::LogCost(RE::PlayerCharacter* player, float current) const
	{
		const auto& settings = Settings::Get();
		auto*       owner = player->AsActorValueOwner();
		const auto  resource = *settings.costResource;  // only called after a cost rejection
		DevLog("cost resource {} current={:.1f} permanent={:.1f} minimum={:.1f}", static_cast<int>(resource), current,
			owner->GetPermanentActorValue(resource), settings.minimum);
	}

	// The game's block handler never saw a button-down that came during the dodge, so it would drop
	// our block on its next update. Keep the block up while the button stays down, end it on release.
	void Controller::MaintainBlock(RE::PlayerCharacter* player)
	{
		if (!blockHolding_) {
			return;
		}
		// The graph reports IsBlocking a tick or two after blockStart; only then can "not blocking"
		// mean the game ended the block (stagger, bash, ...).
		if (++blockAge_ > 3 && !IsBlocking(player)) {
			blockHolding_ = false;
			return;
		}
		auto& state = player->AsActorState()->actorState2;
		if (input::BlockHeld()) {
			state.wantBlocking = 1;
			return;
		}
		state.wantBlocking = 0;
		player->NotifyAnimationGraph("blockStop"sv);
		blockHolding_ = false;
		DevLog("{}", "block released");
	}

	void Controller::LogRejection(const Decision& d)
	{
		if (d.reject == lastReject_ && d.rejectedCommand == lastRejectCommand_) {
			return;
		}
		lastReject_ = d.reject;
		lastRejectCommand_ = d.rejectedCommand;
		if (d.reject != RejectReason::None && d.reject != RejectReason::NoCommand) {
			DevLog("rejected cmd={} reason={}", static_cast<int>(d.rejectedCommand), ToString(d.reject));
		}
	}

	void Controller::Execute(RE::PlayerCharacter* player, const Decision& d)
	{
		LogRejection(d);
		if (d.kind != DecisionKind::None) {
			DevLog("decision {} seq={} phase={} t={:.3f} chain={}", ToString(d.kind), d.seq, ToString(core_->Phase()),
				core_->ActionTime(), core_->ChainCount());
		}
		switch (d.kind) {
		case DecisionKind::None:
			break;
		case DecisionKind::Interrupt:
			core_->Apply(d, true);
			exitSent_ = true;
			break;
		case DecisionKind::StartDodge:
		case DecisionKind::ChainDodge:
			TryStart(player, d);
			break;
		case DecisionKind::Attack:
		case DecisionKind::PowerAttack:
		case DecisionKind::Block:
			CancelInto(player, d);
			break;
		case DecisionKind::LocomotionExit:
			graph::SendExit(player);
			exitSent_ = true;
			core_->Apply(d, true);
			break;
		}
	}

	// A chain only works while the graph is in a dodge state; after it has left (a hand-over in
	// progress) the chained dodge enters like a new one. A new dodge always targets the state not
	// used last, which may still be blending out.
	graph::DodgeEntry Controller::EntryFor(bool chain) const
	{
		if (chain && graph::Depth() > 0) {
			return graph::DodgeEntry::Chain;
		}
		return inStateB_ ? graph::DodgeEntry::StartA : graph::DodgeEntry::StartB;
	}

	void Controller::TryStart(RE::PlayerCharacter* player, const Decision& d)
	{
		const auto& settings = Settings::Get();
		const bool  chain = d.kind == DecisionKind::ChainDodge;
		const float angle = core_->PreviewStartAngleDeg(d);
		const int   clip = ClipIndex(angle, core_->PreviewKeepsFacing(d));
		const auto  entry = EntryFor(chain);
		const auto* camera = RE::PlayerCamera::GetSingleton();
		const bool  firstPerson = camera && camera->IsInFirstPerson();
		const float stance = firstPerson ? settings.armStanceFirstPerson : clip >= kFirstStepClip ? settings.armStanceStep : settings.armStanceRoll;
		const float armStance = HasStanceToKeep(player) ? stance : 0.0f;
		const float clipLength = firstPerson ? settings.clipLengthFirstPerson : settings.clipLength;
		if (!graph::SendStart(player, entry, clip, settings.core.timing.PlaybackSpeed(clipLength, settings.animSpeed), armStance)) {
			DevLog("graph refused {} (clip {}); press stays buffered", chain ? "chain" : "start", clip);
			core_->Apply(d, false);
			return;
		}
		inStateB_ = entry == graph::DodgeEntry::Chain ? !inStateB_ : entry == graph::DodgeEntry::StartB;
		if (firstPerson) {
			DevLog("1P dodge: mouse turn rate {:.3f} rad/s x {:.2f}", engine::LookTurnRate(player), settings.firstPersonLookScale);
		}
		if (escapeStagger_) {
			// The graph leaves the stagger state through our wildcard, so its staggerStop annotation never
			// fires; clear the flag the way the engine's handler would.
			engine::EndStagger(player);
			DevLog("dodge out of stagger after {:.2f}s", staggerAge_);
		}
		graph::LogEvents(settings.devLog);
		effects::ChargeCost(player, settings.cost);
		charged_ = settings.cost;
		core_->Apply(d, true);
		awaitingEnter_ = true;
		enterFrames_ = 0;
		exitSent_ = false;
		startCameraYaw_ = input::CameraYaw();
		if (core_->OwnsYaw()) {
			lockon::AcquireYaw();  // TDM would otherwise turn the player back toward its own target yaw
		}
		DevLog("dodge {} angle={:.1f} clip={} keepFacing={} holdFacing={} ownsYaw={} freeRotation={} ({})", chain ? "chain" : "start",
			angle, clip, core_->KeepsFacing(), core_->HoldsFacing(), core_->OwnsYaw(), freeRotation_, modeName_);
	}

	bool Controller::Perform(RE::PlayerCharacter* player, const Decision& d)
	{
		switch (d.kind) {
		case DecisionKind::PowerAttack:
			return PlayAction(player, RE::DEFAULT_OBJECT::kActionRightPowerAttack);
		case DecisionKind::Block:
			blockHolding_ = StartBlock(player);
			blockAge_ = 0;
			return blockHolding_;
		default:
			return PlayAction(player, RE::DEFAULT_OBJECT::kActionRightAttack);
		}
	}

	// Attack and block live in a nested behavior under the default state, so leaving the dodge and
	// performing the action happen in the same tick. A failed action leaves the press buffered; the
	// dodge is already exiting into locomotion, where Retry picks the press up until it expires.
	void Controller::CancelInto(RE::PlayerCharacter* player, const Decision& d)
	{
		if (!exitSent_) {
			graph::SendExit(player);
			exitSent_ = true;
		}
		if (Perform(player, d)) {
			core_->Apply(d, true);
			return;
		}
		DevLog("action for {} failed, will retry until the press expires", ToString(d.kind));
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
		retry_ = { d, true };
	}

	void Controller::RetryDeferred(RE::PlayerCharacter* player)
	{
		if (!retry_.pending) {
			return;
		}
		const auto* cmd = core_->Buffer().Peek(CommandOf(retry_.decision.kind), core_->Now());
		if (!cmd || cmd->seq != retry_.decision.seq) {
			retry_ = {};
			return;
		}
		if (Perform(player, retry_.decision)) {
			core_->Consume(retry_.decision);
			DevLog("deferred {} succeeded", ToString(retry_.decision.kind));
			retry_ = {};
		}
	}

	void Controller::StepAndPublish(RE::PlayerCharacter* player, float dt)
	{
		const auto& settings = Settings::Get();
		auto step = core_->Step();
		if (step.command.active) {
			// During a hand-over into running the floor already follows the hand-over speed: the engine
			// takes the movement back in the frame the out-blend ends, before this tick sees the dodge
			// finish, and one frame without the floor dropped its speed to ~70 (a one-frame stance jerk).
			// The instantaneous speed, not the command's tick average: on the tick the dodge ends that
			// average spreads its last milliseconds over the whole frame and was a fraction of it.
			const bool handingOver = core_->Exiting() && moveHeld_ && !core_->CancelledIntoAction();
			carry_ = handingOver ? core_->Speed() : 0.0f;
		} else {
			carry_ = moveHeld_ ? std::max(0.0f, carry_ - settings.carryDecay * dt) : 0.0f;
		}
		step.command.speedFloor = carry_;
		if (step.command.active) {
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->IsInFirstPerson()) {
				step.command.lookYawRate = engine::LookTurnRate(player) * settings.firstPersonLookScale;
			}
		}
		motion::Publish(step.command, player->GetFormID());
		requestedDistance_ = step.command.speed * dt;
		requestedHeading_ = core_->Heading();

		effects::SetInvulnerable(settings.iFrames && core_->Invulnerable(settings.iFrameStart, settings.iFrameEnd));

		if (step.completed && !exitSent_) {
			graph::SendExit(player);
			exitSent_ = true;
		}
		const bool running = core_->DodgeRunning();
		if (wasRunning_ && !running) {
			OnStopped();
		}
		wasRunning_ = running;
	}

	void Controller::OnStopped()
	{
		// Only a dodge that ran its course into running carries its speed; an interrupted one does not.
		if (core_->Phase() != DodgePhase::Done || !moveHeld_ || core_->CancelledIntoAction()) {
			carry_ = 0.0f;
		}
		tailFrames_ = 30;  // follow the hand-over into locomotion in the dev trace
		lockon::ReleaseYaw();
		effects::SetInvulnerable(false);
		awaitingEnter_ = false;
		charged_ = 0.0f;
		DevLog("dodge stopped (phase {})", ToString(core_->Phase()));
	}

	void Controller::Trace(RE::PlayerCharacter* player)
	{
		if (!Settings::Get().devLog) {
			return;
		}
		if (!wasRunning_ && core_->Phase() != DodgePhase::Done) {
			if (tailFrames_ <= 0) {
				graph::LogEvents(false);
				return;
			}
			--tailFrames_;
		}
		float speed = 0.0f, sampled = 0.0f, direction = 0.0f;
		player->GetGraphVariableFloat("Speed"sv, speed);
		player->GetGraphVariableFloat("SpeedSampled"sv, sampled);
		player->GetGraphVariableFloat("Direction"sv, direction);
		bool inJump = false;
		player->GetGraphVariableBool("bInJumpState"sv, inJump);
		const auto  hook = motion::ReadStats();
		const float headingErr = AngleDelta(prevRequestedHeading_, measuredHeading_) * kRadToDeg;
		logger::info(
			"trace t={:.3f} phase={} act={:.3f} desiredHdg={:.1f} facing={:.1f} warp={:.1f} actorYaw={:.1f} cmdSpeed={:.1f} | "
			"requested={:.2f} actual={:.2f} measuredHdg={:.1f} err={:.1f} | hook calls={} overrides={} thread={}/{} lastLocal=({:.2f},{:.2f}) yaw={:.4f} graphDodging={}",
			core_->Now(), ToString(core_->Phase()), core_->ActionTime(), prevRequestedHeading_ * kRadToDeg, core_->Facing() * kRadToDeg,
			core_->WarpOffsetDeg(), player->data.angle.z * kRadToDeg, core_->Speed(), prevRequestedDistance_, measuredDistance_, measuredHeading_ * kRadToDeg,
			headingErr, hook.calls, hook.overrides, hook.firstThread, hook.lastThread, hook.lastLocalX, hook.lastLocalY,
			hook.lastYaw, graph::GraphIsDodging(player));
		float midSpeed = -1.0f;
		if (const auto* process = player->GetActorRuntimeData().currentProcess) {
			if (process->middleHigh) {
				midSpeed = process->middleHigh->currentMovementSpeed;
			}
		}
		logger::info("trace graph Speed={:.1f} SpeedSampled={:.1f} Direction={:.2f} inJump={} depth={} | process moveSpeed={:.1f} | carry={:.1f} floorHits={}",
			speed, sampled, direction, inJump, graph::Depth(), midSpeed, carry_, hook.floorHits);
		TracePose(player);
	}

	// Dev: bone positions in the actor's own frame (last rendered pose), to see a one-frame pose pop.
	void Controller::TracePose(RE::PlayerCharacter* player) const
	{
		auto* root = player->Get3D(false);
		if (!root) {
			return;
		}
		static const std::array<RE::BSFixedString, 3> kBones{ "NPC Pelvis [Pelv]", "NPC Head [Head]", "NPC R Hand [RHnd]" };
		std::array<RE::NiPoint3, kBones.size()> local{};
		const auto toLocal = root->world.rotate.Transpose();
		for (std::size_t i = 0; i < kBones.size(); ++i) {
			if (const auto* bone = root->GetObjectByName(kBones[i])) {
				local[i] = toLocal * (bone->world.translate - root->world.translate);
			}
		}
		logger::info("trace pose pelvis=({:.1f},{:.1f},{:.1f}) head=({:.1f},{:.1f},{:.1f}) rhand=({:.1f},{:.1f},{:.1f})",
			local[0].x, local[0].y, local[0].z, local[1].x, local[1].y, local[1].z, local[2].x, local[2].y, local[2].z);
	}

	namespace
	{
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* player, float delta)
			{
				g_controller.Tick(player, delta);
				func(player, delta);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Configure()
	{
		g_controller.Configure();
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = vtbl.write_vfunc(0xAD, PlayerUpdate::thunk);
		logger::info("PlayerCharacter::Update hooked");
	}

	void Reset()
	{
		g_controller.Reset();
	}
}
