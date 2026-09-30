// TheWatcherDump.exe - writes a minidump of another process from the outside.
// Started by The Watcher during a stall capture:  TheWatcherDump.exe <pid> <output.dmp> <level 1-3>
// Writing the dump from a separate process avoids deadlocking on locks held inside the frozen game.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>

#include <cstdlib>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
	if (argc < 4) {
		return 2;  // usage: pid, output path, level
	}

	const DWORD pid = static_cast<DWORD>(::wcstoul(argv[1], nullptr, 10));
	const int   level = ::_wtoi(argv[3]);
	if (pid == 0 || level < 1 || level > 3) {
		return 2;
	}

	const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
	if (!process) {
		return 3;
	}

	const HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		CloseHandle(process);
		return 4;
	}

	// Same dump levels as The Watcher's in-process writer
	DWORD type = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
	if (level >= 2) {
		type |= MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData | MiniDumpWithProcessThreadData;
	}
	if (level >= 3) {
		type |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;
	}

	const BOOL ok = MiniDumpWriteDump(process, pid, file, static_cast<MINIDUMP_TYPE>(type), nullptr, nullptr, nullptr);

	CloseHandle(file);
	CloseHandle(process);
	return ok ? 0 : 5;
}
