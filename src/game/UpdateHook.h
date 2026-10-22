#pragma once

namespace rdf::game::update
{
	// Builds the player's dodge agent from the loaded Settings. Call once after Settings::Load.
	void Configure();
	// Hooks PlayerCharacter::Update (vfunc 0xAD): the single main-thread tick where every dodge's state changes
	// (the player's and the NPCs' that dodge through RequestDodge).
	void Install();
	// Drops every pending command, NPC agent and motion snapshot (load, new game).
	void Reset();
	// Any thread. Asks `actor` to dodge at `angleDeg` relative to where it faces (0 forward, 90 right, 180 back),
	// on the next main-thread tick, with the same rules as a dodge press (buffer, stagger, cost, chains).
	// False if the request could not be queued (no actor / handle).
	bool RequestDodge(RE::Actor* actor, float angleDeg);
}
