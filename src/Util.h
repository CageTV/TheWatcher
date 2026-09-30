#pragma once

namespace Util
{
	// Local time. a_forFile = "20260928_213314", otherwise "2026-09-28 21:33:14"
	inline std::string Stamp(bool a_forFile)
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		if (a_forFile) {
			return std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		}
		return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	}

	// Documents\My Games\Skyrim Special Edition\SKSE\TheWatcher
	inline std::filesystem::path WatchdogDir()
	{
		const auto dir = SKSE::log::log_directory();
		return dir ? *dir / "TheWatcher" : std::filesystem::path("TheWatcher");
	}

	// Keep the newest a_keep entries whose name starts with a_prefix (names are timestamped, so they sort by age)
	inline void Prune(const std::filesystem::path& a_dir, std::string_view a_prefix, int a_keep)
	{
		std::error_code ec;
		std::vector<std::filesystem::path> found;
		for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
			if (e.path().filename().string().starts_with(a_prefix)) {
				found.push_back(e.path());
			}
		}
		if (static_cast<int>(found.size()) <= a_keep) {
			return;
		}
		std::sort(found.begin(), found.end());
		const auto toRemove = found.size() - static_cast<std::size_t>(a_keep);
		for (std::size_t i = 0; i < toRemove; ++i) {
			std::error_code ec2;
			std::filesystem::remove_all(found[i], ec2);
		}
	}
}
