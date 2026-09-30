#include "PCH.h"
#include "Capture.h"
#include "AddressLib.h"
#include "Events.h"
#include "Monitor.h"
#include "Settings.h"
#include "Util.h"

namespace Capture
{
	namespace
	{
		constexpr std::size_t kMaxFrames = 64;
		// (1.1) 256 KB: the stack is now copied and unwound from the copy, so the copy must hold the whole walk
		constexpr std::size_t kStackCopyBytes = 256 * 1024;
		constexpr std::size_t kScanBytes = 64 * 1024;

		std::filesystem::file_time_type g_sessionStart;

		// Static so nothing is heap-allocated while another thread is suspended. Watchdog thread only.
		alignas(16) std::uint8_t g_work[kStackCopyBytes];  // scratch copy of the thread's stack, unwound after resume
		alignas(16) std::uint8_t g_scan[kScanBytes];       // main thread's stack kept for the "stack scan" section
		std::size_t              g_scanLen = 0;
		std::uintptr_t           g_scanBase = 0;

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

		// ---------- (1.1) main-thread history ----------

		struct Sample
		{
			std::int64_t ns = 0;
			ThreadStack  st;
		};
		constexpr std::size_t          kHistoryMax = 256;
		std::array<Sample, kHistoryMax> g_hist;
		std::size_t                     g_histCount = 0;  // samples taken since the last reset (may exceed kHistoryMax)
		std::size_t                     g_histNext = 0;   // ring position

		std::filesystem::path g_slowPath;

		// ---------- functions that touch a suspended thread: no heap, no locks, SEH-guarded ----------

		bool SafeCopy(void* a_dst, const void* a_src, std::size_t a_len)
		{
			__try {
				std::memcpy(a_dst, a_src, a_len);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Copy the used part of the stack (rsp up to the end of its committed region) into g_work
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
			return SafeCopy(g_work, reinterpret_cast<const void*>(a_rsp), len) ? len : 0;
		}

		// ---------- (1.1) unwinding runs on the COPY, after the thread is resumed ----------
		// v1.0.x unwound the live stack while the thread was suspended. RtlLookupFunctionEntry can take a loader /
		// function-table lock; if the suspended thread held it, the watchdog blocked forever with the game suspended.
		// Now the thread is only suspended for GetThreadContext + one memcpy, and every register that points into the
		// original stack is redirected into the copy before each unwind step.

		inline void Rebase(DWORD64& a_reg, std::uintptr_t a_lo, std::uintptr_t a_hi, std::uintptr_t a_newLo)
		{
			if (a_reg >= a_lo && a_reg < a_hi) {
				a_reg = a_reg - a_lo + a_newLo;
			}
		}

		std::size_t UnwindCopy(const CONTEXT* a_start, std::size_t a_len, std::uint64_t* a_out, std::size_t a_max)
		{
			CONTEXT              ctx = *a_start;
			const std::uintptr_t lo = a_start->Rsp;
			const std::uintptr_t hi = lo + a_len;
			const std::uintptr_t copyLo = reinterpret_cast<std::uintptr_t>(g_work);
			const std::uintptr_t copyHi = copyLo + a_len;
			volatile std::size_t n = 0;
			__try {
				while (n < a_max && ctx.Rip != 0) {
					a_out[n] = ctx.Rip;
					n = n + 1;

					Rebase(ctx.Rsp, lo, hi, copyLo);
					Rebase(ctx.Rbp, lo, hi, copyLo);
					Rebase(ctx.Rbx, lo, hi, copyLo);
					Rebase(ctx.Rsi, lo, hi, copyLo);
					Rebase(ctx.Rdi, lo, hi, copyLo);
					Rebase(ctx.R12, lo, hi, copyLo);
					Rebase(ctx.R13, lo, hi, copyLo);
					Rebase(ctx.R14, lo, hi, copyLo);
					Rebase(ctx.R15, lo, hi, copyLo);
					if (ctx.Rsp < copyLo || ctx.Rsp + 8 > copyHi) {
						break;  // walked past the copied part of the stack
					}

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

		bool CaptureThread(DWORD a_tid, ThreadStack& a_out, bool a_keepForScan)
		{
			const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_tid);
			if (!h) {
				return false;
			}
			CONTEXT     ctx{};
			std::size_t len = 0;
			bool        got = false;
			if (SuspendThread(h) != static_cast<DWORD>(-1)) {
				ctx.ContextFlags = CONTEXT_FULL;
				if (GetThreadContext(h, &ctx)) {
					got = true;
					len = CopyStack(ctx.Rsp);
				}
				ResumeThread(h);
			}
			CloseHandle(h);
			if (!got) {
				return false;
			}

			// Thread is running again from here on
			a_out.tid = a_tid;
			a_out.rip = ctx.Rip;
			a_out.rsp = ctx.Rsp;
			if (len >= 8) {
				a_out.count = UnwindCopy(&ctx, len, a_out.frames.data(), a_out.frames.size());
			} else {
				a_out.frames[0] = ctx.Rip;
				a_out.count = ctx.Rip ? 1 : 0;
			}
			if (a_keepForScan) {
				g_scanLen = std::min(len, kScanBytes);
				std::memcpy(g_scan, g_work, g_scanLen);
				g_scanBase = ctx.Rsp;
			}
			return true;
		}

		// ---------- everything below runs with all threads running ----------

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

		bool IsGame(const Module* a_m) { return a_m && _stricmp(a_m->name.c_str(), "SkyrimSE.exe") == 0; }

		std::string Describe(const std::vector<Module>& a_mods, std::uintptr_t a_addr)
		{
			if (const auto m = FindModule(a_mods, a_addr)) {
				const auto off = a_addr - m->base;
				if (IsGame(m)) {
					return std::format("{}+{:07X}{}", m->name, off, AddressLib::Annotate(off));
				}
				return std::format("{}+{:07X}", m->name, off);
			}
			return std::format("0x{:X} (not in a module)", a_addr);
		}

		// (1.1) Version-stable short form for signatures: "SkyrimSE.exe!69464+0x35" or "hdtsmp64.dll+0x4318E"
		std::string SigFrame(const std::vector<Module>& a_mods, std::uintptr_t a_addr)
		{
			const auto m = FindModule(a_mods, a_addr);
			if (!m) {
				return "?";
			}
			const auto    off = a_addr - m->base;
			std::uint64_t id = 0;
			std::uint64_t delta = 0;
			if (IsGame(m) && AddressLib::Lookup(off, id, delta)) {
				return std::format("SkyrimSE.exe!{}+0x{:X}", id, delta);
			}
			return std::format("{}+0x{:X}", m->name, off);
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

		bool IsSystemModule(const std::string& a_name)
		{
			static constexpr const char* kSystem[] = { "ntdll.dll", "KERNELBASE.dll", "KERNEL32.DLL", "win32u.dll", "USER32.dll",
				"MSVCP140.dll", "msvcp_win.dll", "ucrtbase.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll" };
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

		// (1.1) First a_n non-system frames, short form, joined "a < b < c" (callee first)
		std::string Signature(const ThreadStack& a_ts, const std::vector<Module>& a_mods, std::size_t a_n = 3)
		{
			std::string out;
			std::size_t used = 0;
			for (std::size_t i = 0; i < a_ts.count && used < a_n; ++i) {
				const auto m = FindModule(a_mods, a_ts.frames[i]);
				if (!m || IsSystemModule(m->name)) {
					continue;
				}
				if (!out.empty()) {
					out += " < ";
				}
				out += SigFrame(a_mods, a_ts.frames[i]);
				++used;
			}
			return out.empty() ? "(no non-system frames)" : out;
		}

		// (1.1) How many frames (from the top) are real work, excluding the main loop and the chain of plugins that
		// hook its Main::Update call (SkyrimSE.exe 36544 on AE / 35551 on SE, plus the DLLs right below it:
		// hdtsmp64, DebugMenu, PlayerRotationGPSupport... on this list). Those sit on EVERY main-thread sample, so
		// counting them made "modules on the stack" show ~100% for plugins that were merely in the hook chain.
		std::size_t WorkDepth(const ThreadStack& a_ts, const std::vector<Module>& a_mods)
		{
			for (std::size_t k = 0; k < a_ts.count; ++k) {
				const auto m = FindModule(a_mods, a_ts.frames[k]);
				if (!IsGame(m)) {
					continue;
				}
				std::uint64_t id = 0;
				std::uint64_t delta = 0;
				if (AddressLib::Lookup(a_ts.frames[k] - m->base, id, delta) && (id == 36544 || id == 35551)) {
					std::size_t j = k;
					while (j > 0 && !IsGame(FindModule(a_mods, a_ts.frames[j - 1]))) {
						--j;
					}
					return j;
				}
			}
			return a_ts.count;
		}

		bool SameFrames(const ThreadStack& a, const ThreadStack& b)
		{
			if (a.count != b.count) {
				return false;
			}
			for (std::size_t i = 0; i < a.count; ++i) {
				if (a.frames[i] != b.frames[i]) {
					return false;
				}
			}
			return true;
		}

		bool SameStack(const ThreadStack& a, const ThreadStack& b)
		{
			return a.rsp == b.rsp && SameFrames(a, b);
		}

		void AppendFrames(std::string& a_out, const ThreadStack& a_ts, const std::vector<Module>& a_mods, std::size_t a_limit = kMaxFrames)
		{
			for (std::size_t i = 0; i < a_ts.count && i < a_limit; ++i) {
				a_out += std::format("  [{:2}] 0x{:X}  {}\n", i, a_ts.frames[i], Describe(a_mods, a_ts.frames[i]));
			}
			if (a_ts.count > a_limit) {
				a_out += std::format("  ... {} more frames\n", a_ts.count - a_limit);
			}
			if (a_ts.count == 0) {
				a_out += "  (no frames recovered)\n";
			}
		}

		void AppendStack(std::string& a_out, std::string_view a_label, const ThreadStack& a_ts, const std::vector<Module>& a_mods)
		{
			a_out += std::format("[{}] thread {}  rsp=0x{:X}\n", a_label, a_ts.tid, a_ts.rsp);
			AppendFrames(a_out, a_ts, a_mods);
		}

		std::string FramesJson(const ThreadStack& a_ts, const std::vector<Module>& a_mods, std::size_t a_limit = kMaxFrames)
		{
			std::string out = "[";
			for (std::size_t i = 0; i < a_ts.count && i < a_limit; ++i) {
				if (i) {
					out += ",";
				}
				out += Util::Json(Describe(a_mods, a_ts.frames[i]));
			}
			return out + "]";
		}

		void AppendScan(std::string& a_out, const std::vector<Module>& a_mods)
		{
			a_out += std::format("\n[MAIN THREAD stack scan] {} bytes from rsp=0x{:X}. Values that point just after a call instruction:\n",
				g_scanLen, g_scanBase);
			std::size_t shown = 0;
			for (std::size_t off = 0; off + 8 <= g_scanLen && shown < 80; off += 8) {
				std::uint64_t v = 0;
				std::memcpy(&v, g_scan + off, sizeof(v));
				if (!FindModule(a_mods, v) || !IsProbableReturn(v)) {
					continue;
				}
				a_out += std::format("  [rsp+{:5X}] {}\n", off, Describe(a_mods, v));
				++shown;
			}
		}

		// (1.1) Print each distinct stack once with the threads that share it, biggest group first
		void AppendGroupedStacks(std::string& a_out, const std::vector<ThreadStack>& a_stacks, const std::vector<Module>& a_mods, std::string& a_json)
		{
			std::vector<std::vector<std::size_t>> groups;
			for (std::size_t i = 0; i < a_stacks.size(); ++i) {
				bool placed = false;
				for (auto& g : groups) {
					if (SameFrames(a_stacks[g.front()], a_stacks[i])) {
						g.push_back(i);
						placed = true;
						break;
					}
				}
				if (!placed) {
					groups.push_back({ i });
				}
			}
			std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });

			a_out += std::format("\n[OTHER THREADS] {} threads, {} distinct stacks (identical stacks printed once)\n", a_stacks.size(), groups.size());
			a_json = "[";
			for (std::size_t gi = 0; gi < groups.size(); ++gi) {
				const auto& g = groups[gi];
				const auto& rep = a_stacks[g.front()];
				std::string ids;
				for (std::size_t k = 0; k < g.size() && k < 24; ++k) {
					ids += std::format("{}{}", k ? ", " : "", a_stacks[g[k]].tid);
				}
				if (g.size() > 24) {
					ids += std::format(", ... +{}", g.size() - 24);
				}
				a_out += std::format("\n[THREAD x{}] ids: {}\n", g.size(), ids);
				AppendFrames(a_out, rep, a_mods);
				if (gi < 15) {
					a_json += std::format("{}{{\"threads\":{},\"top\":{}}}", gi ? "," : "", g.size(), Util::Json(FirstInterestingFrame(rep, a_mods)));
				}
			}
			a_json += "]";
		}

		struct SamplingResult
		{
			int         same = 0;
			int         changed = 0;
			int         incomplete = 0;
			std::string mainState = "not sampled";
		};

		// Sample threads several times and compare. Labels are clues, not verdicts:
		//   SAME       identical stack in every sample. Stuck OR simply idle/asleep (most idle threads look like this).
		//   CHANGED    the stack differed between samples, so the thread was doing something.
		//   INCOMPLETE at least one sample could not be taken, so no comparison is possible.
		SamplingResult AppendSampling(std::string& a_out, DWORD a_mainTid, const std::vector<Module>& a_mods, int a_samples, int a_intervalMs, bool a_allThreads)
		{
			SamplingResult res;
			const DWORD    self = GetCurrentThreadId();
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

			std::string lines;
			auto describeThread = [&](DWORD a_tid, const std::vector<ThreadStack>& a_vec) {
				const char* label = "SAME";
				if (a_vec.size() != static_cast<std::size_t>(a_samples)) {
					label = "INCOMPLETE";
					++res.incomplete;
				} else {
					bool identical = true;
					for (std::size_t i = 1; identical && i < a_vec.size(); ++i) {
						identical = SameStack(a_vec[0], a_vec[i]);
					}
					label = identical ? "SAME" : "CHANGED";
					identical ? ++res.same : ++res.changed;
				}
				if (a_tid == a_mainTid) {
					res.mainState = label;
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
				a_samples, a_intervalMs, a_allThreads ? "all threads" : "main thread only", res.same, res.changed, res.incomplete);
			a_out += "  SAME = stack never changed (stuck, or just idle/asleep). CHANGED = it ran different code between samples.\n"
			         "  These are clues for where to look, not proof of which mod caused a freeze.\n";
			a_out += lines;
			return res;
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

		// ---------- (1.1) history summary ----------

		struct HistorySummary
		{
			std::string text;
			std::string json = "null";
			std::string signature;
			std::string topFrame;
		};

		HistorySummary SummarizeHistory(const std::vector<Module>& a_mods, std::size_t a_stacksInText)
		{
			HistorySummary out;
			const auto     n = std::min(g_histCount, kHistoryMax);
			if (n == 0) {
				return out;
			}
			// Oldest first
			std::vector<const Sample*> v;
			v.reserve(n);
			const auto start = g_histCount > kHistoryMax ? g_histNext : 0;
			for (std::size_t i = 0; i < n; ++i) {
				v.push_back(&g_hist[(start + i) % kHistoryMax]);
			}
			const double spanS = static_cast<double>(v.back()->ns - v.front()->ns) / 1e9;

			// Distinct stacks
			std::vector<std::pair<const ThreadStack*, std::size_t>> groups;
			for (const auto* s : v) {
				bool placed = false;
				for (auto& g : groups) {
					if (SameFrames(*g.first, s->st)) {
						++g.second;
						placed = true;
						break;
					}
				}
				if (!placed) {
					groups.emplace_back(&s->st, 1);
				}
			}
			std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

			// First interesting frame, and every non-system module seen anywhere on the stack, per sample
			std::unordered_map<std::string, std::size_t> frameCounts;
			std::unordered_map<std::string, std::size_t> moduleCounts;
			for (const auto* s : v) {
				++frameCounts[FirstInterestingFrame(s->st, a_mods)];
				std::vector<std::string> seen;
				const auto               depth = WorkDepth(s->st, a_mods);
				for (std::size_t i = 0; i < depth; ++i) {
					const auto m = FindModule(a_mods, s->st.frames[i]);
					if (m && !IsSystemModule(m->name) && std::find(seen.begin(), seen.end(), m->name) == seen.end()) {
						seen.push_back(m->name);
					}
				}
				for (auto& name : seen) {
					++moduleCounts[name];
				}
			}
			auto sorted = [](const std::unordered_map<std::string, std::size_t>& a_map) {
				std::vector<std::pair<std::string, std::size_t>> r(a_map.begin(), a_map.end());
				std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
				return r;
			};
			const auto frames = sorted(frameCounts);
			const auto modules = sorted(moduleCounts);
			const double total = static_cast<double>(n);

			out.signature = Signature(*groups.front().first, a_mods);
			out.topFrame = frames.front().first;

			auto& t = out.text;
			t += std::format("[MAIN THREAD HISTORY] {} samples over {:.1f}s{}, {} distinct stacks\n", n, spanS,
				g_histCount > kHistoryMax ? std::format(" (last {} of {} kept)", kHistoryMax, g_histCount) : "", groups.size());
			t += std::format("  Signature: {}\n", out.signature);
			t += "  Most common first frame (share of samples):\n";
			for (std::size_t i = 0; i < frames.size() && i < 5; ++i) {
				t += std::format("    {:3.0f}%  {}\n", 100.0 * frames[i].second / total, frames[i].first);
			}
			t += "  Modules on the stack above the main loop / Main::Update hook chain (share of samples):\n   ";
			for (std::size_t i = 0; i < modules.size() && i < 10; ++i) {
				t += std::format(" {} {:.0f}%{}", modules[i].first, 100.0 * modules[i].second / total, i + 1 < std::min<std::size_t>(modules.size(), 10) ? "," : "");
			}
			t += "\n";
			for (std::size_t g = 0; g < groups.size() && g < a_stacksInText; ++g) {
				t += std::format("  Stack #{} ({} of {} samples):\n", g + 1, groups[g].second, n);
				AppendFrames(t, *groups[g].first, a_mods, 30);
			}

			auto& j = out.json;
			j = std::format("{{\"samples\":{},\"span_s\":{:.2f},\"distinct_stacks\":{},\"signature\":{},\"top_frames\":[", n, spanS, groups.size(), Util::Json(out.signature));
			for (std::size_t i = 0; i < frames.size() && i < 5; ++i) {
				j += std::format("{}{{\"frame\":{},\"share\":{:.2f}}}", i ? "," : "", Util::Json(frames[i].first), frames[i].second / total);
			}
			j += "],\"modules\":[";
			for (std::size_t i = 0; i < modules.size() && i < 10; ++i) {
				j += std::format("{}{{\"module\":{},\"share\":{:.2f}}}", i ? "," : "", Util::Json(modules[i].first), modules[i].second / total);
			}
			j += "],\"stacks\":[";
			for (std::size_t g = 0; g < groups.size() && g < 3; ++g) {
				j += std::format("{}{{\"count\":{},\"frames\":{}}}", g ? "," : "", groups[g].second, FramesJson(*groups[g].first, a_mods, 40));
			}
			j += "]}";
			return out;
		}

		std::string ContextJson()
		{
			const auto& s = Monitor::Get();
			const auto  c = Monitor::GetContext();
			const auto  p = Monitor::ReadPapyrus();
			return std::format(
				"{{\"cell_formid\":\"{:08X}\",\"cell_name\":{},\"interior\":{},\"worldspace\":{},\"grid\":[{},{}],\"pos\":[{:.0f},{:.0f},{:.0f}],"
				"\"location\":{},\"open_menus\":{},\"load_kind\":{},\"high_actors\":{},\"spawned_actors\":{},\"mid_high_actors\":{},\"low_actors\":{},"
				"\"actor_plugins\":{},\"spawned_plugins\":{},\"papyrus\":{{\"running_stacks\":{},\"latent_waits\":{},\"queued_messages\":{},\"overstressed\":{}}}}}",
				s.cellFormID.load(), Util::Json(c.cellName), s.cellInterior.load() ? "true" : "false", Util::Json(c.worldspace), s.gridX.load(), s.gridY.load(),
				s.posX.load(), s.posY.load(), s.posZ.load(), Util::Json(c.location), Util::Json(c.openMenus), Util::Json(c.loadKind),
				s.highActors.load(), s.spawnedActors.load(), s.midHighActors.load(), s.lowActors.load(), Util::Json(c.actorPlugins), Util::Json(c.spawnPlugins),
				p.runningStacks, p.latentWaits, p.queuedMessages, p.overstressed);
		}

		int BackupLogs(const std::filesystem::path& a_dst, const std::filesystem::path& a_statsFile)
		{
			const auto src = Util::LogDir();
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
			// (1.1) this session's stats, events and slow-episode files too
			for (const auto& f : { a_statsFile, g_slowPath }) {
				if (f.empty()) {
					continue;
				}
				std::error_code ec3;
				if (std::filesystem::copy_file(f, a_dst / f.filename(), std::filesystem::copy_options::overwrite_existing, ec3)) {
					++copied;
				}
			}
			for (const auto& e : std::filesystem::directory_iterator(Util::WatchdogDir(), ec)) {
				std::error_code ec4;
				const auto      name = e.path().filename().string();
				if (name.starts_with("events_") && e.last_write_time(ec4) >= g_sessionStart &&
					std::filesystem::copy_file(e.path(), a_dst / e.path().filename(), std::filesystem::copy_options::overwrite_existing, ec4)) {
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

	// ---------- (1.1) history ----------

	void HistoryReset()
	{
		g_histCount = 0;
		g_histNext = 0;
	}

	bool HistorySample(std::uint32_t a_mainTid)
	{
		if (a_mainTid == 0) {
			return false;
		}
		auto& slot = g_hist[g_histNext];
		if (!CaptureThread(a_mainTid, slot.st, false)) {
			return false;
		}
		slot.ns = Monitor::NowNs();
		g_histNext = (g_histNext + 1) % kHistoryMax;
		++g_histCount;
		return true;
	}

	std::size_t HistoryCount() { return g_histCount; }

	std::string ReportSlowEpisode(const SlowEpisode& a_ep)
	{
		const auto mods = EnumModules();
		const auto sum = SummarizeHistory(mods, 1);
		const char* kind = a_ep.loading ? "LOADING GAP" : "HITCH";

		std::string head = a_ep.loading ?
		                       std::format("{} {:.1f}s with no load progress during load #{} ({})", kind, a_ep.seconds, a_ep.loadNumber, a_ep.loadKind) :
		                       std::format("{} {:.2f}s (main thread did not finish a frame)", kind, a_ep.seconds);
		if (a_ep.stallCapture) {
			head += std::format(" - stall capture #{} was taken during it", a_ep.stallCapture);
		}
		spdlog::warn("{} | {} | menus [{}]\n{}", head, a_ep.cellText, a_ep.openMenus, sum.text);
		Events::Write(std::format("{} {:.2f}s sig={}", kind, a_ep.seconds, sum.signature));

		if (g_slowPath.empty()) {
			const auto dir = Util::WatchdogDir();
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			Util::Prune(dir, "slow_", Settings::Get().keepStatsFiles - 1);
			g_slowPath = dir / std::format("slow_{}.jsonl", Util::Stamp(true));
		}
		std::ofstream out(g_slowPath, std::ios::out | std::ios::app);
		out << std::format("{{\"time\":{},\"kind\":{},\"seconds\":{:.3f},\"load_number\":{},\"load_kind\":{},\"stall_capture\":{},\"context\":{},\"history\":{}}}\n",
			Util::Json(Util::Stamp(false)), Util::Json(a_ep.loading ? "loading_gap" : "hitch"), a_ep.seconds, a_ep.loadNumber, Util::Json(a_ep.loadKind),
			a_ep.stallCapture, ContextJson(), sum.json);
		return sum.signature;
	}

	bool Run(const std::string& a_reason, int a_index, const std::filesystem::path& a_statsFile)
	{
		const auto& cfg = Settings::Get();
		auto&       s = Monitor::Get();

		const auto folder = Util::WatchdogDir() / std::format("stall_{}_{}", Util::Stamp(true), a_index);
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);

		g_captureIndex.store(a_index);
		g_captureStartNs.store(Monitor::NowNs());
		SetStep("thread stacks");

		// 1) Stacks. The main thread first, then decide whether the stall is still going on.
		//    A thread is only suspended for GetThreadContext + one copy of its stack.
		const DWORD self = GetCurrentThreadId();
		const DWORD mainTid = s.mainThreadId.load();
		ThreadStack mainStack;
		bool        mainOk = false;
		if ((cfg.mainThreadStack || cfg.allThreadStacks) && mainTid != 0) {
			g_scanLen = 0;
			mainOk = CaptureThread(mainTid, mainStack, true);
		}

		const auto   now = Monitor::NowNs();
		const bool   loading = s.loading.load();
		const auto   lastFrame = s.lastFrameNs.load();
		const double hbMs = lastFrame ? static_cast<double>(now - lastFrame) / 1e6 : -1.0;
		const double progressMs = static_cast<double>(now - s.lastProgressNs.load()) / 1e6;
		// Recovered: the stall ended between detection and capture, so the live stack shows ordinary work
		const bool recovered = loading ? progressMs < 1000.0 : (hbMs >= 0.0 && hbMs < 1000.0);
		const bool light = recovered && cfg.skipDumpIfRecovered;

		std::vector<ThreadStack> others;
		if (cfg.allThreadStacks && !light) {
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

		// 2) Build the report
		SetStep("building report");
		const auto mods = EnumModules();
		const auto hist = SummarizeHistory(mods, 3);

		std::string report;
		report += std::format("The Watcher 1.1.2 stall capture #{}  {}\n", a_index, Util::Stamp(false));
		report += std::format("Reason: {}\n", a_reason);
		report += std::format("Frame heartbeat age at stack capture: {:.0f} ms | Loading screen: {}\n", hbMs, loading ? "open" : "closed");
		if (recovered) {
			report += "RECOVERED: the game was running again when the stack was taken. The [MAIN THREAD] stack below is ordinary\n"
			          "work, NOT the stall; the [MAIN THREAD HISTORY] section (sampled during the stall) is the evidence.\n";
			if (light) {
				report += "No minidump / all-thread pass / sampling for a recovered capture (bSkipDumpIfRecovered=1); it does not count toward iMaxCapturesPerSession.\n";
			}
		}
		if (loading) {
			report += std::format("Loading for {:.1f}s | load events {} | longest gap {:.0f} ms | frames during load {}\n",
				static_cast<double>(now - s.loadStartNs.load()) / 1e9, s.loadEvents.load(), s.loadMaxGapUs.load() / 1000.0, s.loadFrames.load());
		}
		report += std::format("Context: {}\n\n", Monitor::ContextLine());

		if (!hist.text.empty()) {
			report += hist.text + "\n";
		} else {
			report += "[MAIN THREAD HISTORY] none (sampling off, or the stall began before sampling could start)\n\n";
		}

		if (cfg.mainThreadStack || cfg.allThreadStacks) {
			if (mainOk) {
				AppendStack(report, recovered ? "MAIN THREAD (after recovery)" : "MAIN THREAD", mainStack, mods);
				AppendScan(report, mods);
			} else {
				report += std::format("[MAIN THREAD] capture failed (thread id {})\n", mainTid);
			}
		}
		std::string groupsJson = "[]";
		if (!others.empty()) {
			if (cfg.groupThreadStacks) {
				AppendGroupedStacks(report, others, mods, groupsJson);
			} else {
				for (const auto& ts : others) {
					report += "\n";
					AppendStack(report, "THREAD", ts, mods);
				}
			}
		}

		const auto mainSig = mainOk ? Signature(mainStack, mods) : std::string();
		auto writeSummary = [&](const SamplingResult& a_sampling) {
			const auto json = std::format(
				"{{\n  \"watcher_version\":\"1.1.2\",\n  \"index\":{},\n  \"time\":{},\n  \"reason\":{},\n  \"recovered\":{},\n  \"heartbeat_age_ms\":{:.0f},\n"
				"  \"loading\":{},\n  \"signature\":{},\n  \"main_stack_signature\":{},\n  \"context\":{},\n  \"history\":{},\n  \"main_stack\":{},\n"
				"  \"sampling\":{{\"same\":{},\"changed\":{},\"incomplete\":{},\"main\":{}}},\n  \"thread_groups\":{}\n}}\n",
				a_index, Util::Json(Util::Stamp(false)), Util::Json(a_reason), recovered ? "true" : "false", hbMs, loading ? "true" : "false",
				Util::Json(!hist.signature.empty() ? hist.signature : mainSig), Util::Json(mainSig), ContextJson(), hist.json,
				mainOk ? FramesJson(mainStack, mods) : std::string("[]"), a_sampling.same, a_sampling.changed, a_sampling.incomplete,
				Util::Json(a_sampling.mainState), groupsJson);
			Util::WriteFileAtomic(folder / "summary.json", json);
		};

		// 3) Save what we have right away, so a later step getting stuck can't lose it
		SetStep("writing report");
		if (recovered) {
			spdlog::warn("===== STALL CAPTURE #{} (RECOVERED before capture) -> {} =====\n{}", a_index, folder.string(), report);
		} else {
			spdlog::critical("===== STALL CAPTURE #{} -> {} =====\n{}", a_index, folder.string(), report);
		}
		{
			std::ofstream out(folder / "stacks.txt");
			out << report;
		}
		writeSummary(SamplingResult{});
		Events::Write(std::format("STALL CAPTURE #{}{} {} -> {}", a_index, recovered ? " (recovered)" : "", a_reason, folder.filename().string()));

		// 4) Back up this session's logs
		if (cfg.backupLogs) {
			SetStep("log backup");
			const int n = BackupLogs(folder / "logs", a_statsFile);
			spdlog::critical("Backed up {} log files to {}", n, (folder / "logs").string());
		}

		if (!light) {
			// 5) Tell the user
			SetStep("alert");
			Alert();

			// 6) Thread sampling (takes about iThreadSamples x iSampleIntervalMs), appended to the saved report
			if (cfg.threadSamples > 1) {
				SetStep("thread sampling");
				std::string sampling;
				const auto  result = AppendSampling(sampling, mainTid, mods, cfg.threadSamples, cfg.sampleIntervalMs, cfg.allThreadStacks);
				spdlog::critical("{}", sampling);
				std::ofstream out(folder / "stacks.txt", std::ios::app);
				out << sampling;
				writeSummary(result);
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
		}

		SetStep("cleanup");
		Util::Prune(Util::WatchdogDir(), "stall_", cfg.keepStallCaptures);
		g_captureStartNs.store(0);
		SetStep("");
		spdlog::critical("===== capture #{} complete{} =====", a_index, recovered ? " (recovered)" : "");
		return !light;
	}
}
