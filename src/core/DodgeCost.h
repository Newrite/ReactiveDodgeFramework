#pragma once

#include <algorithm>
#include <cstdint>

namespace rdf
{
	// What happens when the resource does not cover a dodge ([Cost] sShortfall).
	enum class CostShortfall : std::uint8_t
	{
		Refuse,   // the resource must cover the whole cost
		Minimum,  // allowed from fMinimum of the resource up; what is missing is not taken
		Always,   // never refused
	};

	// [Cost] (and [Stagger] fEscapeCost*): the price of one dodge. Defaults keep the old fixed price.
	struct CostConfig
	{
		float         flat{ 20.0f };               // fCost
		float         maxPercent{ 0.0f };          // fCostMaxPercent: % of the resource's maximum
		float         weightMult{ 0.0f };          // fCostWeightMult: x the equipped weight
		float         chainStep{ 0.0f };           // fChainCostStep: each dodge further down a chain costs this much more (x)
		float         overencumberedMult{ 1.0f };  // fOverencumberedMult: x while over-encumbered
		float         min{ 0.0f };                 // fCostMin
		float         max{ 0.0f };                 // fCostMax, 0 = no cap
		float         escapeFlat{ 0.0f };          // [Stagger] fEscapeCost: added when the dodge cuts a stagger short
		float         escapeMult{ 1.0f };          // [Stagger] fEscapeCostMult: x the cost, escape part included, when it does
		CostShortfall shortfall{ CostShortfall::Minimum };
		float         minimum{ 10.0f };            // fMinimum (Minimum)
		float         perfectRefund{ 0.0f };       // fPerfectRefund: % of the dodge's cost given back on a perfect dodge
	};

	struct CostInputs
	{
		float resourceMax{ 0.0f };      // the resource's maximum (permanent + temporary)
		float equippedWeight{ 0.0f };
		int   chainIndex{ 0 };          // 0 = a fresh dodge, 1 = the first chained one...
		bool  overencumbered{ false };
		bool  escape{ false };          // the dodge cuts a stagger short
		float escapeBonus{ 0.0f };      // the magnitudes of the effects with sEscapeCostKeyword
		// The actor's effects with the cost keywords (summed magnitudes, Detrimental ones negative):
		float costBonus{ 0.0f };        // sCostKeyword: added to the base
		float costPercent{ 0.0f };      // sCostMultKeyword: x (1 + this / 100)
		float chainPercent{ 0.0f };     // sChainCostKeyword: added to fChainCostStep, in percent
		float escapePercent{ 0.0f };    // [Stagger] sEscapeCostMultKeyword: x (1 + this / 100) on fEscapeCostMult
	};

	//   (fCost + fCostMaxPercent% x max + fCostWeightMult x equipped weight + sCostKeyword)
	//   x (1 + (fChainCostStep + sChainCostKeyword %) x chain index) x fOverencumberedMult (over-encumbered)
	//   x (1 + sCostMultKeyword %)
	//   escaping a stagger: (+ max(0, fEscapeCost + sEscapeCostKeyword)) x fEscapeCostMult x (1 + sEscapeCostMultKeyword %)
	//   clamped to [fCostMin, fCostMax] (fCostMax 0 = no cap), never below 0
	inline float DodgeCost(const CostConfig& c, const CostInputs& in)
	{
		const auto  factor = [](float percent) { return std::max(1.0f + percent * 0.01f, 0.0f); };
		float       cost = c.flat + c.maxPercent * 0.01f * in.resourceMax + c.weightMult * in.equippedWeight + in.costBonus;
		const float step = std::max(c.chainStep + in.chainPercent * 0.01f, 0.0f);
		cost *= 1.0f + step * static_cast<float>(std::max(in.chainIndex, 0));
		if (in.overencumbered) {
			cost *= c.overencumberedMult;
		}
		cost *= factor(in.costPercent);
		if (in.escape) {
			cost = (cost + std::max(0.0f, c.escapeFlat + in.escapeBonus)) * c.escapeMult * factor(in.escapePercent);
		}
		cost = std::max(cost, c.min);
		if (c.max > 0.0f) {
			cost = std::min(cost, c.max);
		}
		return std::max(cost, 0.0f);
	}

	// Whether `current` of the resource lets a dodge costing `cost` start.
	inline bool CostAllows(const CostConfig& c, float current, float cost)
	{
		switch (c.shortfall) {
		case CostShortfall::Refuse: return current >= cost;
		case CostShortfall::Always: return true;
		default: return current >= c.minimum;
		}
	}
}
