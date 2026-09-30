#include "PCH.h"
#include "Monitor.h"
#include "Events.h"
#include "Settings.h"

namespace Monitor
{
	namespace
	{
		State        g_state;
		std::mutex   g_nameLock;  // guards g_ctx and the pending load kind
		Context      g_ctx;
		std::int64_t g_hitchUs = 100'000;

		std::vector<std::string> g_openMenus;  // guarded by g_nameLock
		std::string              g_pendingLoadKind;
		std::int64_t             g_pendingLoadKindNs = 0;

		// Main-thread-only bookkeeping
		std::int64_t  g_lastInfoNs = 0;
		int           g_menuClosedChecks = 0;
		std::uint64_t g_seenLoadGen = 0;
		std::uint64_t g_seenFocusGen = 0;
		std::uint32_t g_lastLocationID = 0;

		void UpdateMax(std::atomic<std::uint64_t>& a_value, std::uint64_t a_candidate)
		{
			auto cur = a_value.load(std::memory_order_relaxed);
			while (a_candidate > cur && !a_value.compare_exchange_weak(cur, a_candidate, std::memory_order_relaxed)) {
			}
		}

		std::string FileOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			const auto file = a_form->GetFile(0);
			return file ? std::string(file->GetFilename()) : std::string();
		}

		std::string JoinMenus()
		{
			std::string out;
			for (const auto& m : g_openMenus) {
				if (!out.empty()) {
					out += "|";
				}
				out += m;
			}
			return out;
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

			std::string kind = "transition";
			std::string cellName;
			{
				std::scoped_lock lock(g_nameLock);
				// A save load / new game message may arrive just before the loading screen opens
				if (!g_pendingLoadKind.empty() && now - g_pendingLoadKindNs < 15'000'000'000) {
					kind = g_pendingLoadKind;
				}
				g_pendingLoadKind.clear();
				g_ctx.loadKind = kind;
				cellName = g_ctx.cellName;
			}
			const auto n = g_state.loadCount.load() + 1;
			spdlog::info("Loading screen opened (load #{}, {}), cell before load {:08X} '{}'", n, kind, g_state.cellFormID.load(), cellName);
			Events::Write(std::format("LOAD START #{} ({}) from cell {:08X} '{}'", n, kind, g_state.cellFormID.load(), cellName));
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

			std::string kind;
			{
				std::scoped_lock lock(g_nameLock);
				kind = g_ctx.loadKind;
			}
			spdlog::info("Load #{} finished in {:.2f}s ({}, {}): {} load events, longest gap {:.0f} ms, {} frames during load",
				n, secs, kind, a_how, g_state.loadEvents.load(), g_state.loadMaxGapUs.load() / 1000.0, g_state.loadFrames.load());
			Events::Write(std::format("LOAD END   #{} ({}) {:.2f}s, {} load events, longest gap {:.0f} ms",
				n, kind, secs, g_state.loadEvents.load(), g_state.loadMaxGapUs.load() / 1000.0));
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

		// Top N "plugin:count" entries, most first
		std::string TopPlugins(const std::unordered_map<std::string, std::uint32_t>& a_tally, std::size_t a_n)
		{
			std::vector<std::pair<std::string, std::uint32_t>> v(a_tally.begin(), a_tally.end());
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
			std::string out;
			for (std::size_t i = 0; i < v.size() && i < a_n; ++i) {
				if (!out.empty()) {
					out += " ";
				}
				out += std::format("{}:{}", v[i].first.empty() ? "(generated)" : v[i].first, v[i].second);
			}
			return out;
		}

		// Which plugin the actor "comes from": its base NPC's plugin, looking through runtime-generated bases
		// (leveled spawns get an FF-prefixed temporary base) to the original leveled base when needed.
		std::string ActorPlugin(RE::Actor* a_actor)
		{
			auto base = a_actor->GetActorBase();
			if (base && (base->GetFormID() >> 24) != 0xFF) {
				return FileOf(base);
			}
			if (const auto lvl = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>(); lvl && lvl->originalBase) {
				return FileOf(lvl->originalBase);
			}
			return {};
		}

		void UpdateActors(RE::ProcessLists* a_lists)
		{
			std::unordered_map<std::string, std::uint32_t> all;
			std::unordered_map<std::string, std::uint32_t> spawned;
			std::uint32_t                                   ff = 0;
			for (auto& handle : a_lists->highActorHandles) {
				const auto actor = handle.get();
				if (!actor) {
					continue;
				}
				auto plugin = ActorPlugin(actor.get());
				if ((actor->GetFormID() >> 24) == 0xFF) {
					++ff;
					++spawned[plugin];
				}
				++all[std::move(plugin)];
			}
			g_state.spawnedActors.store(ff);
			auto top = TopPlugins(all, 4);
			auto topSpawned = TopPlugins(spawned, 4);
			std::scoped_lock lock(g_nameLock);
			g_ctx.actorPlugins = std::move(top);
			g_ctx.spawnPlugins = std::move(topSpawned);
		}

		void UpdateGameInfo()
		{
			const auto& cfg = Settings::Get();
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				if (const auto cell = player->GetParentCell()) {
					const auto id = cell->GetFormID();
					const bool interior = cell->IsInteriorCell();
					const auto world = player->GetWorldspace();
					const auto pos = player->GetPosition();
					g_state.posX.store(pos.x);
					g_state.posY.store(pos.y);
					g_state.posZ.store(pos.z);

					std::int32_t gx = 0;
					std::int32_t gy = 0;
					if (!interior) {
						if (const auto ext = cell->GetCoordinates()) {
							gx = ext->cellX;
							gy = ext->cellY;
						}
					}

					const auto loc = player->GetCurrentLocation();
					const auto locID = loc ? loc->GetFormID() : 0;

					if (id != g_state.cellFormID.load() || locID != g_lastLocationID) {
						const char* name = cell->GetName();
						const char* worldEdid = world ? world->GetFormEditorID() : nullptr;
						const char* locName = loc ? loc->GetName() : nullptr;
						std::string line;
						{
							std::scoped_lock lock(g_nameLock);
							g_ctx.cellName = name ? name : "";
							g_ctx.worldspace = interior ? "" : (worldEdid ? worldEdid : "");
							g_ctx.location = locName ? locName : "";
							line = interior ?
							           std::format("CELL {:08X} '{}' (interior) location '{}'", id, g_ctx.cellName, g_ctx.location) :
							           std::format("CELL {:08X} '{}' {} ({},{}) location '{}'", id, g_ctx.cellName, g_ctx.worldspace, gx, gy, g_ctx.location);
						}
						Events::Write(line);
						g_lastLocationID = locID;
					}
					g_state.cellFormID.store(id);
					g_state.cellInterior.store(interior);
					g_state.worldFormID.store(world && !interior ? world->GetFormID() : 0);
					g_state.gridX.store(gx);
					g_state.gridY.store(gy);
				}
			}
			if (const auto lists = RE::ProcessLists::GetSingleton()) {
				g_state.highActors.store(static_cast<std::uint32_t>(lists->highActorHandles.size()));
				g_state.midHighActors.store(static_cast<std::uint32_t>(lists->middleHighActorHandles.size()));
				g_state.lowActors.store(static_cast<std::uint32_t>(lists->lowActorHandles.size()));
				if (cfg.actorBreakdown) {
					UpdateActors(lists);
				}
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
				g_state.lastFrameDtUs.store(dtUs, std::memory_order_relaxed);
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
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string name(a_event->menuName.c_str() ? a_event->menuName.c_str() : "");
				{
					std::scoped_lock lock(g_nameLock);
					const auto it = std::find(g_openMenus.begin(), g_openMenus.end(), name);
					if (a_event->opening && it == g_openMenus.end()) {
						g_openMenus.push_back(name);
					} else if (!a_event->opening && it != g_openMenus.end()) {
						g_openMenus.erase(it);
					}
					g_ctx.openMenus = JoinMenus();
				}
				// Cursor and Fader toggle constantly and add nothing
				if (name != "Cursor Menu" && name != "Fader Menu") {
					Events::Write(std::format("MENU {} {}", a_event->opening ? "open " : "close", name));
				}

				if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
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

		// (1.1) Equip / unequip breadcrumbs (player only unless bEquipAllActors=1) and fast-travel tagging
		class GameSink final :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<RE::TESFastTravelEndEvent>
		{
		public:
			static GameSink* Get()
			{
				static GameSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_event || !a_event->actor) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto actor = a_event->actor.get();
				const bool isPlayer = actor == RE::PlayerCharacter::GetSingleton();
				if (!isPlayer && !Settings::Get().equipAllActors) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto  item = RE::TESForm::LookupByID(a_event->baseObject);
				const char* itemName = item ? item->GetName() : nullptr;
				const char* who = isPlayer ? "player" : actor->GetName();
				Events::Write(std::format("{} {} [{:08X}] '{}' [{:08X} {}]", a_event->equipped ? "EQUIP  " : "UNEQUIP",
					who ? who : "", actor->GetFormID(), itemName ? itemName : "", a_event->baseObject, FileOf(item)));
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
			{
				const auto now = NowNs();
				const bool loading = g_state.loading.load();
				const bool justLoaded = now - g_state.loadEndNs.load() < 10'000'000'000;
				{
					std::scoped_lock lock(g_nameLock);
					if (loading || justLoaded) {
						g_ctx.loadKind = "fast travel";
					}
				}
				const auto n = g_state.loadCount.load() + (loading ? 1 : 0);
				Events::Write(std::format("FAST TRAVEL ended{}", loading || justLoaded ? std::format(" (load #{} was a fast travel)", n) : ""));
				if (!loading && justLoaded) {
					spdlog::info("Load #{} was a fast travel", n);
				}
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
		return g_ctx.cellName;
	}

	Context GetContext()
	{
		std::scoped_lock lock(g_nameLock);
		return g_ctx;
	}

	namespace
	{
		// SEH-guarded: nothing here needs unwinding, so __try is allowed
		bool ReadVM(RE::BSScript::Internal::VirtualMachine* a_vm, long long (&a_out)[4])
		{
			__try {
				a_out[0] = static_cast<long long>(a_vm->allRunningStacks.size());
				a_out[1] = static_cast<long long>(a_vm->waitingLatentReturns.size());
				a_out[2] = static_cast<long long>(a_vm->uiWaitingFunctionMessages);
				a_out[3] = a_vm->overstressed ? 1 : 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	PapyrusStats ReadPapyrus()
	{
		PapyrusStats out;
		const auto   vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return out;
		}
		long long v[4]{};
		if (!ReadVM(vm, v)) {
			return out;
		}
		// A value outside these bounds means the read raced a rehash (or the layout is off): report it as unknown
		auto sane = [](long long a_v) { return a_v >= 0 && a_v < 10'000'000 ? a_v : -1; };
		out.runningStacks = sane(v[0]);
		out.latentWaits = sane(v[1]);
		out.queuedMessages = sane(v[2]);
		out.overstressed = static_cast<int>(v[3]);
		return out;
	}

	std::string ContextLine()
	{
		const auto c = GetContext();
		const auto& s = g_state;
		std::string out;
		if (s.cellInterior.load()) {
			out = std::format("cell {:08X} '{}' (interior)", s.cellFormID.load(), c.cellName);
		} else {
			out = std::format("cell {:08X} '{}' {} ({},{}) pos ({:.0f}, {:.0f}, {:.0f})", s.cellFormID.load(), c.cellName, c.worldspace,
				s.gridX.load(), s.gridY.load(), s.posX.load(), s.posY.load(), s.posZ.load());
		}
		out += std::format(" | location '{}' | menus [{}] | last load kind '{}'", c.location, c.openMenus, c.loadKind);
		out += std::format(" | actors high {} (spawned {}) mid-high {} low {}", s.highActors.load(), s.spawnedActors.load(),
			s.midHighActors.load(), s.lowActors.load());
		if (!c.actorPlugins.empty()) {
			out += std::format(" | by plugin: {}", c.actorPlugins);
		}
		if (!c.spawnPlugins.empty()) {
			out += std::format(" | spawned by base plugin: {}", c.spawnPlugins);
		}
		if (Settings::Get().papyrusStats) {
			const auto p = ReadPapyrus();
			out += std::format(" | papyrus stacks {} latent {} queued {} overstressed {}", p.runningStacks, p.latentWaits,
				p.queuedMessages, p.overstressed);
		}
		return out;
	}

	void NoteGameMessage(std::uint32_t a_type)
	{
		const char* kind = nullptr;
		const char* what = nullptr;
		switch (a_type) {
		case SKSE::MessagingInterface::kPreLoadGame:  kind = "save load"; what = "SAVE LOAD requested"; break;
		case SKSE::MessagingInterface::kPostLoadGame: what = "SAVE LOAD done"; break;
		case SKSE::MessagingInterface::kNewGame:      kind = "new game"; what = "NEW GAME"; break;
		case SKSE::MessagingInterface::kSaveGame:     what = "GAME SAVED"; break;
		default: return;
		}
		if (kind) {
			std::scoped_lock lock(g_nameLock);
			if (g_state.loading.load()) {
				g_ctx.loadKind = kind;  // the loading screen opened first
			} else {
				g_pendingLoadKind = kind;
				g_pendingLoadKindNs = NowNs();
			}
		}
		Events::Write(what);
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
			if (Settings::Get().eventsLog) {
				holder->AddEventSink<RE::TESFastTravelEndEvent>(GameSink::Get());
				if (Settings::Get().equipEvents) {
					holder->AddEventSink<RE::TESEquipEvent>(GameSink::Get());
				}
			}
		} else {
			spdlog::error("ScriptEventSourceHolder missing; load progress will not be tracked");
		}
		spdlog::info("Event sinks registered");
	}
}
