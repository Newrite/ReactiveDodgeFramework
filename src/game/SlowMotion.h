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

	// How much faster than the world the player's own updates run right now (playerMagicTimeSlowdown /
	// magicTimeSlowdown), 1 when no magic slowdown is up. The engine itself applies this factor to the player's frame
	// delta before PlayerCharacter::Update (vfunc 0xAD) and to the player's movement job, so a player-side clock is
	// already on it and must not apply it again -- only the world's dt for an actor that does not get it has to.
	float PlayerOverWorld();

	// The world's frame delta that goes with a player delta of `playerDt` (an NPC dodge, ticked from the player's
	// update, runs on the world clock). `playerDt` unchanged when no magic slowdown runs.
	float WorldDt(float playerDt);

	// The engine's pair as it stands now, world first (0 = no magic slowdown); dev trace and diagnostics only.
	void Read(float& world, float& player);
}
