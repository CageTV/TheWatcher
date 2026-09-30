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

	// Local time with milliseconds, for event lines: "2026-09-28 21:33:14.123"
	inline std::string StampMs()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	}

	// (1.1) The SKSE log folder: the folder where skse64.log itself was written this session.
	// CommonLibSSE-NG 3.7's log_directory() derives the folder name from the game's INI setting and on some installs
	// returns "My Games\Skyrim.INI\SKSE" instead of "My Games\Skyrim Special Edition\SKSE". Look where SKSE actually
	// writes (newest skse64.log among the SE / GOG folders) and fall back to log_directory() only if neither exists.
	inline std::optional<std::filesystem::path> LogDir()
	{
		static const std::optional<std::filesystem::path> dir = []() -> std::optional<std::filesystem::path> {
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs)) && docs) {
				const std::filesystem::path myGames = std::filesystem::path(docs) / "My Games";
				CoTaskMemFree(docs);
				std::optional<std::filesystem::path>   best;
				std::filesystem::file_time_type        bestTime{};
				for (const auto* name : { L"Skyrim Special Edition", L"Skyrim Special Edition GOG" }) {
					const auto      candidate = myGames / name / "SKSE";
					std::error_code ec;
					const auto      t = std::filesystem::last_write_time(candidate / "skse64.log", ec);
					if (!ec && (!best || t > bestTime)) {
						best = candidate;
						bestTime = t;
					}
				}
				if (best) {
					return best;
				}
			}
			return SKSE::log::log_directory();
		}();
		return dir;
	}

	// Documents\My Games\Skyrim Special Edition\SKSE\TheWatcher
	inline std::filesystem::path WatchdogDir()
	{
		const auto dir = LogDir();
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

	// Quote and escape a string for JSON output
	inline std::string Json(std::string_view a_s)
	{
		std::string out;
		out.reserve(a_s.size() + 2);
		out += '"';
		for (const char c : a_s) {
			switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					out += std::format("\\u{:04x}", static_cast<unsigned int>(static_cast<unsigned char>(c)));
				} else {
					out += c;
				}
			}
		}
		out += '"';
		return out;
	}

	// For CSV: wrap in quotes, turn embedded quotes into apostrophes
	inline std::string Csv(std::string a_s)
	{
		std::replace(a_s.begin(), a_s.end(), '"', '\'');
		return "\"" + a_s + "\"";
	}

	// Write a file through a temp file + rename, so a reader never sees it half-written
	inline bool WriteFileAtomic(const std::filesystem::path& a_path, std::string_view a_text)
	{
		auto tmp = a_path;
		tmp += L".tmp";
		{
			std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
			if (!out) {
				return false;
			}
			out.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
		}
		return MoveFileExW(tmp.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
	}
}
