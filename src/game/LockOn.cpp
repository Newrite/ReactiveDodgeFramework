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
			bool FreeRotation() override { return false; }  // vanilla: the camera turns the player
			void AcquireYaw() override {}
			void ReleaseYaw() override {}
		};

		class TdmProvider final : public Provider
		{
		public:
			explicit TdmProvider(TDM_API::IVTDM2* api) :
				api_(api) {}

			bool FreeRotation() override { return api_->GetDirectionalMovementState(); }

			void AcquireYaw() override
			{
				if (owned_) {
					return;
				}
				const auto result = api_->RequestYawControl(SKSE::GetPluginHandle(), 0.0f);
				owned_ = result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven;
				if (!owned_) {
					logger::warn("lock-on: True Directional Movement refused yaw control ({})", static_cast<int>(result));
				}
			}

			void ReleaseYaw() override
			{
				if (owned_) {
					api_->ReleaseYawControl(SKSE::GetPluginHandle());
					owned_ = false;
				}
			}

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
			TDM_API::IVTDM2* api_;
			bool             owned_{ false };
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
		auto* api = static_cast<TDM_API::IVTDM2*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V2));
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

	bool FreeRotation() { return g_provider->FreeRotation(); }
	void AcquireYaw() { g_provider->AcquireYaw(); }
	void ReleaseYaw() { g_provider->ReleaseYaw(); }
}
