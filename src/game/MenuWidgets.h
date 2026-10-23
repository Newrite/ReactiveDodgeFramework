#pragma once

#include <string>

// Widgets the settings pages (Menu.cpp) and the CastOn editor (CastOnEditor.cpp) share. Render thread, inside a FLICK draw.
namespace rdf::game::menu
{
	// A form named by text (Forms.h): the edit box, committed when the edit ends so half-typed text is never resolved; a
	// picker over every form of the type; a clear button; what the text resolves to. True when the text changed.
	// compact (a table cell): the status icon and the two buttons before the box on one line, the resolution as the
	// box's tooltip; else the buttons and the resolution on the line below (the host stretches a labelled box to the end).
	bool FormInput(const char* label, std::string& text, RE::FormType type, bool compact = false);
}
