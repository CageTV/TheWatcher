#include "PCH.h"
#include "Watchdog.h"
#include "Capture.h"
#include "Monitor.h"
#include "Settings.h"
#include "Util.h"

namespace Watchdog
{
	namespace
	{
		std::atomic<bool>     g_running{ false };
		std::ofstream         g_csv;
		std::filesystem::path g_csvPath;
		int                   g_capturesThisSession = 0;

		struct Episode
		{
			bool         active = false;
			bool         loading = false;
			std::int64_t startNs = 0;
			std::int64_t lastCaptureNs = 0;
			int          captures = 0;
		};
		Episode g_ep;

		struct ProcSample
		{
			std::uint64_t cpu100ns = 0;
			std::int64_t  wallNs = 0;
			DWORD         pageFaults = 0;
			bool          valid = false;
		};
		ProcSample g_lastProc;

		bool GameHasFocus()
		{
			const HWND fg = GetForegroundWindow();
			if (!fg) {
				return false;
			}
			DWORD pid = 0;
			GetWindowThreadProcessId(fg, &pid);
			return pid == GetCurrentProcessId();
		}

		std::uint64_t To100ns(const FILETIME& a_ft)
		{
			return (static_cast<std::uint64_t>(a_ft.dwHighDateTime) << 32) | a_ft.dwLowDateTime;
		}

		double UptimeSeconds()
		{
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
				return 0.0;
			}
			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			return static_cast<double>(To100ns(now) - To100ns(created)) / 1e7;
		}

		std::uint32_t CountThreads()
		{
			const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) {
				return 0;
			}
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			std::uint32_t count = 0;
			const auto    pid = GetCurrentProcessId();
			if (Thread32First(snap, &te)) {
				do {
					if (te.th32OwnerProcessID == pid) {
						++count;
					}
				} while (Thread32Next(snap, &te));
			}
			CloseHandle(snap);
			return count;
		}

		// ---------- stall detection ----------

		void EndEpisode(std::string_view a_why)
		{
			if (!g_ep.active) {
				return;
			}
			spdlog::warn("Stall episode ended ({}) after ~{:.1f}s; captures taken: {}",
				a_why, static_cast<double>(Monitor::NowNs() - g_ep.startNs) / 1e9, g_ep.captures);
			g_ep = {};
		}

		void CheckStall(std::int64_t a_now)
		{
			auto&       s = Monitor::Get();
			const auto& cfg = Settings::Get();
			const bool  loading = s.loading.load();
			const auto  lastFrame = s.lastFrameNs.load();

			double      age = 0.0;
			double      warn = 0.0;
			double      stall = 0.0;
			std::string kind;

			if (loading) {
				const auto ref = std::max(s.lastProgressNs.load(), s.loadStartNs.load());
				age = static_cast<double>(a_now - ref) / 1e9;
				warn = cfg.loadWarnSec;
				stall = cfg.loadStallSec;
				kind = "loading screen made no load progress";
			} else {
				if (lastFrame == 0) {
					return;  // frame hook not running (yet)
				}
				if (!GameHasFocus()) {
					s.focusGen.fetch_add(1);  // frames spanning the alt-tab are left out of the frame-time stats
					if (cfg.ignoreUnfocused) {
						EndEpisode("game not in focus");
						return;
					}
				}
				// No frames run during a loading screen, so the clock restarts when the loading screen closes.
				// (v1.0.0 measured from the last pre-load frame and falsely reported a 16s stall right after a load.)
				const auto ref = std::max(lastFrame, s.loadEndNs.load());
				age = static_cast<double>(a_now - ref) / 1e9;
				warn = cfg.frameStallSec;
				stall = cfg.frameStallSec;
				kind = "main thread did not finish a frame";
			}

			if (age < warn) {
				EndEpisode("recovered");
				return;
			}

			if (!g_ep.active || g_ep.loading != loading) {
				EndEpisode("state changed");
				g_ep.active = true;
				g_ep.loading = loading;
				g_ep.startNs = a_now;

				const double hbMs = lastFrame ? static_cast<double>(a_now - lastFrame) / 1e6 : -1.0;
				if (loading) {
					spdlog::warn("WARNING: loading screen open {:.1f}s, no load progress for {:.1f}s "
								 "(load events so far {}, longest gap {:.0f} ms, frames during load {}, frame heartbeat age {:.0f} ms, "
								 "cell before load {:08X} '{}')",
						static_cast<double>(a_now - s.loadStartNs.load()) / 1e9, age, s.loadEvents.load(),
						s.loadMaxGapUs.load() / 1000.0, s.loadFrames.load(), hbMs, s.cellFormID.load(), Monitor::CellName());
				} else {
					spdlog::warn("WARNING: main thread has not finished a frame for {:.1f}s (cell {:08X} '{}', high-process actors {})",
						age, s.cellFormID.load(), Monitor::CellName(), s.highActors.load());
				}
				Capture::WarnBeep();
			}

			const bool allowed = g_capturesThisSession < cfg.maxCaptures;
			const bool due = g_ep.captures == 0 ?
			                     age >= stall :
			                     static_cast<double>(a_now - g_ep.lastCaptureNs) / 1e9 >= cfg.recaptureSec;
			if (allowed && due) {
				++g_capturesThisSession;
				++g_ep.captures;
				g_ep.lastCaptureNs = a_now;
				Capture::Run(std::format("{} for {:.1f}s", kind, age), g_capturesThisSession, g_csvPath);
			}
		}

		// ---------- stats CSV ----------

		void OpenStatsFile()
		{
			const auto dir = Util::WatchdogDir();
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			Util::Prune(dir, "stats_", Settings::Get().keepStatsFiles - 1);

			g_csvPath = dir / std::format("stats_{}.csv", Util::Stamp(true));
			g_csv.open(g_csvPath, std::ios::out | std::ios::trunc);
			if (!g_csv) {
				spdlog::error("Could not open stats file {}", g_csvPath.string());
				g_csvPath.clear();
				return;
			}
			g_csv << "time,uptime_s,state,frames,fps_avg,frame_ms_avg,frame_ms_max,hitches,heartbeat_age_ms,"
					 "loads_done,longest_load_s,load_events,cell_formid,interior,cell_name,high_actors,"
					 "cpu_pct,private_mb,working_set_mb,peak_working_set_mb,page_faults,sys_free_ram_mb,handles,threads\n";
			g_csv.flush();
			spdlog::info("Stats file: {}", g_csvPath.string());
		}

		void WriteStatsRow(std::int64_t a_now, double a_intervalSec)
		{
			auto& s = Monitor::Get();

			const auto frames = s.intervalFrames.exchange(0);
			const auto sumUs = s.intervalFrameUsSum.exchange(0);
			const auto maxUs = s.intervalFrameUsMax.exchange(0);
			const auto hitches = s.intervalHitches.exchange(0);
			const auto loads = s.intervalLoads.exchange(0);
			const auto longestLoadMs = s.intervalLongestLoadMs.exchange(0);
			const auto loadEvents = s.intervalLoadEvents.exchange(0);
			const auto lastFrame = s.lastFrameNs.load();
			const bool loading = s.loading.load();

			PROCESS_MEMORY_COUNTERS_EX pmc{};
			pmc.cb = sizeof(pmc);
			GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

			MEMORYSTATUSEX ms{};
			ms.dwLength = sizeof(ms);
			GlobalMemoryStatusEx(&ms);

			DWORD handles = 0;
			GetProcessHandleCount(GetCurrentProcess(), &handles);

			double   cpuPct = -1.0;
			long long pageFaultDelta = -1;
			FILETIME created{}, exited{}, kernel{}, user{};
			if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
				const auto cpu = To100ns(kernel) + To100ns(user);
				if (g_lastProc.valid) {
					const auto wall100ns = (a_now - g_lastProc.wallNs) / 100;
					SYSTEM_INFO si{};
					GetSystemInfo(&si);
					if (wall100ns > 0 && si.dwNumberOfProcessors > 0) {
						cpuPct = 100.0 * static_cast<double>(cpu - g_lastProc.cpu100ns) /
						         (static_cast<double>(wall100ns) * si.dwNumberOfProcessors);
					}
					pageFaultDelta = static_cast<long long>(pmc.PageFaultCount) - static_cast<long long>(g_lastProc.pageFaults);
				}
				g_lastProc = { cpu, a_now, pmc.PageFaultCount, true };
			}

			const char* state = loading ? "loading" : (GameHasFocus() ? "ingame" : "unfocused");
			const double fps = a_intervalSec > 0.0 ? static_cast<double>(frames) / a_intervalSec : 0.0;
			const double avgMs = frames ? (static_cast<double>(sumUs) / 1000.0) / static_cast<double>(frames) : 0.0;
			const double hbMs = lastFrame ? static_cast<double>(a_now - lastFrame) / 1e6 : -1.0;

			auto name = Monitor::CellName();
			std::replace(name.begin(), name.end(), '"', '\'');

			constexpr double MB = 1024.0 * 1024.0;
			g_csv << std::format("{},{:.0f},{},{},{:.1f},{:.2f},{:.1f},{},{:.0f},{},{:.1f},{},{:08X},{},\"{}\",{},{:.1f},{:.0f},{:.0f},{:.0f},{},{:.0f},{},{}\n",
				Util::Stamp(false), UptimeSeconds(), state, frames, fps, avgMs, maxUs / 1000.0, hitches, hbMs,
				loads, longestLoadMs / 1000.0, loadEvents, s.cellFormID.load(), s.cellInterior.load() ? 1 : 0, name, s.highActors.load(),
				cpuPct, pmc.PrivateUsage / MB, pmc.WorkingSetSize / MB, pmc.PeakWorkingSetSize / MB, pageFaultDelta,
				ms.ullAvailPhys / MB, handles, CountThreads());
			g_csv.flush();
		}

		void Run()
		{
			spdlog::info("Watchdog thread running (thread {})", GetCurrentThreadId());
			auto lastStats = Monitor::NowNs();

			while (g_running.load()) {
				std::this_thread::sleep_for(250ms);
				const auto now = Monitor::NowNs();

				try {
					CheckStall(now);
				} catch (const std::exception& e) {
					spdlog::error("Stall check error: {}", e.what());
				}

				const auto& cfg = Settings::Get();
				if (g_csv.is_open() && (now - lastStats) >= static_cast<std::int64_t>(cfg.statsIntervalSec) * 1'000'000'000) {
					try {
						WriteStatsRow(now, static_cast<double>(now - lastStats) / 1e9);
					} catch (const std::exception& e) {
						spdlog::error("Stats write error: {}", e.what());
					}
					lastStats = now;
				}
			}
		}
	}

	void Start()
	{
		if (g_running.exchange(true)) {
			return;
		}
		if (Settings::Get().StatsEnabled()) {
			OpenStatsFile();
		}
		std::thread(Run).detach();
	}
}
