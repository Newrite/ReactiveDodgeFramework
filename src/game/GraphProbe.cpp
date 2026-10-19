#include "GraphProbe.h"

#include <cstring>
#include <format>
#include <string>

namespace rdf::game::probe
{
	namespace
	{
		// hkbStateMachine::ActiveTransitionInfo (Havok 2010) is not modelled by CommonLib; the transition
		// effect pointer is its first member. Every effect is checked by vtable before it is read.
		constexpr std::size_t kActiveTransitionStride = 0x28;

		RE::hkbStateMachine* FindStateMachine(RE::hkbBehaviorGraph* graph, const char* name)
		{
			if (!graph || !graph->activeNodes) {
				return nullptr;
			}
			const auto& nodes = *graph->activeNodes;
			for (std::int32_t i = 0; i < nodes.size(); ++i) {
				const auto& info = nodes.data()[i];
				const auto* tmpl = info.nodeTemplate;
				const char* nodeName = tmpl ? tmpl->name.data() : nullptr;
				if (nodeName && std::strcmp(nodeName, name) == 0) {
					return static_cast<RE::hkbStateMachine*>(info.nodeClone ? info.nodeClone : info.nodeTemplate);
				}
			}
			return nullptr;
		}

		RE::hkbBehaviorGraph* RootGraph(RE::PlayerCharacter* player)
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!player->GetAnimationGraphManager(manager) || !manager || manager->graphs.empty() ||
				manager->activeGraph >= manager->graphs.size()) {
				return nullptr;
			}
			const auto& graph = manager->graphs[manager->activeGraph];
			return graph ? graph->behaviorGraph : nullptr;
		}
	}

	void LogRootTransitions(RE::PlayerCharacter* player)
	{
		auto* sm = FindStateMachine(RootGraph(player), "RootBehaviorGraph");
		if (!sm) {
			logger::info("{}", "trace root: RootBehaviorGraph not among the active nodes");
			return;
		}
		static const REL::Relocation<std::uintptr_t> blendVtable{ RE::VTABLE_hkbBlendingTransitionEffect[0] };

		const auto* raw = reinterpret_cast<const std::uint8_t*>(sm->activeTransitions.data());
		const auto  count = sm->activeTransitions.size();
		std::string details;
		for (std::int32_t i = 0; raw && i < count && i < 4; ++i) {
			const auto* entry = raw + i * kActiveTransitionStride;
			const auto* effect = *reinterpret_cast<RE::hkbTransitionEffect* const*>(entry);
			if (!effect || *reinterpret_cast<const std::uintptr_t*>(effect) != blendVtable.address()) {
				details += std::format(" [{}: not a blend effect]", i);
				continue;
			}
			const auto* blend = static_cast<const RE::hkbBlendingTransitionEffect*>(effect);
			details += std::format(" [{}: '{}' dur={:.3f} remaining={:.3f} inTransition={:.3f} ids?={}/{}]", i,
				blend->name.data() ? blend->name.data() : "?", blend->duration, blend->timeRemaining, blend->timeInTransition,
				*reinterpret_cast<const std::int32_t*>(entry + 0x1C), *reinterpret_cast<const std::int32_t*>(entry + 0x20));
		}
		logger::info("trace root state={} transitions={}{}", sm->currentStateID, count, details);
	}
}
