#pragma once

namespace rdf::game::probe
{
	// Dev diagnostics: the root state machine's current state and its active transitions (blend effect
	// duration / time remaining), read from the player's live behavior graph.
	void LogRootTransitions(RE::PlayerCharacter* player);
}
