#pragma once

namespace rdf::game::lockon
{
	// Where the camera/lock-on mod keeps the player's target. The default provider reports no lock-on.
	class Provider
	{
	public:
		virtual ~Provider() = default;
		// True while a target is locked (the lock-on mod then owns the player's yaw).
		virtual bool LockedOn(RE::PlayerCharacter* player) = 0;
		// True when the actor may face any direction (TDM 360 movement); false = the camera owns it.
		virtual bool FreeRotation() = 0;
		// Diagnostics: the mod's own name for its current mode.
		virtual const char* ModeName() { return "vanilla camera"; }
		// Keep the mod from turning the player while a dodge turns it; balanced by ReleaseYaw.
		virtual void AcquireYaw() = 0;
		virtual void ReleaseYaw() = 0;
	};

	// kPostPostLoad: connects to True Directional Movement when it is loaded and enabled in the ini.
	void Init();
	bool LockedOn(RE::PlayerCharacter* player);
	bool FreeRotation();
	const char* ModeName();
	void AcquireYaw();
	void ReleaseYaw();
}
