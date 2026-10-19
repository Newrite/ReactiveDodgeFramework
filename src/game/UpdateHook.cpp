#include "UpdateHook.h"

#include "Diagnostics.h"
#include "Effects.h"
#include "EngineState.h"
#include "GraphCoordinator.h"
#include "InputCollector.h"
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
			void       TryStart(RE::PlayerCharacter* player, const Decision& d);
			void       CancelInto(RE::PlayerCharacter* player, const Decision& d);
			void       RetryDeferred(RE::PlayerCharacter* player);
			bool       Perform(RE::PlayerCharacter* player, const Decision& d);
			void       RollbackStart(RE::PlayerCharacter* player);
			void       EndByGraph(const char* why);
			void       StepAndPublish(RE::PlayerCharacter* player, float dt);
			void       OnStopped();
			void       Trace(RE::PlayerCharacter* player);
			void       LogRejection(const Decision& d);
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
		MeasureMotion(player, dt);
		const FrameInput frame = BuildFrame(player, dt);
		core_->BeginFrame(frame);
		DrainInput(frame);
		HandleGraphEvents(player);
		RetryDeferred(player);

		const auto           state = engine::Read(player);
		const auto&          settings = Settings::Get();
		EngineView           view;
		view.engine = state.block;
		view.attackState = state.attackState;
		view.dodgeWindowOpen = graph::DodgeWindowOpen();
		view.staminaOk = !settings.costResource || state.resource >= settings.minimum;
		const Decision decision = core_->Evaluate(view);
		if (decision.reject == RejectReason::StaminaLow) {
			LogCost(player, state.resource);
		}
		Execute(player, decision);

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
		if (core_->DodgeRunning()) {
			EndByGraph("3D reloaded");
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
		f.move = input::WorldMoveInput();
		f.actorYaw = player->data.angle.z;
		f.locomotionSpeed = measuredSpeed_;
		f.lockOn = lockon::LockedOn(player);
		f.freeRotation = lockon::FreeRotation();
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
		const bool entered = graph::TakeEntered();
		const bool clipEnd = graph::TakeClipEnd();
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
			EndByGraph("graph left every dodge state");
			return;
		}
		if (clipEnd && !entered && !core_->Exiting()) {
			// The graph returns to Default_State by itself: start the velocity handover without
			// sending RDF_DodgeExit again.
			DevLog("{}", "clip ended: starting locomotion handover");
			exitSent_ = true;
			core_->Apply(Decision{ DecisionKind::LocomotionExit }, true);
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

	void Controller::LogRejection(const Decision& d)
	{
		if (d.reject == lastReject_) {
			return;
		}
		lastReject_ = d.reject;
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

	void Controller::TryStart(RE::PlayerCharacter* player, const Decision& d)
	{
		const auto& settings = Settings::Get();
		const bool  chain = d.kind == DecisionKind::ChainDodge;
		const float angle = core_->PreviewStartAngleDeg(d);
		const int   clip = ClipIndex(angle, core_->PreviewKeepsFacing(d));
		if (!graph::SendStart(player, chain, clip, settings.core.timing.PlaybackSpeed(settings.clipLength, settings.animSpeed))) {
			DevLog("graph refused {} (clip {}); press stays buffered", chain ? "chain" : "start", clip);
			core_->Apply(d, false);
			return;
		}
		effects::ChargeCost(player, settings.cost);
		charged_ = settings.cost;
		core_->Apply(d, true);
		awaitingEnter_ = true;
		enterFrames_ = 0;
		exitSent_ = false;
		if (!core_->KeepsFacing()) {
			lockon::AcquireYaw();  // TDM would otherwise turn the player back toward its own target yaw
		}
		DevLog("dodge {} angle={:.1f} clip={} keepFacing={} freeRotation={}", chain ? "chain" : "start", angle, clip,
			core_->KeepsFacing(), lockon::FreeRotation());
	}

	bool Controller::Perform(RE::PlayerCharacter* player, const Decision& d)
	{
		switch (d.kind) {
		case DecisionKind::PowerAttack:
			return PlayAction(player, RE::DEFAULT_OBJECT::kActionRightPowerAttack);
		case DecisionKind::Block:
			return StartBlock(player);
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
		const auto  step = core_->Step();
		motion::Publish(step.command, player->GetFormID());
		requestedDistance_ = step.command.speed * dt;
		requestedHeading_ = core_->Heading();

		effects::SetInvulnerable(settings.iFrames && core_->InWindow(settings.iFrameStart, settings.iFrameEnd));

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
		lockon::ReleaseYaw();
		effects::SetInvulnerable(false);
		awaitingEnter_ = false;
		charged_ = 0.0f;
		DevLog("dodge stopped (phase {})", ToString(core_->Phase()));
	}

	void Controller::Trace(RE::PlayerCharacter* player)
	{
		if (!Settings::Get().devLog || !(wasRunning_ || core_->Phase() == DodgePhase::Done)) {
			return;
		}
		const auto  hook = motion::ReadStats();
		const float headingErr = AngleDelta(prevRequestedHeading_, measuredHeading_) * kRadToDeg;
		logger::info(
			"trace t={:.3f} phase={} act={:.3f} desiredHdg={:.1f} facing={:.1f} actorYaw={:.1f} cmdSpeed={:.1f} | "
			"requested={:.2f} actual={:.2f} measuredHdg={:.1f} err={:.1f} | hook calls={} overrides={} thread={}/{} lastLocal=({:.2f},{:.2f}) yaw={:.4f} graphDodging={}",
			core_->Now(), ToString(core_->Phase()), core_->ActionTime(), prevRequestedHeading_ * kRadToDeg, core_->Facing() * kRadToDeg,
			player->data.angle.z * kRadToDeg, core_->Speed(), prevRequestedDistance_, measuredDistance_, measuredHeading_ * kRadToDeg,
			headingErr, hook.calls, hook.overrides, hook.firstThread, hook.lastThread, hook.lastLocalX, hook.lastLocalY,
			hook.lastYaw, graph::GraphIsDodging(player));
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
