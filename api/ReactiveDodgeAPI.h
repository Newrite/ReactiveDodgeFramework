#pragma once

// Reactive Dodge Framework: dodge any actor from another SKSE plugin.
//
//   if (auto request = ReactiveDodgeAPI::Get()) {
//       request(actor, 90.0f);  // dodge to the actor's right
//   }
//
// The dodge is the player's: the same behavior states, clips, annotation windows, chains, exits, cost,
// i-frames (melee, Precision, projectiles, explosions) and perfect dodge. Only the decision is the caller's.
// The actor needs the RDF behavior patch (every humanoid sharing 0_master.hkx has it).

#include <cstdint>
#include <Windows.h>
#include "ReactiveDodgeTypes.h"

namespace RE
{
	class Actor;
}

#define RDF_API_VERSION 1u

namespace ReactiveDodgeAPI
{
	// Any thread. `angleDeg` is relative to where the actor faces: 0 forward, 90 right, 180 back, -90 left.
	// The dodge starts on the next main-thread tick if the actor can dodge then (not staggered beyond the
	// configured escape, not in a non-cancellable state, enough of the cost resource), or within the
	// input-buffer window after. Returns false when the request could not be queued.
	using RequestDodge_t = bool (*)(RE::Actor* actor, float angleDeg);
	using GetApiVersion_t = std::uint32_t (*)();

	// Call at or after SKSE's kPostLoad. Null when ReactiveDodgeFramework.dll is not loaded.
	inline RequestDodge_t Get()
	{
		const auto module = ::GetModuleHandleW(L"ReactiveDodgeFramework.dll");
		if (!module) {
			return nullptr;
		}
		const auto version = reinterpret_cast<GetApiVersion_t>(::GetProcAddress(module, "RDF_GetApiVersion"));
		if (!version || version() < RDF_API_VERSION) {
			return nullptr;
		}
		return reinterpret_cast<RequestDodge_t>(::GetProcAddress(module, "RDF_RequestDodge"));
	}

	// V2: NPC AI only. All calls are thread-safe; game work happens on RDF's update tick.
	// SubmitWorld: a horizontal WORLD vector, not a heading relative to a potentially turning actor.
	// 0 = invalid/not ready/player/busy/capacity limit. A nonzero ticket is NOT a successful dodge.
	// Each request is attempted once on the next active tick, never buffered or chained. Queued requests
	// older than 250ms are rejected. Poll until Started/Rejected; Started means the graph confirmed entry.
	// Results expire after 60 real seconds; Poll(..., true) consumes terminal results immediately.
	// Cancel prevents an unprocessed request from starting; it does not interrupt a dodge already starting.
	// All tickets become Unknown on load/new game; ticket numbers are never reused within the process.
	// Resource, attack-cancel, i-frames, perfect and clip selection belong exclusively to RDF.
	struct V2
	{
		std::uint32_t size;
		std::uint32_t version;
		// maxDistance is the AI's probed travel budget (game units). Must be finite and >0.
		// Caps both INI distance and later RDF_Motion overrides, without changing the chosen clip/timing.
		Ticket (*SubmitWorld)(RE::Actor*, float x, float y, float maxDistance);
		Result (*Poll)(Ticket, bool consumeTerminal);
		ActorState (*State)(RE::Actor*);
		bool (*Cancel)(Ticket);
	};
	inline const V2* GetV2()
	{
		const auto module = ::GetModuleHandleW(L"ReactiveDodgeFramework.dll");
		const auto get = module ? reinterpret_cast<const V2* (*)()>(::GetProcAddress(module, "RDF_GetApiV2")) : nullptr;
		const auto api = get ? get() : nullptr;
		return api && api->version == 2 && api->size >= sizeof(V2) ? api : nullptr;
	}
}
