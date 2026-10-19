#include "LockOn.h"

#include "Settings.h"
#include "TrueDirectionalMovementAPI.h"

#include <Windows.h>

#include <cmath>

namespace rdf::game::lockon
{
	namespace
	{
		class NullProvider final : public Provider
		{
		public:
			bool TargetYaw(RE::PlayerCharacter*, float&) override { return false; }
		};

		class TdmProvider final : public Provider
		{
		public:
			explicit TdmProvider(TDM_API::IVTDM1* api) :
				api_(api) {}

			bool TargetYaw(RE::PlayerCharacter* player, float& yaw) override
			{
				if (!api_->GetTargetLockState()) {
					return false;
				}
				const auto handle = api_->GetCurrentTarget();
				const auto target = handle.get();
				if (!target || target.get() == player) {
					return false;
				}
				const auto delta = target->GetPosition() - player->GetPosition();
				yaw = std::atan2(delta.x, delta.y);
				return true;
			}

		private:
			TDM_API::IVTDM1* api_;
		};

		NullProvider g_null;
		Provider*    g_provider = &g_null;
	}

	void Init()
	{
		if (!Settings::Get().useTrueDirectionalMovement) {
			logger::info("lock-on: True Directional Movement integration disabled in the ini");
			return;
		}
		// RequestPluginAPI would query a null module handle when TDM is absent.
		if (!::GetModuleHandleA("TrueDirectionalMovement.dll")) {
			logger::info("lock-on: True Directional Movement not loaded");
			return;
		}
		auto* api = static_cast<TDM_API::IVTDM1*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V1));
		if (!api) {
			logger::warn("lock-on: True Directional Movement is loaded but gave no API");
			return;
		}
		static TdmProvider provider(api);
		g_provider = &provider;
		logger::info("lock-on: using True Directional Movement target lock");
	}

	bool TargetYaw(RE::PlayerCharacter* player, float& yaw)
	{
		return g_provider->TargetYaw(player, yaw);
	}
}
