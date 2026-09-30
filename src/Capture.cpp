#include "PCH.h"
#include "Capture.h"
#include "AddressLib.h"
#include "Monitor.h"
#include "Settings.h"
#include "Util.h"

namespace Capture
{
	namespace
	{
		constexpr std::size_t kMaxFrames = 64;
		constexpr std::size_t kStackCopyBytes = 64 * 1024;

		std::filesystem::file_time_type g_sessionStart;

		// Static so nothing is heap-allocated while another thread is suspended
		alignas(16) std::uint8_t g_stackCopy[kStackCopyBytes];
		std::size_t              g_stackCopyLen = 0;
		std::uintptr_t           g_stackCopyBase = 0;

		struct ThreadStack
		{
			DWORD                                  tid = 0;
			std::uintptr_t                         rip = 0;
			std::uintptr_t                         rsp = 0;
			std::array<std::uint64_t, kMaxFrames> frames{};
			std::size_t                            count = 0;
		};

		struct Module
		{
			std::uintptr_t base;
			std::uintptr_t end;
			std::string    name;
		};

		// ---------- functions that run while a thread is suspended: no heap, no locks, SEH-guarded ----------

		bool SafeCopy(void* a_dst, const void* a_src, std::size_t a_len)
		{
			__try {
				std::memcpy(a_dst, a_src, a_len);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::size_t SafeUnwind(const CONTEXT* a_start, std::uint64_t* a_out, std::size_t a_max)
		{
			CONTEXT              ctx = *a_start;
			volatile std::size_t n = 0;
			__try {
				while (n < a_max && ctx.Rip != 0) {
					a_out[n] = ctx.Rip;
					n = n + 1;
					DWORD64    imageBase = 0;
					const auto fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
					if (fn) {
						PVOID   handlerData = nullptr;
						DWORD64 establisher = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
					} else {
						// No unwind info (leaf function or generated code such as a hook trampoline)
						ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
						ctx.Rsp += 8;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		std::size_t CopyStack(std::uintptr_t a_rsp)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<LPCVOID>(a_rsp), &mbi, sizeof(mbi))) {
				return 0;
			}
			const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (regionEnd <= a_rsp) {
				return 0;
			}
			const auto len = std::min<std::size_t>(kStackCopyBytes, regionEnd - a_rsp);
			if (!SafeCopy(g_stackCopy, reinterpret_cast<const void*>(a_rsp), len)) {
				return 0;
			}
			g_stackCopyBase = a_rsp;
			return len;
		}

		bool CaptureThread(DWORD a_tid, ThreadStack& a_out, bool a_copyStack)
		{
			const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_tid);
			if (!h) {
				return false;
			}
			bool ok = false;
			if (SuspendThread(h) != static_cast<DWORD>(-1)) {
				CONTEXT ctx{};
				ctx.ContextFlags = CONTEXT_FULL;
				if (GetThreadContext(h, &ctx)) {
					a_out.tid = a_tid;
					a_out.rip = ctx.Rip;
					a_out.rsp = ctx.Rsp;
					a_out.count = SafeUnwind(&ctx, a_out.frames.data(), a_out.frames.size());
					if (a_copyStack) {
						g_stackCopyLen = CopyStack(ctx.Rsp);
					}
					ok = true;
				}
				ResumeThread(h);
			}
			CloseHandle(h);
			return ok;
		}

		// ---------- everything below runs with all threads running again ----------

		std::vector<Module> EnumModules()
		{
			const auto          proc = GetCurrentProcess();
			std::vector<HMODULE> mods(1024);
			DWORD               needed = 0;
			if (!EnumProcessModules(proc, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) {
				return {};
			}
			if (needed > mods.size() * sizeof(HMODULE)) {
				mods.resize(needed / sizeof(HMODULE));
				if (!EnumProcessModules(proc, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) {
					return {};
				}
			}
			mods.resize(std::min<std::size_t>(mods.size(), needed / sizeof(HMODULE)));

			std::vector<Module> out;
			out.reserve(mods.size());
			for (const auto m : mods) {
				MODULEINFO mi{};
				if (!GetModuleInformation(proc, m, &mi, sizeof(mi))) {
					continue;
				}
				char name[MAX_PATH]{};
				GetModuleBaseNameA(proc, m, name, MAX_PATH);
				const auto base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
				out.push_back({ base, base + mi.SizeOfImage, name });
			}
			std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
			return out;
		}

		const Module* FindModule(const std::vector<Module>& a_mods, std::uintptr_t a_addr)
		{
			auto it = std::upper_bound(a_mods.begin(), a_mods.end(), a_addr, [](std::uintptr_t v, const Module& m) { return v < m.base; });
			if (it == a_mods.begin()) {
				return nullptr;
			}
			--it;
			return a_addr < it->end ? &*it : nullptr;
		}

		std::string Describe(const std::vector<Module>& a_mods, std::uintptr_t a_addr)
		{
			if (const auto m = FindModule(a_mods, a_addr)) {
				const auto off = a_addr - m->base;
				if (_stricmp(m->name.c_str(), "SkyrimSE.exe") == 0) {
					return std::format("{}+{:07X}{}", m->name, off, AddressLib::Annotate(off));
				}
				return std::format("{}+{:07X}", m->name, off);
			}
			return std::format("0x{:X} (not in a module)", a_addr);
		}

		// Does the code just before this address end in a call instruction? (filters stack-scan noise)
		bool IsProbableReturn(std::uintptr_t a_addr)
		{
			std::uint8_t b[7]{};  // b[0..6] = bytes at a_addr-7 .. a_addr-1
			if (!SafeCopy(b, reinterpret_cast<const void*>(a_addr - 7), sizeof(b))) {
				return false;
			}
			if (b[2] == 0xE8) return true;                            // call rel32
			if (b[1] == 0xFF && b[2] == 0x15) return true;            // call [rip+disp32]
			if (b[1] == 0xFF && (b[2] & 0xF8) == 0x90) return true;   // call [reg+disp32]
			if (b[4] == 0xFF && (b[5] & 0xF8) == 0x50) return true;   // call [reg+disp8]
			if (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) return true;   // call reg
			if (b[5] == 0xFF && (b[6] & 0xF8) == 0x10) return true;   // call [reg]
			return false;
		}

		std::vector<DWORD> ThreadIds()
		{
			std::vector<DWORD> ids;
			const HANDLE       snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) {
				return ids;
			}
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			const auto pid = GetCurrentProcessId();
			if (Thread32First(snap, &te)) {
				do {
					if (te.th32OwnerProcessID == pid) {
						ids.push_back(te.th32ThreadID);
					}
				} while (Thread32Next(snap, &te));
			}
			CloseHandle(snap);
			return ids;
		}

		void AppendStack(std::string& a_out, std::string_view a_label, const ThreadStack& a_ts, const std::vector<Module>& a_mods)
		{
			a_out += std::format("[{}] thread {}  rsp=0x{:X}\n", a_label, a_ts.tid, a_ts.rsp);
			for (std::size_t i = 0; i < a_ts.count; ++i) {
				a_out += std::format("  [{:2}] 0x{:X}  {}\n", i, a_ts.frames[i], Describe(a_mods, a_ts.frames[i]));
			}
			if (a_ts.count == 0) {
				a_out += "  (no frames recovered)\n";
			}
		}

		void AppendScan(std::string& a_out, const std::vector<Module>& a_mods)
		{
			a_out += std::format("\n[MAIN THREAD stack scan] {} bytes from rsp=0x{:X}. Values that point just after a call instruction:\n",
				g_stackCopyLen, g_stackCopyBase);
			std::size_t shown = 0;
			for (std::size_t off = 0; off + 8 <= g_stackCopyLen && shown < 80; off += 8) {
				std::uint64_t v = 0;
				std::memcpy(&v, g_stackCopy + off, sizeof(v));
				if (!FindModule(a_mods, v) || !IsProbableReturn(v)) {
					continue;
				}
				a_out += std::format("  [rsp+{:5X}] {}\n", off, Describe(a_mods, v));
				++shown;
			}
		}

		int BackupLogs(const std::filesystem::path& a_dst, const std::filesystem::path& a_statsFile)
		{
			const auto src = SKSE::log::log_directory();
			if (!src) {
				return 0;
			}
			std::error_code ec;
			std::filesystem::create_directories(a_dst, ec);
			int copied = 0;
			for (const auto& e : std::filesystem::directory_iterator(*src, ec)) {
				std::error_code ec2;
				if (!e.is_regular_file(ec2) || e.path().extension() != ".log") {
					continue;
				}
				if (e.last_write_time(ec2) < g_sessionStart) {
					continue;  // older sessions (e.g. old crash logs)
				}
				if (std::filesystem::copy_file(e.path(), a_dst / e.path().filename(), std::filesystem::copy_options::overwrite_existing, ec2)) {
					++copied;
				}
			}
			if (!a_statsFile.empty()) {
				std::error_code ec3;
				if (std::filesystem::copy_file(a_statsFile, a_dst / a_statsFile.filename(), std::filesystem::copy_options::overwrite_existing, ec3)) {
					++copied;
				}
			}
			return copied;
		}

		bool WriteDump(const std::filesystem::path& a_file, int a_level)
		{
			const HANDLE file = CreateFileW(a_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}
			DWORD type = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
			if (a_level >= 2) {
				type |= MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData | MiniDumpWithProcessThreadData;
			}
			if (a_level >= 3) {
				type |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;
			}
			const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(type), nullptr, nullptr, nullptr);
			CloseHandle(file);
			return ok != FALSE;
		}

		HWND FindGameWindow()
		{
			struct Search
			{
				DWORD pid;
				HWND  hwnd;
			} search{ GetCurrentProcessId(), nullptr };

			EnumWindows(
				[](HWND a_hwnd, LPARAM a_param) -> BOOL {
					auto* s = reinterpret_cast<Search*>(a_param);
					DWORD pid = 0;
					GetWindowThreadProcessId(a_hwnd, &pid);
					if (pid == s->pid && IsWindowVisible(a_hwnd) && !GetWindow(a_hwnd, GW_OWNER)) {
						s->hwnd = a_hwnd;
						return FALSE;
					}
					return TRUE;
				},
				reinterpret_cast<LPARAM>(&search));
			return search.hwnd;
		}

		void Alert()
		{
			const auto& cfg = Settings::Get();
			if (cfg.flashWindow) {
				if (const auto hwnd = FindGameWindow()) {
					FLASHWINFO fi{};
					fi.cbSize = sizeof(fi);
					fi.hwnd = hwnd;
					fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
					fi.uCount = 5;
					FlashWindowEx(&fi);
				}
			}
			if (cfg.beep) {
				for (int i = 0; i < 3; ++i) {
					Beep(1200, 200);
					Sleep(100);
				}
			}
		}
	}

	namespace
	{
		bool IsSystemModule(const std::string& a_name)
		{
			static constexpr const char* kSystem[] = { "ntdll.dll", "KERNELBASE.dll", "KERNEL32.DLL", "win32u.dll", "USER32.dll",
				"MSVCP140.dll", "msvcp_win.dll", "ucrtbase.dll", "VCRUNTIME140.dll" };
			for (const auto s : kSystem) {
				if (_stricmp(a_name.c_str(), s) == 0) {
					return true;
				}
			}
			return false;
		}

		// First frame that is not Windows/runtime plumbing: usually the code that is actually waiting or working
		std::string FirstInterestingFrame(const ThreadStack& a_ts, const std::vector<Module>& a_mods)
		{
			for (std::size_t i = 0; i < a_ts.count; ++i) {
				const auto m = FindModule(a_mods, a_ts.frames[i]);
				if (m && !IsSystemModule(m->name)) {
					return Describe(a_mods, a_ts.frames[i]);
				}
			}
			return a_ts.count ? Describe(a_mods, a_ts.frames[0]) : std::string("(no frames)");
		}

		bool SameStack(const ThreadStack& a, const ThreadStack& b)
		{
			if (a.count != b.count || a.rsp != b.rsp) {
				return false;
			}
			for (std::size_t i = 0; i < a.count; ++i) {
				if (a.frames[i] != b.frames[i]) {
					return false;
				}
			}
			return true;
		}

		// Sample threads several times and compare. Labels are clues, not verdicts:
		//   SAME       identical stack in every sample. Stuck OR simply idle/asleep (most idle threads look like this).
		//   CHANGED    the stack differed between samples, so the thread was doing something.
		//   INCOMPLETE at least one sample could not be taken, so no comparison is possible.
		void AppendSampling(std::string& a_out, DWORD a_mainTid, const std::vector<Module>& a_mods, int a_samples, int a_intervalMs, bool a_allThreads)
		{
			const DWORD self = GetCurrentThreadId();
			std::unordered_map<DWORD, std::vector<ThreadStack>> samples;
			if (a_allThreads) {
				for (const auto tid : ThreadIds()) {
					if (tid != self) {
						samples[tid].reserve(static_cast<std::size_t>(a_samples));
					}
				}
			} else if (a_mainTid != 0) {
				samples[a_mainTid].reserve(static_cast<std::size_t>(a_samples));
			}

			for (int n = 0; n < a_samples; ++n) {
				if (n > 0) {
					Sleep(static_cast<DWORD>(a_intervalMs));
				}
				for (auto& [tid, vec] : samples) {
					ThreadStack ts;
					if (CaptureThread(tid, ts, false)) {
						vec.push_back(ts);
					}
				}
			}

			int same = 0, changed = 0, incomplete = 0;
			std::string lines;
			auto describeThread = [&](DWORD a_tid, const std::vector<ThreadStack>& a_vec) {
				const char* label = "SAME";
				if (a_vec.size() != static_cast<std::size_t>(a_samples)) {
					label = "INCOMPLETE";
					++incomplete;
				} else {
					bool identical = true;
					for (std::size_t i = 1; identical && i < a_vec.size(); ++i) {
						identical = SameStack(a_vec[0], a_vec[i]);
					}
					label = identical ? "SAME" : "CHANGED";
					identical ? ++same : ++changed;
				}
				lines += std::format("  {:<10} thread {:<6} {}  {}\n", label, a_tid, a_tid == a_mainTid ? "(MAIN)" : "      ",
					a_vec.empty() ? std::string("(no samples)") : FirstInterestingFrame(a_vec.back(), a_mods));
				if (a_tid == a_mainTid && std::string_view(label) == "CHANGED") {
					for (std::size_t i = 0; i < a_vec.size(); ++i) {
						lines += std::format("             sample {}: {}\n", i + 1, FirstInterestingFrame(a_vec[i], a_mods));
					}
				}
			};
			if (const auto it = samples.find(a_mainTid); it != samples.end()) {
				describeThread(it->first, it->second);
			}
			for (const auto& [tid, vec] : samples) {
				if (tid != a_mainTid) {
					describeThread(tid, vec);
				}
			}
			a_out += std::format("\n[THREAD SAMPLING] {} samples, {} ms apart ({}): {} same, {} changed, {} incomplete\n",
				a_samples, a_intervalMs, a_allThreads ? "all threads" : "main thread only", same, changed, incomplete);
			a_out += "  SAME = stack never changed (stuck, or just idle/asleep). CHANGED = it ran different code between samples.\n"
			         "  These are clues for where to look, not proof of which mod caused a freeze.\n";
			a_out += lines;
		}

		// ---------- capture progress, watched by a guard thread ----------
		std::atomic<std::int64_t> g_captureStartNs{ 0 };
		std::atomic<const char*>  g_captureStep{ "" };
		std::atomic<int>          g_captureIndex{ 0 };

		void SetStep(const char* a_step) { g_captureStep.store(a_step); }

		// If the capture itself gets stuck (it runs inside the frozen game), say so in the log and beep,
		// so the user knows the evidence written so far is all there will be.
		void GuardThread()
		{
			std::int64_t warnedFor = 0;
			for (;;) {
				Sleep(2000);
				const auto started = g_captureStartNs.load();
				if (started == 0 || started == warnedFor) {
					continue;
				}
				const double secs = static_cast<double>(Monitor::NowNs() - started) / 1e9;
				const int    level = Settings::Get().minidumpLevel;
				const double limit = (std::string_view(g_captureStep.load()) == "minidump" && level >= 3) ? 1800.0 : 60.0;
				if (secs > limit) {
					warnedFor = started;
					spdlog::critical("Capture #{} has been in step '{}' for {:.0f}s and may itself be stuck. "
									 "Everything saved before this step is already on disk.",
						g_captureIndex.load(), g_captureStep.load(), secs);
					if (Settings::Get().beep) {
						Beep(400, 600);
					}
				}
			}
		}

		// ---------- minidump written by a separate helper process ----------
		// Writing a dump from inside a frozen process can deadlock on the same locks the game is stuck on.
		// TheWatcherDump.exe (shipped next to the DLL) writes it from outside instead.
		std::filesystem::path HelperPath()
		{
			HMODULE self = nullptr;
			wchar_t buf[MAX_PATH]{};
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&HelperPath), &self) &&
				GetModuleFileNameW(self, buf, MAX_PATH)) {
				return std::filesystem::path(buf).parent_path() / L"TheWatcherDump.exe";
			}
			return {};
		}

		// Returns: 1 written, 0 helper ran but failed or timed out, -1 helper not available
		int WriteDumpExternal(const std::filesystem::path& a_file, int a_level)
		{
			const auto exe = HelperPath();
			std::error_code ec;
			if (exe.empty() || !std::filesystem::exists(exe, ec)) {
				return -1;
			}
			std::wstring cmd = std::format(L"\"{}\" {} \"{}\" {}", exe.wstring(), GetCurrentProcessId(), a_file.wstring(), a_level);
			STARTUPINFOW        si{};
			PROCESS_INFORMATION pi{};
			si.cb = sizeof(si);
			if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
				spdlog::error("Could not start {} (error {})", exe.string(), GetLastError());
				return -1;
			}
			const DWORD timeout = a_level >= 3 ? 30 * 60 * 1000 : 3 * 60 * 1000;
			const DWORD wait = WaitForSingleObject(pi.hProcess, timeout);
			DWORD code = 1;
			if (wait == WAIT_OBJECT_0) {
				GetExitCodeProcess(pi.hProcess, &code);
			} else {
				spdlog::error("Dump helper still running after {} s; leaving it to finish on its own", timeout / 1000);
			}
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			if (wait == WAIT_OBJECT_0 && code != 0) {
				spdlog::error("Dump helper failed (exit code {})", code);
			}
			return (wait == WAIT_OBJECT_0 && code == 0) ? 1 : 0;
		}
	}

	void Init()
	{
		// Log backup keeps every log written since the GAME started (not since this plugin loaded), minus a
		// minute of slack, so logs from plugins that loaded before The Watcher and never wrote again are included.
		std::int64_t ageTicks = 0;  // 100 ns units
		FILETIME created{}, exited{}, kernel{}, user{}, now{};
		if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
			GetSystemTimeAsFileTime(&now);
			const auto toU64 = [](const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
			ageTicks = static_cast<std::int64_t>(toU64(now) - toU64(created));
		}
		const auto age = std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(ageTicks * 100));
		g_sessionStart = std::filesystem::file_time_type::clock::now() - age - std::chrono::minutes(1);

		std::thread(GuardThread).detach();
	}

	void WarnBeep()
	{
		const auto& cfg = Settings::Get();
		if (cfg.beep && cfg.alertOnWarning) {
			Beep(700, 150);
		}
	}

	void Run(const std::string& a_reason, int a_index, const std::filesystem::path& a_statsFile)
	{
		const auto& cfg = Settings::Get();
		auto&       s = Monitor::Get();

		const auto folder = Util::WatchdogDir() / std::format("stall_{}_{}", Util::Stamp(true), a_index);
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);

		g_captureIndex.store(a_index);
		g_captureStartNs.store(Monitor::NowNs());
		SetStep("thread stacks");

		// 1) Stacks. Only fixed-size locals are touched while a thread is suspended.
		const DWORD self = GetCurrentThreadId();
		const DWORD mainTid = s.mainThreadId.load();
		ThreadStack mainStack;
		bool        mainOk = false;
		std::vector<ThreadStack> others;

		if ((cfg.mainThreadStack || cfg.allThreadStacks) && mainTid != 0) {
			g_stackCopyLen = 0;
			mainOk = CaptureThread(mainTid, mainStack, true);
		}
		if (cfg.allThreadStacks) {
			for (const auto tid : ThreadIds()) {
				if (tid == self || tid == mainTid) {
					continue;
				}
				ThreadStack ts;
				if (CaptureThread(tid, ts, false)) {
					others.push_back(ts);
				}
			}
		}

		// 2) Build the report (threads are all running again)
		const auto   now = Monitor::NowNs();
		const bool   loading = s.loading.load();
		const auto   lastFrame = s.lastFrameNs.load();
		const double hbMs = lastFrame ? static_cast<double>(now - lastFrame) / 1e6 : -1.0;

		std::string report;
		report += std::format("The Watcher stall capture #{}  {}\n", a_index, Util::Stamp(false));
		report += std::format("Reason: {}\n", a_reason);
		report += std::format("Frame heartbeat age at stack capture: {:.0f} ms{} | Loading screen: {}\n", hbMs,
			hbMs >= 0.0 && hbMs < 1000.0 ? " (main thread was running frames again; stack shows current work, not the stall)" : "",
			loading ? "open" : "closed");
		if (loading) {
			report += std::format("Loading for {:.1f}s | load events {} | longest gap {:.0f} ms | frames during load {}\n",
				static_cast<double>(now - s.loadStartNs.load()) / 1e9, s.loadEvents.load(), s.loadMaxGapUs.load() / 1000.0, s.loadFrames.load());
		}
		report += std::format("Last known cell {:08X} '{}' ({}), high-process actors {}\n\n",
			s.cellFormID.load(), Monitor::CellName(), s.cellInterior.load() ? "interior" : "exterior", s.highActors.load());

		const auto mods = EnumModules();
		if (cfg.mainThreadStack || cfg.allThreadStacks) {
			if (mainOk) {
				AppendStack(report, "MAIN THREAD", mainStack, mods);
				AppendScan(report, mods);
			} else {
				report += std::format("[MAIN THREAD] capture failed (thread id {})\n", mainTid);
			}
		}
		for (const auto& ts : others) {
			report += "\n";
			AppendStack(report, "THREAD", ts, mods);
		}

		// 3) Save what we have right away, so a later step getting stuck can't lose it
		SetStep("writing report");
		spdlog::critical("===== STALL CAPTURE #{} -> {} =====\n{}", a_index, folder.string(), report);
		{
			std::ofstream out(folder / "stacks.txt");
			out << report;
		}

		// 4) Back up this session's logs
		if (cfg.backupLogs) {
			SetStep("log backup");
			const int n = BackupLogs(folder / "logs", a_statsFile);
			spdlog::critical("Backed up {} log files to {}", n, (folder / "logs").string());
		}

		// 5) Tell the user
		SetStep("alert");
		Alert();

		// 6) Thread sampling (takes about iThreadSamples x iSampleIntervalMs), appended to the saved report
		if (cfg.threadSamples > 1) {
			SetStep("thread sampling");
			std::string sampling;
			AppendSampling(sampling, mainTid, mods, cfg.threadSamples, cfg.sampleIntervalMs, cfg.allThreadStacks);
			spdlog::critical("{}", sampling);
			std::ofstream out(folder / "stacks.txt", std::ios::app);
			out << sampling;
		}

		// 7) Minidump last: slowest step. Written by the helper process when available.
		if (cfg.minidumpLevel > 0) {
			if (cfg.minidumpLevel >= 3) {
				spdlog::critical("Writing FULL memory dump; this can take minutes and 10-20+ GB of disk");
			}
			SetStep("minidump");
			const auto dumpPath = folder / "SkyrimSE.dmp";
			int result = -1;
			if (cfg.outOfProcessDump) {
				result = WriteDumpExternal(dumpPath, cfg.minidumpLevel);
				if (result == -1) {
					spdlog::warn("TheWatcherDump.exe not found next to TheWatcher.dll; writing the dump from inside the game instead");
				}
			}
			const char* how = "by helper process";
			if (result == -1) {
				result = WriteDump(dumpPath, cfg.minidumpLevel) ? 1 : 0;
				how = "from inside the game";
			}
			spdlog::critical("Minidump (level {}) {} {}: {}", cfg.minidumpLevel, result == 1 ? "written" : "FAILED", how, dumpPath.string());
		}

		SetStep("cleanup");
		Util::Prune(Util::WatchdogDir(), "stall_", cfg.keepStallCaptures);
		g_captureStartNs.store(0);
		SetStep("");
		spdlog::critical("===== capture #{} complete =====", a_index);
	}
}
