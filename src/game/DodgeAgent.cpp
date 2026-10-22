#include "DodgeAgent.h"

#include "ActionCapture.h"
#include "AttackCancel.h"
#include "Diagnostics.h"
#include "Effects.h"
#include "EngineState.h"
#include "MotionHook.h"
#include "Settings.h"

#include <array>
#include <format>

namespace rdf::game
{
	namespace
	{
		constexpr int kFirstStepClip = 8;    // RDF_DodgeClip 0..7 rolls, 8..15 steps (ANIMATION_CONTRACT.md)
		constexpr int kNeutralRollClip = 16;  // 16 roll n, 17 step n: a dodge pressed without move input

		Vec2 PlanarPosition(const RE::Actor* a)
		{
			const auto pos = a->GetPosition();
			return { pos.x, pos.y };
		}

		// The arm layer holds the stance of what is drawn. iRightHandType / iLeftHandType report the
		// equipped item even while it is sheathed, so by default a sheathed weapon or spell keeps no stance
		// (bArmStanceWhenSheathed opts in); fists have none (the graph's left-hand "none" entry is a one-hander pose).
		bool HasStanceToKeep(RE::Actor* actor)
		{
			if (!actor->AsActorState()->IsWeaponDrawn() && !Settings::Get().armStanceWhenSheathed) {
				return false;
			}
			std::int32_t right = 0;
			actor->GetGraphVariableInt("iRightHandType"sv, right);
			return right != 0;
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

	DodgeAgent::DodgeAgent(RE::Actor* actor, AgentDriver* driver) :
		core_(std::make_unique<ReactiveCore>(Settings::Get().core)),
		driver_(driver),
		formID_(actor ? actor->GetFormID() : 0),
		seenGeneration_(Settings::Generation())
	{}

	DodgeAgent::~DodgeAgent() = default;

	bool DodgeAgent::Busy() const
	{
		return core_->DodgeRunning() || awaitingEnter_ || carry_ > 0.0f;
	}

	// The menu committed new tuning: hand it to the core between dodges.
	void DodgeAgent::RefreshConfig()
	{
		const auto generation = Settings::Generation();
		if (generation != seenGeneration_ && !awaitingEnter_ && core_->Reconfigure(Settings::Get().core)) {
			seenGeneration_ = generation;
			DevLog("{:08X}: tuning updated (generation {})", formID_, generation);
		}
	}

	void DodgeAgent::Reset()
	{
		core_->Reset();
		perfectFired_ = false;
		awaitingEnter_ = false;
		exitSent_ = true;
		wasRunning_ = false;
		carry_ = 0.0f;
		prevDepth_ = 0;
		retry_ = {};
		pendingAttack_.reset();
		havePos_ = false;
		measuredSpeed_ = 0.0f;
		last3D_ = nullptr;
		charged_ = 0.0f;
	}

	void DodgeAgent::Release(RE::Actor* actor)
	{
		if (actor && (core_->DodgeRunning() || awaitingEnter_)) {
			graph::SendExit(actor);
		}
		if (actor) {
			graph::SetChainIndex(actor, 0);
			effects::SetInvulnerable(actor, false);
			graph::UnregisterSink(actor);
			effects::TakeDodgedHit(actor);
		}
		capture::Forget(formID_);
		motion::Release(formID_);
		Reset();
	}

	void DodgeAgent::Tick(RE::Actor* actor, float dt, const AgentInput& in, const std::function<void(ReactiveCore&)>& feed)
	{
		formID_ = actor->GetFormID();  // the player's agent is built before the player exists
		RefreshConfig();
		Refresh3D(actor);
		if (in.trackActiveGraph && graph::TrackActiveGraph(actor) && (core_->DodgeRunning() || awaitingEnter_)) {
			AbandonGraph(actor, "active graph switched (camera)");
		}
		MeasureMotion(actor, dt);
		const FrameInput frame = BuildFrame(actor, dt, in);
		core_->BeginFrame(frame);
		if (feed) {
			feed(*core_);
		}
		// attack attempts the dodge held back (any source: input, attack frameworks, AI) are attack presses
		for (const auto kind : capture::TakeNew(formID_)) {
			core_->Press(kind, {});
			DevLog("{:08X}: held-back {} buffered", formID_, kind == CommandKind::PowerAttack ? "power attack" : "attack");
		}
		HandleGraphEvents(actor);
		ReleaseOrphanedState(actor, dt);
		HandleDodgedHits(actor);
		RetryDeferred(actor);

		const bool  ownDodge = core_->DodgeRunning() || awaitingEnter_;
		const auto  state = engine::Read(actor, ownDodge);
		const auto& settings = Settings::Get();
		VerifyAttack(actor, dt, state.attackState);
		EngineView  view;
		view.engine = state.block;
		staggerAge_ = state.block == EngineBlock::Staggered ? staggerAge_ + dt : 0.0f;
		// [Stagger] bDodgeFromStagger: a dodge may cut the stagger short once fDodgeDelay has passed.
		escapeStagger_ = !ownDodge && state.block == EngineBlock::Staggered && settings.dodgeFromStagger &&
		                 staggerAge_ >= settings.staggerDodgeDelay;
		if (escapeStagger_) {
			view.engine = EngineBlock::None;
		}
		view.attackState = state.attackState;
		view.dodgeWindowOpen = graph::DodgeWindowOpen(actor);
		ReadAttack(actor, state.attackState, view);
		view.staminaOk = !settings.costResource || state.resource >= settings.minimum;
		const Decision decision = core_->Evaluate(view);
		if (decision.reject == RejectReason::StaminaLow) {
			LogCost(actor, state.resource);
		}
		Execute(actor, decision, in);

		StepAndPublish(actor, dt, in);
		// Attack attempts are held back while the dodge can still be cancelled into one; once it is cancelled or hands
		// over they pass (a combo continuing from the attack the dodge became).
		capture::SetCapturing(formID_, awaitingEnter_ || (core_->DodgeRunning() && !core_->Exiting() && !core_->CancelledIntoAction()));
		Trace(actor);
		prevRequestedDistance_ = requestedDistance_;
		prevRequestedHeading_ = requestedHeading_;
	}

	// [AttackCancel] What the actor's attack allows (ActionArbiter.h DodgeCancelsAttack). A new attack starts over: the
	// vanilla state passing into Draw is seen here before the new clip's annotations arrive (our tick runs in the actor's
	// update, ahead of its graph update); the frameworks' own start events reset the graph side in event order.
	void DodgeAgent::ReadAttack(RE::Actor* actor, std::uint8_t attackState, EngineView& view)
	{
		if (attackState != prevAttackState_ && (attackState == attack_state::kDraw || attackState == attack_state::kNone)) {
			graph::ResetAttack(actor);
		}
		prevAttackState_ = attackState;
		if (attackState == attack_state::kNone) {
			return;
		}
		const auto windows = graph::ReadAttack(actor);
		view.attackClipCancel = windows.clipCancel;
		view.attackRecovery = windows.recovery;
		for (const auto& name : Settings::Get().attackCancel.recoveryVariables) {
			bool value = false;
			if (!view.attackRecovery && actor->GetGraphVariableBool(name, value) && value) {
				view.attackRecovery = true;
			}
		}
		// The keyword walk (actor, worn items, active effects) only while a dodge press waits on it.
		if (core_->Buffer().Peek(CommandKind::Dodge, core_->Now())) {
			const bool power = actor->IsPowerAttacking();
			view.cancelAnyTime = attack::AnyTime(actor, power);
			const auto summary = std::format("state={} power={} anytime={} clip={} recovery={} window={}", attackState, power,
				view.cancelAnyTime, view.attackClipCancel ? (*view.attackClipCancel ? "open" : "closed") : "-", view.attackRecovery,
				view.dodgeWindowOpen);
			if (summary != lastAttackLog_) {
				lastAttackLog_ = summary;
				DevLog("{:08X}: dodge pressed during an attack: {}", formID_, summary);
			}
		}
	}

	void DodgeAgent::Refresh3D(RE::Actor* actor)
	{
		auto* current = actor->Get3D();
		if (current == last3D_) {
			return;
		}
		last3D_ = current;
		graph::RegisterSink(actor);
		if (core_->DodgeRunning() || awaitingEnter_) {
			AbandonGraph(actor, "3D reloaded");
		} else {
			graph::SetChainIndex(actor, 0);  // a loaded graph may carry a saved mid-chain value
		}
	}

	void DodgeAgent::MeasureMotion(RE::Actor* actor, float dt)
	{
		const Vec2 pos = PlanarPosition(actor);
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

	FrameInput DodgeAgent::BuildFrame(RE::Actor* actor, float dt, const AgentInput& in)
	{
		FrameInput f;
		f.dt = dt;
		f.actorYaw = actor->data.angle.z;
		f.locomotionSpeed = measuredSpeed_;
		f.lockOn = in.lockOn;
		f.move = in.move;
		f.freeRotation = in.freeRotation && !in.cameraIsYaw;
		f.cameraIsYaw = in.cameraIsYaw;
		f.cameraYaw = in.cameraYaw;
		moveHeld_ = Length(f.move) > Settings::Get().core.steering.deadzone;
		return f;
	}

	void DodgeAgent::HandleGraphEvents(RE::Actor* actor)
	{
		graph::DrainClipWindows(actor, [this](WindowEvent e) {
			core_->OnWindowEvent(e);
			DevLog("{:08X}: clip window event {} at t={:.3f}", formID_, static_cast<int>(e), core_->ActionTime());
		});
		const bool entered = graph::TakeEntered(actor);
		const int  depth = graph::Depth(actor);
		const int  previousDepth = prevDepth_;
		prevDepth_ = depth;
		if (!core_->DodgeRunning()) {
			awaitingEnter_ = false;
			return;
		}

		if (entered) {
			awaitingEnter_ = false;  // any Entered completes the start/chain handshake
			DevLog("{:08X}: graph entered dodge (depth {}, graph var {})", formID_, depth, graph::GraphIsDodging(actor));
		} else if (awaitingEnter_) {
			if (++enterFrames_ > Settings::Get().enterTimeoutFrames) {
				RollbackStart(actor);
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
			DevLog("{:08X}: graph left the dodge state; finishing the hand-over blend", formID_);
		}
	}

	void DodgeAgent::RollbackStart(RE::Actor* actor)
	{
		logger::warn("{:08X}: graph did not report {} within {} frames, rolling back (behavior patch installed? a creature graph?)",
			formID_, Settings::Get().graph.entered, Settings::Get().enterTimeoutFrames);
		effects::RefundCost(actor, charged_);
		charged_ = 0.0f;
		awaitingEnter_ = false;
		exitSent_ = true;
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
	}

	void DodgeAgent::EndByGraph(const char* why)
	{
		DevLog("{:08X}: dodge ended by graph: {}", formID_, why);
		exitSent_ = true;  // the graph is already heading back to locomotion
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
	}

	// The player has a first- and a third-person graph and our start event reached both, so after a
	// camera switch the newly active one is still in a dodge state (bAnimationDriven, no clip-end
	// exit) that we no longer drive. Send the exit so every graph goes back to locomotion.
	void DodgeAgent::AbandonGraph(RE::Actor* actor, const char* why)
	{
		graph::SendExit(actor);
		if (awaitingEnter_) {
			effects::RefundCost(actor, charged_);
			charged_ = 0.0f;
			awaitingEnter_ = false;
		}
		if (core_->DodgeRunning()) {
			EndByGraph(why);
		} else {
			DevLog("{:08X}: graph abandoned before entering: {}", formID_, why);
			exitSent_ = true;
		}
	}

	// Safety net: the graph reports a dodge state while we own no dodge (a switch we did not see, a load
	// mid-dodge). Our states have no exit of their own, so without this the actor would stand animation-
	// driven until the next dodge or jump. The grace period covers the out-blend.
	void DodgeAgent::ReleaseOrphanedState(RE::Actor* actor, float dt)
	{
		if (core_->DodgeRunning() || awaitingEnter_ || !graph::GraphIsDodging(actor)) {
			orphanTime_ = 0.0f;
			return;
		}
		orphanTime_ += dt;
		if (orphanTime_ >= 0.6f) {
			logger::info("{:08X}: graph stuck in a dodge state we do not own; sending {}", formID_, Settings::Get().graph.exit);
			graph::SendExit(actor);
			graph::SetChainIndex(actor, 0);
			orphanTime_ = 0.0f;  // retry after another grace period if it did not take
		}
	}

	// Perfect dodge: a hit the i-frames dodged (melee, Precision, projectile, explosion) inside [fPerfectWindowStart,
	// fPerfectWindowEnd] of the dodge, once per dodge. It tells both graphs and Papyrus; the driver may add more.
	void DodgeAgent::HandleDodgedHits(RE::Actor* actor)
	{
		const auto  attacker = effects::TakeDodgedHit(actor);
		const auto& p = Settings::Get().perfect;
		if (!attacker || !p.enabled || perfectFired_ || !core_->DodgeRunning()) {
			return;
		}
		const float t = core_->ActionTime();
		if (!core_->PerfectWindow(p.windowStart, p.windowEnd)) {
			DevLog("{:08X}: dodged a hit at t={:.3f}, outside the perfect window (RDF_PerfectStart/End of the clip, else [{:.2f}, {:.2f}])",
				formID_, t, p.windowStart, p.windowEnd);
			return;
		}
		perfectFired_ = true;
		const auto ref = *attacker ? RE::TESObjectREFR::LookupByHandle(*attacker) : RE::NiPointer<RE::TESObjectREFR>{};
		auto*      attackerActor = ref ? ref->As<RE::Actor>() : nullptr;
		logger::info("{:08X}: perfect dodge at t={:.3f} against {:08X}", formID_, t, ref ? ref->GetFormID() : 0u);
		if (!p.playerEvent.empty()) {
			actor->NotifyAnimationGraph(p.playerEvent);
		}
		if (attackerActor && !p.attackerEvent.empty()) {
			attackerActor->NotifyAnimationGraph(p.attackerEvent);
		}
		if (!p.modEvent.empty()) {
			if (auto* source = SKSE::GetModCallbackEventSource()) {
				// sender = attacker, strArg = the dodger's FormID (hex), numArg = dodge time
				SKSE::ModCallbackEvent event{ p.modEvent, RE::BSFixedString(std::format("{:08X}", formID_)), t, ref.get() };
				source->SendEvent(&event);
			}
		}
		if (driver_) {
			driver_->OnPerfectDodge(actor, t);
		}
	}

	int DodgeAgent::ClipIndex(float angleDeg, bool keepFacing, bool neutral)
	{
		if (!keepFacing) {
			return 0;  // the character turns to face the trajectory: always the forward roll
		}
		if (neutral) {
			return kNeutralRollClip + (Settings::Get().sidestepWhenKeepFacing ? 1 : 0);
		}
		const int octant = static_cast<int>(std::lround(angleDeg / 45.0f));
		const int index = ((octant % 8) + 8) % 8;  // F, FR, R, BR, B, BL, L, FL
		return index + (Settings::Get().sidestepWhenKeepFacing ? 8 : 0);
	}

	void DodgeAgent::LogCost(RE::Actor* actor, float current) const
	{
		const auto& settings = Settings::Get();
		auto*       owner = actor->AsActorValueOwner();
		const auto  resource = *settings.costResource;  // only called after a cost rejection
		DevLog("{:08X}: cost resource {} current={:.1f} permanent={:.1f} minimum={:.1f}", formID_, static_cast<int>(resource), current,
			owner->GetPermanentActorValue(resource), settings.minimum);
	}

	void DodgeAgent::LogRejection(const Decision& d)
	{
		if (d.reject == lastReject_ && d.rejectedCommand == lastRejectCommand_) {
			return;
		}
		lastReject_ = d.reject;
		if (d.reject == RejectReason::None) {
			lastAttackLog_.clear();
		}
		lastRejectCommand_ = d.rejectedCommand;
		if (d.reject != RejectReason::None && d.reject != RejectReason::NoCommand) {
			DevLog("{:08X}: rejected cmd={} reason={}", formID_, static_cast<int>(d.rejectedCommand), ToString(d.reject));
		}
	}

	void DodgeAgent::Execute(RE::Actor* actor, const Decision& d, const AgentInput& in)
	{
		LogRejection(d);
		if (d.kind != DecisionKind::None) {
			DevLog("{:08X}: decision {} seq={} phase={} t={:.3f} chain={}", formID_, ToString(d.kind), d.seq, ToString(core_->Phase()),
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
			TryStart(actor, d, in);
			break;
		case DecisionKind::Attack:
		case DecisionKind::PowerAttack:
		case DecisionKind::Block:
			CancelInto(actor, d);
			break;
		case DecisionKind::LocomotionExit:
			graph::SendExit(actor);
			exitSent_ = true;
			core_->Apply(d, true);
			break;
		}
	}

	// A chain only works while the graph is in a dodge state; after it has left (a hand-over in
	// progress) the chained dodge enters like a new one. A new dodge always targets the state not
	// used last, which may still be blending out.
	graph::DodgeEntry DodgeAgent::EntryFor(RE::Actor* actor, bool chain) const
	{
		if (chain && graph::Depth(actor) > 0) {
			return graph::DodgeEntry::Chain;
		}
		return inStateB_ ? graph::DodgeEntry::StartA : graph::DodgeEntry::StartB;
	}

	void DodgeAgent::TryStart(RE::Actor* actor, const Decision& d, const AgentInput& in)
	{
		const auto& settings = Settings::Get();
		const bool  chain = d.kind == DecisionKind::ChainDodge;
		const float angle = core_->PreviewStartAngleDeg(d);
		const int   clip = ClipIndex(angle, core_->PreviewKeepsFacing(d), core_->PreviewNeutral(d));
		const auto  entry = EntryFor(actor, chain);
		const bool  step = clip >= kNeutralRollClip ? clip > kNeutralRollClip : clip >= kFirstStepClip;
		const float stance = in.firstPerson ? settings.armStanceFirstPerson : step ? settings.armStanceStep : settings.armStanceRoll;
		const float armStance = HasStanceToKeep(actor) ? stance : 0.0f;
		const float clipLength = in.firstPerson ? settings.clipLengthFirstPerson : settings.clipLength;
		// RDF_ChainIndex: 0 for a fresh dodge, the running dodge's index + 1 for a chain (ChainCount is 1-based).
		const int   chainIndex = chain ? core_->ChainCount() : 0;
		if (!graph::SendStart(actor, entry, clip, settings.core.timing.PlaybackSpeed(clipLength, settings.animSpeed), armStance, chainIndex)) {
			DevLog("{:08X}: graph refused {} (clip {}); press stays buffered", formID_, chain ? "chain" : "start", clip);
			graph::SetChainIndex(actor, core_->DodgeRunning() ? core_->ChainCount() - 1 : 0);  // the dodge that keeps playing
			core_->Apply(d, false);
			return;
		}
		inStateB_ = entry == graph::DodgeEntry::Chain ? !inStateB_ : entry == graph::DodgeEntry::StartB;
		perfectFired_ = false;
		pendingAttack_.reset();
		if (escapeStagger_) {
			// The graph leaves the stagger state through our wildcard, so its staggerStop annotation never
			// fires; clear the flag the way the engine's handler would.
			engine::EndStagger(actor);
			DevLog("{:08X}: dodge out of stagger after {:.2f}s", formID_, staggerAge_);
		}
		graph::LogEvents(settings.devLog);
		effects::ChargeCost(actor, settings.cost);
		charged_ = settings.cost;
		core_->Apply(d, true);
		awaitingEnter_ = true;
		enterFrames_ = 0;
		exitSent_ = false;
		if (driver_) {
			driver_->OnDodgeStarted(actor, core_->OwnsYaw());
		}
		DevLog("{:08X}: dodge {} #{} angle={:.1f} clip={} keepFacing={} holdFacing={} ownsYaw={} freeRotation={} ({})", formID_,
			chain ? "chain" : "start", chainIndex, angle, clip, core_->KeepsFacing(), core_->HoldsFacing(), core_->OwnsYaw(), in.freeRotation, in.modeName);
	}

	// Attack and block live in a nested behavior under the default state, so leaving the dodge and
	// performing the action happen in the same tick. A failed action leaves the press buffered; the
	// dodge is already exiting into locomotion, where Retry picks the press up until it expires.
	void DodgeAgent::CancelInto(RE::Actor* actor, const Decision& d)
	{
		if (!exitSent_) {
			graph::SendExit(actor);
			exitSent_ = true;
		}
		if (d.kind != DecisionKind::Block) {
			// The attack starts once the graph has left the dodge (PerformPendingAttack, next tick): started in the update
			// that also runs the exit into Default_State, an attack framework's attack could start and then be dropped
			// (BFCO's in-place power attack never got past its first frame).
			capture::SetCapturing(formID_, false);
			core_->Apply(d, true);
			pendingAttack_ = PendingAttack{ d };
			DevLog("{:08X}: {} from the dodge: waiting for the graph to leave the dodge", formID_, ToString(d.kind));
			return;
		}
		const bool performed = driver_ && driver_->PerformAction(actor, d);
		if (performed) {
			core_->Apply(d, true);
			return;
		}
		DevLog("{:08X}: action for {} failed, will retry until the press expires", formID_, ToString(d.kind));
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
		retry_ = { d, true };
	}

	void DodgeAgent::VerifyAttack(RE::Actor* actor, float dt, std::uint8_t attackState)
	{
		if (!pendingAttack_) {
			return;
		}
		auto& p = *pendingAttack_;
		p.age += dt;
		// The graph's word, not the engine's: an accepted attack action sets the attack state to Draw at once
		// (ActorMediator 0x14063D0F0), even if the graph then drops the attack. IsAttacking is driven by the attack
		// states of 1hm_behavior themselves (isActive bindings), so it shows the attack really plays.
		bool attacking = false;
		actor->GetGraphVariableBool("IsAttacking"sv, attacking);
		if (attacking) {  // ours, or one the framework started from a press after the dodge let go
			DevLog("{:08X}: {} from the dodge started after {:.3f}s ({} attempt(s), attack state {})", formID_, ToString(p.decision.kind),
				p.age, p.attempts, attackState);
			pendingAttack_.reset();
			return;
		}
		constexpr float kGiveUp = 0.5f;      // a press older than this would feel like a dropped input anyway
		constexpr float kSettle = 0.1f;      // after an attempt: time for an accepted attack to show
		if (p.age > kGiveUp) {
			logger::info("{:08X}: {} from the dodge never started ({} attempt(s) in {:.2f}s, attack state {}); the graph refused it",
				formID_, ToString(p.decision.kind), p.attempts, p.age, attackState);
			pendingAttack_.reset();
			return;
		}
		// Only once the graph left our states (RDF_DodgeLeft): the attack then reaches the attack behaviors like any
		// attack made from locomotion.
		if (graph::Depth(actor) != 0) {
			return;
		}
		if (p.attempts > 0 && p.age - p.lastTry < kSettle) {
			return;
		}
		p.lastTry = p.age;
		++p.attempts;
		// The first time the attempt the dodge held back, exactly as it was made (a framework's idle or action); after
		// that, or without one, our own action.
		const bool replayed = p.attempts == 1 && capture::Replay(actor, p.decision.kind == DecisionKind::PowerAttack);
		const bool performed = replayed || (driver_ && driver_->PerformAction(actor, p.decision));
		DevLog("{:08X}: {} from the dodge, attempt #{} at {:.3f}s: {} {}", formID_, ToString(p.decision.kind), p.attempts, p.age,
			replayed ? "replayed" : "own action", performed ? "accepted" : "refused");
	}

	void DodgeAgent::RetryDeferred(RE::Actor* actor)
	{
		if (!retry_.pending) {
			return;
		}
		const auto* cmd = core_->Buffer().Peek(CommandOf(retry_.decision.kind), core_->Now());
		if (!cmd || cmd->seq != retry_.decision.seq) {
			retry_ = {};
			return;
		}
		if (driver_ && driver_->PerformAction(actor, retry_.decision)) {
			core_->Consume(retry_.decision);
			DevLog("{:08X}: deferred {} succeeded", formID_, ToString(retry_.decision.kind));
			retry_ = {};
		}
	}

	void DodgeAgent::StepAndPublish(RE::Actor* actor, float dt, const AgentInput& in)
	{
		const auto& settings = Settings::Get();
		auto        step = core_->Step();
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
			step.command.lookYawRate = in.lookYawRate;
		}
		motion::Publish(step.command, formID_);
		requestedDistance_ = step.command.speed * dt;
		requestedHeading_ = core_->Heading();

		effects::SetInvulnerable(actor, settings.iFrames && core_->Invulnerable(settings.iFrameStart, settings.iFrameEnd));

		if (step.completed && !exitSent_) {
			graph::SendExit(actor);
			exitSent_ = true;
		}
		const bool running = core_->DodgeRunning();
		if (wasRunning_ && !running) {
			OnStopped(actor);
		}
		wasRunning_ = running;
	}

	void DodgeAgent::OnStopped(RE::Actor* actor)
	{
		// Only a dodge that ran its course into running carries its speed; an interrupted one does not.
		if (core_->Phase() != DodgePhase::Done || !moveHeld_ || core_->CancelledIntoAction()) {
			carry_ = 0.0f;
		}
		tailFrames_ = 30;  // follow the hand-over into locomotion in the dev trace
		graph::SetChainIndex(actor, 0);  // the chain is over, whatever ended it
		effects::SetInvulnerable(actor, false);
		awaitingEnter_ = false;
		charged_ = 0.0f;
		if (driver_) {
			driver_->OnDodgeStopped(actor);
		}
		DevLog("{:08X}: dodge stopped (phase {})", formID_, ToString(core_->Phase()));
	}

	void DodgeAgent::Trace(RE::Actor* actor)
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
		actor->GetGraphVariableFloat("Speed"sv, speed);
		actor->GetGraphVariableFloat("SpeedSampled"sv, sampled);
		actor->GetGraphVariableFloat("Direction"sv, direction);
		bool inJump = false;
		actor->GetGraphVariableBool("bInJumpState"sv, inJump);
		const auto  hook = motion::ReadStats();
		const float headingErr = AngleDelta(prevRequestedHeading_, measuredHeading_) * kRadToDeg;
		logger::info(
			"trace {:08X} t={:.3f} phase={} act={:.3f} desiredHdg={:.1f} facing={:.1f} warp={:.1f} actorYaw={:.1f} cmdSpeed={:.1f} | "
			"requested={:.2f} actual={:.2f} measuredHdg={:.1f} err={:.1f} | hook calls={} overrides={} thread={}/{} lastLocal=({:.2f},{:.2f}) yaw={:.4f} graphDodging={}",
			formID_, core_->Now(), ToString(core_->Phase()), core_->ActionTime(), prevRequestedHeading_ * kRadToDeg, core_->Facing() * kRadToDeg,
			core_->WarpOffsetDeg(), actor->data.angle.z * kRadToDeg, core_->Speed(), prevRequestedDistance_, measuredDistance_, measuredHeading_ * kRadToDeg,
			headingErr, hook.calls, hook.overrides, hook.firstThread, hook.lastThread, hook.lastLocalX, hook.lastLocalY,
			hook.lastYaw, graph::GraphIsDodging(actor));
		float midSpeed = -1.0f;
		if (const auto* process = actor->GetActorRuntimeData().currentProcess) {
			if (process->middleHigh) {
				midSpeed = process->middleHigh->currentMovementSpeed;
			}
		}
		logger::info("trace {:08X} graph Speed={:.1f} SpeedSampled={:.1f} Direction={:.2f} inJump={} depth={} | process moveSpeed={:.1f} | carry={:.1f} floorHits={}",
			formID_, speed, sampled, direction, inJump, graph::Depth(actor), midSpeed, carry_, hook.floorHits);
	}
}
