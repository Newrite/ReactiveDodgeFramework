#pragma once

#include "Math.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rdf
{
	enum class CommandKind : std::uint8_t
	{
		Dodge,
		Attack,
		PowerAttack,
		Block,
		Count,
	};

	constexpr std::size_t kCommandKindCount = static_cast<std::size_t>(CommandKind::Count);

	struct Command
	{
		CommandKind   kind{ CommandKind::Dodge };
		std::uint64_t seq{ 0 };  // 0 = no command
		double        t{ 0.0 };  // game-active clock time of the press
		bool          consumed{ false };
		Vec2          dir{};  // world-space move input when pressed (zero = none)
	};

	// Fixed-capacity ring of timestamped presses. Oldest-first, expiry on the game-active clock,
	// per-kind debounce, one-shot consumption by sequence id. No allocation.
	class InputBuffer
	{
	public:
		static constexpr std::size_t kCapacity = 16;

		InputBuffer() = default;
		InputBuffer(float window, float debounce) :
			window_(window), debounce_(debounce) {}

		// Returns the sequence id, or 0 when the press was debounced.
		std::uint64_t Push(CommandKind kind, double now, Vec2 dir = {});

		// Oldest unconsumed, unexpired command of `kind`; null when none.
		const Command* Peek(CommandKind kind, double now) const;

		// Marks one command consumed. Returns false for an unknown/expired sequence id.
		bool Consume(std::uint64_t seq);
		// Marks every pending command of `kind` pressed at or before `now` consumed (mashed duplicates).
		void ConsumeKindUpTo(CommandKind kind, double now);
		// Undoes Consume for a command that is still in the buffer (an execution that failed).
		void Restore(std::uint64_t seq);
		// Attack held long enough: turn the newest pending light attack into a power attack.
		void PromoteAttack(double now);
		// The button behind pending `kind` presses is still held: they stay fresh while it is.
		void KeepAlive(CommandKind kind, double now);

		void Expire(double now);
		void Clear();
		std::size_t Size() const { return count_; }

	private:
		Command* Find(std::uint64_t seq);
		bool     Alive(const Command& c, double now) const { return now - c.t <= window_ + 1e-9; }

		std::array<Command, kCapacity>              ring_{};  // ring_[0..count_) in press order
		std::size_t                                 count_{ 0 };
		std::array<double, kCommandKindCount>       lastPush_{ -1e9, -1e9, -1e9, -1e9 };
		std::uint64_t                               nextSeq_{ 1 };
		float                                       window_{ 0.20f };
		float                                       debounce_{ 0.06f };
	};
}
