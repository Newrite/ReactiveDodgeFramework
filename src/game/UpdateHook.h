#pragma once

namespace rdf::game::update
{
	// Builds the core from the loaded Settings. Call once after Settings::Load.
	void Configure();
	// Hooks PlayerCharacter::Update (vfunc 0xAD): the single main-thread tick where core state changes.
	void Install();
	// Drops every pending command and motion snapshot (load, new game, 3D reset).
	void Reset();
}
