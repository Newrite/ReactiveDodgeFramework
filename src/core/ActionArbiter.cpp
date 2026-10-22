#include "ActionArbiter.h"

namespace rdf
{
	namespace
	{
		void NoteReject(Decision& d, RejectReason r, CommandKind c)
		{
			if (d.reject == RejectReason::None) {
				d.reject = r;
				d.rejectedCommand = c;
			}
		}

		// Oldest pending command of either attack kind.
		const Command* OldestAttack(const InputBuffer& buffer, double now)
		{
			const auto* light = buffer.Peek(CommandKind::Attack, now);
			const auto* power = buffer.Peek(CommandKind::PowerAttack, now);
			if (light && power) {
				return light->seq < power->seq ? light : power;
			}
			return light ? light : power;
		}
	}

	Decision ActionArbiter::Arbitrate(const ArbiterState& state, const InputBuffer& buffer, double now) const
	{
		if (state.dodgeActive && IsForcedInterruption(state.engine)) {
			Decision d;
			d.kind = DecisionKind::Interrupt;
			return d;
		}
		return state.dodgeActive ? ArbitrateRunning(state, buffer, now) : ArbitrateIdle(state, buffer, now);
	}

	bool DodgeCancelsAttack(const ActionPolicy& policy, const ArbiterState& s)
	{
		if (s.attackState == attack_state::kNone || s.cancelAnyTime) {
			return true;
		}
		if (s.attackClipCancel) {
			return *s.attackClipCancel;
		}
		return s.dodgeWindowOpen || (policy.cancelInRecovery && s.attackRecovery) || policy.DodgeAllowedFromAttackState(s.attackState);
	}

	Decision ActionArbiter::ArbitrateIdle(const ArbiterState& state, const InputBuffer& buffer, double now) const
	{
		Decision d;
		const auto* cmd = buffer.Peek(CommandKind::Dodge, now);
		if (!cmd) {
			d.reject = RejectReason::NoCommand;
			return d;
		}
		if (state.engine != EngineBlock::None) {
			NoteReject(d, RejectReason::EngineBlocked, cmd->kind);
		} else if (!state.staminaOk) {
			NoteReject(d, RejectReason::StaminaLow, cmd->kind);
		} else if (!DodgeCancelsAttack(policy_, state)) {
			NoteReject(d, RejectReason::AttackStateBlocked, cmd->kind);
		} else {
			d.kind = DecisionKind::StartDodge;
			d.seq = cmd->seq;
			d.dir = cmd->dir;
		}
		return d;
	}

	OpenWindows ActionArbiter::WindowsFor(const ArbiterState& state) const
	{
		if (state.exiting) {
			return { true, true, true, true };  // already handing over: nothing is left to protect
		}
		const float t = state.actionTime;
		OpenWindows open = state.annotated ? *state.annotated :
		                                     OpenWindows{ t >= policy_.chainFrom, t >= policy_.attackFrom, t >= policy_.blockFrom, t >= policy_.exitFrom };
		// If the dodge may already be left by running, an attack or a block may leave it too;
		// otherwise a clip that opens its exit first would swallow every buffered attack.
		open.attack = open.attack || open.exit;
		open.block = open.block || open.exit;
		return open;
	}

	Decision ActionArbiter::ArbitrateRunning(const ArbiterState& state, const InputBuffer& buffer, double now) const
	{
		Decision          d;
		const OpenWindows open = WindowsFor(state);

		if (const auto* dodge = buffer.Peek(CommandKind::Dodge, now)) {
			if (!open.chain) {
				NoteReject(d, RejectReason::TooEarly, dodge->kind);
			} else if (policy_.maxChain > 0 && state.chainCount >= policy_.maxChain) {
				NoteReject(d, RejectReason::ChainLimit, dodge->kind);
			} else if (!state.staminaOk) {
				NoteReject(d, RejectReason::StaminaLow, dodge->kind);
			} else {
				d.kind = DecisionKind::ChainDodge;
				d.seq = dodge->seq;
				d.dir = dodge->dir;
				return d;
			}
		}

		if (const auto* attack = OldestAttack(buffer, now)) {
			if (open.attack) {
				d.kind = attack->kind == CommandKind::PowerAttack ? DecisionKind::PowerAttack : DecisionKind::Attack;
				d.seq = attack->seq;
				return d;
			}
			NoteReject(d, RejectReason::TooEarly, attack->kind);
		}

		if (const auto* block = buffer.Peek(CommandKind::Block, now)) {
			if (open.block) {
				d.kind = DecisionKind::Block;
				d.seq = block->seq;
				return d;
			}
			NoteReject(d, RejectReason::TooEarly, block->kind);
		}

		if (state.moveHeld && !state.exiting && open.exit) {
			d.kind = DecisionKind::LocomotionExit;
		}
		return d;
	}

	const char* ToString(RejectReason r)
	{
		switch (r) {
		case RejectReason::None: return "none";
		case RejectReason::NoCommand: return "no-command";
		case RejectReason::EngineBlocked: return "engine-blocked";
		case RejectReason::StaminaLow: return "stamina-low";
		case RejectReason::ChainLimit: return "chain-limit";
		case RejectReason::AttackStateBlocked: return "attack-state";
		case RejectReason::TooEarly: return "too-early";
		}
		return "?";
	}

	const char* ToString(DecisionKind k)
	{
		switch (k) {
		case DecisionKind::None: return "none";
		case DecisionKind::Interrupt: return "interrupt";
		case DecisionKind::StartDodge: return "start-dodge";
		case DecisionKind::ChainDodge: return "chain-dodge";
		case DecisionKind::Attack: return "attack";
		case DecisionKind::PowerAttack: return "power-attack";
		case DecisionKind::Block: return "block";
		case DecisionKind::LocomotionExit: return "locomotion-exit";
		}
		return "?";
	}

	const char* ToString(EngineBlock b)
	{
		switch (b) {
		case EngineBlock::None: return "none";
		case EngineBlock::Dead: return "dead";
		case EngineBlock::Ragdoll: return "ragdoll";
		case EngineBlock::Staggered: return "staggered";
		case EngineBlock::KillMove: return "killmove";
		case EngineBlock::Mounted: return "mounted";
		case EngineBlock::Swimming: return "swimming";
		case EngineBlock::Sitting: return "sitting";
		case EngineBlock::FirstPerson: return "first-person";
		case EngineBlock::Jumping: return "jumping";
		}
		return "?";
	}

	const char* ToString(DodgePhase p)
	{
		switch (p) {
		case DodgePhase::Idle: return "idle";
		case DodgePhase::Startup: return "startup";
		case DodgePhase::Active: return "active";
		case DodgePhase::Recovery: return "recovery";
		case DodgePhase::Done: return "done";
		case DodgePhase::Interrupted: return "interrupted";
		}
		return "?";
	}
}
