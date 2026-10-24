#include "DodgeAgent.h"

#include "ActionCapture.h"
#include "AttackCancel.h"
#include "Diagnostics.h"
#include "Effects.h"
#include "GraphGate.h"
#include "EngineState.h"
#include "MotionHook.h"
#include "Settings.h"

#include <algorithm>
#include <array>
#include <format>

namespace rdf::game
{
	namespace
	{
		constexpr int kNeutralClip = 8;  // RDF_DodgeClip 0..7 directions (ANIMATION_CONTRACT.md), 8: pressed without move input

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
		intoAttack_ = false;
		havePos_ = false;
		measuredSpeed_ = 0.0f;
		last3D_ = nullptr;
		charged_ = 0.0f;
		releasesHeld_ = false;
		releaseRetry_ = 0.0f;
	}

	void DodgeAgent::Release(RE::Actor* actor)
	{
		if (actor && (core_->DodgeRunning() || awaitingEnter_)) {
			graph::SendExit(actor, layer_);
		}
		if (actor) {
			graph::SetChainIndex(actor, 0);
			graph::SetLayer(actor, graph::Layer::Root);
			ClearAttackFromDodge(actor);
			effects::SetInvulnerable(actor, false);
			graph::UnregisterSink(actor);
			effects::TakeDodgedHit(actor);
		}
		capture::Forget(formID_);
		castOn_.Clear();
		gate::Forget(formID_);
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
			DevLog("{:08X}: held-back {} buffered", formID_,
				kind == CommandKind::PowerAttack ? "power attack" : kind == CommandKind::Block ? "block" : "attack");
		}
		HandleGraphEvents(actor);
		GateGraphEvents(actor);
		ReleaseOrphanedState(actor, dt);
		DrainCastOnAnnotations(actor);
		HandleDodgedHits(actor);
		RetryDeferred(actor);

		const bool  ownDodge = core_->DodgeRunning() || awaitingEnter_;
		const auto  state = engine::Read(actor, ownDodge);
		const auto& settings = Settings::Get();
		VerifyAttack(actor, dt, state.attackState);
		TrackAttackFromDodge(actor);
		EngineView  view;
		view.engine = state.block;
		staggerAge_ = state.block == EngineBlock::Staggered ? staggerAge_ + dt : 0.0f;
		// [Stagger] a dodge may cut the stagger short (StaggerEscape); the rules are only asked while a dodge press waits.
		escapeStagger_ = !ownDodge && state.block == EngineBlock::Staggered && core_->Buffer().Peek(CommandKind::Dodge, core_->Now()) &&
		                 StaggerEscape(actor);
		if (escapeStagger_) {
			view.engine = EngineBlock::None;
		}
		view.attackState = state.attackState;
		view.dodgeWindowOpen = graph::DodgeWindowOpen(actor);
		ReadAttack(actor, state.attackState, view);
		// The keyword walk only while a dodge press waits on it.
		const bool dodgeWaits = core_->Buffer().Peek(CommandKind::Dodge, core_->Now()) != nullptr;
		view.dodgeForbidden = dodgeWaits && attack::DodgeForbidden(actor);
		// [Casting] a bow being drawn may be dodged out of at any moment when the dodge keeps the draw going.
		if (dodgeWaits && !ownDodge && !view.cancelAnyTime && CastingLayer(actor, state.attackState) == graph::Layer::Bow) {
			view.cancelAnyTime = true;
		}
		view.staminaOk = !settings.costResource || state.resource >= settings.minimum;
		const Decision decision = core_->Evaluate(view);
		if (decision.reject == RejectReason::StaminaLow) {
			LogCost(actor, state.resource);
		}
		Execute(actor, decision, in);

		StepAndPublish(actor, dt, in);
		HoldReleases(actor, dt);
		castOn_.Update(actor, core_->Now(), !awaitingEnter_);
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
		oneHandBroken_ = false;
		layerBroken_ = false;
		graph::RegisterSink(actor);
		if (core_->DodgeRunning() || awaitingEnter_) {
			AbandonGraph(actor, "3D reloaded");
		} else {
			graph::SetChainIndex(actor, 0);  // a loaded graph may carry a saved mid-chain value
			fromDodge_.active = true;  // ... and a saved attack-from-dodge value
			ClearAttackFromDodge(actor);
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

	// [Stagger] Whether the dodge pressed now may cut the actor's stagger short. The first rule that decides wins: the
	// no-escape keyword, the any-time keyword, the stagger framework's recovery window, the delay.
	bool DodgeAgent::StaggerEscape(RE::Actor* actor)
	{
		const auto& settings = Settings::Get();
		const char* why = nullptr;
		bool        allowed = false;
		if (!settings.dodgeFromStagger) {
			why = "bDodgeFromStagger=0";
		} else if (attack::StaggerNoEscape(actor)) {
			why = "no-escape keyword";
		} else if (attack::StaggerAnyTime(actor)) {
			allowed = true;
			why = "any-time keyword";
		} else if (graph::StaggerRecoveryOpen(actor)) {
			allowed = true;
			why = "recovery window";
		} else {
			const float delay = attack::StaggerDelayOverride(actor).value_or(settings.staggerDodgeDelay);
			allowed = delay >= 0.0f && staggerAge_ >= delay;
			why = delay < 0.0f ? "no time rule, window closed" : allowed ? "delay passed" : "before the delay";
		}
		if (why != lastStaggerRule_) {
			lastStaggerRule_ = why;
			DevLog("{:08X}: dodge out of the stagger at {:.2f}s: {} ({})", formID_, staggerAge_, allowed ? "allowed" : "refused", why);
		}
		return allowed;
	}

	void DodgeAgent::HandleGraphEvents(RE::Actor* actor)
	{
		graph::DrainClipWindows(actor, [this](WindowEvent e) {
			core_->OnWindowEvent(e);
			DevLog("{:08X}: clip window event {} at t={:.3f}", formID_, static_cast<int>(e), core_->ActionTime());
		});
		ApplyClipMotion(actor);
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
		castOn_.CancelDodge();
		logger::warn("{:08X}: graph did not report {} within {} frames, rolling back (behavior patch installed? a creature graph?)",
			formID_, Settings::Get().graph.entered, Settings::Get().enterTimeoutFrames);
		effects::RefundCost(actor, charged_);
		charged_ = 0.0f;
		awaitingEnter_ = false;
		exitSent_ = true;
		core_->Apply(Decision{ DecisionKind::Interrupt }, true);
		if (layer_ == graph::Layer::OneHand) {
			// A graph took the 1hm start but the active one never entered: a behavior output without the 1hm part in this
			// view (first person after an older Pandora run). Root dodges only, until the 3D reloads.
			oneHandBroken_ = true;
			logger::warn("{:08X}: the 1hm dodge did not enter; using the root dodge until the 3D reloads (re-run Pandora / Nemesis?)", formID_);
		} else if (layer_ != graph::Layer::Root) {
			layerBroken_ = true;
			logger::warn("{:08X}: the {} dodge did not enter; no bow / magic dodges until the 3D reloads (re-run Pandora / Nemesis?)", formID_,
				graph::ToString(layer_));
		}
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
		graph::SendAllExits(actor);
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
			logger::info("{:08X}: graph stuck in a dodge state we do not own; sending every layer's exit", formID_);
			graph::SendAllExits(actor);
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
		TriggerCastOn(actor, caston::Trigger::PerfectDodge, attackerActor);
	}

	int DodgeAgent::ClipIndex(float angleDeg, bool keepFacing, bool neutral)
	{
		if (!keepFacing) {
			return 0;  // the character turns to face the trajectory: always the forward clip
		}
		if (neutral) {
			return kNeutralClip;
		}
		const int octant = static_cast<int>(std::lround(angleDeg / 45.0f));
		return ((octant % 8) + 8) % 8;  // F, FR, R, BR, B, BL, L, FL
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
			graph::SendExit(actor, layer_);
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
		const float stance = in.firstPerson ? settings.armStanceFirstPerson : settings.armStance;
		stanceKept_ = HasStanceToKeep(actor);
		const float armStance = stanceKept_ ? stance : 0.0f;
		const float clipLength = in.firstPerson ? settings.clipLengthFirstPerson : settings.clipLength;
		// RDF_ChainIndex: 0 for a fresh dodge, the running dodge's index + 1 for a chain (ChainCount is 1-based).
		const int   chainIndex = chain ? core_->ChainCount() : 0;
		const float playback = settings.core.timing.PlaybackSpeed(clipLength, settings.animSpeed);
		clipLength_ = clipLength;  // until the clip says otherwise (RDF_ClipLength, RDF_Speed)
		clipSpeed_ = settings.animSpeed;
		// [Casting] the bow / magic layer first (the keyword, a draw / a cast running), then the 1hm dodge (a
		// weapon out: 1hm_behavior runs, third or first person), else the root one. A chain stays in the layer of the dodge it
		// continues; outside the root it is the start of the other state.
		auto        sent = entry;
		auto        layer = graph::Layer::Root;
		bool        accepted = false;
		const bool  chaining = entry == graph::DodgeEntry::Chain;
		const auto  tryLayer = [&](graph::Layer l) {
			sent = chaining ? (inStateB_ ? graph::DodgeEntry::StartA : graph::DodgeEntry::StartB) : entry;
			if (!graph::SendStart(actor, sent, clip, playback, armStance, chainIndex, l)) {
				return false;
			}
			layer = l;
			return true;
		};
		if (chaining && layer_ != graph::Layer::Root) {
			accepted = tryLayer(layer_);
		} else if (!chaining) {
			const auto casting = CastingLayer(actor, static_cast<std::uint8_t>(actor->AsActorState()->GetAttackState()));
			accepted = casting != graph::Layer::Root && tryLayer(casting);
			if (!accepted && settings.graph.oneHand && !oneHandBroken_) {
				accepted = tryLayer(graph::Layer::OneHand);
			}
		}
		if (!accepted && (!chaining || layer_ == graph::Layer::Root)) {
			sent = entry;   // the root has its own chain event
			accepted = graph::SendStart(actor, entry, clip, playback, armStance, chainIndex, graph::Layer::Root);
			layer = graph::Layer::Root;
		}
		if (!accepted) {
			DevLog("{:08X}: graph refused {} (clip {}); press stays buffered", formID_, chain ? "chain" : "start", clip);
			graph::SetChainIndex(actor, core_->DodgeRunning() ? core_->ChainCount() - 1 : 0);  // the dodge that keeps playing
			graph::SetLayer(actor, core_->DodgeRunning() ? layer_ : graph::Layer::Root);
			core_->Apply(d, false);
			return;
		}
		layer_ = layer;
		inStateB_ = sent == graph::DodgeEntry::Chain ? !inStateB_ : sent == graph::DodgeEntry::StartB;
		perfectFired_ = false;
		lastClip_ = clip;
		ClearAttackFromDodge(actor);
		intoAttack_ = false;
		pendingAttack_.reset();
		if (escapeStagger_) {
			// The graph leaves the stagger state through our wildcard, so its staggerStop annotation never
			// fires; clear the flag the way the engine's handler would.
			engine::EndStagger(actor);
			DevLog("{:08X}: dodge out of stagger after {:.2f}s", formID_, staggerAge_);
		}
		graph::LogEvents(settings.devLog);
		float cost = settings.cost;
		if (escapeStagger_) {   // [Stagger] the price of leaving a stagger
			cost += std::max(0.0f, settings.stagger.escapeCost + attack::StaggerEscapeCostBonus(actor));
		}
		effects::ChargeCost(actor, cost);
		charged_ = cost;
		core_->Apply(d, true);
		awaitingEnter_ = true;
		enterFrames_ = 0;
		exitSent_ = false;
		if (driver_) {
			driver_->OnDodgeStarted(actor, core_->OwnsYaw());
		}
		// [CastOn] the dodge this one continues is over for its rules; a fresh dodge starts a new chain for them
		if (chain) {
			castOn_.OnDodgeEnd(actor);
		} else {
			castOn_.OnChainOver();
		}
		lastChainIndex_ = chainIndex;
		lastFromStagger_ = escapeStagger_;
		lastFirstPerson_ = in.firstPerson;
		TriggerCastOn(actor, caston::Trigger::Dodge, nullptr);
		DevLog("{:08X}: dodge {} #{} ({}) angle={:.1f} clip={} keepFacing={} holdFacing={} ownsYaw={} freeRotation={} ({})", formID_,
			chain ? "chain" : "start", chainIndex, graph::ToString(layer_), angle, clip, core_->KeepsFacing(), core_->HoldsFacing(), core_->OwnsYaw(), in.freeRotation, in.modeName);
	}

	// Attack and block live in a nested behavior under the default state, so leaving the dodge and
	// performing the action happen in the same tick. A failed action leaves the press buffered; the
	// dodge is already exiting into locomotion, where Retry picks the press up until it expires.
	void DodgeAgent::CancelInto(RE::Actor* actor, const Decision& d)
	{
		// A 1hm dodge is left by the action itself (1HM_Ready_State's transitions): no exit first, nothing to wait for.
		if (layer_ == graph::Layer::OneHand && !exitSent_ && graph::Depth(actor) > 0 && CancelInPlace(actor, d)) {
			return;
		}
		if (!exitSent_) {
			graph::SendExit(actor, layer_);
			exitSent_ = true;
		}
		if (d.kind != DecisionKind::Block) {
			// The attack starts once the graph has left the dodge (PerformPendingAttack, next tick): started in the update
			// that also runs the exit into Default_State, an attack framework's attack could start and then be dropped
			// (BFCO's in-place power attack never got past its first frame).
			capture::SetCapturing(formID_, false);
			core_->Apply(d, true);
			// With the move input held the actor goes on running (speed floor at the run speed) until the attack takes
			// over: an attack framework then sees an attack out of running, as without a dodge. Stopping dead put the
			// graph into idle and back into a move start, which refuses attacks for ~0.25 s; the dodge's own speed made
			// BFCO pick its sprint power attack.
			intoAttack_ = true;
			// For OAR conditions on the attack's clip: set before the attack starts (next tick, PerformPendingAttack).
			graph::SetAttackFromDodge(actor, d.kind == DecisionKind::PowerAttack ? 2 : 1, lastClip_);
			fromDodge_ = FromDodge{ true, false, graph::ReadAttack(actor).starts };
			if (moveHeld_) {
				carry_ = Settings::Get().core.motion.runSpeed;
			}
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

	bool DodgeAgent::CancelInPlace(RE::Actor* actor, const Decision& d)
	{
		if (d.kind == DecisionKind::Block) {
			if (!driver_ || !driver_->PerformAction(actor, d)) {
				DevLog("{:08X}: block out of the 1hm dodge refused; leaving the dodge first", formID_);
				return false;
			}
			exitSent_ = true;   // the block left the dodge
			core_->Apply(d, true);
			DevLog("{:08X}: block straight out of the 1hm dodge", formID_);
			return true;
		}
		const bool bash = driver_ && driver_->PrepareBash(actor);
		const bool replayed = capture::Replay(actor, d.kind == DecisionKind::PowerAttack);
		capture::SetCapturing(formID_, false);
		const bool performed = replayed || (driver_ && driver_->PerformAction(actor, d));
		DevLog("{:08X}: {}{} straight out of the 1hm dodge: {} {}", formID_, ToString(d.kind), bash ? " (bash)" : "",
			replayed ? "replayed" : "own action", performed ? "accepted" : "refused");
		if (!performed) {
			return false;   // the root path: exit, then attempts once the graph has left
		}
		exitSent_ = true;   // the attack left the dodge
		core_->Apply(d, true);
		intoAttack_ = true;
		graph::SetAttackFromDodge(actor, d.kind == DecisionKind::PowerAttack ? 2 : 1, lastClip_);
		fromDodge_ = FromDodge{ true, false, graph::ReadAttack(actor).starts };
		if (moveHeld_) {
			carry_ = Settings::Get().core.motion.runSpeed;
		}
		PendingAttack p{ d };
		p.attempts = 1;   // already made: VerifyAttack only waits for it to show (and retries our own action if not)
		pendingAttack_ = p;
		return true;
	}

	// Graph events other mods sent straight to the graph while the dodge could not be left (gate.h) go out once it can:
	// a 1hm dodge in its block window (they leave it like Ready), a root dodge once the graph has left it.
	void DodgeAgent::GateGraphEvents(RE::Actor* actor)
	{
		const bool dodging = core_->DodgeRunning() || awaitingEnter_;
		const bool mayLeave = layer_ == graph::Layer::OneHand ? (!dodging || core_->BlockWindowOpen()) : (!dodging && graph::Depth(actor) == 0);
		if (!mayLeave) {
			gate::SetLocked(formID_, true);
			return;
		}
		for (const auto& event : gate::TakeHeld(formID_)) {
			const bool taken = gate::Send(actor, event);
			DevLog("{:08X}: held graph event '{}' sent: {}", formID_, event.c_str(), taken ? "taken" : "refused");
		}
		gate::SetLocked(formID_, false);
	}

	// [Casting] Which layer keeps what the actor's arms are doing: a bow being drawn (or held drawn) and the draw keyword,
	// else a cast running (the vanilla IsCasting* graph variables) and the cast keyword. Both views: the first-person files
	// have the same layers (under a drawn bow the 1P dodge keeps the aim and the camera, BEHAVIOR_INTEGRATION.md 8).
	graph::Layer DodgeAgent::CastingLayer(RE::Actor* actor, std::uint8_t attackState) const
	{
		if (layerBroken_) {
			return graph::Layer::Root;
		}
		const bool drawing = attackState == attack_state::kBowDraw || attackState == attack_state::kBowAttached ||
		                     attackState == attack_state::kBowDrawn;
		if (drawing && attack::DrawDuringDodge(actor)) {
			return graph::Layer::Bow;
		}
		const auto& names = Settings::Get().casting.castingVariables;
		const bool  casting = std::any_of(names.begin(), names.end(), [&](const std::string& name) {
			bool value = false;
			return actor->GetGraphVariableBool(name, value) && value;
		});
		if (casting && attack::CastDuringDodge(actor)) {
			return graph::Layer::Magic;
		}
		return graph::Layer::Root;
	}

	// [Casting] A bow / magic dodge plays inside the draw / the cast; a release (an arrow loosed, a spell let go) would end
	// that state and the dodge with it, so releases wait until the graph has left the dodge, then go out as they were made.
	// A release refused right at the exit (magicbehavior restarts the cast arms as the dodge leaves: SBF_CastStop / Start in
	// the same frame) is tried again on the next ticks, for kReleaseRetry at most.
	void DodgeAgent::HoldReleases(RE::Actor* actor, float dt)
	{
		constexpr float kReleaseRetry = 0.4f;
		const bool layered = layer_ == graph::Layer::Bow || layer_ == graph::Layer::Magic;
		const bool hold = layered && (awaitingEnter_ || (core_->DodgeRunning() && graph::Depth(actor) > 0));
		if (hold != releasesHeld_) {
			releasesHeld_ = hold;
			capture::SetHoldingReleases(formID_, hold);
			if (hold) {
				releaseRetry_ = 0.0f;
				if (layer_ == graph::Layer::Bow) {
					capture::SetBusyHands(formID_, true, false, false);
				} else {
					const auto var = [&](const char* name) {
						bool value = false;
						return actor->GetGraphVariableBool(name, value) && value;
					};
					const bool dual = var("IsCastingDual");
					capture::SetBusyHands(formID_, dual || var("IsCastingRight"), dual || var("IsCastingLeft"), dual);
				}
				return;
			}
			capture::SetBusyHands(formID_, false, false, false);
			releaseRetry_ = kReleaseRetry;
			const auto r = capture::ReplayReleases(actor);
			DevLog("{:08X}: the {} dodge is over: {} held release(s) replayed, {} refused", formID_, graph::ToString(layer_), r.accepted,
				r.remaining);
			if (r.remaining == 0) {
				releaseRetry_ = 0.0f;
			}
			return;
		}
		if (hold || releaseRetry_ <= 0.0f) {
			return;
		}
		releaseRetry_ -= dt;
		const auto r = capture::ReplayReleases(actor);
		if (r.accepted > 0 || r.remaining == 0) {
			DevLog("{:08X}: held release(s) accepted on a retry ({:.2f}s after the dodge)", formID_, kReleaseRetry - releaseRetry_);
			releaseRetry_ = 0.0f;
		} else if (releaseRetry_ <= 0.0f) {
			capture::DropReleases(formID_);
			logger::info("{:08X}: held release(s) still refused {:.1f}s after the {} dodge; dropped", formID_, kReleaseRetry,
				graph::ToString(layer_));
		}
	}

	// [CastOn] A trigger with the facts of the dodge playing (docs/CASTON_DESIGN.md).
	void DodgeAgent::TriggerCastOn(RE::Actor* actor, caston::Trigger trigger, RE::Actor* attacker)
	{
		const auto      combatTarget = actor->GetActorRuntimeData().currentCombatTarget.get();
		caston::Context ctx;
		ctx.dodger = actor;
		ctx.attacker = attacker;
		ctx.combatTarget = combatTarget.get();
		ctx.layer = layer_;
		ctx.clip = lastClip_;
		ctx.chainIndex = lastChainIndex_;
		ctx.fromStagger = lastFromStagger_;
		ctx.firstPerson = lastFirstPerson_;
		castOn_.OnTrigger(trigger, ctx, core_->Now());
	}

	// The clip's RDF_Motion / RDF_ClipLength / RDF_Speed ([Graph] bClipMotion): its distance and phases replace the ini's
	// (taken while the dodge is still in its startup); its length and its own fAnimSpeed set the playback speed again, so
	// the clip spans the new timeline.
	void DodgeAgent::ApplyClipMotion(RE::Actor* actor)
	{
		bool retime = false;
		graph::DrainClipMotion(actor, [&](int kind, std::string_view payload) {
			if (!Settings::Get().clipMotion || !core_->DodgeRunning()) {
				return;
			}
			if (kind == 1) {
				if (const auto length = clip_motion::ParseLength(payload)) {
					clipLength_ = *length;
					retime = true;
				} else {
					DevLog("{:08X}: RDF_ClipLength '{}' is not a length in seconds: ignored", formID_, payload);
				}
				return;
			}
			if (kind == 2) {
				if (const auto speed = clip_motion::ParseSpeed(payload)) {
					clipSpeed_ = *speed;
					retime = true;
				} else {
					DevLog("{:08X}: RDF_Speed '{}' is not a multiplier 0.1..5: ignored", formID_, payload);
				}
				return;
			}
			if (kind == 3) {
				const auto blends = clip_motion::ParseExitBlends(payload);
				if (!blends) {
					DevLog("{:08X}: RDF_ExitBlend '{}' is not <all> or <run>,<block>,<attack> seconds: ignored", formID_, payload);
				} else if (core_->OnClipExitBlends(*blends)) {
					DevLog("{:08X}: clip exit blends {:.2f}/{:.2f}/{:.2f}", formID_, (*blends)[0], (*blends)[1], (*blends)[2]);
				}
				return;
			}
			if (kind == 4) {
				if (const auto share = clip_motion::ParseArmStance(payload)) {
					// A stance only where there is one to keep (a drawn weapon...), as at the start.
					graph::SetArmStance(actor, stanceKept_ ? *share : 0.0f);
					DevLog("{:08X}: clip arm stance {:.2f}{}", formID_, *share, stanceKept_ ? "" : " (no stance to keep)");
				} else {
					DevLog("{:08X}: RDF_ArmStance '{}' is not a share 0..1: ignored", formID_, payload);
				}
				return;
			}
			if (kind == 5) {
				const auto steering = clip_motion::ParseSteering(payload);
				if (!steering) {
					DevLog("{:08X}: RDF_Steer '{}' is not <startup>,<active>,<recovery>[,<maxDeviation>]: ignored", formID_, payload);
				} else if (core_->OnClipSteering(*steering)) {
					DevLog("{:08X}: clip steering {:.0f}/{:.0f}/{:.0f} deg/s, max deviation {}", formID_, steering->rates[0],
						steering->rates[1], steering->rates[2], steering->maxDeviation ? std::format("{:.0f}", *steering->maxDeviation) : "ini");
				}
				return;
			}
			const auto motion = clip_motion::ParseMotion(payload);
			if (!motion) {
				DevLog("{:08X}: RDF_Motion '{}' is not <distance>[,<startup>,<active>,<recovery>]: ignored", formID_, payload);
			} else if (!core_->OnClipMotion(*motion)) {
				DevLog("{:08X}: RDF_Motion '{}' came after the startup (t={:.3f}): ignored", formID_, payload, core_->ActionTime());
			} else {
				retime = true;
			}
		});
		if (!retime) {
			return;
		}
		const float speed = core_->DodgeTiming().PlaybackSpeed(clipLength_, clipSpeed_);
		graph::SetPlaybackSpeed(actor, speed);
		const auto& t = core_->DodgeTiming();
		DevLog("{:08X}: clip motion at t={:.3f}: phases {:.3f}/{:.3f}/{:.3f}, clip {:.3f} s x {:.2f}, playback {:.3f}", formID_,
			core_->ActionTime(), t.startup, t.active, t.recovery, clipLength_, clipSpeed_, speed);
	}

	void DodgeAgent::DrainCastOnAnnotations(RE::Actor* actor)
	{
		graph::DrainCastOn(actor, [this](int kind, std::string_view tag) {
			DevLog("{:08X}: CastOn annotation {} '{}' at t={:.3f}", formID_, kind == 0 ? "Dodge" : kind == 1 ? "PerfectDodge" : "Clip", tag,
				core_->ActionTime());
			if (kind == 2) {
				castOn_.OnClipPlacesMoments();
			} else {
				castOn_.OnAnnotation(kind == 0 ? caston::Trigger::Dodge : caston::Trigger::PerfectDodge, tag);
			}
		});
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
		// A spell hand's "attack" is a cast and a bow's a draw: neither sets IsAttacking.
		const auto& casting = Settings::Get().casting.castingVariables;
		attacking = attacking || attackState == attack_state::kBowDraw || attackState == attack_state::kBowAttached ||
		            attackState == attack_state::kBowDrawn || std::any_of(casting.begin(), casting.end(), [&](const std::string& name) {
						bool value = false;
						return actor->GetGraphVariableBool(name, value) && value;
					});
		if (attacking) {  // ours, or one the framework started from a press after the dodge let go
			fromDodge_.started = true;
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
			ClearAttackFromDodge(actor);
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
		if (p.attempts == 1 && driver_) {
			driver_->PrepareBash(actor);
		}
		const bool replayed = p.attempts == 1 && capture::Replay(actor, p.decision.kind == DecisionKind::PowerAttack);
		const bool performed = replayed || (driver_ && driver_->PerformAction(actor, p.decision));
		DevLog("{:08X}: {} from the dodge, attempt #{} at {:.3f}s: {} {}", formID_, ToString(p.decision.kind), p.attempts, p.age,
			replayed ? "replayed" : "own action", performed ? "accepted" : "refused");
	}

	// RDF_AttackFromDodge lasts for the attack the dodge became: it ends with that attack, or when the next one starts (a
	// combo going on from it is made of ordinary attacks). Counted from the cancel: that attack's own start event (if its
	// framework sends one, whenever it arrives against IsAttacking) is the first one after it, the next attack's the second.
	void DodgeAgent::TrackAttackFromDodge(RE::Actor* actor)
	{
		if (!fromDodge_.active || !fromDodge_.started) {
			return;
		}
		bool attacking = false;
		actor->GetGraphVariableBool("IsAttacking"sv, attacking);
		if (!attacking || graph::ReadAttack(actor).starts > fromDodge_.starts + 1) {
			DevLog("{:08X}: the attack out of the dodge is over ({})", formID_, attacking ? "next attack" : "attack ended");
			ClearAttackFromDodge(actor);
		}
	}

	void DodgeAgent::ClearAttackFromDodge(RE::Actor* actor)
	{
		if (fromDodge_.active && actor) {
			graph::SetAttackFromDodge(actor, 0, 0);
		}
		fromDodge_ = {};
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
			const bool handingOver = core_->Exiting() && moveHeld_ && (!core_->CancelledIntoAction() || intoAttack_);
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
			graph::SendExit(actor, layer_);
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
		castOn_.OnDodgeEnd(actor);  // [CastOn] what still waits fires at the dodge's end
		// Only a dodge that ran its course into running carries its speed; an interrupted one does not.
		if (core_->Phase() != DodgePhase::Done || !moveHeld_ || (core_->CancelledIntoAction() && !intoAttack_)) {
			carry_ = 0.0f;
		}
		tailFrames_ = 30;  // follow the hand-over into locomotion in the dev trace
		graph::SetChainIndex(actor, 0);  // the chain is over, whatever ended it
		graph::SetLayer(actor, graph::Layer::Root);
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
