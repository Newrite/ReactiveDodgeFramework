#pragma once

namespace rdf::game::legacy
{
	// STB.dll implements a Left Alt dodge driven by The Ultimate Dodge Mod's globals. When enabled
	// in the ini, zero UDDodgeKey and UDSprintDodge so only this plugin reacts. The values persist
	// in the save; restore them in the console with `set UDDodgeKey to 56` / `set UDSprintDodge to 1`.
	void Suppress();
}
