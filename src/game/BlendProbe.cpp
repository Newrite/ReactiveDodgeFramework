#include "BlendProbe.h"

#include <atomic>
#include <cmath>

namespace rdf::game::blendprobe
{
	namespace
	{
		// hkbBlendingTransitionEffect, verified in IDA (1.5.97): vtable 0x14171D118 = ID 278723;
		// slot 5 update(this, context, timestep) at 0x140A3F820 decrements +0x80 and advances +0x84;
		// slot 7 deactivate(this) at 0x140A3F830 zeroes +0x80; activate (slot 4) copies +0x50 into +0x80.
		constexpr std::size_t kUpdateSlot = 5;
		constexpr std::size_t kDeactivateSlot = 7;
		constexpr std::size_t kDuration = 0x50;
		constexpr std::size_t kTimeRemaining = 0x80;
		constexpr std::size_t kTimeInTransition = 0x84;

		using Update_t = void (*)(void*, void*, float);
		using Deactivate_t = std::uintptr_t (*)(void*, void*);  // (this, context); the original ignores the context
		Update_t     g_update = nullptr;
		Deactivate_t g_deactivate = nullptr;

		float Field(const void* effect, std::size_t offset)
		{
			return *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(effect) + offset);
		}

		// The RDF blends are told apart by their duration (the only field read besides the timers).
		const char* RdfBlend(const void* effect)
		{
			const float d = Field(effect, kDuration);
			if (std::fabs(d - 0.12f) < 1e-5f) return "in";
			if (std::fabs(d - 0.10f) < 1e-5f) return "chain";
			if (std::fabs(d - 0.25f) < 1e-5f) return "out";
			return nullptr;
		}

		void UpdateThunk(void* effect, void* context, float timestep)
		{
			if (const char* tag = effect ? RdfBlend(effect) : nullptr) {
				logger::info("trace blend {} {:p} update dt={:.4f} remaining={:.4f} inTransition={:.4f}", tag, effect, timestep,
					Field(effect, kTimeRemaining), Field(effect, kTimeInTransition));
			}
			g_update(effect, context, timestep);
		}

		std::uintptr_t DeactivateThunk(void* effect, void* context)
		{
			if (const char* tag = effect ? RdfBlend(effect) : nullptr) {
				logger::info("trace blend {} {:p} deactivate remaining={:.4f} inTransition={:.4f}", tag, effect,
					Field(effect, kTimeRemaining), Field(effect, kTimeInTransition));
			}
			return g_deactivate(effect, context);
		}
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_hkbBlendingTransitionEffect[0] };
		g_update = reinterpret_cast<Update_t>(vtbl.write_vfunc(kUpdateSlot, UpdateThunk));
		g_deactivate = reinterpret_cast<Deactivate_t>(vtbl.write_vfunc(kDeactivateSlot, DeactivateThunk));
		logger::info("dev: blend probe installed on hkbBlendingTransitionEffect");
	}
}
