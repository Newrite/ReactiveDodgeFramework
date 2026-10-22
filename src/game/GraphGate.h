#pragma once

#include <cstdint>
#include <vector>

namespace rdf::game::gate
{
	// Graph events other mods send straight to an actor's graph (NotifyAnimationGraph, no action: Input Support's and STB's
	// blockStart) while the actor's dodge must not be left yet. With the dodge inside 1hm_behavior, such an event would leave
	// it through 1HM_Ready_State's transitions at any moment. Held back events ([Graph] sHoldEvents) report "not taken" and
	// wait for the dodge to let them go; an event's cancel event (blockStop for blockStart) drops it again.
	// IAnimationGraphManagerHolder::NotifyAnimationGraph is slot 1 of Character / PlayerCharacter's holder vtable
	// (VTABLE_*[3]: 0x14165E3C8 / 0x141663F90, both -> 0x14060F240).
	void Install();
	// Reads the hold list from Settings.
	void Init();

	// The actor's dodge holds the listed events back (main thread, from its agent).
	void SetLocked(std::uint32_t formID, bool locked);
	// Events held back since the last call, oldest first (each once).
	std::vector<RE::BSFixedString> TakeHeld(std::uint32_t formID);
	void Forget(std::uint32_t formID);
	void Clear();

	// Our own graph events pass the gate (and settle a held event of the same name).
	bool Send(RE::Actor* actor, const RE::BSFixedString& event);
	struct Bypass
	{
		Bypass();
		~Bypass();
		Bypass(const Bypass&) = delete;
		Bypass& operator=(const Bypass&) = delete;
	};
}
