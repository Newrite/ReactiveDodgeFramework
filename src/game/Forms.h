#pragma once

#include <memory>
#include <string>
#include <vector>

namespace rdf::game::forms
{
	// A game form named by text in the ini or the menu: "Plugin.esp|0xFormID" (the local id as xEdit shows it, without
	// the load-order byte) or an EditorID (forms that keep theirs: keywords always; spells, effects and the like only
	// with an EditorID-keeping plugin such as powerofthree's Tweaks, so name those by plugin and id). Empty text = none.
	//
	// Resolution runs on the main thread, once the data is loaded, and is cached per (text, type) until the settings
	// change or the data reloads (Invalidate). Any thread may read the cache; a miss there is "pending" and queues the
	// resolution. Everything returns null rather than failing: a missing plugin, a wrong id or a form of another type.

	enum class Status
	{
		Empty,      // no text: nothing asked for
		Pending,    // not resolved yet (the data is not loaded, or the menu asked off the main thread)
		Found,
		NotFound,   // no such plugin / form / EditorID
		WrongType,  // a form, but not of the type asked for
		BadText,    // "Plugin|id" with an id that is not hexadecimal
	};

	struct Resolution
	{
		RE::TESForm* form{ nullptr };
		Status       status{ Status::Empty };
		std::string  detail;  // Found: "EditorID / name [runtime id] Plugin.esp"; otherwise what was wrong
	};

	// Main thread: resolves (or returns the cached result).
	Resolution Resolve(const std::string& text, RE::FormType type);

	template <class T>
	T* Get(const std::string& text)
	{
		auto* form = Resolve(text, T::FORMTYPE).form;
		return form ? form->As<T>() : nullptr;
	}

	// Any thread (the menu): the cached result, or Pending with the resolution queued for the main thread.
	Resolution Peek(const std::string& text, RE::FormType type);

	// Drops every cached result (settings changed, data reloaded). Main thread.
	void Invalidate();
	// kDataLoaded: forms can be resolved from now on.
	void OnDataLoaded();

	// The text that names `form` best: its EditorID when the game keeps one, else "Plugin.esp|0xLocalID".
	std::string TextOf(const RE::TESForm* form);

	// Every form of a type, for the menu's picker. Built on the main thread on first request (null until then).
	struct Entry
	{
		std::string label;   // what the picker shows: "EditorID / name [id] Plugin.esp"
		std::string text;    // what picking it writes (TextOf)
		std::string search;  // lower-case label, for the filter
	};
	using Catalog = std::shared_ptr<const std::vector<Entry>>;
	Catalog CatalogOf(RE::FormType type);
}
