#include "check.h"
#include "sim.h"
#include "core/AiRequests.h"
#include <limits>
#include <atomic>
#include <thread>

using namespace rdf;
using Book = AiRequests;

TEST(ai_concurrent_callers_reserve_one_attempt_per_actor)
{
	Book book;
	std::atomic<int> admitted{ 0 };
	std::vector<std::thread> callers;
	for (int i = 0; i < 16; ++i) {
		callers.emplace_back([&] { if (book.Submit(42, { 1, 0 }, 0)) { ++admitted; } });
	}
	for (auto& caller : callers) { caller.join(); }
	CHECK(admitted.load() == 1);
	CHECK(book.Drain(0).size() == 1);
}

TEST(ai_result_waits_for_graph_confirmation_and_consumes_once)
{
	Book book;
	const auto id = book.Submit(42, { 3, 4 }, 0);
	CHECK(id != 0);
	CHECK(!book.Submit(42, { 1, 0 }, 0));
	CHECK(book.GetState(42) == Book::State::Pending);
	const auto requests = book.Drain(0.01);
	CHECK(requests.size() == 1);
	CHECK_NEAR(Length(requests[0].direction), 1, 1e-5);
	CHECK(book.Poll(id, true, 0.01).status == Book::Status::Pending);
	CHECK(book.Claim(id, 0.01));
	CHECK(!book.Cancel(id, 0.02));
	CHECK(book.Poll(id, false, 0.02).status == Book::Status::Starting);
	book.Resolve(id, Book::Status::Started, Book::Reject::None, 0.03);
	CHECK(book.Poll(id, true, 0.03).status == Book::Status::Started);
	CHECK(book.Poll(id, false, 0.03).status == Book::Status::Unknown);
	CHECK(book.GetState(42) == Book::State::Dodging);
	book.Publish(42, Book::State::Exiting);
	CHECK(!book.Submit(42, { 1, 0 }, 0.04));
	book.Publish(42, Book::State::Idle);
	CHECK(book.Submit(42, { 1, 0 }, 0.05) != 0);
}

TEST(ai_cancel_expiry_reset_and_capacity_never_leave_stale_work)
{
	Book book;
	const auto cancelled = book.Submit(1, { 1, 0 }, 0);
	CHECK(book.Cancel(cancelled, 0));
	CHECK(book.Drain(0.01).empty());
	CHECK(book.Poll(cancelled, true, 0.01).reason == Book::Reject::Cancelled);
	const auto expired = book.Submit(1, { 1, 0 }, 0.1);
	CHECK(book.Drain(0.4).empty());
	CHECK(book.Poll(expired, true, 0.4).reason == Book::Reject::Expired);
	const auto old = book.Submit(1, { 1, 0 }, 1);
	book.Reset();
	CHECK(book.Poll(old, true, 1).status == Book::Status::Unknown);
	CHECK(book.Submit(1, { 1, 0 }, 1) > old);
	book.Reset();
	for (std::uint32_t i = 1; i <= 512; ++i) { CHECK(book.Submit(i, { 1, 0 }, 2) != 0); }
	CHECK(!book.Submit(513, { 1, 0 }, 2));
	book.Drain(3);
	CHECK(book.Submit(513, { 1, 0 }, 64) != 0); // abandoned terminal results expire
}

TEST(ai_cancel_race_and_invalid_directions)
{
	Book book;
	CHECK(!book.Submit(0, { 1, 0 }, 0));
	CHECK(!book.Submit(1, {}, 0));
	CHECK(!book.Submit(1, { std::numeric_limits<float>::infinity(), 0 }, 0));
	CHECK(!book.Submit(1, { 0, std::nanf("") }, 0));
	CHECK(!book.Submit(1, { 1, 0 }, 0, -1));
	CHECK(!book.Submit(1, { 1, 0 }, 0, std::nanf("")));
	const auto id = book.Submit(1, { 1, 0 }, 0);
	book.Drain(0);
	book.Publish(1, Book::State::Idle); // a concurrent idle tick cannot lose the reservation
	CHECK(!book.Submit(1, { 1, 0 }, 0));
	CHECK(book.Cancel(id, 0));
	CHECK(!book.Claim(id, 0));
	book.Resolve(id, Book::Status::Started, Book::Reject::None, 0);
	CHECK(book.Poll(id, true, 0).reason == Book::Reject::Cancelled);
}

TEST(ai_motion_is_capped_by_probed_distance_even_after_clip_metadata)
{
	Config cfg;
	cfg.motion.maxCarrySpeed = 0;
	sim::Sim s(cfg);
	s.core.LimitNextDodgeDistance(180);
	s.Frame(0, {}, { CommandKind::Dodge });
	ClipMotion clip;
	clip.distance = 400.0f;
	CHECK(s.core.OnClipMotion(clip));
	s.Run(1.0, 1.0f / 144.0f);
	CHECK_NEAR(Length(s.pos), 180, 2);
	// The cap belongs only to that request. The next normal dodge uses normal distance.
	s.pos = {};
	s.Frame(0, {}, { CommandKind::Dodge });
	s.Run(1.0, 1.0f / 144.0f);
	CHECK_NEAR(Length(s.pos), cfg.motion.distance, 3);
}

TEST(ai_cancelled_and_consumed_requests_cannot_grow_the_undrained_queue)
{
	Book book;
	for (int i = 0; i < 512; ++i) {
		const auto ticket = book.Submit(1, { 1, 0 }, 0);
		CHECK(ticket != 0);
		CHECK(book.Cancel(ticket, 0));
		book.Poll(ticket, true, 0);
	}
	CHECK(!book.Submit(1, { 1, 0 }, 0));
	CHECK(book.Drain(0).empty());
	CHECK(book.Submit(1, { 1, 0 }, 0) != 0);
}

TEST(rejected_ai_press_is_discarded_instead_of_buffered)
{
	sim::Sim s;
	s.Frame(0, {}, {});
	const auto seq = s.core.Press(CommandKind::Dodge, sim::Dir(0));
	EngineView blocked;
	blocked.engine = EngineBlock::Swimming;
	CHECK(s.core.Evaluate(blocked).kind == DecisionKind::None);
	s.core.DiscardPress(seq);
	CHECK(s.core.Evaluate({}).kind == DecisionKind::None);
}
