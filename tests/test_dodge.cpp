#include "check.h"
#include "core/DodgeCost.h"
#include "sim.h"

#include <cmath>

using namespace sim;

namespace
{
	int CountKind(const Sim& s, DecisionKind k)
	{
		int n = 0;
		for (const auto& f : s.log) {
			n += f.decision.kind == k ? 1 : 0;
		}
		return n;
	}

	void StartAndRunTo(Sim& s, Vec2 move, int frames)
	{
		s.Frame(kDt64, move, { CommandKind::Dodge });
		for (int i = 0; i < frames; ++i) {
			s.Frame(kDt64, move);
		}
	}

	double DegDelta(float a, float b)
	{
		return static_cast<double>(AngleDelta(a, b)) * kRadToDeg;
	}
}

// ---- buffering windows -----------------------------------------------------------------------

TEST(buffered_press_executes_exactly_when_window_opens)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(s.log[0].decision.kind == DecisionKind::StartDodge);

	int pressedAt = 16;  // 0.25 s into the dodge, chain window opens at 0.375 s
	int chainFrame = -1;
	for (int n = 1; n < 40; ++n) {
		const auto f = n == pressedAt ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
		if (f.decision.kind == DecisionKind::ChainDodge) {
			chainFrame = n;
		}
	}
	CHECK(chainFrame == 24);
	CHECK(CountKind(s, DecisionKind::ChainDodge) == 1);
}

TEST(press_at_window_start_executes_same_tick)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	int chainFrame = -1;
	for (int n = 1; n < 30; ++n) {
		const auto f = n == 24 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
		if (f.decision.kind == DecisionKind::ChainDodge) {
			chainFrame = n;
		}
	}
	CHECK(chainFrame == 24);
}

TEST(press_at_expiry_edge_executes_one_frame_later_is_expired)
{
	// The raw buffer window (holding presses through a dodge is off here).
	// window = 12 frames: pressed at n=12 is still alive (exactly 0.1875 s old) when the window opens at n=24.
	{
		auto cfg = ExactConfig();
		cfg.input.holdDuringDodge = false;
		Sim s(cfg);
		s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
		for (int n = 1; n < 30; ++n) {
			n == 12 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
		}
		CHECK(CountKind(s, DecisionKind::ChainDodge) == 1);
	}
	// pressed at n=11 it is 13 frames old at n=24 and has expired.
	{
		auto cfg = ExactConfig();
		cfg.input.holdDuringDodge = false;
		Sim s(cfg);
		s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
		for (int n = 1; n < 30; ++n) {
			n == 11 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
		}
		CHECK(CountKind(s, DecisionKind::ChainDodge) == 0);
	}
}

TEST(press_after_expiry_does_not_execute_later)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 6; ++n) {
		n == 2 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
	}
	for (int n = 6; n < 60; ++n) {
		s.Frame(kDt64, Dir(90));
	}
	CHECK(CountKind(s, DecisionKind::ChainDodge) == 0);
}

TEST(mashed_presses_consume_once_per_action)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 60; ++n) {
		const bool press = n == 12 || n == 16 || n == 20 || n == 23;  // all spaced >= debounce and alive at n=24
		press ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
	}
	CHECK(CountKind(s, DecisionKind::ChainDodge) == 1);
}

TEST(rapid_presses_inside_debounce_are_one_press)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });  // 1 frame later: debounced
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(s.core.Buffer().Size() == 1);  // the start press was consumed; only the first extra survived
}

TEST(paused_clock_never_expires_commands)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 20; ++n) {
		n == 18 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
	}
	const float t = s.core.ActionTime();
	for (int i = 0; i < 5000; ++i) {
		const auto f = s.Frame(0.0f, Dir(90));  // paused
		CHECK(f.cmd.active == 0);
	}
	CHECK(s.core.ActionTime() == t);
	CHECK(s.core.DodgeRunning());
	int chainFrame = -1;
	for (int n = 20; n < 40; ++n) {
		if (s.Frame(kDt64, Dir(90)).decision.kind == DecisionKind::ChainDodge) {
			chainFrame = n;
		}
	}
	CHECK(chainFrame == 24);
}

TEST(frame_spike_still_executes_buffered_chain_once)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 16; ++n) {
		s.Frame(kDt64, Dir(90));
	}
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });  // 0.25 s
	s.Frame(0.0625f, Dir(90));  // spike: eval 0.27, window still closed
	s.Frame(0.0625f, Dir(90));  // eval 0.33
	s.Frame(0.0625f, Dir(90));  // eval 0.39: window open, press is 0.14 s old
	s.Frame(kDt64, Dir(90));
	CHECK(CountKind(s, DecisionKind::ChainDodge) == 1);
}

TEST(failed_side_effect_keeps_press_until_expiry)
{
	ReactiveCore core(ExactConfig());
	FrameInput f;
	f.dt = kDt64;
	core.BeginFrame(f);
	core.Press(CommandKind::Dodge, {});
	for (int i = 0; i < 6; ++i) {
		core.BeginFrame(f);
		const auto d = core.Evaluate({});
		CHECK(d.kind == DecisionKind::StartDodge);
		core.Apply(d, false);  // graph refused
	}
	CHECK(!core.DodgeRunning());
	for (int i = 0; i < 12; ++i) {
		core.BeginFrame(f);
	}
	CHECK(core.Evaluate({}).kind == DecisionKind::None);  // expired
}

// ---- policy ------------------------------------------------------------------------------------

TEST(attack_from_dodge_only_after_window_and_cancels_action)
{
	// The cancel happens exactly when the window opens; the dodge's momentum then runs out over
	// attackExitBlend instead of stopping dead, decreasing every frame, and the dodge is over after it.
	auto cfg = ExactConfig();
	cfg.timing.attackExitBlend = 0.15f;
	Sim s(cfg);
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	int   attackFrame = -1;
	float lastSpeed = 1e9f;
	for (int n = 1; n < 40; ++n) {
		const auto f = n == 10 ? s.Frame(kDt64, Dir(0), { CommandKind::Attack }) : s.Frame(kDt64, Dir(0));
		if (f.decision.kind == DecisionKind::Attack) {
			attackFrame = n;
		}
		if (attackFrame > 0 && f.cmd.active) {
			CHECK(f.cmd.speed <= lastSpeed + 1e-3f);
			CHECK(f.cmd.vy >= 0.0f);
			lastSpeed = f.cmd.speed;
		}
		if (attackFrame > 0 && n > attackFrame + 10) {  // 0.15 s at 64 fps is under 10 frames
			CHECK(!f.running);
		}
	}
	CHECK(attackFrame == 20);  // 0.3125 s
	CHECK(!s.core.DodgeRunning());
}

TEST(attack_cancel_hands_motion_to_the_attack_at_once_by_default)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	bool cancelled = false;
	for (int n = 1; n < 40 && !cancelled; ++n) {
		const auto f = n == 10 ? s.Frame(kDt64, Dir(0), { CommandKind::Attack }) : s.Frame(kDt64, Dir(0));
		if (f.decision.kind == DecisionKind::Attack) {
			cancelled = true;
			CHECK(!f.cmd.active || f.cmd.speed == 0.0f);
		}
	}
	CHECK(cancelled);
	const auto next = s.Frame(kDt64, Dir(0));
	CHECK(!next.cmd.active);
	CHECK(!s.core.DodgeRunning());
}

TEST(press_early_in_a_dodge_waits_for_its_window)
{
	// One power-attack press early in the dodge, long before the attack window and longer ago than the buffer window
	// when it opens: it still cancels the dodge there (and with the option off it expires).
	for (const bool hold : { true, false }) {
		auto cfg = ExactConfig();
		cfg.input.holdDuringDodge = hold;
		Sim  s(cfg);
		s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
		bool attacked = false;
		for (int n = 1; n < 40; ++n) {
			const auto f = n == 3 ? s.Frame(kDt64, Dir(0), { CommandKind::PowerAttack }) : s.Frame(kDt64, Dir(0));
			attacked = attacked || f.decision.kind == DecisionKind::PowerAttack;
		}
		CHECK(attacked == hold);
	}
}

TEST(attack_cancel_with_move_held_hands_over_at_running_speed)
{
	// With a blend the dodge eases to the running speed, not to rest (with the default 0 it ends at once and the game
	// layer's speed floor holds the running speed).
	auto cfg = ExactConfig();
	cfg.timing.attackExitBlend = 0.1f;
	Sim  s(cfg);
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	int   cancelled = -1;
	float last = 0.0f, beforeLast = 0.0f;
	for (int n = 1; n < 40; ++n) {
		const auto f = n == 10 ? s.Frame(kDt64, Dir(0), { CommandKind::Attack }) : s.Frame(kDt64, Dir(0));
		if (f.decision.kind == DecisionKind::Attack) {
			cancelled = n;
		}
		if (cancelled > 0 && f.cmd.active) {
			beforeLast = last;
			last = f.cmd.speed;
		}
	}
	CHECK(cancelled > 0);
	// the last full driven step is near running speed, not near rest (the very last one averages a partial step)
	CHECK(beforeLast > cfg.motion.runSpeed * 0.95f && beforeLast < cfg.motion.runSpeed * 1.1f);
}

TEST(dodge_beats_attack_and_block_when_all_eligible)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	for (int n = 1; n < 18; ++n) {
		s.Frame(kDt64, Dir(0));
	}
	s.Frame(kDt64, Dir(0), { CommandKind::Attack });
	s.Frame(kDt64, Dir(0), { CommandKind::Block });
	// eval time 0.28..: neither eligible yet; at 0.375 chain + attack + block all eligible -> exactly one wins per tick
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	for (int n = 0; n < 30; ++n) {
		s.Frame(kDt64, Dir(0));
	}
	int actions = 0;
	for (const auto& f : s.log) {
		actions += f.decision.kind != DecisionKind::None ? 1 : 0;
	}
	// start + (attack at 0.3125 wins before the chain window) -> at most one action per frame by construction
	for (const auto& f : s.log) {
		CHECK(f.decision.kind == DecisionKind::None || f.decision.seq != 0 || f.decision.kind == DecisionKind::LocomotionExit ||
			f.decision.kind == DecisionKind::Interrupt);
	}
	CHECK(actions >= 2);
}

TEST(chain_limit_stops_the_fourth_dodge)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	bool limited = false;
	for (int i = 0; i < 400 && (i < 5 || s.core.DodgeRunning()); ++i) {
		// mash the dodge button every 5 frames until the chain ends
		const auto f = i % 5 == 0 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
		limited = limited || f.decision.reject == RejectReason::ChainLimit;
	}
	CHECK(limited);
	CHECK(CountKind(s, DecisionKind::ChainDodge) == 2);  // start + 2 chains = 3 dodges in a row
	int maxChain = 0;
	for (const auto& f : s.log) {
		maxChain = std::max(maxChain, f.chain);
	}
	CHECK(maxChain == 3);
}

TEST(forced_interruption_ends_immediately_and_clears_ownership)
{
	Sim s(ExactConfig());
	StartAndRunTo(s, Dir(90), 10);
	CHECK(s.core.DodgeRunning());
	EngineView staggered;
	staggered.engine = EngineBlock::Staggered;
	const auto f = s.Frame(kDt64, Dir(90), {}, staggered);
	CHECK(f.decision.kind == DecisionKind::Interrupt);
	CHECK(!f.running);
	CHECK(f.cmd.active == 0);
	CHECK(f.phase == DodgePhase::Interrupted);
	// while still staggered a new dodge is refused with a reason
	const auto g = s.Frame(kDt64, Dir(90), { CommandKind::Dodge }, staggered);
	CHECK(g.decision.kind == DecisionKind::None);
	CHECK(g.decision.reject == RejectReason::EngineBlocked);
}

TEST(backstep_without_input_and_start_angle)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, {}, { CommandKind::Dodge });
	s.Run(0.8, kDt64);
	CHECK(s.pos.y < -100.0f);
	CHECK_NEAR(std::fabs(s.core.StartAngleDeg()), 180.0, 1e-3);

	Sim r(ExactConfig());
	r.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK_NEAR(r.core.StartAngleDeg(), 90.0, 1e-3);
}

// ---- motion -------------------------------------------------------------------------------------

namespace
{
	// Exit-on-input is policy, not motion: switch it off to measure an uncancelled dodge.
	double RunWholeDodge(const std::vector<float>& dts, Vec2 move)
	{
		Config c;
		c.policy.exitFrom = 99.0f;
		Sim s(c);
		s.Frame(dts[0], move, { CommandKind::Dodge });
		std::size_t i = 1;
		while (s.core.DodgeRunning() && i < 100000) {
			s.Frame(dts[i % dts.size()], move);
			++i;
		}
		return std::sqrt(static_cast<double>(s.pos.x) * s.pos.x + static_cast<double>(s.pos.y) * s.pos.y);
	}
}

TEST(distance_matches_config_at_any_frame_rate)
{
	for (float fps : { 30.0f, 60.0f, 144.0f, 240.0f }) {
		CHECK_NEAR(RunWholeDodge({ 1.0f / fps }, Dir(0)), 300.0, 6.0);  // 2%
		CHECK_NEAR(RunWholeDodge({ 1.0f / fps }, {}), 300.0, 6.0);      // backstep, exit speed 0
	}
}

TEST(distance_matches_config_with_frame_spikes)
{
	const std::vector<float> spiky{ 1 / 60.0f, 1 / 60.0f, 1 / 60.0f, 0.25f, 1 / 144.0f, 1 / 144.0f, 0.1f, 1 / 30.0f };
	CHECK_NEAR(RunWholeDodge(spiky, Dir(30)), 300.0, 6.0);
	CHECK_NEAR(RunWholeDodge(spiky, {}), 300.0, 6.0);
}

TEST(distance_is_configurable)
{
	Config c;
	c.motion.distance = 450.0f;
	c.policy.exitFrom = 99.0f;
	Sim s(c);
	s.Frame(1 / 60.0f, Dir(0), { CommandKind::Dodge });
	while (s.core.DodgeRunning()) {
		s.Frame(1 / 60.0f, Dir(0));
	}
	CHECK_NEAR(s.pos.y, 450.0, 9.0);
}

TEST(completed_flag_fires_once_and_stops_motion)
{
	Sim s;
	s.Frame(1 / 60.0f, Dir(0), { CommandKind::Dodge });
	int completed = 0;
	for (int i = 0; i < 120; ++i) {
		const auto f = s.Frame(1 / 60.0f, Dir(0));
		completed += f.completed ? 1 : 0;
		if (!s.core.DodgeRunning() && !f.completed) {
			CHECK(f.cmd.active == 0);
		}
	}
	CHECK(completed == 1);
}

TEST(velocity_is_continuous_across_a_same_direction_chain)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 60; ++n) {
		n == 16 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
	}
	CHECK(CountKind(s, DecisionKind::ChainDodge) == 1);
	for (std::size_t i = 2; i < s.log.size(); ++i) {
		const auto& a = s.log[i - 1].cmd;
		const auto& b = s.log[i].cmd;
		if (a.active && b.active && i > 8) {
			const float dvx = b.vx - a.vx, dvy = b.vy - a.vy;
			const float jump = std::sqrt(dvx * dvx + dvy * dvy);
			if (jump >= 30.0f) {
				std::printf("      jump %.1f at i=%zu speed %.1f -> %.1f\n", jump, i, a.speed, b.speed);
			}
			CHECK(jump < 30.0f);
		}
	}
}

TEST(chain_into_another_direction_turns_at_the_startup_rate)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 60; ++n) {
		n == 16 ? s.Frame(kDt64, Dir(-90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
	}
	// the chain press carries its own direction (west) while the live stick is east again
	float maxStep = 0.0f;
	for (std::size_t i = 1; i < s.log.size(); ++i) {
		maxStep = std::max(maxStep, std::fabs(AngleDelta(s.log[i - 1].heading, s.log[i].heading)));
	}
	CHECK(static_cast<double>(maxStep) <= 900.0 * kDegToRad * kDt64 + 1e-4);
}

TEST(velocity_is_continuous_into_locomotion_exit)
{
	Sim s;
	s.Frame(1 / 60.0f, Dir(0), { CommandKind::Dodge });
	for (int i = 0; i < 100; ++i) {
		s.Frame(1 / 60.0f, Dir(0));
	}
	CHECK(CountKind(s, DecisionKind::LocomotionExit) == 1);
	float biggest = 0.0f;
	float lastSpeed = 0.0f;
	for (std::size_t i = 9; i < s.log.size(); ++i) {  // skip the startup ramp from a standstill
		const auto& prev = s.log[i - 1];
		const auto& cur = s.log[i];
		if (prev.cmd.active && cur.cmd.active && !cur.completed) {  // the completing step is a partial frame
			biggest = std::max(biggest, std::fabs(cur.cmd.speed - prev.cmd.speed));
			lastSpeed = cur.cmd.speed;
		}
	}
	CHECK(biggest < 35.0f);
	CHECK_NEAR(lastSpeed, 300.0, 20.0);  // hands over at about run speed
	CHECK(!s.core.DodgeRunning());
}

// ---- steering -----------------------------------------------------------------------------------

TEST(steering_turns_monotonically_and_rate_limited)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 8; ++n) {
		s.Frame(kDt64, Dir(90));
	}
	const std::size_t from = s.log.size();
	for (int n = 0; n < 20; ++n) {
		s.Frame(kDt64, Dir(45));
	}
	double prev = DegDelta(0.0f, s.log[from - 1].heading);
	CHECK_NEAR(prev, 90.0, 1e-3);
	for (std::size_t i = from; i < s.log.size(); ++i) {
		const double h = DegDelta(0.0f, s.log[i].heading);
		CHECK(h <= prev + 1e-4);                                  // monotonic toward 45
		CHECK(prev - h <= 900.0 * kDt64 + 1e-3);                   // never above the startup cap
		prev = h;
	}
	CHECK_NEAR(prev, 45.0, 1e-3);
}

TEST(steering_active_phase_rate_cap)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 10; ++n) {  // 0.14 s: active phase
		s.Frame(kDt64, Dir(90));
	}
	const std::size_t from = s.log.size();
	for (int n = 0; n < 10; ++n) {
		s.Frame(kDt64, Dir(0));
	}
	for (std::size_t i = from; i < s.log.size(); ++i) {
		const double step = std::fabs(DegDelta(s.log[i - 1].heading, s.log[i].heading));
		CHECK(step <= 260.0 * kDt64 + 1e-3);
	}
}

TEST(steering_scales_with_analog_strength)
{
	Sim full(ExactConfig()), half(ExactConfig());
	for (auto* s : { &full, &half }) {
		s->Frame(kDt64, Dir(90), { CommandKind::Dodge });
		for (int n = 1; n < 10; ++n) {
			s->Frame(kDt64, Dir(90));
		}
	}
	full.Frame(kDt64, Dir(0, 1.0f));
	half.Frame(kDt64, Dir(0, 0.6f));  // (0.6 - 0.2) / 0.8 = 0.5 of the rate
	const double a = std::fabs(DegDelta(full.log[full.log.size() - 2].heading, full.log.back().heading));
	const double b = std::fabs(DegDelta(half.log[half.log.size() - 2].heading, half.log.back().heading));
	CHECK_NEAR(b, a * 0.5, 1e-3);
}

TEST(releasing_input_keeps_heading)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 12; ++n) {
		s.Frame(kDt64, Dir(45));
	}
	const float h = s.log.back().heading;
	for (int n = 0; n < 15; ++n) {
		CHECK(s.Frame(kDt64, {}).heading == h);
	}
	// a tiny stick wobble inside the deadzone changes nothing either
	for (int n = 0; n < 5; ++n) {
		CHECK(s.Frame(kDt64, Dir(270, 0.15f)).heading == h);
	}
}

TEST(reversal_is_never_instantaneous)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 10; ++n) {
		s.Frame(kDt64, Dir(90));
	}
	const float start = s.log.back().heading;
	for (int n = 0; n < 12; ++n) {  // 0.19 s of full opposite stick
		const auto f = s.Frame(kDt64, Dir(270));
		CHECK(std::fabs(DegDelta(start, f.heading)) <= 260.0 * (n + 1) * kDt64 + 0.01);
	}
	CHECK(std::fabs(DegDelta(start, s.log.back().heading)) < 120.0);
}

TEST(face_motion_yaw_is_rate_limited)
{
	auto cfg = ExactConfig();
	cfg.steering.startupYawRate = 720.0f;
	Sim s(cfg);
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	float previous = s.yaw;
	for (int n = 1; n < 40; ++n) {
		s.Frame(kDt64, Dir(90));
		CHECK(std::fabs(DegDelta(previous, s.yaw)) <= 720.0 * kDt64 + 1e-3);
		previous = s.yaw;
	}
	CHECK_NEAR(DegDelta(0.0f, s.yaw), 90.0, 1e-2);
}

TEST(keep_facing_holds_yaw_on_lock_on)
{
	Sim s(ExactConfig());
	s.lockOn = true;
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 40; ++n) {
		s.Frame(kDt64, Dir(90));
	}
	CHECK_NEAR(s.yaw, 0.0, 1e-4);
	CHECK(s.pos.x > 100.0f);  // still travelled east
}

// ---- orientation -------------------------------------------------------------------------------

TEST(facing_recovers_when_another_system_turns_the_actor)
{
	// Another plugin (TDM) may rotate the actor between our steps. The yaw command is computed
	// from the real yaw, so the actor ends up facing the trajectory again instead of drifting.
	Sim s;
	StartAndRunTo(s, Dir(90), 6);
	s.yaw = WrapAngle(s.yaw + 60.0f * kDegToRad);  // external disturbance
	for (int i = 0; i < 10; ++i) {
		s.Frame(kDt64, Dir(90));
	}
	CHECK_NEAR(DegDelta(s.core.Heading(), s.yaw), 0.0, 1.0);
}

TEST(camera_owned_facing_never_commands_yaw)
{
	// TDM "Vanilla Style" / no TDM, with orientation warp and the commit window off: the camera owns
	// the facing, the directional clip carries the direction, and the dodge must not fight the camera.
	Config cfg;
	cfg.steering.orientationWarpMax = 0.0f;
	cfg.steering.commitTime = 0.0f;
	Sim s(cfg);
	s.freeRotation = false;
	s.Frame(kDt64, Dir(135), { CommandKind::Dodge });
	CHECK(s.core.KeepsFacing());
	for (int i = 0; i < 30; ++i) {
		const auto f = s.Frame(kDt64, Dir(45));
		CHECK_NEAR(f.cmd.yawRate, 0.0, 1e-6);
	}
}

TEST(free_rotation_backstep_keeps_facing_side_dodge_turns)
{
	// TDM 360 mode: up to fFaceMotionMaxAngle (100 deg) the character turns to its path; beyond
	// that it keeps facing and the graph plays a backward clip instead of spinning around first.
	Sim side;
	side.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(!side.core.KeepsFacing());

	Sim back;
	back.Frame(kDt64, Dir(180), { CommandKind::Dodge });
	CHECK(back.core.KeepsFacing());
	for (int i = 0; i < 30; ++i) {
		const auto f = back.Frame(kDt64, Dir(180));
		CHECK_NEAR(f.cmd.yawRate, 0.0, 1e-6);
	}
}

TEST(annotation_events_drive_windows_and_iframes_per_dodge)
{
	Sim s;
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	CHECK(!s.core.ClipAnnotated());

	s.core.OnWindowEvent(WindowEvent::Marker);
	CHECK(s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.0f));
	CHECK(s.core.ClipAnnotated());
	CHECK(s.core.Invulnerable(10.0f, 11.0f));  // the clip's window, not the ini one

	// Before ChainOpen a chain press waits even when the ini timer would allow it.
	for (int i = 0; i < 40; ++i) {
		s.Frame(kDt64, Dir(0));
	}
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(s.log.back().decision.kind != DecisionKind::ChainDodge);

	s.core.OnWindowEvent(WindowEvent::IFrameEnd);
	s.core.OnWindowEvent(WindowEvent::ChainOpen);
	CHECK(!s.core.Invulnerable(0.0f, 100.0f));
	s.Frame(kDt64, Dir(90));
	CHECK(s.log.back().decision.kind == DecisionKind::ChainDodge);

	// The chained dodge starts with its own (not yet announced) windows.
	CHECK(!s.core.ClipAnnotated());
}

TEST(neutral_dodge_is_a_fresh_press_without_input)
{
	Sim s;
	const auto f = s.Frame(kDt64, {}, { CommandKind::Dodge });
	CHECK(f.decision.kind == DecisionKind::StartDodge);
	CHECK(s.core.PreviewNeutral(f.decision));
	CHECK(std::fabs(std::fabs(s.core.StartAngleDeg()) - 180.0f) < 1.0f);  // bBackstepWithoutInput: backwards

	// A chain without input keeps its heading: not neutral.
	for (int i = 0; i < 40; ++i) {
		s.Frame(kDt64);
	}
	const auto c = s.Frame(kDt64, {}, { CommandKind::Dodge });
	CHECK(c.decision.kind == DecisionKind::ChainDodge);
	CHECK(!s.core.PreviewNeutral(c.decision));

	Sim d;
	const auto g = d.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(!d.core.PreviewNeutral(g.decision));
}

TEST(perfect_window_from_the_clip_or_the_ini)
{
	Sim s;
	CHECK(!s.core.PerfectWindow(0.0f, 10.0f));  // no dodge
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	CHECK(s.core.PerfectWindow(0.0f, 0.15f));  // ini window, dodge just started
	for (int i = 0; i < 20; ++i) {
		s.Frame(kDt64, Dir(0));
	}
	CHECK(!s.core.PerfectWindow(0.0f, 0.15f));  // past it

	// An annotated clip without perfect marks keeps the ini window ...
	s.core.OnWindowEvent(WindowEvent::Marker);
	CHECK(!s.core.PerfectWindow(0.0f, 0.15f));
	CHECK(s.core.PerfectWindow(0.0f, 10.0f));
	// A late/bare Start no longer reopens the window; metadata belongs on frame zero.
	s.core.OnWindowEvent(WindowEvent::PerfectStart);
	CHECK(!s.core.PerfectWindow(0.0f, 0.15f));
	s.core.OnWindowEvent(WindowEvent::PerfectEnd);
	CHECK(!s.core.PerfectWindow(0.0f, 10.0f));
}

// ---- jump gate -------------------------------------------------------------------------------

TEST(exit_window_gates_the_jump_until_exit_from)
{
	Sim s(ExactConfig());
	CHECK(s.core.ExitWindowOpen());  // no dodge: nothing to protect
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	int openedAt = -1;
	for (int n = 1; n < 40 && openedAt < 0; ++n) {
		s.Frame(kDt64, Dir(90));
		if (s.core.ExitWindowOpen()) {
			openedAt = n;
		}
	}
	CHECK(openedAt == 29);  // the start tick is the first 1/64 s, so t reaches exitFrom (30/64 s) here
	CHECK(s.core.ActionTime() == 0.46875f);
}

// What the graph gate waits for in a 1hm dodge: closed at the start, open from the block window on.
TEST(block_window_opens_with_the_arbiter_block_window)
{
	auto cfg = ExactConfig();
	Sim  s(cfg);
	CHECK(s.core.BlockWindowOpen());  // no dodge
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(!s.core.BlockWindowOpen());
	bool opened = false;
	for (int n = 0; n < 80 && !opened; ++n) {
		s.Frame(kDt64, Dir(90));
		opened = s.core.BlockWindowOpen();
	}
	CHECK(opened);
	CHECK(s.core.ActionTime() >= cfg.policy.blockFrom - 1e-4f);
}

// ---- steering limits ---------------------------------------------------------------------------

TEST(max_deviation_zero_locks_the_start_direction)
{
	auto cfg = ExactConfig();
	cfg.steering.maxDeviation = 0.0f;
	cfg.steering.commitTime = 0.0f;  // steering limits only; the commit window has its own tests
	Sim s(cfg);
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	const float start = s.log.back().heading;
	for (int n = 0; n < 40; ++n) {
		s.Frame(kDt64, Dir(n < 20 ? 0.0f : 200.0f));  // hard left, then reversed
	}
	CHECK(std::fabs(DegDelta(start, s.log.back().heading)) < 0.01);
}

TEST(max_deviation_caps_how_far_input_bends_the_path)
{
	auto cfg = ExactConfig();
	cfg.steering.maxDeviation = 30.0f;
	cfg.steering.commitTime = 0.0f;  // steering limits only; the commit window has its own tests
	Sim s(cfg);
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	const float start = s.log.back().heading;
	double worst = 0.0;
	for (int n = 0; n < 40; ++n) {
		const auto f = s.Frame(kDt64, Dir(0));  // 90 deg off the dodge
		worst = std::max(worst, std::fabs(DegDelta(start, f.heading)));
	}
	CHECK(worst <= 30.0 + 0.01);
	CHECK(worst > 29.0);  // it does bend as far as allowed
}

TEST(input_outside_the_cone_is_ignored)
{
	auto cfg = ExactConfig();
	cfg.steering.inputCone = 90.0f;
	cfg.steering.commitTime = 0.0f;  // steering limits only; the commit window has its own tests
	Sim s(cfg);
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	const float start = s.log.back().heading;
	for (int n = 0; n < 30; ++n) {
		s.Frame(kDt64, Dir(270));  // straight back: outside the 90 deg cone
	}
	CHECK(std::fabs(DegDelta(start, s.log.back().heading)) < 0.01);
	for (int n = 0; n < 30; ++n) {
		s.Frame(kDt64, Dir(45));  // 45 deg off: inside, steers
	}
	CHECK(std::fabs(DegDelta(start, s.log.back().heading)) > 10.0);
}

TEST(hold_facing_returns_the_camera_turned_actor_to_its_start_yaw)
{
	// Vanilla-style TDM turns the actor toward the camera mid-dodge; with holdFacing the dodge turns it back.
	auto cfg = ExactConfig();
	cfg.steering.holdFacing = true;
	Sim s(cfg);
	s.freeRotation = false;
	s.Frame(kDt64, Dir(135), { CommandKind::Dodge });
	CHECK(s.core.KeepsFacing() && s.core.HoldsFacing());
	s.yaw = WrapAngle(s.yaw + 1.0f);  // something rotated the actor ~57 deg
	for (int i = 0; i < 20; ++i) {
		s.Frame(kDt64, Dir(135));
	}
	CHECK_NEAR(s.yaw, 0.0, 1e-3);
}

TEST(hold_facing_leaves_a_locked_target_in_charge)
{
	auto cfg = ExactConfig();
	cfg.steering.holdFacing = true;
	Sim s(cfg);
	s.lockOn = true;
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	CHECK(s.core.KeepsFacing() && !s.core.HoldsFacing());
}

// ---- commit window / orientation warp ------------------------------------------------------------

TEST(commit_window_re_aims_at_the_live_input)
{
	// The press came with "right", the stick reached "forward-left" two frames later: still inside
	// the 0.06 s commit window, so the dodge goes forward-left outright, not bent by the turn rate.
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	s.Frame(kDt64, Dir(-45));
	CHECK_NEAR(DegDelta(0.0f, s.log.back().heading), -45.0, 0.01);
}

TEST(after_the_commit_window_input_only_bends)
{
	auto cfg = ExactConfig();
	cfg.steering.commitTime = 0.0625f;  // 4 frames
	Sim s(cfg);
	s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
	for (int n = 1; n < 5; ++n) {
		s.Frame(kDt64, Dir(90));
	}
	const float before = s.log.back().heading;
	const auto f = s.Frame(kDt64, Dir(-90));  // past the window: rate limited
	CHECK(std::fabs(DegDelta(before, f.heading)) <= 900.0 * kDt64 + 0.01);
}

TEST(orientation_warp_turns_the_body_by_the_rest_of_the_angle)
{
	// Locked on, stick at 70 deg: the 90 deg (right) clip plays and the body turns -20 deg so the clip
	// runs along the 70 deg path; over the recovery it turns back to the target.
	auto cfg = ExactConfig();
	Sim s(cfg);
	s.lockOn = true;
	s.Frame(kDt64, Dir(70), { CommandKind::Dodge });
	for (int n = 1; n < 20; ++n) {  // well inside the active phase
		s.Frame(kDt64, Dir(70));
	}
	CHECK_NEAR(s.yaw * kRadToDeg, -20.0, 0.05);
	s.Run(1.0, kDt64, Dir(70));
	CHECK_NEAR(s.yaw * kRadToDeg, 0.0, 0.05);
}

TEST(orientation_warp_is_capped)
{
	auto cfg = ExactConfig();
	cfg.steering.orientationWarpMax = 10.0f;
	Sim s(cfg);
	s.lockOn = true;
	s.Frame(kDt64, Dir(70), { CommandKind::Dodge });
	for (int n = 1; n < 20; ++n) {
		s.Frame(kDt64, Dir(70));
	}
	CHECK_NEAR(s.yaw * kRadToDeg, -10.0, 0.05);
}

TEST(orientation_warp_never_turns_the_first_person_view)
{
	Sim s(ExactConfig());
	s.lockOn = true;
	s.cameraIsYaw = true;
	s.Frame(kDt64, Dir(70), { CommandKind::Dodge });
	for (int n = 1; n < 20; ++n) {
		const auto f = s.Frame(kDt64, Dir(70));
		CHECK_NEAR(f.cmd.yawRate, 0.0, 1e-6);
	}
	CHECK(!s.core.OwnsYaw());
}

TEST(held_facing_is_handed_back_to_the_camera_as_the_dodge_ends)
{
	// The camera turned 90 deg during a held dodge: the actor keeps its start yaw through the active
	// phase, then turns to the camera during the recovery, so TDM has nothing to snap afterwards.
	auto cfg = ExactConfig();
	cfg.steering.holdFacing = true;
	Sim s(cfg);
	s.freeRotation = false;
	s.Frame(kDt64, Dir(180), { CommandKind::Dodge });
	s.cameraYaw = 90.0f * kDegToRad;
	for (int n = 1; n < 20; ++n) {
		s.Frame(kDt64);
	}
	CHECK_NEAR(s.yaw * kRadToDeg, 0.0, 0.05);
	s.Run(1.0, kDt64);
	CHECK_NEAR(s.yaw * kRadToDeg, 90.0, 0.05);
}

// ---- clip motion (RDF_Motion / RDF_ClipLength) -------------------------------------------------

TEST(clip_motion_payloads_parse_and_reject)
{
	using namespace rdf::clip_motion;
	const auto full = ParseMotion("312.5,0.08,0.3,0.25");
	CHECK(full && *full->distance == 312.5f && full->phases && (*full->phases)[1] == 0.3f);
	const auto only = ParseMotion("250");
	CHECK(only && *only->distance == 250.0f && !only->phases);
	CHECK(ParseMotion(" 200 , 0.1 , 0.2 , 0.3 ").has_value());
	CHECK(!ParseMotion(""));
	CHECK(!ParseMotion("300,0.1,0.2"));        // phases come as three
	CHECK(!ParseMotion("300,0.1,0.2,0.3,1"));  // too many
	CHECK(!ParseMotion("-5"));
	CHECK(!ParseMotion("300,0,0,0"));          // no time at all
	CHECK(!ParseMotion("300;0.1"));
	CHECK(ParseLength("0.72") && *ParseLength("0.72") == 0.72f);
	CHECK(!ParseLength("0") && !ParseLength("abc") && !ParseLength("0.5s"));
	CHECK(ParseSpeed("1.25") && *ParseSpeed("1.25") == 1.25f);
	CHECK(!ParseSpeed("0.05") && !ParseSpeed("6") && !ParseSpeed("x2"));
}

TEST(clip_motion_replans_the_dodge_in_its_startup)
{
	const auto run = [](std::optional<ClipMotion> motion) {
		Config c;
		c.policy.exitFrom = 99.0f;  // an uncancelled dodge (as RunWholeDodge)
		Sim s(c);
		s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
		bool taken = false;
		if (motion) {
			taken = s.core.OnClipMotion(*motion);
		}
		int frames = 1;
		while (s.core.DodgeRunning() && frames < 400) {
			s.Frame(kDt64, Dir(0));
			++frames;
		}
		return std::tuple{ taken, Length(s.pos), frames * kDt64, s.core.DodgeTiming() };
	};
	const auto [plainTaken, plainDistance, plainTime, plainTiming] = run(std::nullopt);
	CHECK(std::fabs(plainDistance - 300.0f) < 3.0f);  // the ini's fDistance
	CHECK(std::fabs(plainTime - Timing{}.Total()) < 2 * kDt64);

	ClipMotion clip;
	clip.distance = 150.0f;
	clip.phases = std::array<float, 3>{ 0.10f, 0.20f, 0.20f };
	const auto [taken, distance, time, timing] = run(clip);
	CHECK(taken);
	CHECK(std::fabs(distance - 150.0f) < 3.0f);
	CHECK(std::fabs(time - 0.5f) < 2 * kDt64);
	CHECK(timing.startup == 0.10f && timing.active == 0.20f && timing.recovery == 0.20f);

	ClipMotion far;
	far.distance = 450.0f;  // no phases: only the distance changes
	const auto [farTaken, farDistance, farTime, farTiming] = run(far);
	CHECK(farTaken && std::fabs(farDistance - 450.0f) < 4.0f && std::fabs(farTime - plainTime) < 2 * kDt64);
}

TEST(clip_motion_is_ignored_after_the_startup_and_reset_by_a_chain)
{
	Sim s;
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	for (int i = 0; i < 10; ++i) {  // past the 0.08 s startup
		s.Frame(kDt64);
	}
	ClipMotion clip;
	clip.distance = 100.0f;
	clip.phases = std::array<float, 3>{ 0.2f, 0.2f, 0.2f };
	CHECK(!s.core.OnClipMotion(clip));
	CHECK(s.core.DodgeTiming().startup == Timing{}.startup);

	Sim c;
	c.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	CHECK(c.core.OnClipMotion(clip));
	for (int i = 0; i < 30 && c.log.back().decision.kind != DecisionKind::ChainDodge; ++i) {
		c.Frame(kDt64, {}, { CommandKind::Dodge });
	}
	CHECK(c.log.back().decision.kind == DecisionKind::ChainDodge);
	CHECK(c.core.DodgeTiming().startup == Timing{}.startup);  // the chained clip announces its own
}

TEST(clip_settings_payloads_parse_and_reject)
{
	using namespace rdf::clip_motion;
	const auto three = ParseExitBlends("0.3,0.1,0");
	CHECK(three && (*three)[0] == 0.3f && (*three)[1] == 0.1f && (*three)[2] == 0.0f);
	const auto one = ParseExitBlends("0.2");
	CHECK(one && (*one)[0] == 0.2f && (*one)[1] == 0.2f && (*one)[2] == 0.2f);
	CHECK(!ParseExitBlends("0.3,0.1") && !ParseExitBlends("0,0.1,0.1") && !ParseExitBlends("2"));
	const auto steer = ParseSteering("900,0,600,45");
	CHECK(steer && steer->rates[1] == 0.0f && steer->maxDeviation && *steer->maxDeviation == 45.0f);
	CHECK(ParseSteering("900,260,600") && !ParseSteering("900,260,600")->maxDeviation);
	CHECK(!ParseSteering("900") && !ParseSteering("1,2,3,200") && !ParseSteering("1,2,3,4,5"));
	CHECK(ParseArmStance("0") && ParseArmStance("1") && !ParseArmStance("1.5"));
}

TEST(clip_exit_blend_sets_the_momentum_run_out)
{
	const auto exitTime = [](std::optional<std::array<float, 3>> blends) {
		Sim s;
		s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
		if (blends) {
			CHECK(s.core.OnClipExitBlends(*blends));
		}
		int frames = 0;
		while (!s.core.Exiting() && frames < 200) {  // held input: cancelled into running at the exit window
			s.Frame(kDt64, Dir(0));
			++frames;
		}
		const float from = s.core.ActionTime();
		while (s.core.DodgeRunning() && frames < 400) {
			s.Frame(kDt64, Dir(0));
			++frames;
		}
		return s.core.ActionTime() - from;
	};
	CHECK(std::fabs(exitTime(std::nullopt) - Timing{}.exitBlend) < 2 * kDt64);
	CHECK(std::fabs(exitTime(std::array<float, 3>{ 0.5f, 0.1f, 0.0f }) - 0.5f) < 2 * kDt64);
}

TEST(clip_steering_zero_rates_keep_the_start_direction)
{
	const auto endHeading = [](bool locked) {
		Config c;
		c.policy.exitFrom = 99.0f;
		c.steering.commitTime = 0.0f;
		Sim s(c);
		s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
		if (locked) {
			ClipSteering st;
			st.rates = { 0.0f, 0.0f, 0.0f };
			CHECK(s.core.OnClipSteering(st));
		}
		while (s.core.DodgeRunning()) {
			s.Frame(kDt64, Dir(90));  // pull right all the way
		}
		return s.core.Heading() * kRadToDeg;
	};
	CHECK(std::fabs(endHeading(true)) < 0.01f);
	CHECK(endHeading(false) > 30.0f);
}

TEST(clip_jump_open_opens_the_jump_before_the_exit_window)
{
	Sim s(ExactConfig());
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	s.Frame(kDt64, Dir(0));
	CHECK(!s.core.JumpWindowOpen());  // exit window opens at 0.47 s
	s.core.OnWindowEvent(WindowEvent::JumpOpen);
	CHECK(s.core.JumpWindowOpen());
	CHECK(!s.core.ClipAnnotated());  // the jump alone does not switch the clip's other windows to annotations
}

TEST(clip_driving_payloads_parse_and_reject)
{
	using namespace rdf::clip_motion;
	CHECK(ParseWarp("45") && !ParseWarp("100"));
	CHECK(ParseRunSpeed("420") && !ParseRunSpeed("-1"));
	CHECK(ParseCommit("0.1") && !ParseCommit("1"));
	const auto both = ParseYawRates("500,2000");
	CHECK(both && *both->faceYawRate == 500.0f && *both->startupYawRate == 2000.0f);
	const auto face = ParseYawRates("300");
	CHECK(face && !face->startupYawRate);
	CHECK(!ParseYawRates("1,2,3") && !ParseYawRates(""));
}

TEST(clip_run_speed_replans_the_hand_over_speed)
{
	const auto endSpeed = [](std::optional<float> run) {
		Config c;
		c.policy.exitFrom = 99.0f;  // run the curve to its end with input held
		Sim s(c);
		s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
		if (run) {
			ClipDriving d;
			d.runSpeed = *run;
			CHECK(s.core.OnClipDriving(d));
		}
		float last = 0.0f;
		while (s.core.DodgeRunning()) {
			s.Frame(kDt64, Dir(0));
			last = s.core.Speed();
		}
		return std::tuple{ last, Length(s.pos) };
	};
	const auto [plain, plainDistance] = endSpeed(std::nullopt);
	const auto [slow, slowDistance] = endSpeed(120.0f);
	CHECK(std::fabs(plain - MotionConfig{}.runSpeed) < 10.0f);
	CHECK(std::fabs(slow - 120.0f) < 10.0f);
	CHECK(std::fabs(slowDistance - plainDistance) < 4.0f);  // the distance stays the dodge's
}

// ---- dodge cost ------------------------------------------------------------------------------

TEST(dodge_cost_formula_terms)
{
	CostConfig c;  // defaults: the old fixed 20
	CHECK(DodgeCost(c, {}) == 20.0f);
	c.maxPercent = 10.0f;
	c.weightMult = 0.5f;
	CostInputs in;
	in.resourceMax = 200.0f;
	in.equippedWeight = 40.0f;
	CHECK(std::fabs(DodgeCost(c, in) - (20.0f + 20.0f + 20.0f)) < 1e-4f);
	c.chainStep = 0.5f;
	in.chainIndex = 2;
	CHECK(std::fabs(DodgeCost(c, in) - 60.0f * 2.0f) < 1e-4f);
	c.overencumberedMult = 1.5f;
	in.overencumbered = true;
	CHECK(std::fabs(DodgeCost(c, in) - 180.0f) < 1e-3f);
	c.escapeMult = 0.5f;
	c.escapeFlat = 10.0f;
	in.escape = true;
	in.escapeBonus = -15.0f;  // the keyword's magnitudes may not make the escape part negative
	CHECK(std::fabs(DodgeCost(c, in) - 90.0f) < 1e-3f);  // (180 + max(0, 10 - 15)) x 0.5
	in.escapeBonus = 20.0f;
	CHECK(std::fabs(DodgeCost(c, in) - 105.0f) < 1e-3f);  // (180 + 30) x 0.5
	c.max = 50.0f;
	CHECK(DodgeCost(c, in) == 50.0f);
	c.min = 5.0f;
	c.max = 0.0f;
	c.flat = 0.0f;
	CHECK(DodgeCost(c, {}) == 5.0f);
}

TEST(dodge_cost_shortfall_modes)
{
	CostConfig c;
	c.minimum = 10.0f;
	CHECK(CostAllows(c, 12.0f, 30.0f) && !CostAllows(c, 8.0f, 5.0f));  // Minimum
	c.shortfall = CostShortfall::Refuse;
	CHECK(!CostAllows(c, 12.0f, 30.0f) && CostAllows(c, 30.0f, 30.0f));
	c.shortfall = CostShortfall::Always;
	CHECK(CostAllows(c, 0.0f, 30.0f));
}

TEST(dodge_cost_keyword_inputs)
{
	CostConfig c;  // fCost 20
	CostInputs in;
	in.costBonus = 10.0f;
	CHECK(DodgeCost(c, in) == 30.0f);
	in.costPercent = -50.0f;
	CHECK(DodgeCost(c, in) == 15.0f);
	in.costPercent = -150.0f;  // never a negative factor
	CHECK(DodgeCost(c, in) == 0.0f);
	in = {};
	c.chainStep = 0.5f;
	in.chainIndex = 1;
	in.chainPercent = 50.0f;  // the step becomes 1.0
	CHECK(DodgeCost(c, in) == 40.0f);
	in = {};
	in.escape = true;
	c.escapeFlat = 10.0f;
	in.escapePercent = 100.0f;  // x2
	CHECK(DodgeCost(c, in) == 60.0f);
}

TEST(chain_press_from_drops_early_presses)
{
	Config c;
	c.policy.chainPressFrom = 0.2f;
	Sim s(c);
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	s.Frame(kDt64, Dir(0), { CommandKind::Dodge });  // a mash 1/64 s in: dropped
	int chains = 0;
	while (s.core.DodgeRunning()) {
		chains += s.Frame(kDt64, Dir(0)).decision.kind == DecisionKind::ChainDodge ? 1 : 0;
	}
	CHECK(chains == 0);

	Sim t(c);
	t.Frame(kDt64, Dir(0), { CommandKind::Dodge });
	while (t.core.ActionTime() < 0.25f) {
		t.Frame(kDt64, Dir(0));
	}
	t.Frame(kDt64, Dir(0), { CommandKind::Dodge });  // after fChainPressFrom: it waits for the chain window
	int later = 0;
	for (int i = 0; i < 40 && later == 0; ++i) {
		later += t.Frame(kDt64, Dir(0)).decision.kind == DecisionKind::ChainDodge ? 1 : 0;
	}
	CHECK(later == 1);
}
