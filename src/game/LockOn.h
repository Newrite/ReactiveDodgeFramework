#pragma once

namespace rdf::game::lockon
{
	// Where the camera/lock-on mod keeps the player's target. The default provider reports no lock-on.
	class Provider
	{
	public:
		virtual ~Provider() = default;
		// True and the heading toward the target when locked on.
		virtual bool TargetYaw(RE::PlayerCharacter* player, float& yaw) = 0;
		// True when the actor may face any direction (TDM 360 movement); false = the camera owns it.
		virtual bool FreeRotation() = 0;
		// Keep the mod from turning the player while a dodge turns it; balanced by ReleaseYaw.
		virtual void AcquireYaw() = 0;
		virtual void ReleaseYaw() = 0;
	};

	// kPostPostLoad: connects to True Directional Movement when it is loaded and enabled in the ini.
	void Init();
	bool TargetYaw(RE::PlayerCharacter* player, float& yaw);
	bool FreeRotation();
	void AcquireYaw();
	void ReleaseYaw();
}
