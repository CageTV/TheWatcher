#include "PCH.h"
#include "Settings.h"

namespace
{
	using Section = std::unordered_map<std::string, std::string>;
	std::unordered_map<std::string, Section> g_ini;

	std::string Lower(std::string a_s)
	{
		std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_s;
	}

	std::string Trim(std::string_view a_s)
	{
		const auto b = a_s.find_first_not_of(" \t\r\n");
		if (b == std::string_view::npos) {
			return {};
		}
		const auto e = a_s.find_last_not_of(" \t\r\n");
		return std::string(a_s.substr(b, e - b + 1));
	}

	// Own parser (std::ifstream) instead of GetPrivateProfileString, so MO2's virtual Data folder always works
	bool Parse(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path);
		if (!file) {
			return false;
		}
		std::string line;
		std::string section;
		while (std::getline(file, line)) {
			const auto t = Trim(line);
			if (t.empty() || t[0] == ';' || t[0] == '#') {
				continue;
			}
			if (t.front() == '[' && t.back() == ']') {
				section = Lower(Trim(std::string_view(t).substr(1, t.size() - 2)));
				continue;
			}
			const auto eq = t.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = Lower(Trim(std::string_view(t).substr(0, eq)));
			auto value = std::string_view(t).substr(eq + 1);
			if (const auto sc = value.find(';'); sc != std::string_view::npos) {
				value = value.substr(0, sc);
			}
			g_ini[section][key] = Trim(value);
		}
		return true;
	}

	const std::string* Find(std::string_view a_sec, std::string_view a_key)
	{
		const auto s = g_ini.find(std::string(a_sec));
		if (s == g_ini.end()) {
			return nullptr;
		}
		const auto k = s->second.find(std::string(a_key));
		return k == s->second.end() ? nullptr : &k->second;
	}

	float GetF(std::string_view a_sec, std::string_view a_key, float a_def)
	{
		if (const auto v = Find(a_sec, a_key)) {
			try {
				return std::stof(*v);
			} catch (...) {
			}
		}
		return a_def;
	}

	int GetI(std::string_view a_sec, std::string_view a_key, int a_def)
	{
		if (const auto v = Find(a_sec, a_key)) {
			try {
				return std::stoi(*v);
			} catch (...) {
			}
		}
		return a_def;
	}

	bool GetB(std::string_view a_sec, std::string_view a_key, bool a_def)
	{
		if (const auto v = Find(a_sec, a_key)) {
			const auto l = Lower(*v);
			if (l == "1" || l == "true") {
				return true;
			}
			if (l == "0" || l == "false") {
				return false;
			}
		}
		return a_def;
	}
}

Settings& Settings::Get()
{
	static Settings instance;
	return instance;
}

void Settings::Load()
{
	const std::filesystem::path path = "Data/SKSE/Plugins/TheWatcher.ini";
	if (Parse(path)) {
		spdlog::info("Loaded {}", path.string());
	} else {
		spdlog::warn("{} not found; using built-in defaults", path.string());
	}

	mode = std::clamp(GetI("general", "imode", mode), 0, 2);
	statsLog = GetB("general", "bstatslog", statsLog);
	keepStatsFiles = GetI("general", "ikeepstatsfiles", keepStatsFiles);
	keepStallCaptures = GetI("general", "ikeepstallcaptures", keepStallCaptures);

	// Tuning keys are accepted from any of these sections; [Aggressive] is read last so it wins in mode 2
	auto readTuning = [this](std::string_view a_sec) {
		statsIntervalSec = GetI(a_sec, "istatsintervalseconds", statsIntervalSec);
		hitchMs = GetF(a_sec, "fhitchms", hitchMs);
		frameStallSec = GetF(a_sec, "fframestallseconds", frameStallSec);
		loadWarnSec = GetF(a_sec, "floadwarnseconds", loadWarnSec);
		loadStallSec = GetF(a_sec, "floadstallseconds", loadStallSec);
		recaptureSec = GetF(a_sec, "frecaptureseconds", recaptureSec);
		maxCaptures = GetI(a_sec, "imaxcapturespersession", maxCaptures);
		ignoreUnfocused = GetB(a_sec, "bignorewhenunfocused", ignoreUnfocused);
		mainThreadStack = GetB(a_sec, "bmainthreadstack", mainThreadStack);
		allThreadStacks = GetB(a_sec, "ballthreadstacks", allThreadStacks);
		minidumpLevel = GetI(a_sec, "iminidumplevel", minidumpLevel);
		outOfProcessDump = GetB(a_sec, "boutofprocessdump", outOfProcessDump);
		backupLogs = GetB(a_sec, "bbackuplogs", backupLogs);
		beep = GetB(a_sec, "bbeep", beep);
		flashWindow = GetB(a_sec, "bflashwindow", flashWindow);
		alertOnWarning = GetB(a_sec, "balertonwarning", alertOnWarning);
		addressLibIDs = GetB(a_sec, "baddresslibraryids", addressLibIDs);
		threadSamples = GetI(a_sec, "ithreadsamples", threadSamples);
		sampleIntervalMs = GetI(a_sec, "isampleintervalms", sampleIntervalMs);
	};
	readTuning("general");
	readTuning("detection");
	readTuning("capture");
	if (mode == 2) {
		readTuning("aggressive");
	}

	// Sanity limits
	statsIntervalSec = std::max(statsIntervalSec, 5);
	hitchMs = std::max(hitchMs, 1.0f);
	frameStallSec = std::max(frameStallSec, 1.0f);
	loadWarnSec = std::max(loadWarnSec, 1.0f);
	loadStallSec = std::max(loadStallSec, loadWarnSec);
	recaptureSec = std::max(recaptureSec, 5.0f);
	maxCaptures = std::clamp(maxCaptures, 0, 20);
	minidumpLevel = std::clamp(minidumpLevel, 0, 3);
	keepStatsFiles = std::max(keepStatsFiles, 1);
	keepStallCaptures = std::max(keepStallCaptures, 1);
	threadSamples = std::clamp(threadSamples, 1, 20);
	sampleIntervalMs = std::clamp(sampleIntervalMs, 10, 2000);
}

void Settings::LogValues() const
{
	static constexpr const char* kModes[] = { "off", "normal", "aggressive" };
	spdlog::info("Mode: {} ({})", mode, kModes[mode]);
	spdlog::info("Stats log: {} every {}s, hitch threshold {:.0f} ms", StatsEnabled() ? "on" : "off", statsIntervalSec, hitchMs);
	spdlog::info("Frame stall: {:.1f}s | Load warn: {:.1f}s | Load stall: {:.1f}s | Recapture every {:.0f}s, max {} per session",
		frameStallSec, loadWarnSec, loadStallSec, recaptureSec, maxCaptures);
	spdlog::info("Capture: main stack {}, all stacks {}, minidump level {}, backup logs {}, beep {}, flash {}, warn alert {}, ignore unfocused {}",
		mainThreadStack, allThreadStacks, minidumpLevel, backupLogs, beep, flashWindow, alertOnWarning, ignoreUnfocused);
	spdlog::info("Address Library IDs {}, thread sampling {} x {} ms ({}), dump via helper process {}", addressLibIDs, threadSamples,
		sampleIntervalMs, allThreadStacks ? "all threads" : "main thread only", outOfProcessDump);
}
