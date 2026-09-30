#pragma once

struct Settings;

// Everything the game threads report, read by the watchdog thread. All atomics: no locks on the frame path.
namespace Monitor
{
	struct State
	{
		std::atomic<std::uint32_t> mainThreadId{ 0 };
		std::atomic<std::int64_t>  lastFrameNs{ 0 };  // heartbeat: stamped every frame by the Main::Update hook

		// Current loading screen
		std::atomic<bool>          loading{ false };
		std::atomic<std::int64_t>  loadStartNs{ 0 };
		std::atomic<std::int64_t>  lastProgressNs{ 0 };  // last object/cell load event during this loading screen
		std::atomic<std::uint64_t> loadEvents{ 0 };
		std::atomic<std::uint64_t> loadMaxGapUs{ 0 };    // longest wait between load events (includes wait for the first one)
		std::atomic<std::uint64_t> loadFrames{ 0 };      // frames finished while the loading screen was up
		std::atomic<std::uint64_t> loadGen{ 0 };
		std::atomic<std::uint64_t> loadCount{ 0 };
		std::atomic<std::int64_t>  loadEndNs{ 0 };   // when the last loading screen closed (no frames run during loads)

		// Bumped by the watchdog while the game is alt-tabbed; frames spanning that time are not timed
		std::atomic<std::uint64_t> focusGen{ 0 };

		// Per-interval counters, reset by the stats writer
		std::atomic<std::uint64_t> intervalFrames{ 0 };
		std::atomic<std::uint64_t> intervalFrameUsSum{ 0 };
		std::atomic<std::uint64_t> intervalFrameUsMax{ 0 };
		std::atomic<std::uint64_t> intervalHitches{ 0 };
		std::atomic<std::uint64_t> intervalLoads{ 0 };
		std::atomic<std::uint64_t> intervalLongestLoadMs{ 0 };
		std::atomic<std::uint64_t> intervalLoadEvents{ 0 };

		// Game context, written by the main thread about once a second (never during loading)
		std::atomic<std::uint32_t> cellFormID{ 0 };
		std::atomic<bool>          cellInterior{ false };
		std::atomic<std::uint32_t> highActors{ 0 };
	};

	State&       Get();
	std::int64_t NowNs();
	std::string  CellName();

	bool InstallFrameHook(const Settings& a_settings);
	void RegisterEventSinks();
}
