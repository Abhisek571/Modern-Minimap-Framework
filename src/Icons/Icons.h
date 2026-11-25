#pragma once

// Minimap icons: shapes in a small procedural atlas, map-marker classes, projection from world to
// minimap pixels, mask clipping / rim clamping, and a fixed-interval scheduler for the data timers.
// Pure maths and CPU pixels only, shared by the plugin and the offline test tool.

#include "MapMath.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
		kHorse,
		kAnvil,  // town services
		kMug,
		kFlask,
		kBag,
		kBook,
		kGlyphFirst,  // text glyphs follow, one cell per character of kGlyphChars
		kCount = kGlyphFirst + 67
	};

	// Characters the atlas has a glyph for, in cell order after kGlyphFirst. Space has no cell (advance only).
	inline constexpr std::string_view kGlyphChars = "0123456789.?:'-ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	static_assert(kGlyphChars.size() == static_cast<std::size_t>(IconShape::kCount) - static_cast<std::size_t>(IconShape::kGlyphFirst));

	inline constexpr int kAtlasGrid = 10;    // cells per side
	inline constexpr int kAtlasInnerPx = 64;   // drawn area of a cell at mip 0
	inline constexpr int kAtlasGutterPx = 8;   // empty border around it, so the small mips never blend in a neighbour
	inline constexpr int kAtlasCellPx = kAtlasInnerPx + 2 * kAtlasGutterPx;  // cell side at mip 0
	inline constexpr int kAtlasSize = kAtlasGrid * kAtlasCellPx;
	inline constexpr int kAtlasMips = 4;  // 800, 400, 200, 100 (gutter still 1 texel at the last one)
	static_assert(static_cast<int>(IconShape::kCount) <= kAtlasGrid * kAtlasGrid);

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

	// Glyph cell for a character, or false when the atlas has none (space, non-Latin letters).
	bool GlyphShape(char a_char, IconShape& a_out) noexcept;

	// One character of a laid-out text line: its glyph and its centre's x offset from the line's anchor (px).
	struct GlyphPlacement
	{
		IconShape shape{ IconShape::kGlyphFirst };
		float     x{ 0.0f };
	};

	enum class TextAlign : std::uint8_t
	{
		kLeft,    // anchor at the left edge of the line
		kCenter,
		kRight
	};

	// Pen advance of a glyph, in glyph sizes (px per px of glyph size): its own ink width plus a gap. A space
	// advances as far as an 'n'.
	float GlyphAdvanceEm(IconShape a_glyph) noexcept;
	float SpaceAdvanceEm() noexcept;

	// Places a line of text drawn with glyphs of a_glyphPx: characters without a glyph are skipped, spaces only
	// advance. Appends to a_out and returns the line width in px.
	float LayoutText(std::string_view a_text, float a_glyphPx, TextAlign a_align, std::vector<GlyphPlacement>& a_out);

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

	// Town services, from the vendor faction's buy/sell list.
	enum class ServiceKind : std::uint8_t
	{
		kNone,
		kBlacksmith,
		kInnkeeper,
		kAlchemist,
		kGeneralGoods,
		kSpells,
		kCount
	};

	// What a vendor list holds (VendorItem* keywords), as bits.
	enum VendorGoods : std::uint32_t
	{
		kGoodsWeaponsArmor = 1 << 0,  // VendorItemWeapon / VendorItemArmor
		kGoodsPotions = 1 << 1,       // VendorItemPotion / VendorItemIngredient / VendorItemPoison
		kGoodsSpells = 1 << 2,        // VendorItemSpellTome / VendorItemScroll / VendorItemStaff
		kGoodsFood = 1 << 3,          // VendorItemFood / VendorItemFoodRaw
	};

	// a_sellsAllButList: the faction trades everything except its list (general stores).
	ServiceKind ClassifyVendor(bool a_sellsAllButList, std::uint32_t a_goods) noexcept;
	IconShape   ShapeForService(ServiceKind a_kind) noexcept;

	// Enemy marker or not. The game reports prey (rabbits, deer, elk, foxes) as hostile to the player,
	// but they have Confidence 0 (cowardly) and only ever flee, so they count as neutral.
	inline bool CountsAsEnemy(bool a_hostileToPlayer, float a_confidence) noexcept { return a_hostileToPlayer && a_confidence > 0.0f; }

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
		Vec2          offsetPx;             // screen offset from the projected point (labels), at 1080p before the scale
		float         anchorSizePx{ 0.0f }; // rim clamp margin from this size instead of sizePx (a label clamps like its icon)
		bool          flipOffsetInside{ false };  // label: put the offset above instead of below when below leaves the map
		std::uint32_t outlineRgba{ 0 };           // text: outline colour drawn around the glyph (alpha 0 = none)
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

	// A short text label under an icon (above it when below would leave the map), one glyph source per character.
	// On the rim the label clamps a little farther in than its icon so it is never cut off.
	void AppendTextLabel(std::vector<IconSource>& a_out, std::string_view a_text, Vec2 a_world, float a_iconSizePx, float a_glyphPx, bool a_clampToRim,
		std::uint32_t a_rgba, std::uint32_t a_outlineRgba);

	// A text glyph with an outline in a_outlineRgba: four copies 1 px (more on big glyphs) off diagonally, then the
	// glyph on top. Works for light text on a dark outline and dark text on a light one. Alpha 0 = no outline.
	inline constexpr int kOutlineCopies = 4;
	void AppendOutlinedGlyph(const IconInstance& a_glyph, std::uint32_t a_outlineRgba, std::vector<IconInstance>& a_out);

	// Compass letters: off, just inside the rim, or just outside the border ring (readable against the game world).
	enum class CompassMode : std::uint8_t
	{
		kOff,
		kInside,
		kOutside
	};
	inline constexpr int kCompassModeCount = 3;

	// [Icons] iCompassLetters (a_iniValue < 0 = missing) with the old bCompassLetters as fallback
	// (a_legacyBool: -1 = missing, 0 = false, 1 = true). false -> off, true or nothing -> outside.
	CompassMode CompassModeFromIni(long a_iniValue, int a_legacyBool) noexcept;

	// Distance from the minimap centre to a letter's centre (round radius, or half side for square).
	float CompassLetterDistance(float a_half, float a_letterPx, CompassMode a_mode) noexcept;

	// N, E, S, W turned with the map (N always points north); N larger and at full alpha. Each letter is outlined
	// (AppendOutlinedGlyph). Inside letters are clipped to the mask, outside ones are not.
	void AppendCompassLetters(const IconView& a_view, float a_glyphPx, std::uint32_t a_rgba, std::uint32_t a_outlineRgba, CompassMode a_mode,
		std::vector<IconInstance>& a_out);

	// Dead bodies reported by the game's death event: FormID and whether the player was the killer.
	// Small and capped (oldest out); the caller drops entries that unload or leave the player's space.
	class DeadBodyLog
	{
	public:
		static constexpr int kCapacity = 64;

		struct Entry
		{
			std::uint32_t formId{ 0 };
			bool          killedByPlayer{ false };
		};

		bool Add(std::uint32_t a_formId, bool a_killedByPlayer);  // false when already listed (killer flag merged)
		template <class Pred>
		void RemoveIf(Pred a_pred)
		{
			std::erase_if(_entries, a_pred);
		}
		void                                 Clear() noexcept { _entries.clear(); }
		[[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return _entries; }

	private:
		std::vector<Entry> _entries;
	};

	// ---- small pure helpers for the timed features ----

	// Skyrim's usual scale: about 70 world units per metre.
	inline constexpr float kUnitsPerMetre = 70.0f;

	// World distance -> short label: "45m", "1.2km" from 1000 m.
	std::string FormatDistance(float a_units);

	// Game hour (0..24, fractional) -> "07:05" (24 h) or "7:05 AM" (12 h).
	std::string FormatClock(float a_gameHour, bool a_24h);

	// True when every character but spaces has a glyph (a location name that can be shown whole).
	bool AllCharsHaveGlyphs(std::string_view a_text) noexcept;

	// Dead bodies on the minimap.
	enum class DeadMode : std::uint8_t
	{
		kOff,
		kPlayerKills,  // only actors the player killed
		kAll
	};
	inline constexpr int kDeadModeCount = 3;

	// [Icons] iDead (a_iniValue < 0 = key missing) with the old bDead key as fallback: bDead = true meant all.
	DeadMode DeadModeFromIni(long a_iniValue, bool a_legacyBDead) noexcept;

	// Whether a dead actor gets a cross in this mode.
	inline bool ShowsDeadBody(DeadMode a_mode, bool a_killedByPlayer) noexcept
	{
		return a_mode == DeadMode::kAll || (a_mode == DeadMode::kPlayerKills && a_killedByPlayer);
	}

	// Hold-to-zoom-out: the zoom radius while the hold key is down. The factor is kept in its INI range.
	inline constexpr float kHoldZoomMin = 1.5f;
	inline constexpr float kHoldZoomMax = 6.0f;
	inline float HoldZoomRadius(float a_radius, bool a_held, float a_factor) noexcept
	{
		if (!a_held)
			return a_radius;
		const float factor = std::isfinite(a_factor) ? std::fmin(std::fmax(a_factor, kHoldZoomMin), kHoldZoomMax) : kHoldZoomMin;
		return a_radius * factor;
	}

	// Path trail: the player's recent positions, one point each time they moved a_minSpacing from the last one.
	class TrailBuffer
	{
	public:
		static constexpr int kCapacity = 256;

		bool        Offer(Vec2 a_point, float a_minSpacing);  // true when the point was added
		void        Clear() noexcept { _count = 0; }
		int         Size() const noexcept { return _count; }
		Vec2        At(int a_index) const noexcept;  // 0 = oldest
		static float AgeAlpha(int a_index, int a_count) noexcept;  // 0..1, older = fainter

	private:
		std::array<Vec2, kCapacity> _points{};
		int                         _head{ 0 };  // next write slot
		int                         _count{ 0 };
	};

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
