#include "TileFormats.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace PhotoBaker
{
	namespace
	{
		std::uint32_t Read32(std::span<const std::uint8_t> a_bytes, std::size_t a_at)
		{
			std::uint32_t v = 0;
			std::memcpy(&v, a_bytes.data() + a_at, 4);
			return v;
		}

		std::uint16_t Read16(std::span<const std::uint8_t> a_bytes, std::size_t a_at)
		{
			std::uint16_t v = 0;
			std::memcpy(&v, a_bytes.data() + a_at, 2);
			return v;
		}

		const char* Bool(bool a_on) { return a_on ? "true" : "false"; }
	}

	bool IsBc7(TileFormat a_format) { return a_format != TileFormat::kBc1; }

	int TileFormatIndex(TileFormat a_format)
	{
		const auto it = std::ranges::find(kTileFormats, a_format, &TileFormatChoice::format);
		return static_cast<int>(it - kTileFormats.begin());
	}

	TileFormat TileFormatAt(int a_index)
	{
		if (a_index < 0 || a_index >= static_cast<int>(kTileFormats.size()))
			return kDefaultTileFormat;
		return kTileFormats[static_cast<std::size_t>(a_index)].format;
	}

	TileFormat TileFormatFromKey(std::wstring_view a_key)
	{
		const auto it = std::ranges::find_if(kTileFormats, [&](const TileFormatChoice& a_choice) { return a_key == a_choice.key; });
		return it == kTileFormats.end() ? kDefaultTileFormat : it->format;
	}

	int GridFromQuality(int a_position) { return kQualityMax + kQualityMin - std::clamp(a_position, kQualityMin, kQualityMax); }

	int QualityFromGrid(int a_grid) { return GridFromQuality(a_grid); }  // the mapping is its own inverse

	int TileSizeIndex(int a_size)
	{
		const auto it = std::ranges::find(kTileSizes, a_size);
		return it == kTileSizes.end() ? 0 : static_cast<int>(it - kTileSizes.begin());
	}

	std::vector<std::pair<std::string, std::string>> BakeSwitches(const BakeChoices& a_choices)
	{
		return {
			{ "bHideActorsOnNextStart", Bool(a_choices.hideNpcs) },
			{ "fAreaScale", std::format("{:.2f}", a_choices.area) },
			{ "fSettleSeconds", std::format("{}.0", a_choices.seconds) },
			{ "bKeepExistingTiles", Bool(a_choices.keepExisting) },
			{ "iPhotoGrid", std::to_string(a_choices.grid) },
			{ "bFullMap", Bool(a_choices.fullMap) },
			{ "iTileSize", std::to_string(a_choices.tileSize) },
			// BC7 tiles are encoded from the lossless masters, never from the BC1 tiles.
			{ "bSaveMasters", Bool(IsBc7(a_choices.format)) },
		};
	}

	bool DecodeBmp24(std::span<const std::uint8_t> a_file, int& a_w, int& a_h, std::vector<std::uint8_t>& a_rgba)
	{
		constexpr std::size_t kHeaderBytes = 54;
		if (a_file.size() < kHeaderBytes || a_file[0] != 'B' || a_file[1] != 'M')
			return false;
		const auto offset = Read32(a_file, 10);
		const auto w = static_cast<std::int32_t>(Read32(a_file, 18));
		const auto h = static_cast<std::int32_t>(Read32(a_file, 22));
		if (Read32(a_file, 14) < 40 || Read16(a_file, 28) != 24 || Read32(a_file, 30) != 0 || w <= 0 || h <= 0 || w > 16384 || h > 16384)
			return false;
		const std::size_t row = (static_cast<std::size_t>(w) * 3 + 3) & ~std::size_t{ 3 };
		if (offset < kHeaderBytes || a_file.size() < offset + row * static_cast<std::size_t>(h))
			return false;
		a_w = w;
		a_h = h;
		a_rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
		for (int y = 0; y < h; ++y) {
			const auto* src = a_file.data() + offset + row * static_cast<std::size_t>(h - 1 - y);  // bottom row first
			auto*       dst = a_rgba.data() + static_cast<std::size_t>(y) * w * 4;
			for (int x = 0; x < w; ++x) {
				dst[x * 4 + 0] = src[x * 3 + 2];
				dst[x * 4 + 1] = src[x * 3 + 1];
				dst[x * 4 + 2] = src[x * 3 + 0];
			}
		}
		return true;
	}

	bool IsBc7Dds(std::span<const std::uint8_t> a_head)
	{
		constexpr std::uint32_t kBc7Unorm = 98, kBc7Srgb = 99;  // DXGI_FORMAT_BC7_UNORM / _SRGB
		if (a_head.size() < kDdsHeadBytes || std::memcmp(a_head.data(), "DDS ", 4) != 0 || std::memcmp(a_head.data() + 84, "DX10", 4) != 0)
			return false;
		const auto format = Read32(a_head, 128);
		return format == kBc7Unorm || format == kBc7Srgb;
	}

	int DdsWidth(std::span<const std::uint8_t> a_head)
	{
		if (a_head.size() < 20 || std::memcmp(a_head.data(), "DDS ", 4) != 0)
			return 0;
		return static_cast<int>(std::min<std::uint32_t>(Read32(a_head, 16), 65536));
	}

	int BmpWidth(std::span<const std::uint8_t> a_head)
	{
		if (a_head.size() < 26 || a_head[0] != 'B' || a_head[1] != 'M')
			return 0;
		const auto width = static_cast<std::int32_t>(Read32(a_head, 18));
		return width > 0 && width <= 16384 ? width : 0;
	}

	int EncodedSide(int a_masterWidth, int a_maxSide) { return a_maxSide > 0 ? std::min(a_masterWidth, a_maxSide) : a_masterWidth; }

	bool ShouldSkipEncode(std::span<const std::uint8_t> a_ddsHead, int a_wantedSide, std::uint64_t a_ddsWriteTime, std::uint64_t a_masterWriteTime)
	{
		return IsBc7Dds(a_ddsHead) && DdsWidth(a_ddsHead) == a_wantedSide && a_ddsWriteTime > a_masterWriteTime;
	}
}
