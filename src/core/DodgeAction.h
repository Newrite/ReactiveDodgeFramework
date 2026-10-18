#pragma once

#include "Config.h"

namespace rdf
{
	enum class DodgePhase : std::uint8_t
	{
		Idle,
		Startup,
		Active,
		Recovery,
		Done,
		Interrupted,
	};

	// Timeline of one dodge. Phase boundaries are derived from the elapsed time, so any frame
	// length (including a spike that jumps over a whole phase) lands in the right phase.
	class DodgeAction
	{
	public:
		void Start(const Timing& timing, int chainCount);
		void Tick(float dt);
		void Interrupt();
		// Cancelled into locomotion: jump to Recovery and finish after `blend` seconds.
		void RequestExit(float blend);
		void Reset();

		DodgePhase Phase() const { return phase_; }
		bool       Running() const { return phase_ == DodgePhase::Startup || phase_ == DodgePhase::Active || phase_ == DodgePhase::Recovery; }
		bool       Exiting() const { return exiting_; }
		float      Time() const { return t_; }
		float      Duration() const { return total_; }
		int        ChainCount() const { return chain_; }
		DodgePhase PhaseAt(float t) const;

	private:
		DodgePhase phase_{ DodgePhase::Idle };
		float      t_{ 0.0f };
		float      startupEnd_{ 0.0f };
		float      activeEnd_{ 0.0f };
		float      total_{ 0.0f };
		int        chain_{ 0 };
		bool       exiting_{ false };
	};
}
