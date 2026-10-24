#pragma once

#include "Config.h"
#include "DodgeAction.h"
#include "InputBuffer.h"
#include "Windows.h"

namespace rdf
{
	// Why the engine currently forbids starting a dodge. The first four also end a running one.
	enum class EngineBlock : std::uint8_t
	{
		None,
		Dead,
		Ragdoll,
		Staggered,
		KillMove,
		Mounted,
		Swimming,
		Sitting,
		FirstPerson,
		Jumping,
	};

	constexpr bool IsForcedInterruption(EngineBlock b)
	{
		return b == EngineBlock::Dead || b == EngineBlock::Ragdoll || b == EngineBlock::Staggered || b == EngineBlock::KillMove;
	}

	enum class RejectReason : std::uint8_t
	{
		None,
		NoCommand,
		EngineBlocked,
		StaminaLow,
		ChainLimit,
		AttackStateBlocked,
		TooEarly,
		Forbidden,  // a keyword on the actor forbids dodging
	};

	enum class DecisionKind : std::uint8_t
	{
		None,
		Interrupt,  // forced interruption: end the action now
		StartDodge,
		ChainDodge,
		Attack,
		PowerAttack,
		Block,
		LocomotionExit,
	};

	struct Decision
	{
		DecisionKind  kind{ DecisionKind::None };
		std::uint64_t seq{ 0 };  // buffered command that triggered it (0 for Interrupt / LocomotionExit)
		Vec2          dir{};
		RejectReason  reject{ RejectReason::None };  // best rejected candidate, for diagnostics
		CommandKind   rejectedCommand{ CommandKind::Dodge };
	};

	struct ArbiterState
	{
		bool          dodgeActive{ false };
		bool          exiting{ false };
		float         actionTime{ 0.0f };
		int           chainCount{ 0 };
		EngineBlock   engine{ EngineBlock::None };
		std::uint8_t  attackState{ 0 };
		bool          dodgeWindowOpen{ false };  // anim-event driven attack->dodge window
		bool          attackRecovery{ false };   // the attack reached its recovery (MCO_IsInRecovery, BFCO_DIY_recovery, ...)
		std::optional<bool> attackClipCancel;    // RDF_DodgeCancelOpen / Close of the attack's clip, once it announced one
		bool          cancelAnyTime{ false };    // the actor's keyword lets it cancel this attack at any moment
		bool          dodgeForbidden{ false };   // the actor's keyword forbids dodging (no start, no chain)
		int           chainBonus{ 0 };           // chained dodges the actor's keyword adds (or takes away)
		bool          staminaOk{ true };
		bool          moveHeld{ false };
		std::optional<OpenWindows> annotated;  // windows announced by the clip; empty = ini timers
	};

	// May a dodge start during the attack described by `s`? Strongest rule first: no attack; the anytime keyword; the
	// attack clip's own RDF_DodgeCancel windows (they replace everything below); a dodge window event; the attack
	// framework's recovery; the vanilla attack states of sAttackStates.
	bool DodgeCancelsAttack(const ActionPolicy& policy, const ArbiterState& s);

	// Picks at most one action per call. Priority: forced interruption > dodge/chain > attack >
	// block > locomotion exit. Pure: it never mutates the buffer; the caller consumes on success.
	class ActionArbiter
	{
	public:
		explicit ActionArbiter(const ActionPolicy& policy) :
			policy_(policy) {}

		Decision Arbitrate(const ArbiterState& state, const InputBuffer& buffer, double now) const;
		OpenWindows WindowsFor(const ArbiterState& state) const;

	private:
		Decision ArbitrateRunning(const ArbiterState& state, const InputBuffer& buffer, double now) const;
		Decision ArbitrateIdle(const ArbiterState& state, const InputBuffer& buffer, double now) const;

		ActionPolicy policy_;
	};

	const char* ToString(RejectReason r);
	const char* ToString(DecisionKind k);
	const char* ToString(EngineBlock b);
	const char* ToString(DodgePhase p);
}
