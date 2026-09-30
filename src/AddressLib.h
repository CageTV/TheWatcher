#pragma once

// Reads the Address Library database (Data/SKSE/Plugins/versionlib-<game version>.bin) so stack frames in
// SkyrimSE.exe can be labeled "ID+offset", the same way CrashLogger labels them.
namespace AddressLib
{
	// Load once at startup (main thread). Returns false if the file is missing or unreadable.
	bool Load();

	// For an offset from SkyrimSE.exe's base: " -> 12345+0x1A" (nearest ID at or below), or "" if unavailable.
	std::string Annotate(std::uintptr_t a_offset);

	// (1.1) Same lookup as numbers: nearest ID at or below the offset, and the distance from it
	bool Lookup(std::uintptr_t a_offset, std::uint64_t& a_id, std::uint64_t& a_delta);
}
