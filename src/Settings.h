#pragma once

struct Settings
{
	// [General]
	int   mode = 1;  // 0 off, 1 normal, 2 aggressive
	bool  statsLog = false;
	int   statsIntervalSec = 60;
	float hitchMs = 100.0f;
	int   keepStatsFiles = 20;
	int   keepStallCaptures = 10;

	// [Detection]
	float frameStallSec = 15.0f;
	float loadWarnSec = 15.0f;
	float loadStallSec = 45.0f;
	float recaptureSec = 45.0f;
	int   maxCaptures = 3;
	bool  ignoreUnfocused = true;

	// [Capture]
	bool mainThreadStack = true;
	bool allThreadStacks = false;
	int  minidumpLevel = 1;
	bool outOfProcessDump = true;  // write the dump with TheWatcherDump.exe
	bool backupLogs = true;
	bool beep = true;
	bool flashWindow = true;
	bool alertOnWarning = true;
	bool addressLibIDs = true;   // label SkyrimSE.exe frames with Address Library IDs
	int  threadSamples = 5;      // how many times to sample every thread per capture (1 = no sampling)
	int  sampleIntervalMs = 200; // time between samples
	bool groupThreadStacks = true;   // (1.1) print identical thread stacks once, with a thread count
	bool skipDumpIfRecovered = true; // (1.1) no minidump / all-thread pass when the game was already running again

	// [Sampling]  (1.1) main-thread stack history during hitches and slow loading gaps
	bool  slowSampling = true;
	float hitchSampleSec = 0.5f;     // start sampling once the current frame has taken this long
	float hitchReportSec = 1.0f;     // write a hitch report when the hitch lasted at least this long
	float loadGapSampleSec = 2.0f;   // during a loading screen: start sampling after this long with no load progress
	int   slowSampleIntervalMs = 100;
	int   maxSlowReports = 50;       // per session

	// [Context]  (1.1) extra data in the stats CSV, events log and live status file
	bool actorBreakdown = true;
	bool papyrusStats = true;
	bool vramStats = true;
	bool statusFile = true;
	bool eventsLog = true;
	bool equipEvents = true;
	bool equipAllActors = false;
	int  keepEventFiles = 20;

	[[nodiscard]] bool StatsEnabled() const { return mode == 2 || statsLog; }

	static Settings& Get();
	void Load();
	void LogValues() const;
};
