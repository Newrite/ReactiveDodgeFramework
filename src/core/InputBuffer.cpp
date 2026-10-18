#include "InputBuffer.h"

namespace rdf
{
	std::uint64_t InputBuffer::Push(CommandKind kind, double now, Vec2 dir)
	{
		const auto k = static_cast<std::size_t>(kind);
		if (now - lastPush_[k] < debounce_ - 1e-9) {
			return 0;
		}
		lastPush_[k] = now;

		if (count_ == kCapacity) {  // full: drop the oldest
			for (std::size_t i = 1; i < count_; ++i) {
				ring_[i - 1] = ring_[i];
			}
			--count_;
		}
		Command c;
		c.kind = kind;
		c.seq = nextSeq_++;
		c.t = now;
		c.dir = dir;
		ring_[count_++] = c;
		return c.seq;
	}

	const Command* InputBuffer::Peek(CommandKind kind, double now) const
	{
		for (std::size_t i = 0; i < count_; ++i) {
			const auto& c = ring_[i];
			if (c.kind == kind && !c.consumed && Alive(c, now)) {
				return &c;
			}
		}
		return nullptr;
	}

	Command* InputBuffer::Find(std::uint64_t seq)
	{
		for (std::size_t i = 0; i < count_; ++i) {
			if (ring_[i].seq == seq) {
				return &ring_[i];
			}
		}
		return nullptr;
	}

	bool InputBuffer::Consume(std::uint64_t seq)
	{
		auto* c = Find(seq);
		if (!c || c->consumed) {
			return false;
		}
		c->consumed = true;
		return true;
	}

	void InputBuffer::ConsumeKindUpTo(CommandKind kind, double now)
	{
		for (std::size_t i = 0; i < count_; ++i) {
			if (ring_[i].kind == kind && ring_[i].t <= now + 1e-9) {
				ring_[i].consumed = true;
			}
		}
	}

	void InputBuffer::Restore(std::uint64_t seq)
	{
		if (auto* c = Find(seq)) {
			c->consumed = false;
		}
	}

	void InputBuffer::PromoteAttack(double now)
	{
		for (std::size_t i = count_; i-- > 0;) {
			auto& c = ring_[i];
			if (c.kind == CommandKind::Attack && !c.consumed && Alive(c, now)) {
				c.kind = CommandKind::PowerAttack;
				return;
			}
		}
	}

	void InputBuffer::Expire(double now)
	{
		std::size_t keep = 0;
		for (std::size_t i = 0; i < count_; ++i) {
			if (Alive(ring_[i], now)) {
				ring_[keep++] = ring_[i];
			}
		}
		count_ = keep;
	}

	void InputBuffer::Clear()
	{
		count_ = 0;
	}
}
