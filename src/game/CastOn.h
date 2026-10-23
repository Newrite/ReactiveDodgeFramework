#pragma once

#include "GraphCoordinator.h"
#include "core/CastOnRules.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// CastOn rules in game (docs/CASTON_DESIGN.md): the rule files compiled against the loaded forms, the conditions
// evaluated on a trigger, the casts and their cost. Main thread only.
namespace rdf::game::caston
{
	using rdf::caston::Trigger;

	// What a trigger knows. The actors may be null (no attacker on a plain dodge, no combat target out of combat).
	struct Context
	{
		RE::Actor*   dodger{ nullptr };
		RE::Actor*   attacker{ nullptr };
		RE::Actor*   combatTarget{ nullptr };
		graph::Layer layer{ graph::Layer::Root };
		int          clip{ 0 };
		int          chainIndex{ 0 };
		bool         fromStagger{ false };
		bool         firstPerson{ false };
	};

	struct RuleSet;  // compiled; immutable, replaced whole by Load
	using RuleSetPtr = std::shared_ptr<const RuleSet>;

	// kDataLoaded (and later reloads): reads SKSE/Plugins/ReactiveDodgeFramework/CastOn/*.json (a <name>.user.json beside a
	// <name>.json replaces it), resolves the forms and compiles. A rule with a missing form is dropped and logged.
	void Load();
	RuleSetPtr Current();

	// Every frame (game seconds, not while paused): ends the concentration spells CastOn started once their duration is
	// over, or (Engine cost) once the caster has nothing left to pay.
	void UpdateChannels(float dt);
	// Load / new game: forgets them (the casters of the last session are gone).
	void ClearChannels();

	// The CastOn state of one dodging actor (its DodgeAgent owns it): evaluates a trigger's rules, keeps their limits,
	// and fires the casts at their moment.
	class Scheduler
	{
	public:
		// A trigger happened at `now` (seconds, the agent's clock): the rules of the trigger whose conditions and limits
		// hold are scheduled. Dodge: call it once the dodge started; it also forgets the last dodge's annotations.
		void OnTrigger(Trigger trigger, const Context& ctx, double now);
		// A CastOn clip annotation (RDF_CastOnDodge / RDF_CastOnPerfectDodge, payload = tag) or the RDF_CastOnClip
		// declaration (the clip places its own moments) of the dodge playing.
		void OnAnnotation(Trigger trigger, std::string_view tag);
		void OnClipPlacesMoments() { clipPlaces_ = true; }
		// Every tick: fires what is due. `entered`: the graph has entered the dodge (its clip's declaration is known by then).
		void Update(RE::Actor* dodger, double now, bool entered);
		// The dodge is over (stopped, or a chain replaced it): whatever still waits fires now.
		void OnDodgeEnd(RE::Actor* dodger);
		// The chain is over: once-per-chain rules may fire again.
		void OnChainOver() { firedInChain_.clear(); }
		// The dodge never started (the graph refused it): its Dodge casts are dropped.
		void CancelDodge();
		void Clear();

	private:
		struct Pending
		{
			RuleSetPtr          set;
			std::uint32_t       rule{ 0 };
			Trigger             trigger{ Trigger::Dodge };
			RE::ObjectRefHandle dodger, attacker, combatTarget;
			Context             facts;  // the dodge facts at the trigger (actors re-read from the handles)
			double              triggeredAt{ 0.0 };
			bool                annotationArrived{ false };
		};

		void Fire(const Pending& p);
		bool Waits(const Pending& p, double now, bool entered) const;

		std::vector<Pending>                    pending_;
		std::unordered_map<std::uint64_t, double> lastFired_;   // rule key -> trigger time (cooldowns)
		std::unordered_set<std::uint64_t>       firedInChain_;
		std::vector<std::pair<Trigger, std::string>> seen_;     // annotations of this dodge (a PerfectDodge may come after its own)
		bool                                    clipPlaces_{ false };
	};
}
