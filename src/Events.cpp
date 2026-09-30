#include "PCH.h"
#include "Events.h"
#include "Settings.h"
#include "Util.h"

namespace Events
{
	namespace
	{
		std::mutex        g_lock;
		std::ofstream     g_file;
		std::atomic<bool> g_open{ false };
	}

	void Open()
	{
		std::scoped_lock lock(g_lock);
		if (g_open.load()) {
			return;
		}
		const auto dir = Util::WatchdogDir();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		Util::Prune(dir, "events_", Settings::Get().keepEventFiles - 1);

		const auto path = dir / std::format("events_{}.log", Util::Stamp(true));
		g_file.open(path, std::ios::out | std::ios::trunc);
		if (!g_file) {
			spdlog::error("Could not open events file {}", path.string());
			return;
		}
		g_open.store(true);
		spdlog::info("Events file: {}", path.string());
	}

	bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

	void Write(std::string_view a_line)
	{
		if (!IsOpen()) {
			return;
		}
		const auto stamp = Util::StampMs();
		std::scoped_lock lock(g_lock);
		g_file << '[' << stamp << "] " << a_line << '\n';
		g_file.flush();  // a crash must not lose buffered lines
	}
}
