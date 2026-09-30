#include "PCH.h"
#include "AddressLib.h"

namespace AddressLib
{
	namespace
	{
		struct Entry
		{
			std::uint64_t offset;
			std::uint64_t id;
		};

		std::vector<Entry> g_entries;  // sorted by offset
		bool               g_loaded = false;

		template <class T>
		bool ReadRaw(std::ifstream& a_file, T& a_out)
		{
			a_file.read(reinterpret_cast<char*>(&a_out), sizeof(T));
			return static_cast<bool>(a_file);
		}

		std::uint64_t ReadU8(std::ifstream& f)
		{
			std::uint8_t v = 0;
			ReadRaw(f, v);
			return v;
		}
		std::uint64_t ReadU16(std::ifstream& f)
		{
			std::uint16_t v = 0;
			ReadRaw(f, v);
			return v;
		}
		std::uint64_t ReadU32(std::ifstream& f)
		{
			std::uint32_t v = 0;
			ReadRaw(f, v);
			return v;
		}
		std::uint64_t ReadU64(std::ifstream& f)
		{
			std::uint64_t v = 0;
			ReadRaw(f, v);
			return v;
		}
	}

	bool Load()
	{
		// AE (1.6.x) ships versionlib-<ver>.bin (format 2); SE (1.5.x) ships version-<ver>.bin (format 1).
		// Both use the same delta-encoded body, so one reader handles both. Try whichever file exists.
		const auto version = REL::Module::get().version().string("-"sv);
		const std::filesystem::path aePath = std::format("Data/SKSE/Plugins/versionlib-{}.bin", version);
		const std::filesystem::path sePath = std::format("Data/SKSE/Plugins/version-{}.bin", version);

		std::filesystem::path path = aePath;
		std::int32_t          expectedFormat = 2;
		std::ifstream         file(aePath, std::ios::binary);
		if (!file) {
			file.clear();
			file.open(sePath, std::ios::binary);
			path = sePath;
			expectedFormat = 1;
		}
		if (!file) {
			spdlog::warn("Address Library file not found ({} or {}); stack frames will not show IDs",
				aePath.string(), sePath.string());
			return false;
		}

		std::int32_t format = 0;
		ReadRaw(file, format);
		if (format != expectedFormat) {
			spdlog::warn("Address Library file {} has format {} (expected {}); stack frames will not show IDs",
				path.string(), format, expectedFormat);
			return false;
		}
		std::int32_t ver[4]{};
		for (auto& v : ver) {
			ReadRaw(file, v);
		}
		std::int32_t nameLen = 0;
		ReadRaw(file, nameLen);
		if (nameLen < 0 || nameLen > 0x10000) {
			spdlog::warn("Address Library file {} has a bad header", path.string());
			return false;
		}
		file.seekg(nameLen, std::ios::cur);
		std::int32_t pointerSize = 0;
		std::int32_t count = 0;
		ReadRaw(file, pointerSize);
		ReadRaw(file, count);
		if (!file || pointerSize <= 0 || count <= 0) {
			spdlog::warn("Address Library file {} has a bad header", path.string());
			return false;
		}

		// Delta-encoded (id, offset) pairs
		std::vector<Entry> entries;
		entries.reserve(static_cast<std::size_t>(count));
		std::uint64_t prevID = 0;
		std::uint64_t prevOffset = 0;
		for (std::int32_t i = 0; i < count; ++i) {
			const auto type = static_cast<std::uint8_t>(ReadU8(file));
			const auto lo = type & 0xF;
			const auto hi = type >> 4;

			std::uint64_t id = 0;
			switch (lo) {
			case 0: id = ReadU64(file); break;
			case 1: id = prevID + 1; break;
			case 2: id = prevID + ReadU8(file); break;
			case 3: id = prevID - ReadU8(file); break;
			case 4: id = prevID + ReadU16(file); break;
			case 5: id = prevID - ReadU16(file); break;
			case 6: id = ReadU16(file); break;
			case 7: id = ReadU32(file); break;
			default:
				spdlog::warn("Address Library file {} is corrupt (entry {})", path.string(), i);
				return false;
			}

			const std::uint64_t base = (hi & 8) ? prevOffset / static_cast<std::uint64_t>(pointerSize) : prevOffset;
			std::uint64_t offset = 0;
			switch (hi & 7) {
			case 0: offset = ReadU64(file); break;
			case 1: offset = base + 1; break;
			case 2: offset = base + ReadU8(file); break;
			case 3: offset = base - ReadU8(file); break;
			case 4: offset = base + ReadU16(file); break;
			case 5: offset = base - ReadU16(file); break;
			case 6: offset = ReadU16(file); break;
			case 7: offset = ReadU32(file); break;
			}
			if (hi & 8) {
				offset *= static_cast<std::uint64_t>(pointerSize);
			}

			if (!file) {
				spdlog::warn("Address Library file {} ended early (entry {} of {})", path.string(), i, count);
				return false;
			}
			entries.push_back({ offset, id });
			prevID = id;
			prevOffset = offset;
		}

		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.offset < b.offset; });
		g_entries = std::move(entries);
		g_loaded = true;
		spdlog::info("Address Library loaded: {} IDs from {}", g_entries.size(), path.string());
		return true;
	}

	bool Lookup(std::uintptr_t a_offset, std::uint64_t& a_id, std::uint64_t& a_delta)
	{
		if (!g_loaded || g_entries.empty()) {
			return false;
		}
		auto it = std::upper_bound(g_entries.begin(), g_entries.end(), static_cast<std::uint64_t>(a_offset),
			[](std::uint64_t v, const Entry& e) { return v < e.offset; });
		if (it == g_entries.begin()) {
			return false;
		}
		--it;
		a_id = it->id;
		a_delta = a_offset - it->offset;
		return true;
	}

	std::string Annotate(std::uintptr_t a_offset)
	{
		std::uint64_t id = 0;
		std::uint64_t delta = 0;
		if (!Lookup(a_offset, id, delta)) {
			return {};
		}
		return std::format(" -> {}+0x{:X}", id, delta);
	}
}
