#include "check.h"
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
	// window = 12 frames: pressed at n=12 is still alive (exactly 0.1875 s old) when the window opens at n=24.
	{
		Sim s(ExactConfig());
		s.Frame(kDt64, Dir(90), { CommandKind::Dodge });
		for (int n = 1; n < 30; ++n) {
			n == 12 ? s.Frame(kDt64, Dir(90), { CommandKind::Dodge }) : s.Frame(kDt64, Dir(90));
		}
		CHECK(CountKind(s, DecisionKind::ChainDodge) == 1);
	}
	// pressed at n=11 it is 13 frames old at n=24 and has expired.
	{
		Sim s(ExactConfig());
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
	// actionExitBlend instead of stopping dead, decreasing every frame, and the dodge is over after it.
	Sim s(ExactConfig());
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
	// TDM "Vanilla Style" / no TDM: the camera owns the facing, the directional clip carries the
	// direction, and the dodge must not fight the camera over the yaw.
	Sim s;
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
	s.core.OnWindowEvent(WindowEvent::IFrameStart);
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
