#include "check.h"
#include "sim.h"

using namespace rdf;
using namespace sim;

TEST(protection_uses_start_and_duration_and_ignores_cancel_annotations)
{
	Sim s;
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	for (auto event : { WindowEvent::Marker, WindowEvent::AttackOpen, WindowEvent::BlockOpen,
		WindowEvent::ChainOpen, WindowEvent::ExitOpen, WindowEvent::JumpOpen }) {
		s.core.OnWindowEvent(event);
	}
	s.Frame(0.125f);
	CHECK(s.core.Invulnerable(0.125f, 0.25f));
	CHECK(s.core.PerfectWindow(0.125f, 0.25f));
	s.Frame(0.1875f);
	CHECK(s.core.Invulnerable(0.125f, 0.25f)); // t=.3125, end=.375, not .25
	CHECK(s.core.PerfectWindow(0.125f, 0.25f));
	s.Frame(0.0625f);
	CHECK(!s.core.Invulnerable(0.125f, 0.25f));
	CHECK(!s.core.PerfectWindow(0.125f, 0.25f));
}

TEST(protection_modifiers_change_duration_never_start)
{
	Sim s;
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	s.Frame(0.125f);
	CHECK(s.core.Invulnerable(0.125f, 0.25f, -0.125f));
	CHECK(s.core.PerfectWindow(0.125f, 0.25f, -0.125f));
	CHECK(!s.core.Invulnerable(0.125f, 0.25f, -1.0f));
	CHECK(!s.core.PerfectWindow(0.125f, 0.25f, -1.0f));
	s.Frame(0.125f);
	CHECK(!s.core.Invulnerable(0.125f, 0.25f, -0.125f));
	CHECK(!s.core.PerfectWindow(0.125f, 0.25f, -0.125f));
	s.Frame(0.125f);
	CHECK(s.core.Invulnerable(0.125f, 0.25f, 0.125f));
	CHECK(s.core.PerfectWindow(0.125f, 0.25f, 0.125f));
}

TEST(protection_end_is_absolute_even_without_start_and_with_a_bonus)
{
	Sim s;
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	s.core.OnWindowEvent(WindowEvent::IFrameEnd);
	CHECK(!s.core.Invulnerable(0.0f, 0.25f, 10.0f));
	CHECK(s.core.PerfectWindow(0.0f, 0.25f)); // independent
	s.core.OnWindowEvent(WindowEvent::PerfectEnd);
	CHECK(!s.core.PerfectWindow(0.0f, 0.25f, 10.0f));
	s.Frame(0.125f);
	CHECK(!s.core.Invulnerable(0.25f, 0.25f, 10.0f));
	CHECK(!s.core.PerfectWindow(0.25f, 0.25f, 10.0f));
}

TEST(protection_never_outlives_the_dodge)
{
	Sim s;
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	CHECK(s.core.Invulnerable(0.0f, 0.25f, 10.0f));
	s.core.Apply(Decision{ DecisionKind::Interrupt }, true);
	CHECK(!s.core.Invulnerable(0.0f, 0.25f, 10.0f));
	CHECK(!s.core.PerfectWindow(0.0f, 0.25f, 10.0f));
	Sim finished;
	finished.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	finished.Run(2.0, 1.0f / 64.0f);
	CHECK(!finished.core.Invulnerable(0.0f, 0.25f, 10.0f));
	CHECK(!finished.core.PerfectWindow(0.0f, 0.25f, 10.0f));
}

TEST(protection_clip_metadata_changes_only_its_start_once)
{
	Sim s;
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	CHECK(s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.125f));
	CHECK(s.core.OnClipWindowStart(WindowEvent::PerfectStart, 0.25f));
	CHECK(!s.core.ClipAnnotated()); // no coupling to cancel windows
	CHECK(!s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.0f));
	CHECK(!s.core.Invulnerable(0.0f, 0.25f));
	CHECK(!s.core.PerfectWindow(0.0f, 0.25f));
	s.Frame(0.125f);
	CHECK(s.core.Invulnerable(0.0f, 0.25f, -0.125f));
	CHECK(!s.core.PerfectWindow(0.0f, 0.25f));
	s.Frame(0.125f);
	CHECK(!s.core.Invulnerable(0.0f, 0.25f, -0.125f));
	CHECK(s.core.PerfectWindow(0.0f, 0.25f));
	s.core.OnWindowEvent(WindowEvent::IFrameEnd);
	s.core.OnWindowEvent(WindowEvent::PerfectEnd);
	CHECK(!s.core.Invulnerable(0.0f, 0.25f, 10.0f));
	CHECK(!s.core.PerfectWindow(0.0f, 0.25f, 10.0f));
}

TEST(protection_end_cannot_be_reopened_and_chain_resets_both_windows)
{
	Sim s;
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	s.core.OnWindowEvent(WindowEvent::IFrameEnd);
	s.core.OnWindowEvent(WindowEvent::PerfectEnd);
	CHECK(!s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.0f));
	CHECK(!s.core.OnClipWindowStart(WindowEvent::PerfectStart, 0.0f));
	s.core.OnWindowEvent(WindowEvent::IFrameStart);
	s.core.OnWindowEvent(WindowEvent::PerfectStart);
	CHECK(!s.core.Invulnerable(0.0f, 1.0f));
	CHECK(!s.core.PerfectWindow(0.0f, 1.0f));
	s.core.Apply(Decision{ .kind = DecisionKind::ChainDodge, .dir = Dir(0) }, true);
	CHECK(s.core.Invulnerable(0.0f, 0.25f));
	CHECK(s.core.PerfectWindow(0.0f, 0.25f));
	CHECK(s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.125f));
	CHECK(!s.core.Invulnerable(0.0f, 0.25f));
}

TEST(protection_start_metadata_rejects_malformed_and_late_values)
{
	using rdf::clip_motion::ParseWindowStart;
	CHECK(ParseWindowStart("0.125") == 0.125f);
	CHECK(ParseWindowStart("0") == 0.0f);
	for (const auto text : { "", "nan", "inf", "-0.1", "0.1,0.2", "0.1garbage", "11" }) {
		CHECK(!ParseWindowStart(text));
	}
	Sim s;
	CHECK(!s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.0f));
	s.Frame(0.0f, Dir(0), { CommandKind::Dodge });
	CHECK(!s.core.OnClipWindowStart(WindowEvent::IFrameStart, -1.0f));
	CHECK(!s.core.OnClipWindowStart(WindowEvent::PerfectStart, -1.0f));
	s.Frame(0.25f);
	CHECK(!s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.0f));
	CHECK(!s.core.OnClipWindowStart(WindowEvent::PerfectStart, 0.0f));
}

TEST(frame_zero_motion_can_shorten_startup_without_losing_protection_metadata)
{
	Sim s;
	s.Frame(1.0f / 30.0f, Dir(0), { CommandKind::Dodge });
	ClipMotion motion;
	motion.phases = std::array{ 0.01f, 0.3f, 0.39f };
	CHECK(s.core.OnClipMotion(motion));
	CHECK(s.core.Phase() == DodgePhase::Active);
	CHECK(s.core.OnClipWindowStart(WindowEvent::IFrameStart, 0.125f));
	CHECK(!s.core.Invulnerable(0.0f, 0.25f));
	s.Frame(1.0f / 30.0f);
	CHECK(!s.core.OnClipWindowStart(WindowEvent::PerfectStart, 0.0f));
}
