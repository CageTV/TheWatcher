#include "PCH.h"
#include "AddressLib.h"
#include "Capture.h"
#include "Monitor.h"
#include "Settings.h"
#include "Watchdog.h"

// Printed in the build output, so you can see whether CommonLib was built with SE (1.5.97) support.
// The authoritative check is the DLL's exports: SKSEPlugin_Query (SE) and SKSEPlugin_Version (AE).
#if defined(ENABLE_SKYRIM_SE)
#	pragma message("The Watcher: ENABLE_SKYRIM_SE is defined (SE 1.5.97 support compiled in)")
#else
#	pragma message("The Watcher: ENABLE_SKYRIM_SE is NOT defined - check the DLL exports for SKSEPlugin_Query")
#endif
#if defined(ENABLE_SKYRIM_AE)
#	pragma message("The Watcher: ENABLE_SKYRIM_AE is defined (AE 1.6.x support compiled in)")
#endif

namespace
{
	void SetupLog()
	{
		const auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		const auto path = *dir / "TheWatcher.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("TheWatcher", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);  // flush every line: a hang must not lose buffered log text
		log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] %v");
		spdlog::set_default_logger(std::move(log));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Monitor::RegisterEventSinks();
			Watchdog::Start();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();

	const auto ver = REL::Module::get().version();
	const char* runtime = ver.minor() >= 6 ? "AE (1.6.x)" : (ver.minor() == 5 ? "SE (1.5.x)" : "unsupported/untested (VR or other)");
	spdlog::info("The Watcher 1.0.4, game {} - runtime {}", ver.string(), runtime);

	// Plugins load on the game's main thread; the frame hook confirms this on its first call anyway
	Monitor::Get().mainThreadId.store(GetCurrentThreadId());
	Capture::Init();

	auto& cfg = Settings::Get();
	cfg.Load();
	cfg.LogValues();
	if (cfg.addressLibIDs) {
		AddressLib::Load();
	}

	if (cfg.mode == 0) {
		spdlog::info("iMode=0: watchdog is off");
		return true;
	}

	if (!Monitor::InstallFrameHook(cfg)) {
		spdlog::error("Continuing without frame heartbeat; only loading-screen stalls will be detected");
	}

	if (const auto messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}
	return true;
}
