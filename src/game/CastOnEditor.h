#pragma once

// The CastOn rules window (docs/CASTON_DESIGN.md 7): rule files, their rules and presets, an OAR-like condition tree with
// a live result per condition for the player, casts, cost, timing. Edits go to <name>.user.json; the game reloads the
// rules after a save.
namespace rdf::game::caston_editor
{
	// Registers the window with FLICK (menu::Install, after the pages).
	void Install();
	// Opens it (the Rules page's button).
	void Open();
}
