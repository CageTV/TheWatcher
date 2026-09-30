#include "PCH.h"
#include "Watchdog.h"
#include "Capture.h"
#include "Events.h"
#include "Monitor.h"
#include "Settings.h"
#include "Util.h"

#include <dxgi1_4.h>

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
			int          lastCaptureIndex = 0;
		};
		Episode g_ep;

		// (1.1) A hitch or a slow loading gap, sampled while it lasts
		struct Slow
		{
			bool          active = false;
			bool          loading = false;
			std::int64_t  lastSampleNs = 0;
			double        maxAge = 0.0;
			std::uint64_t loadNumber = 0;
			int           stallCapture = 0;
		};
		Slow g_slow;
		int  g_slowReports = 0;

		// Per stats interval
		std::uint32_t g_intervalSlowCount = 0;
		double        g_intervalSlowMaxS = 0.0;

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

		// ---------- (1.1) VRAM ----------
		// Per-process video memory from DXGI. The adapter the game renders on is the one where this process uses the
		// most local memory, so every adapter is asked and the busiest wins (works on hybrid-GPU laptops too).
		struct Vram
		{
			double localMb = -1.0;
			double budgetMb = -1.0;
			double nonLocalMb = -1.0;  // video memory that lives in system RAM (spill when local is over budget)
		};

		Vram QueryVram()
		{
			Vram               best;
			IDXGIFactory1*     factory = nullptr;
			if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
				return best;
			}
			constexpr double MB = 1024.0 * 1024.0;
			IDXGIAdapter1*   adapter = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
				IDXGIAdapter3* a3 = nullptr;
				if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
					DXGI_QUERY_VIDEO_MEMORY_INFO local{};
					DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
					if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
						SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
						const double used = static_cast<double>(local.CurrentUsage) / MB;
						if (used > best.localMb) {
							best.localMb = used;
							best.budgetMb = static_cast<double>(local.Budget) / MB;
							best.nonLocalMb = static_cast<double>(nonLocal.CurrentUsage) / MB;
						}
					}
					a3->Release();
				}
				adapter->Release();
			}
			factory->Release();
			return best;
		}

		// ---------- measuring ----------

		struct Measure
		{
			bool   valid = false;    // false = nothing to judge (no frames yet, or alt-tabbed and ignored)
			bool   loading = false;
			double age = 0.0;        // seconds without a frame (in game) or without load progress (loading screen)
		};

		Measure Take(std::int64_t a_now)
		{
			auto&       s = Monitor::Get();
			const auto& cfg = Settings::Get();
			Measure     m;
			m.loading = s.loading.load();
			if (m.loading) {
				const auto ref = std::max(s.lastProgressNs.load(), s.loadStartNs.load());
				m.age = static_cast<double>(a_now - ref) / 1e9;
				m.valid = true;
				return m;
			}
			const auto lastFrame = s.lastFrameNs.load();
			if (lastFrame == 0) {
				return m;  // frame hook not running (yet)
			}
			if (!GameHasFocus()) {
				s.focusGen.fetch_add(1);  // frames spanning the alt-tab are left out of the frame-time stats
				if (cfg.ignoreUnfocused) {
					return m;
				}
			}
			// No frames run during a loading screen, so the clock restarts when the loading screen closes.
			// (v1.0.0 measured from the last pre-load frame and falsely reported a 16s stall right after a load.)
			const auto ref = std::max(lastFrame, s.loadEndNs.load());
			m.age = static_cast<double>(a_now - ref) / 1e9;
			m.valid = true;
			return m;
		}

		// ---------- (1.1) slow-episode sampling ----------

		void FinishSlow()
		{
			if (!g_slow.active) {
				return;
			}
			const auto& cfg = Settings::Get();
			auto&       s = Monitor::Get();
			double      secs = g_slow.maxAge;
			if (!g_slow.loading) {
				// The frame that ended the hitch knows its exact length
				secs = std::max(secs, static_cast<double>(s.lastFrameDtUs.load()) / 1e6);
			}
			const double threshold = g_slow.loading ? cfg.loadGapSampleSec : cfg.hitchReportSec;
			if (secs >= threshold && Capture::HistoryCount() > 0) {
				++g_intervalSlowCount;
				g_intervalSlowMaxS = std::max(g_intervalSlowMaxS, secs);
				if (g_slowReports < cfg.maxSlowReports) {
					++g_slowReports;
					const auto c = Monitor::GetContext();
					Capture::SlowEpisode ep;
					ep.loading = g_slow.loading;
					ep.seconds = secs;
					ep.cellText = Monitor::ContextLine();
					ep.location = c.location;
					ep.openMenus = c.openMenus;
					ep.loadKind = c.loadKind;
					ep.loadNumber = g_slow.loadNumber;
					ep.stallCapture = g_slow.stallCapture;
					try {
						Capture::ReportSlowEpisode(ep);
					} catch (const std::exception& e) {
						spdlog::error("Slow-episode report error: {}", e.what());
					}
					if (g_slowReports == cfg.maxSlowReports) {
						spdlog::info("Reached iMaxSlowReportsPerSession={}; further hitches are only counted in the stats CSV", cfg.maxSlowReports);
					}
				}
			}
			g_slow = {};
		}

		void CheckSlow(std::int64_t a_now, const Measure& a_m)
		{
			const auto& cfg = Settings::Get();
			if (!cfg.slowSampling) {
				return;
			}
			const double threshold = a_m.loading ? cfg.loadGapSampleSec : cfg.hitchSampleSec;
			const bool   slowNow = a_m.valid && a_m.age >= threshold;
			if (g_slow.active && (!slowNow || g_slow.loading != a_m.loading)) {
				FinishSlow();
			}
			if (!slowNow) {
				return;
			}
			auto& s = Monitor::Get();
			if (!g_slow.active) {
				g_slow.active = true;
				g_slow.loading = a_m.loading;
				g_slow.loadNumber = s.loadCount.load() + (a_m.loading ? 1 : 0);
				Capture::HistoryReset();
			}
			g_slow.maxAge = std::max(g_slow.maxAge, a_m.age);
			if (a_now - g_slow.lastSampleNs >= static_cast<std::int64_t>(cfg.slowSampleIntervalMs) * 1'000'000) {
				g_slow.lastSampleNs = a_now;
				Capture::HistorySample(s.mainThreadId.load());
			}
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

		void CheckStall(std::int64_t a_now, const Measure& a_m)
		{
			auto&       s = Monitor::Get();
			const auto& cfg = Settings::Get();
			if (!a_m.valid) {
				EndEpisode(s.lastFrameNs.load() == 0 ? "no frames yet" : "game not in focus");
				return;
			}
			const double age = a_m.age;
			const double warn = a_m.loading ? cfg.loadWarnSec : cfg.frameStallSec;
			const double stall = a_m.loading ? cfg.loadStallSec : cfg.frameStallSec;
			const char*  kind = a_m.loading ? "loading screen made no load progress" : "main thread did not finish a frame";

			if (age < warn) {
				EndEpisode("recovered");
				return;
			}

			if (!g_ep.active || g_ep.loading != a_m.loading) {
				EndEpisode("state changed");
				g_ep.active = true;
				g_ep.loading = a_m.loading;
				g_ep.startNs = a_now;

				const auto   lastFrame = s.lastFrameNs.load();
				const double hbMs = lastFrame ? static_cast<double>(a_now - lastFrame) / 1e6 : -1.0;
				if (a_m.loading) {
					spdlog::warn("WARNING: loading screen open {:.1f}s, no load progress for {:.1f}s "
								 "(load events so far {}, longest gap {:.0f} ms, frames during load {}, frame heartbeat age {:.0f} ms) | {}",
						static_cast<double>(a_now - s.loadStartNs.load()) / 1e9, age, s.loadEvents.load(),
						s.loadMaxGapUs.load() / 1000.0, s.loadFrames.load(), hbMs, Monitor::ContextLine());
				} else {
					spdlog::warn("WARNING: main thread has not finished a frame for {:.1f}s | {}", age, Monitor::ContextLine());
				}
				Events::Write(std::format("WARNING {} for {:.1f}s", kind, age));
				Capture::WarnBeep();
			}

			const bool allowed = g_capturesThisSession < cfg.maxCaptures;
			const bool due = g_ep.captures == 0 ?
			                     age >= stall :
			                     static_cast<double>(a_now - g_ep.lastCaptureNs) / 1e9 >= cfg.recaptureSec;
			if (allowed && due) {
				const int index = ++g_capturesThisSession;
				++g_ep.captures;
				g_ep.lastCaptureNs = a_now;
				const bool counted = Capture::Run(std::format("{} for {:.1f}s", kind, age), index, g_csvPath);
				if (!counted) {
					--g_capturesThisSession;  // (1.1) a capture of an already-recovered stall does not use up the limit
				}
				g_slow.stallCapture = index;
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
			// The first 24 columns are unchanged from 1.0.x; 1.1 columns are appended after them
			g_csv << "time,uptime_s,state,frames,fps_avg,frame_ms_avg,frame_ms_max,hitches,heartbeat_age_ms,"
					 "loads_done,longest_load_s,load_events,cell_formid,interior,cell_name,high_actors,"
					 "cpu_pct,private_mb,working_set_mb,peak_working_set_mb,page_faults,sys_free_ram_mb,handles,threads,"
					 "worldspace,cell_x,cell_y,pos_x,pos_y,pos_z,location,open_menus,"
					 "mid_high_actors,low_actors,spawned_actors,actor_plugins,spawned_plugins,"
					 "papyrus_stacks,papyrus_latent,papyrus_queued,papyrus_overstressed,"
					 "vram_local_mb,vram_budget_mb,vram_nonlocal_mb,slow_episodes,slow_max_s\n";
			g_csv.flush();
			spdlog::info("Stats file: {}", g_csvPath.string());
		}

		void WriteStatsRow(std::int64_t a_now, double a_intervalSec)
		{
			auto&       s = Monitor::Get();
			const auto& cfg = Settings::Get();

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

			const auto c = Monitor::GetContext();
			const auto p = cfg.papyrusStats ? Monitor::ReadPapyrus() : Monitor::PapyrusStats{};
			const auto v = cfg.vramStats ? QueryVram() : Vram{};
			const bool interior = s.cellInterior.load();

			constexpr double MB = 1024.0 * 1024.0;
			g_csv << std::format("{},{:.0f},{},{},{:.1f},{:.2f},{:.1f},{},{:.0f},{},{:.1f},{},{:08X},{},{},{},{:.1f},{:.0f},{:.0f},{:.0f},{},{:.0f},{},{},",
				Util::Stamp(false), UptimeSeconds(), state, frames, fps, avgMs, maxUs / 1000.0, hitches, hbMs,
				loads, longestLoadMs / 1000.0, loadEvents, s.cellFormID.load(), interior ? 1 : 0, Util::Csv(c.cellName), s.highActors.load(),
				cpuPct, pmc.PrivateUsage / MB, pmc.WorkingSetSize / MB, pmc.PeakWorkingSetSize / MB, pageFaultDelta,
				ms.ullAvailPhys / MB, handles, CountThreads());
			g_csv << std::format("{},{},{},{:.0f},{:.0f},{:.0f},{},{},{},{},{},{},{},{},{},{},{},{:.0f},{:.0f},{:.0f},{},{:.2f}\n",
				Util::Csv(c.worldspace), interior ? "" : std::to_string(s.gridX.load()), interior ? "" : std::to_string(s.gridY.load()),
				s.posX.load(), s.posY.load(), s.posZ.load(), Util::Csv(c.location), Util::Csv(c.openMenus),
				s.midHighActors.load(), s.lowActors.load(), s.spawnedActors.load(), Util::Csv(c.actorPlugins), Util::Csv(c.spawnPlugins),
				p.runningStacks, p.latentWaits, p.queuedMessages, p.overstressed,
				v.localMb, v.budgetMb, v.nonLocalMb, g_intervalSlowCount, g_intervalSlowMaxS);
			g_csv.flush();
			g_intervalSlowCount = 0;
			g_intervalSlowMaxS = 0.0;
		}

		// ---------- (1.1) live status file ----------
		// TheWatcher\status.json, rewritten every second from the watchdog thread. Readable from disk even while the
		// game's main thread is hung (when in-game tools that run on the main thread time out).
		void WriteStatus(std::int64_t a_now, const Measure& a_m)
		{
			auto&        s = Monitor::Get();
			const auto   lastFrame = s.lastFrameNs.load();
			const double hbMs = lastFrame ? static_cast<double>(a_now - lastFrame) / 1e6 : -1.0;
			const char*  state = !a_m.valid ? (lastFrame == 0 ? "starting" : "unfocused") :
			                     a_m.loading ? (a_m.age >= Settings::Get().loadWarnSec ? "loading_stuck" : "loading") :
			                     a_m.age >= Settings::Get().frameStallSec ? "stalled" :
			                     a_m.age >= Settings::Get().hitchSampleSec ? "hitching" : "running";
			const auto text = std::format(
				"{{\"time\":{},\"pid\":{},\"uptime_s\":{:.0f},\"state\":{},\"heartbeat_age_ms\":{:.0f},\"measured_age_s\":{:.2f},\"loading\":{},"
				"\"load_number\":{},\"stall_episode\":{},\"captures\":{},\"slow_reports\":{},\"context\":{}}}\n",
				Util::Json(Util::Stamp(false)), GetCurrentProcessId(), UptimeSeconds(), Util::Json(state), hbMs, a_m.age, a_m.loading ? "true" : "false",
				s.loadCount.load() + (a_m.loading ? 1 : 0), g_ep.active ? "true" : "false", g_capturesThisSession, g_slowReports,
				Util::Json(Monitor::ContextLine()));
			Util::WriteFileAtomic(Util::WatchdogDir() / "status.json", text);
		}

		void Run()
		{
			spdlog::info("Watchdog thread running (thread {})", GetCurrentThreadId());
			auto lastStats = Monitor::NowNs();
			auto lastStatus = lastStats;

			while (g_running.load()) {
				// (1.1) 50 ms tick (was 250 ms) so a hitch can be sampled while it is happening
				std::this_thread::sleep_for(50ms);
				const auto  now = Monitor::NowNs();
				const auto& cfg = Settings::Get();

				try {
					const auto m = Take(now);
					CheckSlow(now, m);
					CheckStall(now, m);
					if (cfg.statusFile && now - lastStatus >= 1'000'000'000) {
						lastStatus = now;
						WriteStatus(now, m);
					}
				} catch (const std::exception& e) {
					spdlog::error("Stall check error: {}", e.what());
				}

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
