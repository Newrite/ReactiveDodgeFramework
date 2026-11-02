#include "EngineState.h"

#include "Settings.h"

namespace rdf::game::engine
{
	namespace
	{
		EngineBlock Classify(RE::Actor* p, const RE::ActorState* state, bool ownDodge)
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
			bool graphStagger = false;
			if (state->IsStaggered() || (!ownDodge && p->GetGraphVariableBool("IsStaggering"sv, graphStagger) && graphStagger)) {
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
			// The game's own lock on the player (ControlMap::enabledControls +0x118, verified in IDA through GetControlsState
			// 67248 / ToggleControls 67245): a scene or a script that took movement or looking away also takes the dodge.
			if (p->IsPlayerRef() && Settings::Get().respectDisabledControls) {
				const auto* map = RE::ControlMap::GetSingleton();
				if (map && (!map->IsMovementControlsEnabled() || !map->IsLookingControlsEnabled())) {
					return EngineBlock::ControlsDisabled;
				}
			}
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (p->IsPlayerRef() && !Settings::Get().allowFirstPerson && camera && camera->IsInFirstPerson()) {
				return EngineBlock::FirstPerson;  // no first-person graph support yet
			}
			return EngineBlock::None;
		}
	}

	Snapshot Read(RE::Actor* player, bool ownDodge)
	{
		const auto* state = player->AsActorState();
		Snapshot    s;
		s.block = Classify(player, state, ownDodge);
		s.attackState = static_cast<std::uint8_t>(state->GetAttackState());
		const auto resource = Settings::Get().costResource;
		s.resource = resource ? player->AsActorValueOwner()->GetActorValue(*resource) : 0.0f;
		return s;
	}

	void EndStagger(RE::Actor* player)
	{
		player->AsActorState()->actorState2.staggered = 0;
	}

	bool LeftButtonCasts(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		// The engine's own first test is `(actorState2 & 0x100) == 0` (ActorState2::wantBlocking, Actor+0xC4 bit 8):
		// while a block is wanted -- the player's, or a block-button mod's such as CGO's / Input Support's -- the left
		// press goes to the attack/block path instead, so it is not a cast and this whole question is moot.
		if (actor->AsActorState()->actorState2.wantBlocking) {
			return false;
		}
		const auto  source = static_cast<std::size_t>(RE::MagicSystem::CastingSource::kLeftHand);
		const auto* spell = actor->selectedSpells[source];
		return spell && spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment;
	}

	float LookTurnRate(RE::PlayerCharacter* player)
	{
		constexpr std::size_t kGetMovementData = 8;
		constexpr std::size_t kRotateInPlaceRun = 9;  // speeds[4][2], rotateInPlaceWalk, rotateInPlaceRun, ...
		using GetMovementData_t = bool (*)(RE::ActorState*, float*);
		if (!player) {
			return 0.0f;
		}
		auto* state = player->AsActorState();
		alignas(16) float data[16]{};  // the callee writes 56 bytes
		const auto fn = reinterpret_cast<GetMovementData_t>((*reinterpret_cast<std::uintptr_t**>(state))[kGetMovementData]);
		if (!fn(state, data)) {
			return 0.0f;
		}
		const float rate = data[kRotateInPlaceRun];
		return std::isfinite(rate) && rate > 0.0f && rate < 100.0f ? rate : 0.0f;
	}
}
