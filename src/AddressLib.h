#pragma once

// Reads the Address Library database (Data/SKSE/Plugins/versionlib-<game version>.bin) so stack frames in
// SkyrimSE.exe can be labeled "ID+offset", the same way CrashLogger labels them.
namespace AddressLib
{
	// Load once at startup (main thread). Returns false if the file is missing or unreadable.
	bool Load();

	// For an offset from SkyrimSE.exe's base: " -> 12345+0x1A" (nearest ID at or below), or "" if unavailable.
	std::string Annotate(std::uintptr_t a_offset);
}
