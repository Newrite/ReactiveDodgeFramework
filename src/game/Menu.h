#pragma once

namespace rdf::game::menu
{
	// kDataLoaded: connects to FLICK (FUCK.dll) and adds the tuning pages to its menu (F7 by default).
	// Without FLICK nothing happens; the ini stays the way to configure the plugin.
	void Install();
}
