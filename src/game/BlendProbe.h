#pragma once

namespace rdf::game::blendprobe
{
	// Dev only (installed when bDevLog=1): logs update / deactivate of RDF's blending transition
	// effects (recognised by duration) with their timers, read at offsets verified in IDA.
	void Install();
}
