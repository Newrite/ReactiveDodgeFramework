#pragma once

#include "Config.h"
#include "DodgeAction.h"
#include "InputBuffer.h"

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
		bool          staminaOk{ true };
		bool          moveHeld{ false };
	};

	// Picks at most one action per call. Priority: forced interruption > dodge/chain > attack >
	// block > locomotion exit. Pure: it never mutates the buffer; the caller consumes on success.
	class ActionArbiter
	{
	public:
		explicit ActionArbiter(const ActionPolicy& policy) :
			policy_(policy) {}

		Decision Arbitrate(const ArbiterState& state, const InputBuffer& buffer, double now) const;

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
