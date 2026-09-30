#pragma once

struct Settings;

// Everything the game threads report, read by the watchdog thread. All atomics: no locks on the frame path.
namespace Monitor
{
	struct State
	{
		std::atomic<std::uint32_t> mainThreadId{ 0 };
		std::atomic<std::int64_t>  lastFrameNs{ 0 };  // heartbeat: stamped every frame by the Main::Update hook
		std::atomic<std::uint64_t> lastFrameDtUs{ 0 };  // (1.1) length of the most recent timed frame

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

		// (1.1) where the player is: exterior cells have no name, so the grid and position are what locate them
		std::atomic<std::uint32_t> worldFormID{ 0 };
		std::atomic<std::int32_t>  gridX{ 0 };
		std::atomic<std::int32_t>  gridY{ 0 };
		std::atomic<float>         posX{ 0.0f };
		std::atomic<float>         posY{ 0.0f };
		std::atomic<float>         posZ{ 0.0f };

		// (1.1) actor load beyond the high process
		std::atomic<std::uint32_t> midHighActors{ 0 };
		std::atomic<std::uint32_t> lowActors{ 0 };
		std::atomic<std::uint32_t> spawnedActors{ 0 };  // high-process actors whose reference is runtime-created (FF......)
	};

	// (1.1) Text context, copied out under a lock
	struct Context
	{
		std::string cellName;
		std::string worldspace;     // EditorID of the current worldspace ("" in interiors)
		std::string location;       // BGSLocation name, e.g. "Riverwood"
		std::string actorPlugins;   // top plugins by high-process actor base, e.g. "Skyrim.esm:14 OCW.esp:3"
		std::string spawnPlugins;   // same, runtime-spawned actors only
		std::string openMenus;      // menus currently open, from open/close events
		std::string loadKind;       // current or last loading screen: "save load", "new game", "fast travel", "transition"
	};

	State&       Get();
	std::int64_t NowNs();
	std::string  CellName();
	Context      GetContext();

	bool InstallFrameHook(const Settings& a_settings);
	void RegisterEventSinks();

	// (1.1) From SKSE messages: tag the next (or current) loading screen
	void NoteGameMessage(std::uint32_t a_type);

	// (1.1) Papyrus VM load, read without taking the VM's locks (plain counters; a torn read only skews one row).
	// Safe to call from any thread; all fields are -1 when the VM is not available.
	struct PapyrusStats
	{
		long long runningStacks = -1;   // script call stacks currently alive
		long long latentWaits = -1;     // stacks waiting on a latent native call (Wait, MoveTo, ...)
		long long queuedMessages = -1;  // function calls queued for the VM (events waiting to be dispatched)
		int       overstressed = -1;    // 1 = VM flagged itself overstressed (the "stack dump" condition)
	};
	PapyrusStats ReadPapyrus();

	// (1.1) One line describing where the player is and what is going on, for reports
	std::string ContextLine();
}
