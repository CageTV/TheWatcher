#pragma once

namespace Watchdog
{
	// Starts the background thread (stall detection + optional stats CSV). Call at kDataLoaded.
	void Start();
}
