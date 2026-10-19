#pragma once

#include <cstdint>

namespace rdf::game::graph
{
	// Builds the BSFixedString names from Settings.
	void Init();
	// (Re-)registers the animation-graph sink on the player; safe to call repeatedly.
	void RegisterSink(RE::PlayerCharacter* player);

	// What the graph reported since the last Take*.
	// Entered is the start/chain handshake. Depth counts dodge states the graph is in: +1 on
	// Entered, -1 on Left (clamped at 0). A chain A->B reports Entered(B) when the transition begins
	// and Left(A) when the blend ends, so depth goes 1 -> 2 -> 1; only 1 -> 0 is a real exit.
	bool TakeEntered();
	int  Depth();
	bool TakeClipEnd();
	bool DodgeWindowOpen();
	void ClearEvents();

	// Sets RDF_DodgeClip / RDF_DodgeSpeed, then sends RDF_DodgeStart or RDF_DodgeChain.
	// False = the graph refused the event.
	bool SendStart(RE::PlayerCharacter* player, bool chain, int clip, float animSpeed);
	void SendExit(RE::PlayerCharacter* player);
	// RDF_IsDodging is set by the graph itself; read-only truth for "graph is in a dodge state".
	bool GraphIsDodging(const RE::PlayerCharacter* player);
}
