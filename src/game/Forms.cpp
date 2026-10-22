#include "Forms.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace rdf::game::forms
{
	namespace
	{
		std::string Key(const std::string& text, RE::FormType type)
		{
			return std::format("{}\x1f{}", static_cast<int>(type), text);
		}

		std::string Trim(const std::string& s)
		{
			const auto b = s.find_first_not_of(" \t");
			const auto e = s.find_last_not_of(" \t");
			return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
		}

		std::string Describe(const RE::TESForm* form)
		{
			const char* edid = form->GetFormEditorID();
			const char* name = form->GetName();
			const auto* file = form->GetFile(0);
			std::string what = edid && edid[0] ? edid : "";
			if (name && name[0] && (what.empty() || what != name)) {
				what += what.empty() ? name : std::string(" / ") + name;
			}
			if (what.empty()) {
				what = "(no EditorID)";
			}
			return std::format("{} [{:08X}] {}", what, form->GetFormID(), file ? file->GetFilename() : std::string_view("(created)"));
		}

		Resolution Lookup(const std::string& raw, RE::FormType type)
		{
			const std::string text = Trim(raw);
			if (text.empty()) {
				return {};
			}
			RE::TESForm* form = nullptr;
			if (const auto bar = text.find('|'); bar != std::string::npos) {
				const auto plugin = Trim(text.substr(0, bar));
				const auto idText = Trim(text.substr(bar + 1));
				std::uint32_t id = 0;
				try {
					std::size_t used = 0;
					id = static_cast<std::uint32_t>(std::stoul(idText, &used, 16));
					if (used != idText.size()) {
						throw std::invalid_argument("trailing characters");
					}
				} catch (...) {
					return { nullptr, Status::BadText, std::format("'{}' is not a hexadecimal form id", idText) };
				}
				auto* handler = RE::TESDataHandler::GetSingleton();
				if (!handler || !handler->LookupModByName(plugin)) {
					return { nullptr, Status::NotFound, std::format("plugin '{}' is not loaded", plugin) };
				}
				form = handler->LookupForm(id & 0x00FFFFFF, plugin);
				if (!form) {
					form = handler->LookupForm(id & 0x00000FFF, plugin);  // a light plugin's local id
				}
				if (!form) {
					return { nullptr, Status::NotFound, std::format("no form {:X} in {}", id, plugin) };
				}
			} else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {  // a runtime form id
				try {
					form = RE::TESForm::LookupByID(static_cast<RE::FormID>(std::stoul(text, nullptr, 16)));
				} catch (...) {
					return { nullptr, Status::BadText, std::format("'{}' is not a hexadecimal form id", text) };
				}
				if (!form) {
					return { nullptr, Status::NotFound, std::format("no form {}", text) };
				}
			} else {
				form = RE::TESForm::LookupByEditorID(text);
				if (!form) {
					return { nullptr, Status::NotFound, std::format("no EditorID '{}' (forms other than keywords keep theirs only with an EditorID plugin: use Plugin.esp|0xID)", text) };
				}
			}
			if (form->GetFormType() != type) {
				return { nullptr, Status::WrongType, std::format("{} is not a {}", Describe(form), RE::FormTypeToString(type)) };
			}
			return { form, Status::Found, Describe(form) };
		}

		std::mutex                                  g_lock;
		std::unordered_map<std::string, Resolution> g_cache;
		std::unordered_set<std::string>             g_pending;  // keys waiting for the main thread
		std::vector<std::pair<std::string, RE::FormType>> g_pendingList;
		std::atomic<bool>                           g_dataLoaded{ false };
		std::unordered_map<int, Catalog>            g_catalogs;
		std::unordered_set<int>                     g_catalogPending;

		void Store(const std::string& key, const Resolution& r)
		{
			std::scoped_lock lock(g_lock);
			g_cache[key] = r;
		}

		void ResolvePending()
		{
			std::vector<std::pair<std::string, RE::FormType>> work;
			{
				std::scoped_lock lock(g_lock);
				work.swap(g_pendingList);
				g_pending.clear();
			}
			for (const auto& [text, type] : work) {
				Resolve(text, type);
			}
		}

		Catalog Build(RE::FormType type)
		{
			auto list = std::make_shared<std::vector<Entry>>();
			if (auto* handler = RE::TESDataHandler::GetSingleton()) {
				for (auto* form : handler->GetFormArray(type)) {
					if (!form || form->GetFormType() != type) {
						continue;
					}
					Entry e{ Describe(form), TextOf(form), {} };
					e.search = e.label;
					std::transform(e.search.begin(), e.search.end(), e.search.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					list->push_back(std::move(e));
				}
			}
			std::sort(list->begin(), list->end(), [](const Entry& a, const Entry& b) { return a.search < b.search; });
			return list;
		}
	}

	Resolution Resolve(const std::string& text, RE::FormType type)
	{
		if (Trim(text).empty()) {
			return {};
		}
		const auto key = Key(text, type);
		{
			std::scoped_lock lock(g_lock);
			if (const auto it = g_cache.find(key); it != g_cache.end()) {
				return it->second;
			}
		}
		if (!g_dataLoaded.load()) {
			return { nullptr, Status::Pending, "the game data is not loaded yet" };
		}
		auto r = Lookup(text, type);
		if (r.status == Status::Found) {
			logger::info("form '{}' -> {}", text, r.detail);
		} else {
			logger::warn("form '{}': {}", text, r.detail);
		}
		Store(key, r);
		return r;
	}

	Resolution Peek(const std::string& text, RE::FormType type)
	{
		if (Trim(text).empty()) {
			return {};
		}
		const auto key = Key(text, type);
		std::scoped_lock lock(g_lock);
		if (const auto it = g_cache.find(key); it != g_cache.end()) {
			return it->second;
		}
		if (g_dataLoaded.load() && g_pending.insert(key).second) {
			const bool first = g_pendingList.empty();
			g_pendingList.emplace_back(text, type);
			if (first) {
				SKSE::GetTaskInterface()->AddTask([] { ResolvePending(); });
			}
		}
		return { nullptr, Status::Pending, "resolving..." };
	}

	void Invalidate()
	{
		std::scoped_lock lock(g_lock);
		g_cache.clear();
	}

	void OnDataLoaded()
	{
		g_dataLoaded = true;
		std::scoped_lock lock(g_lock);
		g_cache.clear();
		g_catalogs.clear();
	}

	std::string TextOf(const RE::TESForm* form)
	{
		if (!form) {
			return {};
		}
		if (const char* edid = form->GetFormEditorID(); edid && edid[0] && RE::TESForm::LookupByEditorID(edid) == form) {
			return edid;
		}
		const auto* file = form->GetFile(0);
		if (!file) {
			return std::format("0x{:08X}", form->GetFormID());  // a created form: only its runtime id names it
		}
		return std::format("{}|0x{:X}", file->GetFilename(), form->GetLocalFormID());
	}

	Catalog CatalogOf(RE::FormType type)
	{
		const int key = static_cast<int>(type);
		{
			std::scoped_lock lock(g_lock);
			if (const auto it = g_catalogs.find(key); it != g_catalogs.end()) {
				return it->second;
			}
			if (!g_dataLoaded.load() || !g_catalogPending.insert(key).second) {
				return nullptr;
			}
		}
		SKSE::GetTaskInterface()->AddTask([type, key] {
			auto built = Build(type);
			std::scoped_lock lock(g_lock);
			g_catalogs[key] = std::move(built);
			g_catalogPending.erase(key);
		});
		return nullptr;
	}
}
