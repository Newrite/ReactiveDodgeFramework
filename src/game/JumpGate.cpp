#include "JumpGate.h"

namespace rdf::game::jump
{
	namespace
	{
		constexpr std::size_t kProcessButtonVfunc = 4;  // PlayerInputHandler::ProcessButton

		std::atomic<bool> g_locked{ false };

		struct ProcessButtonHook
		{
			static void Thunk(RE::JumpHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
			{
				if (g_locked.load(std::memory_order_acquire)) {
					return;  // the root jump wildcards would otherwise cut the dodge off mid-roll
				}
				original(a_this, a_event, a_data);
			}

			static inline REL::Relocation<decltype(Thunk)> original;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_JumpHandler[0] };
		ProcessButtonHook::original = vtbl.write_vfunc(kProcessButtonVfunc, ProcessButtonHook::Thunk);
		logger::info("jump gate installed");
	}

	void SetLocked(bool locked)
	{
		g_locked.store(locked, std::memory_order_release);
	}
}
