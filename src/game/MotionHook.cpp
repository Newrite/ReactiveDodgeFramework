#include "MotionHook.h"

#include "Settings.h"
#include "core/Math.h"
#include "core/Seqlock.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>

namespace rdf::game::motion
{
	namespace
	{
		constexpr std::uint32_t kBuildOutputId = 41160;       // MovementTweenerAgentAnimationDriven::BuildOutput
		constexpr std::uint32_t kProcessMotionDataId = 31949;  // Character::ProcessMotionData (diagnostics only)
		constexpr std::ptrdiff_t kCallOffset = 0x111;          // 0x1407021F1 in 1.5.97

		// One slot per actor whose dodge drives its motion (the player and any NPC dodging through the API).
		// The main thread writes, the movement threads read. A slot is matched by the actor's FormID, the cheapest
		// key in this hot path (it runs for every animation-driven actor every frame); the command repeats the
		// owner, so a slot reused by another actor in between is never applied to the wrong one.
		constexpr std::size_t kSlots = 32;
		struct Slot
		{
			std::atomic<std::uint32_t> owner{ 0 };
			Seqlock<MotionCommand>     box;
		};
		std::array<Slot, kSlots>   g_slots;
		std::atomic<int>           g_used{ 0 };
		std::atomic<std::uint32_t> g_generation{ 1 };

		bool ReadFor(const RE::Actor* actor, MotionCommand& cmd)
		{
			if (!actor || g_used.load(std::memory_order_acquire) == 0) {
				return false;
			}
			const auto id = actor->GetFormID();
			for (const auto& slot : g_slots) {
				if (slot.owner.load(std::memory_order_acquire) == id) {
					return slot.box.TryRead(cmd) && cmd.ownerFormId == id && cmd.generation == g_generation.load(std::memory_order_acquire);
				}
			}
			return false;
		}

		std::atomic<std::uint32_t> g_calls{ 0 };
		std::atomic<std::uint32_t> g_overrides{ 0 };
		std::atomic<std::uint32_t> g_firstThread{ 0 };
		std::atomic<std::uint32_t> g_lastThread{ 0 };
		std::atomic<std::uint32_t> g_lastLocalX{ 0 }, g_lastLocalY{ 0 }, g_lastYaw{ 0 };
		std::atomic<float>         g_yawFactor{ 1.0f };

		using ProcessMotionData_t = bool (*)(RE::Actor*, float, RE::NiPoint3*, RE::NiPoint3*, bool*);
		ProcessMotionData_t g_original = nullptr;

		// MovementControllerAI::BuildAggregateStatePayload(this, dt, payload). The caller,
		// Character::ApplyMovementDeltaAndFeedback (0x1405D87F0), moves the actor by
		// direction(payload+0x00) * speed(payload+0x0C) * dt (speed read at 0x1405D8AC0).
		constexpr std::size_t kPayloadVfunc = 7;
		constexpr std::size_t kPayloadSpeed = 0x0C;
		using BuildPayload_t = std::uintptr_t (*)(RE::MovementControllerNPC*, float, std::uint8_t*);
		BuildPayload_t g_originalPayload = nullptr;
		std::atomic<std::uint32_t> g_floorHits{ 0 };

		std::uint32_t Bits(float f)
		{
			std::uint32_t u;
			std::memcpy(&u, &f, sizeof(u));
			return u;
		}

		float FromBits(std::uint32_t u)
		{
			float f;
			std::memcpy(&f, &u, sizeof(f));
			return f;
		}

		// Runs on whatever thread drives the movement update. Reads only the immutable snapshot and its
		// own arguments; no allocation, no locks, no logging.
		bool Thunk(RE::Actor* actor, float dt, RE::NiPoint3* outTranslation, RE::NiPoint3* outRotation, bool* outFlag)
		{
			g_calls.fetch_add(1, std::memory_order_relaxed);
			const auto thread = ::GetCurrentThreadId();
			std::uint32_t expected = 0;
			g_firstThread.compare_exchange_strong(expected, thread, std::memory_order_relaxed);
			g_lastThread.store(thread, std::memory_order_relaxed);

			MotionCommand cmd;
			const bool owned = actor && outTranslation && outRotation && dt > 0.0f && ReadFor(actor, cmd) && cmd.active;
			if (!owned) {
				return g_original(actor, dt, outTranslation, outRotation, outFlag);
			}

			// World velocity -> actor-local displacement for this dt, using the heading at the middle of
			// the step because the yaw delta is applied during the same step.
			float lookYaw = 0.0f;
			if (cmd.lookYawRate != 0.0f) {
				if (const auto* controls = RE::PlayerControls::GetSingleton()) {
					lookYaw = controls->data.lookInputVec.x * cmd.lookYawRate;
				}
			}
			const float yawStep = (cmd.yawRate + lookYaw) * dt;
			const float heading = actor->data.angle.z + 0.5f * yawStep;
			const Vec2  local = WorldToLocal({ cmd.vx, cmd.vy }, heading);
			const float yaw = g_yawFactor.load(std::memory_order_relaxed) * yawStep;

			*outTranslation = { local.x * dt, local.y * dt, 0.0f };
			*outRotation = { 0.0f, 0.0f, yaw };
			if (outFlag) {
				*outFlag = false;
			}

			g_overrides.fetch_add(1, std::memory_order_relaxed);
			g_lastLocalX.store(Bits(outTranslation->x), std::memory_order_relaxed);
			g_lastLocalY.store(Bits(outTranslation->y), std::memory_order_relaxed);
			g_lastYaw.store(Bits(yaw), std::memory_order_relaxed);
			return true;
		}
	}

	namespace
	{
		std::uintptr_t PayloadThunk(RE::MovementControllerNPC* self, float dt, std::uint8_t* payload)
		{
			const auto result = g_originalPayload(self, dt, payload);
			MotionCommand cmd;
			// The payload is only built for controls-driven movement, so the floor can be published while the
			// dodge still owns the animation-driven motion: it is in place on the first engine-driven frame.
			if (!payload || !self->actor || !ReadFor(self->actor, cmd) || !(cmd.speedFloor > 0.0f)) {
				return result;
			}
			auto* speed = reinterpret_cast<float*>(payload + kPayloadSpeed);
			if (*speed < cmd.speedFloor) {
				*speed = cmd.speedFloor;
				g_floorHits.fetch_add(1, std::memory_order_relaxed);
			}
			return result;
		}
	}

	bool InstallSpeedFloor()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MovementControllerNPC[0] };
		g_originalPayload = reinterpret_cast<BuildPayload_t>(vtbl.write_vfunc(kPayloadVfunc, PayloadThunk));
		logger::info("speed floor hook installed (MovementControllerNPC vfunc {})", kPayloadVfunc);
		return true;
	}

	bool Install()
	{
		const REL::Relocation<std::uintptr_t> function{ REL::ID(kBuildOutputId) };
		const auto                           site = function.address() + kCallOffset;
		const auto*                          bytes = reinterpret_cast<const std::uint8_t*>(site);
		logger::info("motion hook site {:#x}: {:02X} {:02X} {:02X} {:02X} {:02X}", site, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
		if (bytes[0] != 0xE8) {
			logger::error("motion hook: expected CALL (E8) at the site, not installing");
			return false;
		}
		g_yawFactor.store(static_cast<float>(Settings::Get().yawSign), std::memory_order_relaxed);
		REL::Relocation<std::uintptr_t> callSite{ site };
		// Preserve the current CALL target, including another plugin's trampoline. Thunk
		// chains to it whenever RDF does not own this actor's dodge motion.
		g_original = reinterpret_cast<ProcessMotionData_t>(callSite.write_call<5>(Thunk));
		const REL::Relocation<std::uintptr_t> vanilla{ REL::ID(kProcessMotionDataId) };
		const auto                           chained = reinterpret_cast<std::uintptr_t>(g_original);
		if (chained == vanilla.address()) {
			logger::info("motion hook installed (calls the game's ProcessMotionData {:#x})", chained);
		} else {
			// An SKSE branch trampoline (jmp [rip+0] + absolute address) lives outside any module: follow it once.
			auto        target = chained;
			const auto* code = reinterpret_cast<const std::uint8_t*>(chained);
			if (code[0] == 0xFF && code[1] == 0x25 && code[2] == 0 && code[3] == 0 && code[4] == 0 && code[5] == 0) {
				std::memcpy(&target, code + 6, sizeof(target));
			}
			HMODULE owner = nullptr;
			char    path[MAX_PATH]{};
			if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(target), &owner) && owner) {
				::GetModuleFileNameA(owner, path, MAX_PATH);
			}
			const char* name = std::strrchr(path, '\\');
			logger::info("motion hook installed over another plugin's hook: chains to {:#x} ({}), not the game's {:#x}", chained,
				name ? name + 1 : (path[0] ? path : "unknown module"), vanilla.address());
		}
		return true;
	}

	void Publish(MotionCommand cmd, std::uint32_t ownerFormId)
	{
		cmd.ownerFormId = ownerFormId;
		cmd.generation = g_generation.load(std::memory_order_relaxed);
		Slot* free = nullptr;
		for (auto& slot : g_slots) {
			const auto owner = slot.owner.load(std::memory_order_relaxed);
			if (owner == ownerFormId) {
				slot.box.Write(cmd);
				return;
			}
			if (owner == 0 && !free) {
				free = &slot;
			}
		}
		if (!free) {
			static bool warned = false;
			if (!std::exchange(warned, true)) {
				logger::warn("motion hook: more than {} actors dodging at once; {:08X} is not driven", kSlots, ownerFormId);
			}
			return;
		}
		free->box.Write(cmd);
		free->owner.store(ownerFormId, std::memory_order_release);
		g_used.fetch_add(1, std::memory_order_acq_rel);
	}

	void Release(std::uint32_t ownerFormId)
	{
		for (auto& slot : g_slots) {
			if (slot.owner.load(std::memory_order_relaxed) == ownerFormId) {
				slot.box.Write(MotionCommand{});
				slot.owner.store(0, std::memory_order_release);
				g_used.fetch_sub(1, std::memory_order_acq_rel);
				return;
			}
		}
	}

	void Reset()
	{
		g_generation.fetch_add(1, std::memory_order_release);
		for (auto& slot : g_slots) {
			slot.box.Write(MotionCommand{});
			slot.owner.store(0, std::memory_order_release);
		}
		g_used.store(0, std::memory_order_release);
	}

	HookStats ReadStats()
	{
		HookStats s;
		s.calls = g_calls.load(std::memory_order_relaxed);
		s.overrides = g_overrides.load(std::memory_order_relaxed);
		s.floorHits = g_floorHits.load(std::memory_order_relaxed);
		s.firstThread = g_firstThread.load(std::memory_order_relaxed);
		s.lastThread = g_lastThread.load(std::memory_order_relaxed);
		s.lastLocalX = FromBits(g_lastLocalX.load(std::memory_order_relaxed));
		s.lastLocalY = FromBits(g_lastLocalY.load(std::memory_order_relaxed));
		s.lastYaw = FromBits(g_lastYaw.load(std::memory_order_relaxed));
		return s;
	}
}
