#include "check.h"
#include "game/IFrameHookCode.h"

#include <array>
#include <cstdint>

namespace
{
	using namespace rdf::game::effects::code;
	const void* seenTarget;
	const void* seenContext;
	bool rejectArea;

	bool AreaPredicate(const void* target, const void* context)
	{
		seenTarget = target;
		seenContext = context;
		return rejectArea;
	}

	// Execute the production generated code in a synthetic engine frame. The
	// continuations restore our frame and return different markers; the delivery
	// continuation also exposes the displaced instruction's RCX output.
	struct AreaHarness : Xbyak::CodeGenerator
	{
		AreaHarness(const void* target, const void* context, std::uintptr_t* deliveredEffect)
		{
			Xbyak::Label deliver, skip, done, filter;
			push(r12);
			push(r13);
			sub(rsp, 0x28);  // align RSP as at the engine's mid-function entry
			mov(r12, reinterpret_cast<std::uintptr_t>(context));
			mov(r13, reinterpret_cast<std::uintptr_t>(target));
			jmp(filter);
			L(deliver);
			mov(rax, reinterpret_cast<std::uintptr_t>(deliveredEffect));
			mov(ptr[rax], rcx);
			mov(eax, 1);
			jmp(done);
			L(skip);
			mov(eax, 2);
			L(done);
			add(rsp, 0x28);
			pop(r13);
			pop(r12);
			ret();
			L(filter);
			AreaTargetFilter stub{ reinterpret_cast<std::uintptr_t>(AreaPredicate),
				reinterpret_cast<std::uintptr_t>(deliver.getAddress()), reinterpret_cast<std::uintptr_t>(skip.getAddress()) };
			db(stub.getCode(), stub.getSize());
			ready();
		}
	};

	const void* seenCollidable;
	const void* seenProjectile;
	void* RayFilter(const void* collidable, const void* projectile)
	{
		seenCollidable = collidable;
		seenProjectile = projectile;
		return rejectArea ? nullptr : const_cast<void*>(collidable);
	}
}

TEST(area_hook_preserves_engine_frame_and_replays_only_delivered_target)
{
	int target = 0;
	std::array<std::uintptr_t, 7> context{};
	context[2] = 0x245231093c0ULL;  // real context's Effect* at +0x10
	std::uintptr_t deliveredEffect = 0;
	AreaHarness harness{ &target, &context, &deliveredEffect };
	const auto run = harness.getCode<int (*)()>();
	for (bool reject : { false, true, false }) {
		rejectArea = reject;
		deliveredEffect = 0;
		CHECK(run() == (reject ? 2 : 1));
		CHECK(seenTarget == &target);
		CHECK(seenContext == &context);
		CHECK(deliveredEffect == (reject ? 0 : context[2]));
	}
}

TEST(ray_hook_passes_projectile_without_losing_collision_argument_or_return)
{
	int collidable = 0, projectile = 0;
	RayCandidateFilter stub{ reinterpret_cast<std::uintptr_t>(RayFilter) };
	Xbyak::CodeGenerator harness;
	harness.push(harness.rbx);
	harness.sub(harness.rsp, 0x20);
	harness.mov(harness.rbx, reinterpret_cast<std::uintptr_t>(&projectile));
	harness.mov(harness.rcx, reinterpret_cast<std::uintptr_t>(&collidable));
	harness.mov(harness.rax, reinterpret_cast<std::uintptr_t>(stub.getCode()));
	harness.call(harness.rax);
	harness.add(harness.rsp, 0x20);
	harness.pop(harness.rbx);
	harness.ret();
	harness.ready();
	const auto run = harness.getCode<void* (*)()>();
	for (bool reject : { true, false }) {
		rejectArea = reject;
		CHECK(run() == (reject ? nullptr : &collidable));
		CHECK(seenCollidable == &collidable);
		CHECK(seenProjectile == &projectile);
	}
}
