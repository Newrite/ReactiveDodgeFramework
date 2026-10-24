#pragma once

// [Perfect] bPerfectGhost: a perfect dodge leaves a frozen glowing silhouette of the dodger where it happened, through
// the Ghost Trails API plugin (GhostTrails.dll, API v1: S:\Programming\Ghost Trails API\docs\API.md). Without the plugin
// nothing happens beyond one log line. Any actor (player or NPC) that perfect-dodges, NPCs only with bPerfectGhostNPC.
//
// The provider clones the actor's 3D on the main thread, and a cold first clone can take a while: a silhouette asked for
// only at the perfect dodge could appear late, somewhere along the dodge. So every dodge start warms the actor's pool
// (Prepare); the perfect dodge then copies the pose of that moment into a warm slot.
namespace rdf::game::ghost
{
	// kPostPostLoad: asks GhostTrails.dll for its interface (null when absent or of another version).
	void Init();
	// Main thread. A dodge began: warm the actor's pool for the silhouettes a perfect dodge may ask for.
	void OnDodgeStart(RE::Actor* actor);
	// Main thread. The actor's dodge just turned perfect: leave its silhouettes there.
	void OnPerfectDodge(RE::Actor* actor);
	// Main thread, every frame, game seconds: releases the requests whose silhouettes have faded.
	void Update(float dt);
	// A load, a new game: the provider forgot every request already.
	void Reset();
}
