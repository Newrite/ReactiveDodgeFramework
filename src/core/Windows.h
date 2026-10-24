#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace rdf
{
	// Clip annotations the graph forwards as animation events (ANIMATION_CONTRACT.md).
	enum class WindowEvent : std::uint8_t
	{
		Marker,  // RDF_Windows at clip start: this clip carries its own windows
		IFrameStart,
		IFrameEnd,
		ChainOpen,
		AttackOpen,
		BlockOpen,
		ExitOpen,
		PerfectStart,  // numeric start metadata, independent of cancel windows
		PerfectEnd,
		JumpOpen,  // the jump may cut the dodge from here (without it: the exit window)
	};

	struct OpenWindows
	{
		bool chain{ false };
		bool attack{ false };
		bool block{ false };
		bool exit{ false };
	};

	// Protection has a configured start and duration; clip metadata may replace only the start.
	// An End event is a terminal cap for this dodge, including any duration bonus.
	class ProtectionWindow
	{
	public:
		bool SetStart(float start)
		{
			if (closed_ || start_ || !std::isfinite(start) || start < 0.0f) {
				return false;
			}
			start_ = start;
			return true;
		}
		void Close() { closed_ = true; }
		bool Open(float t, float start, float duration, float delta) const
		{
			const float from = start_.value_or(start);
			const float length = std::max(0.0f, duration + delta);
			return !closed_ && t >= from && t < from + length;
		}

	private:
		std::optional<float> start_;
		bool closed_{ false };
	};

	// Cancel windows are animation-driven. Protection windows are independent of their marker.
	class WindowTracker
	{
	public:
		void Reset() { *this = {}; }

		void On(WindowEvent e)
		{
			switch (e) {
			case WindowEvent::IFrameEnd: iframe_.Close(); return;
			case WindowEvent::PerfectEnd: perfect_.Close(); return;
			case WindowEvent::IFrameStart:
			case WindowEvent::PerfectStart: return; // only numeric metadata at clip start can override the start
			case WindowEvent::JumpOpen: jump_ = true; return;
			default: break;
			}
			auto& w = marks_.emplace(marks_.value_or(OpenWindows{}));
			switch (e) {
			case WindowEvent::ChainOpen: w.chain = true; break;
			case WindowEvent::AttackOpen: w.attack = true; break;
			case WindowEvent::BlockOpen: w.block = true; break;
			case WindowEvent::ExitOpen: w.exit = true; break;
			default: break;
			}
		}

		bool SetProtectionStart(WindowEvent e, float start)
		{
			if (e == WindowEvent::IFrameStart) { return iframe_.SetStart(start); }
			if (e == WindowEvent::PerfectStart) { return perfect_.SetStart(start); }
			return false;
		}
		const std::optional<OpenWindows>& Annotated() const { return marks_; }
		const ProtectionWindow& IFrames() const { return iframe_; }
		const ProtectionWindow& Perfect() const { return perfect_; }
		bool JumpOpen() const { return jump_; }

	private:
		std::optional<OpenWindows> marks_;
		ProtectionWindow iframe_, perfect_;
		bool jump_{ false };
	};
}
