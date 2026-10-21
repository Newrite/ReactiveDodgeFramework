#pragma once

#include <span>
#include <string>

namespace rdf::ini
{
	struct Entry
	{
		std::string section;
		std::string key;
		std::string value;
	};

	// Returns `text` with every entry set, keeping comments, order and line endings: an existing `key=` line
	// (case-insensitive, not commented out) in its section is rewritten in place, a missing key is added after
	// the last non-empty line of its section, a missing section is appended at the end.
	std::string Update(const std::string& text, std::span<const Entry> entries);
}
