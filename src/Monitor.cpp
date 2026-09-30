#include "PCH.h"
#include "Monitor.h"
#include "Settings.h"

namespace Monitor
{
	namespace
	{
		State        g_state;
		std::mutex   g_nameLock;
		std::string  g_cellName;
		std::int64_t g_hitchUs = 100'000;

		// Main-thread-only bookkeeping
		std::int64_t  g_lastInfoNs = 0;
		int           g_menuClosedChecks = 0;
		std::uint64_t g_seenLoadGen = 0;
		std::uint64_t g_seenFocusGen = 0;

		void UpdateMax(std::atomic<std::uint64_t>& a_value, std::uint64_t a_candidate)
		{
			auto cur = a_value.load(std::memory_order_relaxed);
			while (a_candidate > cur && !a_value.compare_exchange_weak(cur, a_candidate, std::memory_order_relaxed)) {
			}
		}

		// ---------- loading screens ----------

		void OnLoadStart()
		{
			const auto now = NowNs();
			g_state.loadStartNs.store(now);
			g_state.lastProgressNs.store(now);
			g_state.loadEvents.store(0);
			g_state.loadMaxGapUs.store(0);
			g_state.loadFrames.store(0);
			g_state.loadGen.fetch_add(1);
			g_state.loading.store(true);
			spdlog::info("Loading screen opened (load #{}), cell before load {:08X} '{}'",
				g_state.loadCount.load() + 1, g_state.cellFormID.load(), CellName());
		}

		void OnLoadEnd(std::string_view a_how)
		{
			if (!g_state.loading.exchange(false)) {
				return;
			}
			const auto   now = NowNs();
			const double secs = static_cast<double>(now - g_state.loadStartNs.load()) / 1e9;
			const auto   n = g_state.loadCount.fetch_add(1) + 1;
			g_state.loadEndNs.store(now);
			// Count the quiet stretch between the last load event and the loading screen closing
			if (const auto last = g_state.lastProgressNs.load(); last > 0 && now > last) {
				UpdateMax(g_state.loadMaxGapUs, static_cast<std::uint64_t>((now - last) / 1000));
			}
			g_state.loadGen.fetch_add(1);
			g_state.intervalLoads.fetch_add(1);
			UpdateMax(g_state.intervalLongestLoadMs, static_cast<std::uint64_t>(secs * 1000.0));

			spdlog::info("Load #{} finished in {:.2f}s ({}): {} load events, longest gap {:.0f} ms, {} frames during load",
				n, secs, a_how, g_state.loadEvents.load(), g_state.loadMaxGapUs.load() / 1000.0, g_state.loadFrames.load());
		}

		void OnLoadProgress()
		{
			g_state.intervalLoadEvents.fetch_add(1, std::memory_order_relaxed);
			if (!g_state.loading.load(std::memory_order_relaxed)) {
				return;
			}
			const auto now = NowNs();
			const auto prev = g_state.lastProgressNs.exchange(now);
			if (prev > 0 && now > prev) {
				UpdateMax(g_state.loadMaxGapUs, static_cast<std::uint64_t>((now - prev) / 1000));
			}
			g_state.loadEvents.fetch_add(1, std::memory_order_relaxed);
		}

		// ---------- main thread, once a second ----------

		void UpdateGameInfo()
		{
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				if (const auto cell = player->GetParentCell()) {
					const auto id = cell->GetFormID();
					if (id != g_state.cellFormID.load()) {
						const char*       name = cell->GetName();
						std::scoped_lock lock(g_nameLock);
						g_cellName = name ? name : "";
					}
					g_state.cellFormID.store(id);
					g_state.cellInterior.store(cell->IsInteriorCell());
				}
			}
			if (const auto lists = RE::ProcessLists::GetSingleton()) {
				g_state.highActors.store(static_cast<std::uint32_t>(lists->highActorHandles.size()));
			}
		}

		// Safety net: if the "closed" event is ever missed, don't stay in loading mode forever
		void ReconcileLoading()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				g_menuClosedChecks = 0;
				return;
			}
			if (++g_menuClosedChecks >= 3) {
				g_menuClosedChecks = 0;
				OnLoadEnd("menu no longer open, close event missed");
			}
		}

		void OnFrame()
		{
			static bool threadRecorded = false;
			if (!threadRecorded) {
				threadRecorded = true;
				const auto tid = static_cast<std::uint32_t>(GetCurrentThreadId());
				if (g_state.mainThreadId.exchange(tid) != tid) {
					spdlog::info("Main thread id set from frame hook: {}", tid);
				}
			}

			const auto now = NowNs();
			const auto prev = g_state.lastFrameNs.exchange(now);
			const auto gen = g_state.loadGen.load(std::memory_order_relaxed);
			const bool loading = g_state.loading.load(std::memory_order_relaxed);
			const auto focusGen = g_state.focusGen.load(std::memory_order_relaxed);

			if (loading) {
				g_state.loadFrames.fetch_add(1, std::memory_order_relaxed);
			} else if (prev > 0 && gen == g_seenLoadGen && focusGen == g_seenFocusGen) {
				// Only time frames that did not straddle a loading screen or an alt-tab
				const auto dtUs = static_cast<std::uint64_t>((now - prev) / 1000);
				g_state.intervalFrames.fetch_add(1, std::memory_order_relaxed);
				g_state.intervalFrameUsSum.fetch_add(dtUs, std::memory_order_relaxed);
				UpdateMax(g_state.intervalFrameUsMax, dtUs);
				if (static_cast<std::int64_t>(dtUs) >= g_hitchUs) {
					g_state.intervalHitches.fetch_add(1, std::memory_order_relaxed);
				}
			}
			g_seenLoadGen = gen;
			g_seenFocusGen = focusGen;

			if (now - g_lastInfoNs >= 1'000'000'000) {
				g_lastInfoNs = now;
				if (loading) {
					ReconcileLoading();
				} else {
					g_menuClosedChecks = 0;
					UpdateGameInfo();
				}
			}
		}

		// Chains the same call site hdtsmp64 hooks (SkyrimSE.exe 36544+0x160 on AE, visible in your crash logs)
		struct MainUpdateHook
		{
			static void thunk(RE::Main* a_main, float a_arg)
			{
				OnFrame();
				func(a_main, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------- event sinks ----------

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuSink* Get()
			{
				static MenuSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					if (a_event->opening) {
						OnLoadStart();
					} else {
						OnLoadEnd("loading menu closed");
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class LoadSink final :
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
			public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
		{
		public:
			static LoadSink* Get()
			{
				static LoadSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent*, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				OnLoadProgress();
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent*, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				OnLoadProgress();
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent*, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
			{
				OnLoadProgress();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	State& Get() { return g_state; }

	std::int64_t NowNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::string CellName()
	{
		std::scoped_lock lock(g_nameLock);
		return g_cellName;
	}

	bool InstallFrameHook(const Settings& a_settings)
	{
		g_hitchUs = static_cast<std::int64_t>(a_settings.hitchMs * 1000.0f);

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(35551, 36544), REL::Relocate(0x11F, 0x160) };
		const auto addr = target.address();
		const auto opcode = *reinterpret_cast<const std::uint8_t*>(addr);
		if (opcode != 0xE8) {
			spdlog::error("Frame hook site SkyrimSE.exe+{:X} is not a call (byte {:02X}); frame-stall detection disabled",
				addr - REL::Module::get().base(), opcode);
			return false;
		}

		SKSE::AllocTrampoline(14);
		MainUpdateHook::func = SKSE::GetTrampoline().write_call<5>(addr, MainUpdateHook::thunk);
		spdlog::info("Frame hook installed at SkyrimSE.exe+{:X} (ID {}+0x{:X}, {} address set)", addr - REL::Module::get().base(),
			REL::Module::get().version().minor() >= 6 ? 36544 : 35551,
			REL::Module::get().version().minor() >= 6 ? 0x160 : 0x11F,
			REL::Module::get().version().minor() >= 6 ? "AE" : "SE");
		return true;
	}

	void RegisterEventSinks()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
		} else {
			spdlog::error("UI singleton missing; loading screens will not be tracked");
		}

		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESObjectLoadedEvent>(LoadSink::Get());
			holder->AddEventSink<RE::TESCellAttachDetachEvent>(LoadSink::Get());
			holder->AddEventSink<RE::TESCellFullyLoadedEvent>(LoadSink::Get());
		} else {
			spdlog::error("ScriptEventSourceHolder missing; load progress will not be tracked");
		}
		spdlog::info("Event sinks registered");
	}
}
