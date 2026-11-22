#pragma once

// Image work for the in-game town photo bake: turning a captured back buffer into minimap tiles.
// Pure CPU code (no game types) so the offline test tool can check it.

#include <array>
#include <cstdint>
#include <dxgiformat.h>
#include <span>
#include <utility>
#include <vector>

namespace MM::BakeImage
{
	inline constexpr int   kTileSize = 512;       // output tile side, px
	inline constexpr int   kSampleSize = 64;      // per-cell size used to fit the colour match (reference px per cell)
	inline constexpr float kCameraHeight = 115000.0f;  // camera above the ground under the centre of a 2x2 block
	inline constexpr float kBaseFovDeg = 7.7f;    // horizontal FOV at 16:9: a 2x2 block fits the picture height at kCameraHeight

	// The photo grid (cells per photo side, 1..4) sets the block each photo covers; the photo is centred on it.
	// Camera height grows with the grid at the same FOV, so the block always fits the picture height.
	float CameraHeight(int a_grid);

	// Side of the reference render of one block: kSampleSize px per cell.
	int ReferenceSize(int a_grid);

	// RGB, 0..255 floats, row 0 = top (north).
	struct RgbImage
	{
		int                w{ 0 };
		int                h{ 0 };
		std::vector<float> px;
	};

	// Back buffer formats the bake can read. Anything else is refused.
	bool IsSupportedFormat(DXGI_FORMAT a_format);

	// Captured rows -> RGB in display (sRGB-encoded) values. 8-bit and 10-bit UNORM are taken as is;
	// 16-bit float (scRGB, linear) is clamped to 0..1 and sRGB-encoded.
	bool ConvertPhoto(const std::uint8_t* a_data, int a_w, int a_h, std::size_t a_rowPitch, DXGI_FORMAT a_format, RgbImage& a_out);

	// Luma standard deviation of the picture's centre half (fog lowers it).
	float CentreContrast(const RgbImage& a_img);

	// FOV for the bake on this screen: 7.7 at 16:9 or narrower; wider screens get more so the block still fits the height.
	float BakeFovDeg(int a_w, int a_h);

	// World units per photo pixel for this width, FOV and grid.
	double UnitsPerPixel(int a_w, float a_fovDeg, int a_grid);

	// One cell of a block photo resampled (Lanczos 3) to kTileSize. a_dx east, a_dy north of the block's south-west cell.
	// Returns RGB bytes, row 0 = north.
	std::vector<std::uint8_t> CropCell(const RgbImage& a_photo, float a_fovDeg, int a_grid, int a_dx, int a_dy);

	// One cell (kSampleSize square, RGB bytes) of a block's RGBA reference render (ReferenceSize(a_grid) square, row 0 = north).
	std::vector<std::uint8_t> ReferenceCell(std::span<const std::uint8_t> a_rgba, int a_grid, int a_dx, int a_dy);

	// Box-filtered downscale of a square RGB byte image (a_size divisible by a_out).
	std::vector<std::uint8_t> BoxDown(std::span<const std::uint8_t> a_rgb, int a_size, int a_out);

	// Colour match of the photos to a reference: one luma quantile curve plus a per-channel
	// tint gain at half strength. Applied with saturation 0.9. An unfitted tone uses fixed fallback constants.
	struct Tone
	{
		bool                   fitted{ false };
		std::array<float, 256> lut{};
		std::array<float, 3>   gain{ 1.0f, 1.0f, 1.0f };
	};

	// Pairs of kSampleSize x kSampleSize RGB byte images: (photo cell, reference cell).
	Tone FitMatch(std::span<const std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> a_pairs);

	// Tones a RGB byte image in place.
	void ApplyTone(const Tone& a_tone, std::span<std::uint8_t> a_rgb);

	// Complete .dds file: BC1 with a full mip chain, from a square power-of-two RGB byte image.
	std::vector<std::uint8_t> EncodeDdsBc1(std::span<const std::uint8_t> a_rgb, int a_size);

	// One 4x4 block, RGB 0..255 per pixel (row-major) -> 8 bytes of BC1.
	void EncodeBc1Block(const float (&a_px)[16][3], std::uint8_t (&a_out)[8]);

	// Decodes a BC1 block back to RGB bytes (tests).
	void DecodeBc1Block(const std::uint8_t (&a_in)[8], std::uint8_t (&a_px)[16][3]);
}
