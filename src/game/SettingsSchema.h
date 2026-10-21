#pragma once

#include "Settings.h"

#include <span>
#include <string>

namespace rdf::game::schema
{
	// The live-tunable settings: one row per ini key, shared by the in-game menu and the ini writer.
	// Keys that only take effect at startup (hooks, key bindings, graph names) stay ini-only.
	enum class Page
	{
		Timing,
		Motion,
		Steering,
		Animation,
		Rules,
		Debug,
	};

	enum class Kind
	{
		Float,
		Int,
		Bool,
		Choice,  // an int index into `choices`, written to the ini as the choice's name
	};

	struct Field
	{
		Page        page;
		const char* section;
		const char* key;  // also the translation stem: "$RDF_<key>" and "$RDF_<key>_Desc"
		Kind        kind;
		float       min{ 0.0f };
		float       max{ 1.0f };
		const char* format{ "%.2f" };
		float* (*f)(Settings&){ nullptr };
		int* (*i)(Settings&){ nullptr };
		bool* (*b)(Settings&){ nullptr };
		int (*getChoice)(const Settings&){ nullptr };
		void (*setChoice)(Settings&, int){ nullptr };
		std::span<const char* const> choices{};  // ini names; labels are "$RDF_<key>_<name>"
	};

	std::span<const Field> Fields();

	// Copies every field of `page` from `from` into `to`.
	void CopyPage(Page page, const Settings& from, Settings& to);

	// Writes every field into the plugin ini, keeping its comments, order and line endings: an existing
	// `key=` line in its section is rewritten in place, a missing one is added to the end of its section.
	bool SaveToIni(const Settings& s, std::string* error = nullptr);
}
