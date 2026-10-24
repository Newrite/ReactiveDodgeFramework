#include "DodgeAction.h"

#include <algorithm>

namespace rdf
{
	void DodgeAction::Start(const Timing& timing, int chainCount)
	{
		t_ = 0.0f;
		startupEnd_ = timing.startup;
		activeEnd_ = timing.startup + timing.active;
		total_ = timing.Total();
		chain_ = chainCount;
		exiting_ = false;
		phase_ = PhaseAt(0.0f);
	}

	DodgePhase DodgeAction::PhaseAt(float t) const
	{
		if (t >= total_) {
			return DodgePhase::Done;
		}
		if (t < startupEnd_) {
			return DodgePhase::Startup;
		}
		return t < activeEnd_ ? DodgePhase::Active : DodgePhase::Recovery;
	}

	void DodgeAction::Tick(float dt)
	{
		if (!Running() || !(dt > 0.0f)) {
			return;
		}
		t_ = std::min(t_ + dt, total_);
		phase_ = PhaseAt(t_);
	}

	void DodgeAction::Interrupt()
	{
		if (Running()) {
			phase_ = DodgePhase::Interrupted;
		}
	}

	void DodgeAction::RequestExit(float blend)
	{
		if (!Running() || exiting_) {
			return;
		}
		exiting_ = true;
		startupEnd_ = std::min(startupEnd_, t_);
		activeEnd_ = std::min(activeEnd_, t_);
		total_ = t_ + blend;
		phase_ = PhaseAt(t_);
	}

	void DodgeAction::Retime(const Timing& timing)
	{
		if (!Running() || exiting_) {
			return;
		}
		startupEnd_ = timing.startup;
		activeEnd_ = timing.startup + timing.active;
		total_ = std::max(timing.Total(), t_);
		phase_ = PhaseAt(t_);
	}

	void DodgeAction::Reset()
	{
		*this = DodgeAction{};
	}
}
