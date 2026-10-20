#pragma once

namespace rdf::game::jump
{
	// Vtable hook on JumpHandler::ProcessButton (vfunc 4, verified in IDA: 0x1407090A0 calls Actor::Jump
	// with no knowledge of a dodge). While locked, the jump button is swallowed.
	void Install();
	// Main thread, every tick: lock while a dodge runs and its exit window is still closed.
	void SetLocked(bool locked);
}
