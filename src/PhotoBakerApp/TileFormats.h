#pragma once

// Pure helpers for the Photo Baker app (no window, no DirectXTex), so the offline test tool can check them:
// the tile format list, the [Bake] switches a run writes, the photo grid slider and the lossless master files.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PhotoBaker
{
	enum class TileFormat
	{
		kBc7Max,    // BC7 with the 3-subset modes
		kBc7,       // BC7, DirectXTex defaults
		kBc7Quick,  // BC7, mostly mode 6
		kBc1        // the plugin's own tiles, copied as they are
	};

	struct TileFormatChoice
	{
		TileFormat     format;
		const wchar_t* label;  // drop-down text
		const wchar_t* key;    // app settings value
	};

	// Drop-down order: longest encoding time first.
	inline constexpr std::array<TileFormatChoice, 4> kTileFormats{ {
		{ TileFormat::kBc7Max, L"BC7 Max (best quality, 2x size, slowest encode)", L"BC7Max" },
		{ TileFormat::kBc7, L"BC7 (high quality, 2x size, slow encode)", L"BC7" },
		{ TileFormat::kBc7Quick, L"BC7 Quick (good quality, 2x size, faster encode)", L"BC7Quick" },
		{ TileFormat::kBc1, L"BC1 (lower quality, smallest, fastest)", L"BC1" },
	} };
	inline constexpr TileFormat kDefaultTileFormat = TileFormat::kBc1;

	bool       IsBc7(TileFormat a_format);
	int        TileFormatIndex(TileFormat a_format);
	TileFormat TileFormatAt(int a_index);                // out of range: the default (BC1)
	TileFormat TileFormatFromKey(std::wstring_view a_key);  // unknown: the default (BC1)

	// Photo grid slider: left end = lower map quality (4x4), right end = higher map quality (1x1).
	inline constexpr int kQualityMin = 1;
	inline constexpr int kQualityMax = 4;
	int GridFromQuality(int a_position);  // slider position -> cells per photo side
	int QualityFromGrid(int a_grid);

	// Tile side choices ([Bake] iTileSize), in drop-down order.
	inline constexpr std::array<int, 2> kTileSizes{ 512, 1024 };
	int TileSizeIndex(int a_size);  // not a choice: 512

	// Everything one bake run writes into the plugin's [Bake] section (the start switches are written separately).
	struct BakeChoices
	{
		float      area{ 1.0f };
		int        seconds{ 7 };
		bool       hideNpcs{ true };
		bool       keepExisting{ false };
		int        grid{ 2 };
		bool       fullMap{ false };
		int        tileSize{ 512 };
		TileFormat format{ kDefaultTileFormat };
	};
	std::vector<std::pair<std::string, std::string>> BakeSwitches(const BakeChoices& a_choices);

	// The plugin's lossless master tiles (.bmp, 24-bit, bottom-up) -> RGBA bytes, row 0 = top, alpha 255.
	// False for anything else (other bit depths, compressed or top-down files, short data).
	bool DecodeBmp24(std::span<const std::uint8_t> a_file, int& a_w, int& a_h, std::vector<std::uint8_t>& a_rgba);

	// True when a .dds file starts with a DX10 header for BC7 (UNORM or SRGB): a tile that is already encoded.
	bool IsBc7Dds(std::span<const std::uint8_t> a_head);
	inline constexpr std::size_t kDdsHeadBytes = 148;  // magic + DDS_HEADER + DDS_HEADER_DXT10
	inline constexpr std::size_t kBmpHeadBytes = 54;   // BITMAPFILEHEADER + BITMAPINFOHEADER

	int DdsWidth(std::span<const std::uint8_t> a_head);  // 0 when not a .dds header
	int BmpWidth(std::span<const std::uint8_t> a_head);  // 0 when not a .bmp header

	// Side of the encoded tile: the master's own side, or at most a_maxSide (0 = no limit) for a smaller set.
	inline constexpr int kHalfSetSide = 512;
	int EncodedSide(int a_masterWidth, int a_maxSide);

	// True when the existing .dds already is this master's BC7 tile: BC7, the wanted side and written after the
	// master (times as FILETIME values). Anything else (a copied BC1 tile, an older or other-size tile) is encoded again.
	bool ShouldSkipEncode(std::span<const std::uint8_t> a_ddsHead, int a_wantedSide, std::uint64_t a_ddsWriteTime, std::uint64_t a_masterWriteTime);
}
