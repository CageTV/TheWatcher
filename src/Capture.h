#pragma once

namespace Capture
{
	// Call once at plugin load (records the session start so log backup only copies this session's logs)
	void Init();

	// Grab stacks, back up logs, alert, write minidump. Runs on the watchdog thread.
	// (1.1) Returns false when the game was already running again by the time the stack was taken ("recovered"):
	// the capture is then written without a minidump and should not count against the per-session limit.
	bool Run(const std::string& a_reason, int a_index, const std::filesystem::path& a_statsFile);

	// Short single beep (used for warnings)
	void WarnBeep();

	// ---------- (1.1) main-thread stack history for slow episodes (hitches, stalls, slow loading gaps) ----------
	// All of these run on the watchdog thread only.

	// Start a new history (forgets the previous samples)
	void HistoryReset();

	// Take one main-thread stack sample into the history. Returns false if the thread could not be read.
	bool HistorySample(std::uint32_t a_mainTid);

	[[nodiscard]] std::size_t HistoryCount();

	struct SlowEpisode
	{
		bool         loading = false;    // loading-screen gap (true) or in-game hitch (false)
		double       seconds = 0.0;      // how long the frame / the load gap lasted
		std::string  cellText;           // "0001A26F 'Riverwood' Tamriel (5,-11)"
		std::string  location;
		std::string  openMenus;
		std::string  loadKind;
		std::uint64_t loadNumber = 0;
		int          stallCapture = 0;   // index of a stall capture taken during this episode, 0 = none
	};

	// Write the history of a finished slow episode: a readable block in TheWatcher.log and one JSON line in
	// TheWatcher\slow_<session>.jsonl. Returns the episode's signature (top non-system frames of the most common stack).
	std::string ReportSlowEpisode(const SlowEpisode& a_ep);
}
