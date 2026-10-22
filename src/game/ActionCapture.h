#pragma once

#include "core/InputBuffer.h"

#include <vector>

namespace rdf::game::capture
{
	// Attack attempts made while an actor dodges, whoever makes them: the game's own attack input, an attack framework
	// (BFCO's power-attack idles, Input Support's dispatched actions), the AI, Papyrus. Every action and every special
	// idle of the game reaches ActorMediator::PerformAction (0x14063AF10): TESActionData::Process tail-calls it, and
	// AIProcess::SetupSpecialIdle queues a RunActionAnimationLoadedCallback that rebuilds a TESActionData with the idle
	// and processes it. While the actor's dodge may still be cancelled into an attack, such an attempt is held back
	// (the action reports "not performed") and handed to the dodge as an attack / power-attack press; when the dodge
	// opens its attack window it replays that exact attempt (the same action, or the same idle).
	void Install();

	// The actor's dodge holds attack attempts back (main thread, from its agent).
	void SetCapturing(std::uint32_t formID, bool on);
	// Attempts held back since the last call, oldest first.
	std::vector<CommandKind> TakeNew(std::uint32_t formID);
	// Replays the latest held-back attempt of that kind (a power attack falls back to any). False if there is none or
	// the game refused it again. A replayed idle is queued: its outcome shows a few frames later.
	bool Replay(RE::Actor* actor, bool power);
	// Sprint: the dodge stops a running sprint when it starts (remembering it), holds sprint starts back while it runs,
	// and at its end resumes the sprint if no stop came meanwhile (the button is still held) and `resume`, otherwise
	// ends a sprint state the graph kept because it refused the stop during the dodge.
	bool StopSprint(RE::Actor* actor, bool remember);
	void FinishSprint(RE::Actor* actor, bool resume);
	void Forget(std::uint32_t formID);
	void Clear();  // load / new game
}
