#pragma once

namespace rdf::game::slowmo
{
	// Perfect-dodge slow motion through the engine's own magic slowdown (VATS::SetMagicTimeSlowdown, 43103, the call
	// the Slow Time shout makes): the world runs at fPerfectSlowMotion, the player at fPerfectSlowMotionPlayer, both
	// in real-time terms. Eased in and out; timing in real seconds. Main thread.
	void Start();
	// Every player tick: advances the envelope, ends it, notices someone else (the shout) taking the slowdown over.
	void Update();
	// Load / new game: ends a slow motion of ours at once.
	void Stop();
	// How much faster than the world the player runs right now (the engine scales the player's own updates by it,
	// ours or the shout's): the player's dodge clock has to follow.
	float PlayerDtScale();
}
