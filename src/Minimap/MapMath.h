#pragma once

// Pure map maths shared by the plugin and the offline test tool. No game or D3D types here.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace MM
{
	inline constexpr float kCellUnits = 4096.0f;
	inline constexpr std::array<int, 4> kLodLevels{ 4, 8, 16, 32 };

	struct Vec2
	{
		float x{ 0.0f };
		float y{ 0.0f };
	};

	struct Rect
	{
		float minX{ 0.0f };
		float minY{ 0.0f };
		float maxX{ 0.0f };
		float maxY{ 0.0f };
	};

	struct TileId
	{
		int lvl{ 0 };
		int x{ 0 };  // south-west cell of the tile, multiple of lvl
		int y{ 0 };

		friend bool operator==(const TileId&, const TileId&) = default;
	};

	// TileId level used for baked top-down pictures (one cell each, made by the bake tools), drawn over the LOD terrain.
	inline constexpr int kBakedLevel = 1;

	// Floor division that rounds toward negative infinity.
	constexpr int FloorDiv(int a_value, int a_div) noexcept
	{
		const int q = a_value / a_div;
		return (a_value % a_div != 0 && ((a_value < 0) != (a_div < 0))) ? q - 1 : q;
	}

	inline int WorldToCell(float a_world) noexcept { return static_cast<int>(std::floor(a_world / kCellUnits)); }

	// Tile at level a_lvl that contains the world point.
	inline TileId TileForWorld(float a_wx, float a_wy, int a_lvl) noexcept
	{
		return { a_lvl, FloorDiv(WorldToCell(a_wx), a_lvl) * a_lvl, FloorDiv(WorldToCell(a_wy), a_lvl) * a_lvl };
	}

	// World rect covered by a tile: [x*4096, (x+lvl)*4096) by [y*4096, (y+lvl)*4096).
	inline Rect TileWorldRect(const TileId& a_tile) noexcept
	{
		return { a_tile.x * kCellUnits, a_tile.y * kCellUnits, (a_tile.x + a_tile.lvl) * kCellUnits, (a_tile.y + a_tile.lvl) * kCellUnits };
	}

	// All tiles of one level that intersect a world rect, nearest to the rect centre first.
	std::vector<TileId> TilesInRect(const Rect& a_rect, int a_lvl);

	// World units per minimap pixel for a zoom radius (world units from centre to edge).
	inline float UnitsPerPixel(float a_radius, float a_sizePx) noexcept { return a_radius / (a_sizePx * 0.5f); }

	// Nominal texel size of a LOD level: level 4 tiles are about 1024 px over 4 cells = 16 units/texel.
	inline float NominalUnitsPerTexel(int a_lvl) noexcept { return static_cast<float>(a_lvl) * 4.0f; }

	// Coarsest level whose nominal texels are not larger than one screen pixel (so texels >= pixels).
	int ChooseLevel(float a_unitsPerPixel) noexcept;

	// Largest DDS mip worth loading for a level at this zoom (power of two, 256..2048).
	int ChooseMaxTextureSize(int a_lvl, float a_unitsPerPixel) noexcept;

	// Render target side for a minimap of a_sizePx so that any rotation plus a few px of drift stays inside.
	inline int ComposeTargetSize(int a_sizePx) noexcept { return static_cast<int>(std::ceil(a_sizePx * 1.41421356f)) + 8; }

	// Screen-space offset from the minimap centre (x right, y down, px) -> world offset (x east, y north, px).
	// a_mapAngle is the heading the map is rotated to (radians, clockwise from north); 0 = north up.
	inline Vec2 ScreenToMapOffset(Vec2 a_q, float a_mapAngle) noexcept
	{
		const float c = std::cos(a_mapAngle);
		const float s = std::sin(a_mapAngle);
		return { a_q.x * c - a_q.y * s, -a_q.x * s - a_q.y * c };
	}

	// Inverse of ScreenToMapOffset.
	inline Vec2 MapToScreenOffset(Vec2 a_m, float a_mapAngle) noexcept
	{
		const float c = std::cos(a_mapAngle);
		const float s = std::sin(a_mapAngle);
		return { a_m.x * c - a_m.y * s, -a_m.x * s - a_m.y * c };
	}

	// World point -> composed-target pixel (x right, y down) for a target centred on a_center.
	inline Vec2 WorldToTarget(Vec2 a_world, Vec2 a_center, float a_upp, float a_targetSize) noexcept
	{
		return { (a_world.x - a_center.x) / a_upp + a_targetSize * 0.5f, a_targetSize * 0.5f - (a_world.y - a_center.y) / a_upp };
	}

	// Mask coverage of a minimap pixel at offset a_q from the centre (anti-aliased over one pixel).
	inline float MaskCoverage(Vec2 a_q, float a_sizePx, bool a_round) noexcept
	{
		const float half = a_sizePx * 0.5f;
		const float d = a_round ? std::sqrt(a_q.x * a_q.x + a_q.y * a_q.y) : std::fmax(std::fabs(a_q.x), std::fabs(a_q.y));
		const float c = half - d + 0.5f;
		return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	}

	// Heading (radians, clockwise from north) of a forward vector given in world x (east) / y (north).
	inline float HeadingFromForward(float a_east, float a_north) noexcept { return std::atan2(a_east, a_north); }

	// Look of the whole map (photos, terrain and interiors; not the icons), applied when it is drawn on the HUD.
	enum class MapStyle
	{
		kNatural,
		kMuted,
		kBlackWhite,
		kParchment,
		kCount
	};

	struct MapLook
	{
		float saturation{ 1.0f };  // 0 = grey
		float contrast{ 1.0f };    // around mid grey
		float tintR{ 1.0f }, tintG{ 1.0f }, tintB{ 1.0f };
	};

	MapLook LookForStyle(MapStyle a_style) noexcept;

	// Auto zoom: how much farther the map shows at a speed (world units per second). 1 = the chosen zoom step
	// (walking and running), up to 1 + a_strength at a horse gallop.
	float AutoZoomTarget(float a_speed, float a_strength) noexcept;

	// One step of the eased zoom factor toward its target: zooms out quickly when you speed up and back in slowly.
	float EaseAutoZoom(float a_current, float a_target, float a_dt) noexcept;

	// The zoom factor in steps of 1/32, so the map is redrawn a few times per change, not every frame.
	inline float QuantizeZoom(float a_factor) noexcept { return std::round(a_factor * 32.0f) / 32.0f; }

	// Hillshade constants (all INI tunable).
	struct ShadeParams
	{
		float lightX{ -1.0f };
		float lightY{ 1.0f };
		float lightZ{ 1.2f };
		float ambient{ 0.35f };
		float diffuse{ 0.9f };
		float shadeMin{ 0.2f };
		float shadeMax{ 1.6f };
		float brightness{ 1.15f };
		float saturation{ 1.35f };
		bool  water{ true };
		float waterR{ 60.0f / 255.0f };
		float waterG{ 110.0f / 255.0f };
		float waterB{ 120.0f / 255.0f };
		float waterAlpha{ 0.7f };
	};
}
