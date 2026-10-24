#pragma once

// The actor's active magic effects, read one way everywhere (keywords on effects, magnitudes for the stagger rules and
// CastOn conditions). What the engine does, verified in IDA (SE 1.5.97, docs/CASTON_DESIGN.md 15):
// - An effect whose spell's conditions fail stays in the list with ActiveEffect flag kInactive (0x8000 at +0x7C, set
//   by vfunc 5); a dispelled one carries kDispelled (0x40000) until it is removed. The engine's HasMagicEffect counts
//   both. Here they do not count unless asked.
// - ActiveEffect::GetMagnitude (33282 = 0x14053E120) is the magnitude now: as cast when the effect recovers (ActiveEffect
//   flag 0x200), else tapered by the base effect's taper curve over the time left, so it changes while the effect runs.
// - A value modifier (ValueModifierEffect::OnAdd, 0x140567470) stores its magnitude negated when the base effect is
//   Detrimental (the other way round for an actor value flagged as inverted); other archetypes keep the sign as cast.
// - A value modifier with Recover changes its value once at the start and gives it back at the end; without Recover and
//   with a duration it changes the value by its magnitude every second (ShouldModifyOnStart / OnUpdate, vfuncs 1A / 1C).
namespace rdf::game::magic
{
	// How a magnitude is read.
	enum class Reading
	{
		// The number the spell gives (perks, effectiveness and taper applied) with the author's sign: the engine's flip
		// for a detrimental value modifier undone. For effects that carry a number (a delay, a cost bonus).
		Authored,
		// What the effect does to its value: a Detrimental effect subtracts, of any archetype. A value modifier without
		// Recover is a rate per second.
		Applied,
	};

	// Which effects: with this keyword on the base effect, and/or of this base effect. `inactive`: also the effects
	// their spell's conditions switched off and the dispelled ones (the engine's own view). `recoverOnly`: only those
	// whose base effect has Recover (an amount that holds while the effect lasts, not one that keeps changing a value).
	struct Filter
	{
		const RE::BGSKeyword*    keyword{ nullptr };
		const RE::EffectSetting* effect{ nullptr };
		bool                     inactive{ false };
		bool                     recoverOnly{ false };
	};

	struct Sum
	{
		int   count{ 0 };  // 0: none matched (total / lowest / highest mean nothing)
		float total{ 0.0f };
		float lowest{ 0.0f };
		float highest{ 0.0f };
	};

	// Whether an effect counts (active and not dispelled, unless `inactive`).
	bool Counts(RE::ActiveEffect* effect, bool inactive);
	float MagnitudeOf(RE::ActiveEffect* effect, Reading reading);

	// Main thread (the walk is MagicTarget::VisitActiveEffects, the engine's own).
	bool Any(RE::Actor* actor, const Filter& filter);
	Sum  Query(RE::Actor* actor, const Filter& filter, Reading reading);
}
