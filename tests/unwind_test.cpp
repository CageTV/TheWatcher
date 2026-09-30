// Standalone check for The Watcher 1.1's copy-then-unwind stack walk.
// A worker thread recurses to a known depth (mixing frame-pointer and plain frames) and then blocks.
// The test walks it two ways and requires identical frames:
//   OLD: unwind the live stack while the thread is suspended (The Watcher 1.0.x)
//   NEW: suspend, copy the stack, resume, unwind the copy with register rebasing (1.1)
// Build: cl /O2 /EHsc /std:c++20 unwind_test.cpp
#include <Windows.h>
#include <malloc.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

constexpr std::size_t kMaxFrames = 64;
constexpr std::size_t kStackCopyBytes = 256 * 1024;
alignas(16) static std::uint8_t g_work[kStackCopyBytes];

static bool SafeCopy(void* d, const void* s, std::size_t n)
{
	__try { std::memcpy(d, s, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static std::size_t CopyStack(std::uintptr_t rsp)
{
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(reinterpret_cast<LPCVOID>(rsp), &mbi, sizeof(mbi))) return 0;
	const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	if (end <= rsp) return 0;
	const auto len = (std::min<std::size_t>)(kStackCopyBytes, end - rsp);
	return SafeCopy(g_work, reinterpret_cast<const void*>(rsp), len) ? len : 0;
}

static std::size_t UnwindLive(const CONTEXT* a, std::uint64_t* out, std::size_t max)
{
	CONTEXT ctx = *a;
	volatile std::size_t n = 0;
	__try {
		while (n < max && ctx.Rip != 0) {
			out[n] = ctx.Rip; n = n + 1;
			DWORD64 ib = 0;
			auto fn = RtlLookupFunctionEntry(ctx.Rip, &ib, nullptr);
			if (fn) { PVOID hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, fn, &ctx, &hd, &est, nullptr); }
			else { ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp); ctx.Rsp += 8; }
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return n;
}

inline void Rebase(DWORD64& r, std::uintptr_t lo, std::uintptr_t hi, std::uintptr_t nlo) { if (r >= lo && r < hi) r = r - lo + nlo; }

static std::size_t UnwindCopy(const CONTEXT* a, std::size_t len, std::uint64_t* out, std::size_t max)
{
	CONTEXT ctx = *a;
	const std::uintptr_t lo = a->Rsp, hi = lo + len, clo = reinterpret_cast<std::uintptr_t>(g_work), chi = clo + len;
	volatile std::size_t n = 0;
	__try {
		while (n < max && ctx.Rip != 0) {
			out[n] = ctx.Rip; n = n + 1;
			Rebase(ctx.Rsp, lo, hi, clo); Rebase(ctx.Rbp, lo, hi, clo); Rebase(ctx.Rbx, lo, hi, clo);
			Rebase(ctx.Rsi, lo, hi, clo); Rebase(ctx.Rdi, lo, hi, clo); Rebase(ctx.R12, lo, hi, clo);
			Rebase(ctx.R13, lo, hi, clo); Rebase(ctx.R14, lo, hi, clo); Rebase(ctx.R15, lo, hi, clo);
			if (ctx.Rsp < clo || ctx.Rsp + 8 > chi) break;
			DWORD64 ib = 0;
			auto fn = RtlLookupFunctionEntry(ctx.Rip, &ib, nullptr);
			if (fn) { PVOID hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, fn, &ctx, &hd, &est, nullptr); }
			else { ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp); ctx.Rsp += 8; }
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return n;
}

static HANDLE g_ready, g_release;
volatile int g_sink;

// _alloca forces an RBP frame (UWOP_SET_FPREG), which is the case register rebasing exists for
__declspec(noinline) int DeepFP(int d);
__declspec(noinline) int Deep(int d)
{
	volatile char buf[64]; buf[0] = static_cast<char>(d);
	if (d == 0) { SetEvent(g_ready); WaitForSingleObject(g_release, INFINITE); return buf[0]; }
	return (d % 3 == 0 ? DeepFP(d - 1) : Deep(d - 1)) + buf[0];
}
__declspec(noinline) int DeepFP(int d)
{
	volatile char* p = static_cast<char*>(_alloca(32 + (d & 7) * 16)); p[0] = static_cast<char>(d);
	return Deep(d) + p[0];
}

static DWORD WINAPI Worker(LPVOID p) { g_sink = Deep(static_cast<int>(reinterpret_cast<std::intptr_t>(p))); return 0; }

int main()
{
	int failures = 0;
	for (int depth : { 5, 25, 60, 400 }) {
		g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		HANDLE t = CreateThread(nullptr, 1 << 20, Worker, reinterpret_cast<LPVOID>(static_cast<std::intptr_t>(depth)), 0, nullptr);
		WaitForSingleObject(g_ready, INFINITE);
		Sleep(50);

		std::uint64_t oldF[kMaxFrames]{}, newF[kMaxFrames]{};
		std::size_t oldN = 0, newN = 0, len = 0;
		CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
		// OLD method: unwind live while suspended
		SuspendThread(t); GetThreadContext(t, &ctx); oldN = UnwindLive(&ctx, oldF, kMaxFrames); ResumeThread(t);
		// NEW method: copy while suspended, unwind after resume
		CONTEXT ctx2{}; ctx2.ContextFlags = CONTEXT_FULL;
		SuspendThread(t); GetThreadContext(t, &ctx2); len = CopyStack(ctx2.Rsp); ResumeThread(t);
		newN = UnwindCopy(&ctx2, len, newF, kMaxFrames);

		bool same = oldN == newN && std::memcmp(oldF, newF, oldN * 8) == 0;
		std::printf("depth %3d: old %2zu frames, new %2zu frames, copy %6zu bytes -> %s\n", depth, oldN, newN, len, same ? "IDENTICAL" : "DIFFERENT");
		if (!same) {
			++failures;
			for (std::size_t i = 0; i < (std::max)(oldN, newN); ++i)
				std::printf("   [%2zu] %016llX  %016llX\n", i, i < oldN ? oldF[i] : 0ull, i < newN ? newF[i] : 0ull);
		}
		SetEvent(g_release); WaitForSingleObject(t, INFINITE);
		CloseHandle(t); CloseHandle(g_ready); CloseHandle(g_release);
	}
	std::printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
	return failures;
}
