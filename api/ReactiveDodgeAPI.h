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
}
