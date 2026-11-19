#pragma once

// Minimap icons: shapes in a small procedural atlas, map-marker classes, projection from world to
// minimap pixels, mask clipping / rim clamping, and a fixed-interval scheduler for the data timers.
// Pure maths and CPU pixels only, shared by the plugin and the offline test tool.

#include "MapMath.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace MM
{
	enum class IconShape : std::uint8_t
	{
		kCircle,
		kDiamond,
		kTriangle,
		kHouse,
		kCity,
		kCave,
		kCross,
		kStar,
		kDoor,
		kPin,
		kArrow,
		kCount
	};

	inline constexpr int kAtlasGrid = 4;     // cells per side
	inline constexpr int kAtlasCellPx = 64;  // cell side at mip 0
	inline constexpr int kAtlasSize = kAtlasGrid * kAtlasCellPx;
	inline constexpr int kAtlasMips = 4;  // 256, 128, 64, 32 (cell 8 px at the last one)

	struct AtlasImage
	{
		int                                    size{ kAtlasSize };
		std::vector<std::vector<std::uint8_t>> mips;  // RGBA8, straight alpha; mips[i] is (size >> i)^2 * 4 bytes
	};

	// White shapes with a dark outline, tinted per icon at draw time.
	AtlasImage BuildIconAtlas();

	struct UvRect
	{
		float u0{ 0 }, v0{ 0 }, u1{ 0 }, v1{ 0 };
	};
	UvRect AtlasUv(IconShape a_shape) noexcept;

	// Map marker classes shown on the minimap. a_markerType is the MARKER_TYPE value from ExtraMapMarker.
	enum class MarkerClass : std::uint8_t
	{
		kCity,
		kSettlement,
		kDungeon,
		kOther
	};
	MarkerClass ClassifyMapMarker(int a_markerType) noexcept;
	IconShape   ShapeForMarkerClass(MarkerClass a_class) noexcept;

	// RGBA packed as 0xAABBGGRR (byte order R, G, B, A in memory = DXGI R8G8B8A8_UNORM).
	constexpr std::uint32_t PackRgba(int a_r, int a_g, int a_b, int a_a = 255) noexcept
	{
		return static_cast<std::uint32_t>(a_r & 255) | (static_cast<std::uint32_t>(a_g & 255) << 8) | (static_cast<std::uint32_t>(a_b & 255) << 16) |
		       (static_cast<std::uint32_t>(a_a & 255) << 24);
	}

	// One icon to place, in world space. Lists are gathered on timers, projected every frame.
	struct IconSource
	{
		Vec2          world;
		IconShape     shape{ IconShape::kCircle };
		std::uint32_t rgba{ 0xFFFFFFFF };
		float         sizePx{ 12.0f };  // at 1080p, before the resolution / INI scale
		float         rotation{ 0.0f };  // radians clockwise on screen
		bool          clampToRim{ false };  // quest targets: stick to the rim when off the map; others hide
	};

	// What the minimap shows this frame.
	struct IconView
	{
		Vec2  player;               // world point at the minimap centre
		float unitsPerPixel{ 32.0f };
		float mapAngle{ 0.0f };     // radians clockwise from north the map is turned to
		float sizePx{ 256.0f };     // minimap side in screen px
		bool  round{ true };
		Vec2  centerPx;             // minimap centre in viewport px
		float iconScale{ 1.0f };    // resolution scale x INI icon scale
	};

	// Per-instance data for the icon draw (matches the input layout in MapRenderer).
	struct IconInstance
	{
		float         x{ 0 }, y{ 0 };       // centre, viewport px
		float         size{ 0 };            // px
		float         rotation{ 0 };        // radians clockwise
		float         clip{ 1 };            // 1 = clip to the minimap mask, 0 = draw whole (player arrow)
		float         pad{ 0 };
		UvRect        uv;
		std::uint32_t rgba{ 0xFFFFFFFF };
	};
	static_assert(sizeof(IconInstance) == 44);

	// World point -> offset from the minimap centre in screen px (x right, y down).
	inline Vec2 ProjectToMinimap(Vec2 a_world, const IconView& a_view) noexcept
	{
		const Vec2 m{ (a_world.x - a_view.player.x) / a_view.unitsPerPixel, (a_world.y - a_view.player.y) / a_view.unitsPerPixel };
		return MapToScreenOffset(m, a_view.mapAngle);
	}

	// Distance used by the mask: radius for round, Chebyshev for square.
	inline float MaskDistance(Vec2 a_q, bool a_round) noexcept
	{
		return a_round ? std::sqrt(a_q.x * a_q.x + a_q.y * a_q.y) : std::fmax(std::fabs(a_q.x), std::fabs(a_q.y));
	}

	// True when the offset lies inside the mask shrunk by a_margin px.
	inline bool InsideMask(Vec2 a_q, float a_half, bool a_round, float a_margin = 0.0f) noexcept { return MaskDistance(a_q, a_round) <= a_half - a_margin; }

	// Pulls an offset outside the shrunk mask back onto its edge along the same direction.
	inline Vec2 ClampToRim(Vec2 a_q, float a_half, bool a_round, float a_margin) noexcept
	{
		const float limit = std::fmax(a_half - a_margin, 0.0f);
		const float d = MaskDistance(a_q, a_round);
		if (d <= limit || d <= 1e-6f)
			return a_q;
		const float k = limit / d;
		return { a_q.x * k, a_q.y * k };
	}

	// Projects, clips or clamps every source and appends the visible ones, in input order (= draw order).
	void BuildIconInstances(std::span<const IconSource> a_sources, const IconView& a_view, std::vector<IconInstance>& a_out);

	// Instance for the player arrow at the minimap centre (drawn last, never clipped).
	IconInstance ArrowInstance(const IconView& a_view, float a_sizePx, float a_rotation, std::uint32_t a_rgba);

	// Fixed-interval jobs on a caller-supplied clock (seconds). Tick() returns a bit per job that is due.
	class Schedule
	{
	public:
		static constexpr int kMaxJobs = 16;

		int  Add(double a_intervalSec);  // returns the job index
		void SetInterval(int a_job, double a_intervalSec);
		void Force(int a_job);            // due on the next Tick
		void ForceAll();
		std::uint32_t Tick(double a_now);

	private:
		struct Job
		{
			double interval{ 1.0 };
			double next{ 0.0 };
			bool   forced{ true };
		};
		std::array<Job, kMaxJobs> _jobs{};
		int                       _count{ 0 };
	};
}
