#include "EngineState.h"

#include "Settings.h"

namespace rdf::game::engine
{
	namespace
	{
		EngineBlock Classify(RE::PlayerCharacter* p, const RE::ActorState* state)
		{
			if (p->IsDead() || state->IsBleedingOut() || state->IsUnconscious()) {
				return EngineBlock::Dead;
			}
			if (p->IsInKillMove()) {
				return EngineBlock::KillMove;
			}
			if (p->IsInRagdollState() || state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) {
				return EngineBlock::Ragdoll;
			}
			if (state->IsStaggered() || p->IsStaggering()) {
				return EngineBlock::Staggered;
			}
			if (p->IsOnMount()) {
				return EngineBlock::Mounted;
			}
			if (state->IsSwimming()) {
				return EngineBlock::Swimming;
			}
			if (state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
				return EngineBlock::Sitting;
			}
			bool jumping = false;
			if (p->GetGraphVariableBool("bInJumpState"sv, jumping) && jumping) {
				return EngineBlock::Jumping;
			}
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (!Settings::Get().allowFirstPerson && camera && camera->IsInFirstPerson()) {
				return EngineBlock::FirstPerson;  // no first-person graph support yet
			}
			return EngineBlock::None;
		}
	}

	Snapshot Read(RE::PlayerCharacter* player)
	{
		const auto* state = player->AsActorState();
		Snapshot    s;
		s.block = Classify(player, state);
		s.attackState = static_cast<std::uint8_t>(state->GetAttackState());
		const auto resource = Settings::Get().costResource;
		s.resource = resource ? player->AsActorValueOwner()->GetActorValue(*resource) : 0.0f;
		return s;
	}
}
