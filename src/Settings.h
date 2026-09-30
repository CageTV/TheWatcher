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

	[[nodiscard]] bool StatsEnabled() const { return mode == 2 || statsLog; }

	static Settings& Get();
	void Load();
	void LogValues() const;
};
