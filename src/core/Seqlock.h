#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rdf
{
	// Single-writer / multi-reader seqlock. The payload lives in relaxed atomic words, so
	// concurrent reads are race-free in the C++ memory model. No locks, no allocation.
	template <class T>
	class Seqlock
	{
		static_assert(std::is_trivially_copyable_v<T>);
		static constexpr std::size_t kWords = (sizeof(T) + 3) / 4;

	public:
		void Write(const T& value)
		{
			std::array<std::uint32_t, kWords> buf{};
			std::memcpy(buf.data(), &value, sizeof(T));

			const auto s = seq_.load(std::memory_order_relaxed);
			seq_.store(s + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			for (std::size_t i = 0; i < kWords; ++i) {
				words_[i].store(buf[i], std::memory_order_relaxed);
			}
			seq_.store(s + 2, std::memory_order_release);
		}

		// Returns false when a consistent copy could not be obtained within `attempts`.
		bool TryRead(T& out, int attempts = 4) const
		{
			for (int a = 0; a < attempts; ++a) {
				const auto s1 = seq_.load(std::memory_order_acquire);
				if (s1 & 1u) {
					continue;
				}
				std::array<std::uint32_t, kWords> buf{};
				for (std::size_t i = 0; i < kWords; ++i) {
					buf[i] = words_[i].load(std::memory_order_relaxed);
				}
				std::atomic_thread_fence(std::memory_order_acquire);
				if (seq_.load(std::memory_order_relaxed) == s1) {
					std::memcpy(&out, buf.data(), sizeof(T));
					return true;
				}
			}
			return false;
		}

	private:
		std::atomic<std::uint32_t>                     seq_{ 0 };
		std::array<std::atomic<std::uint32_t>, kWords> words_{};
	};
}
