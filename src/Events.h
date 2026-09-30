#pragma once

// (1.1) Breadcrumb log: one timestamped line per game event (loads, menus, cell changes, equips, hitches),
// flushed immediately so the last lines before a crash survive it. Compare its tail with CrashLogger's crash-*.log.
// File: TheWatcher\events_<date_time>.log
namespace Events
{
	void Open();
	[[nodiscard]] bool IsOpen();
	void Write(std::string_view a_line);
}
