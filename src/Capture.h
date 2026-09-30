#pragma once

namespace Capture
{
	// Call once at plugin load (records the session start so log backup only copies this session's logs)
	void Init();

	// Grab stacks, back up logs, alert, write minidump. Runs on the watchdog thread.
	void Run(const std::string& a_reason, int a_index, const std::filesystem::path& a_statsFile);

	// Short single beep (used for warnings)
	void WarnBeep();
}
