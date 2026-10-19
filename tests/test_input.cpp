#include "check.h"
#include "core/Clock.h"
#include "core/InputBuffer.h"
#include "core/Seqlock.h"
#include "core/MotionCommand.h"

#include <atomic>
#include <thread>

using namespace rdf;

TEST(clock_ignores_pause_and_garbage)
{
	Clock c;
	c.Advance(0.0f);
	c.Advance(-1.0f);
	c.Advance(std::nanf(""));
	CHECK(c.Now() == 0.0);
	CHECK(c.Frame() == 0);
	c.Advance(0.25f);
	CHECK_NEAR(c.Now(), 0.25, 1e-12);
	CHECK(c.Frame() == 1);
}

TEST(buffer_expiry_is_inclusive_at_window_edge)
{
	InputBuffer b(0.25f, 0.0f);
	b.Push(CommandKind::Dodge, 1.0);
	CHECK(b.Peek(CommandKind::Dodge, 1.25) != nullptr);
	CHECK(b.Peek(CommandKind::Dodge, 1.2501) == nullptr);
	b.Expire(1.3);
	CHECK(b.Size() == 0);
}

TEST(buffer_debounce_per_kind)
{
	InputBuffer b(0.2f, 0.06f);
	CHECK(b.Push(CommandKind::Dodge, 0.0) != 0);
	CHECK(b.Push(CommandKind::Dodge, 0.03) == 0);
	CHECK(b.Push(CommandKind::Attack, 0.03) != 0);  // other kind unaffected
	CHECK(b.Push(CommandKind::Dodge, 0.06) != 0);
}

TEST(buffer_consume_once_and_oldest_first)
{
	InputBuffer b(0.5f, 0.0f);
	const auto a = b.Push(CommandKind::Dodge, 0.0);
	const auto c = b.Push(CommandKind::Dodge, 0.1);
	CHECK(b.Peek(CommandKind::Dodge, 0.2)->seq == a);
	CHECK(b.Consume(a));
	CHECK(!b.Consume(a));
	CHECK(b.Peek(CommandKind::Dodge, 0.2)->seq == c);
	b.Restore(a);
	CHECK(b.Peek(CommandKind::Dodge, 0.2)->seq == a);
}

TEST(buffer_consume_kind_up_to_merges_mashed_presses)
{
	InputBuffer b(0.5f, 0.0f);
	b.Push(CommandKind::Dodge, 0.0);
	b.Push(CommandKind::Dodge, 0.1);
	b.Push(CommandKind::Dodge, 0.3);
	b.ConsumeKindUpTo(CommandKind::Dodge, 0.2);
	CHECK(b.Peek(CommandKind::Dodge, 0.35)->t == 0.3);
}

TEST(buffer_full_drops_oldest)
{
	InputBuffer b(100.0f, 0.0f);
	for (int i = 0; i < 20; ++i) {
		b.Push(CommandKind::Attack, i * 0.01);
	}
	CHECK(b.Size() == InputBuffer::kCapacity);
	CHECK_NEAR(b.Peek(CommandKind::Attack, 1.0)->t, 0.04, 1e-9);
}

TEST(buffer_promote_attack)
{
	InputBuffer b(0.5f, 0.0f);
	b.Push(CommandKind::Attack, 0.0);
	b.PromoteAttack(0.1);
	CHECK(b.Peek(CommandKind::Attack, 0.1) == nullptr);
	CHECK(b.Peek(CommandKind::PowerAttack, 0.1) != nullptr);
}

TEST(seqlock_never_tears)
{
	Seqlock<MotionCommand> lock;
	std::atomic<bool>      stop{ false };
	std::thread            writer([&] {
        float v = 0.0f;
        while (!stop.load()) {
            MotionCommand c;
            c.active = 1;
            c.vx = v;
            c.vy = v;
            c.yawRate = v;
            c.speed = v;
            lock.Write(c);
            v += 1.0f;
        }
    });

	int torn = 0;
	for (int i = 0; i < 200000; ++i) {
		MotionCommand c;
		if (lock.TryRead(c) && (c.vx != c.vy || c.vx != c.yawRate || c.vx != c.speed)) {
			++torn;
		}
	}
	stop = true;
	writer.join();
	CHECK(torn == 0);
}

TEST(held_button_keeps_press_alive_released_one_expires)
{
	InputBuffer b(0.20f, 0.0f);
	b.Push(CommandKind::Block, 0.0);
	for (double t = 0.05; t <= 1.0; t += 0.05) {
		b.KeepAlive(CommandKind::Block, t);  // held for a second
	}
	CHECK(b.Peek(CommandKind::Block, 1.0) != nullptr);
	CHECK(b.Peek(CommandKind::Block, 1.25) == nullptr);  // released at 1.0: gone after the window
}
