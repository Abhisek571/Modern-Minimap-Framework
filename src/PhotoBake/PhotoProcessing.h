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
	inline constexpr int   kDefaultTileSize = 512;  // output tile side, px ([Bake] iTileSize)
	inline constexpr int   kMinTileSize = 512;
	inline constexpr int   kMaxTileSize = 2048;
	inline constexpr int   kSampleSize = 64;      // per-cell size used to fit the colour match (reference px per cell)
	inline constexpr float kCameraHeight = 115000.0f;  // camera above the ground under the centre of a 2x2 block
	inline constexpr float kBaseFovDeg = 7.7f;    // horizontal FOV at 16:9: a 2x2 block fits the picture height at kCameraHeight

	// Tile side for [Bake] iTileSize: the largest power of two in 512..2048 not above a_size (512 below that).
	int ClampTileSize(int a_size);

	// [Bake] fPhotoExposure: factor on the picture brightness while baking, kept to 0.25..1.0 (1 = as the game shows it;
	// anything unreadable = 1).
	float ClampPhotoExposure(float a_exposure);

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

	// Small luma picture (a_size x a_size, 0..255) of the centre square of captured rows, for the adaptive settle:
	// each pixel averages 4 x 4 samples, so grass, water and leaf motion mostly cancel out. Empty on bad input.
	std::vector<float> LumaThumbnail(const std::uint8_t* a_data, int a_w, int a_h, std::size_t a_rowPitch, DXGI_FORMAT a_format, int a_size);

	// Mean absolute difference of two equal-sized luma pictures; -1 when they cannot be compared.
	float MeanAbsDiff(std::span<const float> a_lhs, std::span<const float> a_rhs);

	// Luma standard deviation of the picture's centre half (fog lowers it).
	float CentreContrast(const RgbImage& a_img);

	// CentreContrast of a square RGBA reference render (a_size x a_size); -1 when the data is too short.
	float ReferenceContrast(std::span<const std::uint8_t> a_rgba, int a_size);

	// Flat photo check. Normal ground photos score 30-72, empty spots (sea, map edge) 0-3, plain snow or ice
	// fields about 14. Between kEmptyContrast and kFlatContrast the terrain LOD reference of the block decides.
	inline constexpr float kFlatContrast = 15.0f;         // at or above: a normal photo
	inline constexpr float kEmptyContrast = 5.0f;         // below: nothing to see there, whatever the reference says
	inline constexpr float kClearlyEmptyContrast = 1.0f;  // below: empty for sure, not worth another fog test
	inline constexpr float kPlainGroundRatio = 2.0f;      // the photo has at least this many times the reference's contrast

	enum class ContrastVerdict
	{
		kKeep,   // a normal photo, or plain ground that the reference shows plain too
		kEmpty,  // (almost) one colour: sea, map edge, or the fog
		kFlat    // low contrast over ground the reference shows with more detail: fogged
	};

	// True when the photo contrast is in the band where the reference contrast is needed.
	bool NeedsReferenceContrast(float a_photo);

	// a_reference < 0: unknown (no reference render), which keeps the old rule (below kFlatContrast = flat).
	ContrastVerdict JudgeContrast(float a_photo, float a_reference);

	// Share of pixels (0..1) whose luma is at or above kClippedLuma: blown-out white.
	inline constexpr float kClippedLuma = 250.0f;
	float ClippedWhiteFraction(const RgbImage& a_img);

	// True when the photo has fewer pixels across one cell than the tile side (the tiles are upscaled).
	bool TooFewPixelsPerCell(int a_w, float a_fovDeg, int a_grid, int a_tileSize);

	// FOV for the bake on this screen: 7.7 at 16:9 or narrower; wider screens get more so the block still fits the height.
	float BakeFovDeg(int a_w, int a_h);

	// World units per photo pixel for this width, FOV and grid.
	double UnitsPerPixel(int a_w, float a_fovDeg, int a_grid);

	// Photo pixels across one cell for this width, FOV and grid (about 1080 at 1080p with a 1x1 grid).
	double PhotoPixelsPerCell(int a_w, float a_fovDeg, int a_grid);

	// One cell of a block photo resampled (Lanczos 3) to a_tileSize. a_dx east, a_dy north of the block's south-west cell.
	// Returns RGB bytes, row 0 = north.
	std::vector<std::uint8_t> CropCell(const RgbImage& a_photo, float a_fovDeg, int a_grid, int a_dx, int a_dy, int a_tileSize);

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

	// Highlight guard of a tone curve: above kHighlightKnee the curve may not rise above the straight line from
	// the knee to 255 -> 255, and the knee itself is lifted by at most half of the room left above it. Bright input
	// (snow) then keeps its detail instead of being pushed into clipped white. Stays monotonic.
	inline constexpr int kHighlightKnee = 192;
	void CapHighlights(std::array<float, 256>& a_lut);

	// Pairs of kSampleSize x kSampleSize RGB byte images: (photo cell, reference cell). The curve gets CapHighlights.
	Tone FitMatch(std::span<const std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> a_pairs);

	// Tones a RGB byte image in place.
	void ApplyTone(const Tone& a_tone, std::span<std::uint8_t> a_rgb);

	// Complete .dds file: BC1 with a full mip chain, from a square power-of-two RGB byte image.
	std::vector<std::uint8_t> EncodeDdsBc1(std::span<const std::uint8_t> a_rgb, int a_size);

	// Complete uncompressed 24-bit .bmp file from a RGB byte image (row 0 = top). Lossless master copy of a tile.
	std::vector<std::uint8_t> EncodeBmp24(std::span<const std::uint8_t> a_rgb, int a_w, int a_h);

	// One 4x4 block, RGB 0..255 per pixel (row-major) -> 8 bytes of BC1.
	void EncodeBc1Block(const float (&a_px)[16][3], std::uint8_t (&a_out)[8]);

	// Decodes a BC1 block back to RGB bytes (tests).
	void DecodeBc1Block(const std::uint8_t (&a_in)[8], std::uint8_t (&a_px)[16][3]);
}
