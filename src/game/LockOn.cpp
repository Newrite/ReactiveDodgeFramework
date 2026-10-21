#include "LockOn.h"

#include "Settings.h"
#include "TrueDirectionalMovementAPI.h"

#include <Windows.h>


namespace rdf::game::lockon
{
	namespace
	{
		class NullProvider final : public Provider
		{
		public:
			bool LockedOn(RE::PlayerCharacter*) override { return false; }
			bool FreeRotation() override { return false; }  // vanilla: the camera turns the player
			void AcquireYaw() override {}
			void ReleaseYaw() override {}
		};

		class TdmProvider final : public Provider
		{
		public:
			explicit TdmProvider(TDM_API::IVTDM3* api) :
				api_(api) {}

			// GetDirectionalMovementState() is true in "Vanilla Style" too; only kDirectional lets the
			// player face the way it moves.
			bool FreeRotation() override
			{
				return api_->GetDirectionalMovementMode() == TDM_API::DirectionalMovementMode::kDirectional;
			}

			// TDM reports kDirectional only in its 360 mode with no face-crosshair reason active (attacking,
			// blocking, casting, aiming, shouting); its MCM "Vanilla" setting is always kVanillaStyle.
			const char* ModeName() override
			{
				switch (api_->GetDirectionalMovementMode()) {
				case TDM_API::DirectionalMovementMode::kDisabled: return "TDM disabled";
				case TDM_API::DirectionalMovementMode::kVanillaStyle: return "TDM vanilla style";
				case TDM_API::DirectionalMovementMode::kDirectional: return "TDM 360";
				case TDM_API::DirectionalMovementMode::kTargetLock: return "TDM target lock";
				default: return "TDM ?";
				}
			}

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

			bool LockedOn(RE::PlayerCharacter* player) override
			{
				if (!api_->GetTargetLockState()) {
					return false;
				}
				const auto target = api_->GetCurrentTarget().get();
				return target && target.get() != player;
			}

		private:
			TDM_API::IVTDM3* api_;
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
		auto* api = static_cast<TDM_API::IVTDM3*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V3));
		if (!api) {
			logger::warn("lock-on: True Directional Movement is loaded but gave no API");
			return;
		}
		static TdmProvider provider(api);
		g_provider = &provider;
		logger::info("lock-on: using True Directional Movement target lock");
	}

	bool LockedOn(RE::PlayerCharacter* player)
	{
		return g_provider->LockedOn(player);
	}

	bool FreeRotation() { return g_provider->FreeRotation(); }
	const char* ModeName() { return g_provider->ModeName(); }
	void AcquireYaw() { g_provider->AcquireYaw(); }
	void ReleaseYaw() { g_provider->ReleaseYaw(); }
}
