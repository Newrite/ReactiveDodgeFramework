#pragma once

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
	};

	struct OpenWindows
	{
		bool chain{ false };
		bool attack{ false };
		bool block{ false };
		bool exit{ false };
	};

	// Cancel windows of the current dodge as announced by its clip. Empty until the clip announces
	// itself, so a clip without annotations leaves the ini timers in charge.
	class WindowTracker
	{
	public:
		void Reset() { *this = {}; }

		void On(WindowEvent e)
		{
			auto& w = marks_.emplace(marks_.value_or(OpenWindows{}));
			switch (e) {
			case WindowEvent::Marker: break;
			case WindowEvent::IFrameStart: invulnerable_ = true; break;
			case WindowEvent::IFrameEnd: invulnerable_ = false; break;
			case WindowEvent::ChainOpen: w.chain = true; break;
			case WindowEvent::AttackOpen: w.attack = true; break;
			case WindowEvent::BlockOpen: w.block = true; break;
			case WindowEvent::ExitOpen: w.exit = true; break;
			}
		}

		const std::optional<OpenWindows>& Annotated() const { return marks_; }
		bool                              Invulnerable() const { return invulnerable_; }

	private:
		std::optional<OpenWindows> marks_;
		bool                       invulnerable_{ false };
	};
}
