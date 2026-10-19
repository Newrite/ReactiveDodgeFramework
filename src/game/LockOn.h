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
	};

	// kPostPostLoad: connects to True Directional Movement when it is loaded and enabled in the ini.
	void Init();
	bool TargetYaw(RE::PlayerCharacter* player, float& yaw);
}
