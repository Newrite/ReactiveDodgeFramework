#pragma once

#include "core/Windows.h"

#include <cstdint>
#include <functional>

namespace rdf::game::graph
{
	// Everything here is per actor (player or NPC): an actor's graph events are counted under its ref handle.

	// Builds the BSFixedString names from Settings.
	void Init();
	// (Re-)registers the animation-graph sink on every graph of the actor; safe to call repeatedly.
	void RegisterSink(RE::Actor* actor);
	// Stops counting the actor's events (its dodge agent is gone).
	void UnregisterSink(RE::Actor* actor);
	// Main thread, every tick, for an actor with several graphs (the player: first / third person): only the
	// active one counts. Returns true when the active graph changed since the last call.
	bool TrackActiveGraph(RE::Actor* actor);

	// What the actor's graph reported since the last Take*.
	// Entered is the start/chain handshake. Depth counts dodge states the graph is in: +1 on
	// Entered, -1 on Left (clamped at 0). A chain A->B reports Entered(B) when the transition begins
	// and Left(A) when the blend ends, so depth goes 1 -> 2 -> 1; only 1 -> 0 is a real exit.
	bool TakeEntered(RE::Actor* actor);
	int  Depth(RE::Actor* actor);
	// Dev diagnostics: log every animation-graph event of the tracked actors while on.
	void LogEvents(bool on);
	bool DodgeWindowOpen(RE::Actor* actor);
	// RDF_* clip annotations seen since the last call, in WindowEvent order.
	void DrainClipWindows(RE::Actor* actor, const std::function<void(WindowEvent)>& sink);
	void ClearEvents(RE::Actor* actor);

	// The actor's current attack as its graph reported it ([AttackCancel]): a recovery event seen (and no end since),
	// and the attack clip's RDF_DodgeCancelOpen / Close state once it announced one.
	struct AttackWindows
	{
		bool                recovery{ false };
		std::optional<bool> clipCancel;
		std::uint32_t       starts{ 0 };  // attack-start events seen so far (changes when a new attack starts)
	};
	AttackWindows ReadAttack(RE::Actor* actor);
	// CastOn clip annotations seen since the last call (docs/CASTON_DESIGN.md 5), in order: kind 0 RDF_CastOnDodge,
	// 1 RDF_CastOnPerfectDodge, 2 RDF_CastOnClip (the clip places its own moments); tag = the annotation's payload.
	void DrainCastOn(RE::Actor* actor, const std::function<void(int kind, std::string_view tag)>& sink);
	// [Stagger] the stagger framework's recovery window: an open event since the stagger started (and no close), or a
	// recovery variable true.
	bool StaggerRecoveryOpen(RE::Actor* actor);
	// A new attack started (or none runs): its recovery and clip windows start over.
	void ResetAttack(RE::Actor* actor);
	void ClearAll();  // load / new game

	enum class DodgeEntry
	{
		StartA,  // root wildcard into dodge state A
		StartB,  // root wildcard into dodge state B
		Chain,   // from the dodge state the graph is in to the other one
	};
	// Where a dodge plays (the value of RDF_DodgeLayer).
	enum class Layer
	{
		Root,     // the root behavior's dodge states
		OneHand,  // 1hm_behavior's (BEHAVIOR_INTEGRATION.md 7), refused while 1hm does not run
		Bow,      // under a bow draw: Bow_Attack_LowerBodyBehavior (section 8), refused unless the bow is being drawn
		Magic,    // under a cast: DefaultMagicCastingBehavior (section 8), refused while magicbehavior does not run
	};
	const char* ToString(Layer layer);
	// Sets RDF_DodgeClip / RDF_DodgeSpeed / RDF_ArmBlend / RDF_ChainIndex / RDF_DodgeLayer, then sends the entry event.
	// False = the graph refused it. armStance: share (0..1) of the current weapon stance on the arms, over the dodge clip.
	// chainIndex: which dodge of the chain this is (0 = the first). Outside the root a chain is the start of the other
	// state (the caller passes StartA / StartB).
	bool SendStart(RE::Actor* actor, DodgeEntry entry, int clip, float animSpeed, float armStance, int chainIndex, Layer layer);
	// RDF_ChainIndex alone: restored after a refused chain, 0 once no dodge runs.
	void SetChainIndex(RE::Actor* actor, int chainIndex);
	// RDF_AttackFromDodge (0 none, 1 attack, 2 power attack) and RDF_AttackFromDodgeClip.
	void SetAttackFromDodge(RE::Actor* actor, int kind, int clip);
	// The graph value of RDF_ArmBlend that gives `stanceShare` of the stance on the arms.
	float ArmBlendWeight(float stanceShare);
	// The layer's exit event (the root and 1hm share one).
	void SendExit(RE::Actor* actor, Layer layer);
	// Every layer's exit: for a dodge state nobody owns (orphaned, abandoned).
	void SendAllExits(RE::Actor* actor);
	// RDF_DodgeLayer alone: restored after a refused start, Root once no dodge runs.
	void SetLayer(RE::Actor* actor, Layer layer);
	// RDF_IsDodging is set by the graph itself; read-only truth for "graph is in a dodge state".
	bool GraphIsDodging(RE::Actor* actor);
}
