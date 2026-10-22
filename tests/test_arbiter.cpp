#include "check.h"
#include "core/ActionArbiter.h"
#include "core/MotionProfile.h"
#include "core/Math.h"

using namespace rdf;

namespace
{
	ArbiterState Running(float t)
	{
		ArbiterState s;
		s.dodgeActive = true;
		s.actionTime = t;
		s.chainCount = 1;
		return s;
	}
}

TEST(arbiter_idle_start_and_reject_reasons)
{
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(0.2f, 0.0f);
	ArbiterState  s;
	CHECK(arb.Arbitrate(s, b, 0.0).reject == RejectReason::NoCommand);

	b.Push(CommandKind::Dodge, 0.0);
	CHECK(arb.Arbitrate(s, b, 0.0).kind == DecisionKind::StartDodge);

	s.engine = EngineBlock::Swimming;
	CHECK(arb.Arbitrate(s, b, 0.0).reject == RejectReason::EngineBlocked);
	s.engine = EngineBlock::None;

	s.staminaOk = false;
	CHECK(arb.Arbitrate(s, b, 0.0).reject == RejectReason::StaminaLow);
	s.staminaOk = true;

	s.attackState = attack_state::kSwing;
	CHECK(arb.Arbitrate(s, b, 0.0).reject == RejectReason::AttackStateBlocked);
	s.dodgeWindowOpen = true;  // animation event opens a dodge window mid-swing
	CHECK(arb.Arbitrate(s, b, 0.0).kind == DecisionKind::StartDodge);
	s.dodgeWindowOpen = false;
	s.attackState = attack_state::kFollowThrough;
	CHECK(arb.Arbitrate(s, b, 0.0).kind == DecisionKind::StartDodge);
}

TEST(attack_cancel_rules_in_order)
{
	ActionPolicy p;
	ArbiterState s;
	CHECK(DodgeCancelsAttack(p, s));  // no attack

	s.attackState = attack_state::kHit;  // MCO / BFCO attacks sit here: their clips have no vanilla windows
	CHECK(!DodgeCancelsAttack(p, s));
	s.attackRecovery = true;
	CHECK(DodgeCancelsAttack(p, s));
	p.cancelInRecovery = false;
	CHECK(!DodgeCancelsAttack(p, s));
	p.cancelInRecovery = true;

	// The attack clip's own windows replace recovery, dodge windows and attack states ...
	s.attackClipCancel = false;
	s.dodgeWindowOpen = true;
	CHECK(!DodgeCancelsAttack(p, s));
	s.attackState = attack_state::kFollowThrough;
	CHECK(!DodgeCancelsAttack(p, s));
	s.attackClipCancel = true;
	s.attackRecovery = false;
	s.dodgeWindowOpen = false;
	s.attackState = attack_state::kDraw;  // an early (feint) window
	CHECK(DodgeCancelsAttack(p, s));

	// ... and the anytime keyword beats everything.
	s.attackClipCancel = false;
	s.cancelAnyTime = true;
	CHECK(DodgeCancelsAttack(p, s));

	// Through the arbiter: refused, then allowed by the recovery.
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(0.2f, 0.0f);
	b.Push(CommandKind::Dodge, 0.0);
	ArbiterState  a;
	a.attackState = attack_state::kHit;
	CHECK(arb.Arbitrate(a, b, 0.0).reject == RejectReason::AttackStateBlocked);
	a.attackRecovery = true;
	CHECK(arb.Arbitrate(a, b, 0.0).kind == DecisionKind::StartDodge);
}

TEST(arbiter_ignores_attack_and_block_when_not_dodging)
{
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(0.2f, 0.0f);
	b.Push(CommandKind::Attack, 0.0);
	b.Push(CommandKind::Block, 0.0);
	CHECK(arb.Arbitrate(ArbiterState{}, b, 0.0).kind == DecisionKind::None);
}

TEST(arbiter_priority_dodge_attack_block_exit)
{
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(5.0f, 0.0f);
	auto          s = Running(0.5f);
	s.moveHeld = true;
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::LocomotionExit);
	b.Push(CommandKind::Block, 0.9);
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::Block);
	b.Push(CommandKind::Attack, 0.95);
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::Attack);
	b.Push(CommandKind::Dodge, 0.99);
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::ChainDodge);
}

TEST(arbiter_forced_interruption_beats_everything)
{
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(5.0f, 0.0f);
	b.Push(CommandKind::Dodge, 0.9);
	auto s = Running(0.5f);
	s.engine = EngineBlock::Ragdoll;
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::Interrupt);
	s.engine = EngineBlock::Swimming;  // not forcing
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::ChainDodge);
}

TEST(arbiter_windows_and_limits)
{
	ActionPolicy p;
	ActionArbiter arb{ p };
	InputBuffer  b(5.0f, 0.0f);
	b.Push(CommandKind::Dodge, 0.9);
	CHECK(arb.Arbitrate(Running(0.33f), b, 1.0).reject == RejectReason::TooEarly);
	CHECK(arb.Arbitrate(Running(0.34f), b, 1.0).kind == DecisionKind::ChainDodge);

	auto limited = Running(0.4f);
	limited.chainCount = 3;
	CHECK(arb.Arbitrate(limited, b, 1.0).reject == RejectReason::ChainLimit);

	p.maxChain = 0;  // unlimited
	ActionArbiter unlimited{ p };
	limited.chainCount = 50;
	CHECK(unlimited.Arbitrate(limited, b, 1.0).kind == DecisionKind::ChainDodge);

	auto poor = Running(0.4f);
	poor.staminaOk = false;
	CHECK(arb.Arbitrate(poor, b, 1.0).reject == RejectReason::StaminaLow);

	InputBuffer a(5.0f, 0.0f);
	a.Push(CommandKind::Attack, 0.9);
	CHECK(arb.Arbitrate(Running(0.29f), a, 1.0).reject == RejectReason::TooEarly);
	CHECK(arb.Arbitrate(Running(0.30f), a, 1.0).kind == DecisionKind::Attack);
}

TEST(arbiter_exit_only_once_and_only_after_threshold)
{
	const ActionPolicy policy{};
	ActionArbiter      arb{ policy };
	InputBuffer        b(5.0f, 0.0f);
	auto               s = Running(policy.exitFrom - 0.01f);
	s.moveHeld = true;
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::None);
	s.actionTime = policy.exitFrom;
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::LocomotionExit);
	s.exiting = true;
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::None);
}

TEST(profile_integrates_to_distance_for_any_carry_and_exit)
{
	for (float carry : { 0.0f, 150.0f, 300.0f, 450.0f }) {
		for (float exit : { 0.0f, 300.0f }) {
			MotionProfile p;
			Timing t;
			MotionConfig m;
			p.Plan(t, m, carry, exit);
			CHECK_NEAR(p.Distance(t.Total()), m.distance, 1e-2);
			CHECK_NEAR(p.Speed(0.0f), carry, 1e-3);
			CHECK_NEAR(p.Speed(t.Total()), exit, 1e-3);
		}
	}
}

TEST(profile_blend_out_is_continuous)
{
	MotionProfile p;
	p.Plan(Timing{}, MotionConfig{}, 0.0f, 300.0f);
	const float before = p.Speed(0.5f);
	const float d = p.Distance(0.5f);
	p.BlendOut(0.5f, 300.0f, 0.12f);
	CHECK_NEAR(p.Speed(0.5f), before, 1e-3);
	CHECK_NEAR(p.Distance(0.5f), d, 1e-3);
	CHECK_NEAR(p.Total(), 0.62, 1e-6);
	CHECK_NEAR(p.Speed(0.62f), 300.0, 1e-3);
}

TEST(world_to_local_matches_skyrim_axes)
{
	// facing east (90 deg): world north is on the actor's left, world east is forward
	const auto north = WorldToLocal({ 0, 1 }, 90.0f * kDegToRad);
	CHECK_NEAR(north.x, -1.0, 1e-5);
	CHECK_NEAR(north.y, 0.0, 1e-5);
	const auto east = WorldToLocal({ 1, 0 }, 90.0f * kDegToRad);
	CHECK_NEAR(east.x, 0.0, 1e-5);
	CHECK_NEAR(east.y, 1.0, 1e-5);
}

TEST(playback_speed_fits_clip_to_timeline)
{
	Timing t;  // 0.70 s total
	CHECK_NEAR(t.PlaybackSpeed(0.667f, 1.0f), 0.667 / 0.70, 1e-6);
	CHECK_NEAR(t.PlaybackSpeed(0.667f, 1.5f), 1.5 * 0.667 / 0.70, 1e-6);
	CHECK_NEAR(t.PlaybackSpeed(t.Total(), 1.0f), 1.0, 1e-6);
}

TEST(arbiter_annotated_windows_replace_timers)
{
	// A clip that announced its windows decides by events alone: late in time but not yet
	// announced stays closed, early in time but announced is open.
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(5.0f, 0.0f);
	b.Push(CommandKind::Dodge, 0.9);

	auto s = Running(0.9f);
	s.annotated = OpenWindows{};
	CHECK(arb.Arbitrate(s, b, 1.0).reject == RejectReason::TooEarly);

	s.actionTime = 0.05f;
	s.annotated->chain = true;
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::ChainDodge);
}

TEST(arbiter_attack_allowed_once_exit_window_or_handover_open)
{
	ActionArbiter arb{ ActionPolicy{} };
	InputBuffer   b(5.0f, 0.0f);
	b.Push(CommandKind::Attack, 0.9);

	auto s = Running(0.2f);
	s.annotated = OpenWindows{};
	s.annotated->exit = true;  // the clip opened its exit before its attack window
	CHECK(arb.Arbitrate(s, b, 1.0).kind == DecisionKind::Attack);

	auto h = Running(0.05f);
	h.annotated = OpenWindows{};
	h.exiting = true;  // hand-over into running already under way
	CHECK(arb.Arbitrate(h, b, 1.0).kind == DecisionKind::Attack);
}
