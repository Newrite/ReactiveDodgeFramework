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
		PerfectStart,  // the clip's own perfect-dodge window (optional: without it the ini window counts)
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

	// Cancel windows of the current dodge as announced by its clip. Empty until the clip announces
	// itself, so a clip without annotations leaves the ini timers in charge.
	class WindowTracker
	{
	public:
		void Reset() { *this = {}; }

		void On(WindowEvent e, float t = 0.0f)
		{
			if (e == WindowEvent::JumpOpen) {  // on its own: the clip's other windows stay as they are
				jump_ = true;
				return;
			}
			auto& w = marks_.emplace(marks_.value_or(OpenWindows{}));
			switch (e) {
			case WindowEvent::Marker: break;
			case WindowEvent::IFrameStart:
				invulnerable_ = true;
				iframeStart_ = t;
				iframeEnd_.reset();
				break;
			case WindowEvent::IFrameEnd:
				invulnerable_ = false;
				if (iframeStart_) {
					iframeEnd_ = t;
				}
				break;
			case WindowEvent::ChainOpen: w.chain = true; break;
			case WindowEvent::AttackOpen: w.attack = true; break;
			case WindowEvent::BlockOpen: w.block = true; break;
			case WindowEvent::ExitOpen: w.exit = true; break;
			case WindowEvent::PerfectStart:
				perfect_ = true;
				perfectStart_ = t;
				perfectEnd_.reset();
				break;
			case WindowEvent::PerfectEnd:
				perfect_ = false;
				if (perfectStart_) {
					perfectEnd_ = t;
				}
				break;
			default: break;
			}
		}

		const std::optional<OpenWindows>& Annotated() const { return marks_; }
		bool                              Invulnerable() const { return invulnerable_; }
		// Set once the clip announced a perfect window: whether it is open now.
		const std::optional<bool>&        Perfect() const { return perfect_; }
		// When the clip's windows opened / closed (action time); empty until they did.
		const std::optional<float>&       IFrameStart() const { return iframeStart_; }
		const std::optional<float>&       IFrameEnd() const { return iframeEnd_; }
		const std::optional<float>&       PerfectStart() const { return perfectStart_; }
		const std::optional<float>&       PerfectEnd() const { return perfectEnd_; }
		// Set once the clip announced RDF_JumpOpen.
		bool                              JumpOpen() const { return jump_; }

	private:
		std::optional<OpenWindows> marks_;
		bool                       invulnerable_{ false };
		std::optional<bool>        perfect_;
		bool                       jump_{ false };
		std::optional<float>       iframeStart_, iframeEnd_, perfectStart_, perfectEnd_;
	};
}
