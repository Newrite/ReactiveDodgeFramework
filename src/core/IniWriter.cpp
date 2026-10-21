#include "IniWriter.h"

#include <cctype>
#include <string_view>
#include <vector>

namespace rdf::ini
{
	namespace
	{
		std::string_view Trim(std::string_view v)
		{
			const auto b = v.find_first_not_of(" \t");
			if (b == std::string_view::npos) {
				return {};
			}
			const auto e = v.find_last_not_of(" \t");
			return v.substr(b, e - b + 1);
		}

		bool EqualNoCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size()) {
				return false;
			}
			for (std::size_t n = 0; n < a.size(); ++n) {
				if (std::tolower(static_cast<unsigned char>(a[n])) != std::tolower(static_cast<unsigned char>(b[n]))) {
					return false;
				}
			}
			return true;
		}

		// "[Name]" -> "Name", otherwise empty.
		std::string_view SectionOf(std::string_view line)
		{
			const auto t = Trim(line);
			return t.size() >= 2 && t.front() == '[' && t.back() == ']' ? t.substr(1, t.size() - 2) : std::string_view{};
		}

		bool IsKey(std::string_view line, std::string_view key)
		{
			const auto t = Trim(line);
			if (t.empty() || t[0] == ';' || t[0] == '#') {
				return false;
			}
			const auto eq = t.find('=');
			return eq != std::string_view::npos && EqualNoCase(Trim(t.substr(0, eq)), key);
		}
	}

	std::string Update(const std::string& text, std::span<const Entry> entries)
	{
		const std::string eol = text.empty() || text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
		const bool        finalEol = text.empty() || text.back() == '\n';

		std::vector<std::string> lines;
		for (std::size_t pos = 0; pos < text.size();) {
			const auto nl = text.find('\n', pos);
			auto       line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			lines.push_back(std::move(line));
			if (nl == std::string::npos) {
				break;
			}
			pos = nl + 1;
		}

		for (const auto& e : entries) {
			const std::string assignment = e.key + "=" + e.value;
			std::ptrdiff_t    insertAfter = -1;  // last non-empty line of the section
			bool              inSection = false;
			bool              done = false;
			for (std::size_t n = 0; n < lines.size() && !done; ++n) {
				if (const auto sec = SectionOf(lines[n]); !sec.empty()) {
					inSection = EqualNoCase(sec, e.section);
					if (inSection) {
						insertAfter = static_cast<std::ptrdiff_t>(n);
					}
					continue;
				}
				if (!inSection) {
					continue;
				}
				if (IsKey(lines[n], e.key)) {
					lines[n] = assignment;
					done = true;
				} else if (!Trim(lines[n]).empty()) {
					insertAfter = static_cast<std::ptrdiff_t>(n);
				}
			}
			if (done) {
				continue;
			}
			if (insertAfter >= 0) {
				lines.insert(lines.begin() + insertAfter + 1, assignment);
			} else {
				if (!lines.empty() && !Trim(lines.back()).empty()) {
					lines.emplace_back();
				}
				lines.push_back("[" + e.section + "]");
				lines.push_back(assignment);
			}
		}

		std::string out;
		for (std::size_t n = 0; n < lines.size(); ++n) {
			out += lines[n];
			if (n + 1 < lines.size() || finalEol) {
				out += eol;
			}
		}
		return out;
	}
}
