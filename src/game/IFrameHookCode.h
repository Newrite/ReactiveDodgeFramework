#pragma once

#include <cstdint>
#include <xbyak/xbyak.h>

namespace rdf::game::effects::code
{
	// SE 1.5.97, 33686 + 0x10B. Entered by JMP with the engine's call-aligned
	// stack, R13 = candidate reference and R12 = area delivery context. All
	// volatile registers are dead here: the next instruction calls the effect's
	// magnitude getter. Callee preserves the live nonvolatile registers/XMM6+.
	struct AreaTargetFilter : Xbyak::CodeGenerator
	{
		AreaTargetFilter(std::uintptr_t predicate, std::uintptr_t deliver, std::uintptr_t skip)
		{
			sub(rsp, 0x20);  // shadow space; no extra return address at this JMP entry
			mov(rcx, r13);
			mov(rdx, r12);
			mov(rax, predicate);
			call(rax);
			add(rsp, 0x20);
			test(al, al);
			jnz("skip");
			mov(rcx, ptr[r12 + 0x10]);  // displaced 49 8B 4C 24 10
			mov(rax, deliver);
			jmp(rax);
			L("skip");
			mov(rax, skip);
			jmp(rax);
			ready();
		}
	};

	// SE 1.5.97, 42983 + 0x1BC. The original argument (RCX = collidable)
	// remains intact. RBX holds the projectile. Tail-call retains the original
	// call's shadow space, alignment and return address.
	struct RayCandidateFilter : Xbyak::CodeGenerator
	{
		explicit RayCandidateFilter(std::uintptr_t filter)
		{
			mov(rdx, rbx);
			mov(rax, filter);
			jmp(rax);
			ready();
		}
	};
}
