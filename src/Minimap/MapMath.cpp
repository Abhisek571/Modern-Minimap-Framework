#include "MapMath.h"

#include <algorithm>

namespace MM
{
	std::vector<TileId> TilesInRect(const Rect& a_rect, int a_lvl)
	{
		std::vector<TileId> tiles;
		const TileId lo = TileForWorld(a_rect.minX, a_rect.minY, a_lvl);
		// maxX/maxY are exclusive edges; nudge inward so a rect ending exactly on a tile edge does not pull in the next tile.
		const TileId hi = TileForWorld(std::nextafter(a_rect.maxX, -1e30f), std::nextafter(a_rect.maxY, -1e30f), a_lvl);
		for (int y = lo.y; y <= hi.y; y += a_lvl) {
			for (int x = lo.x; x <= hi.x; x += a_lvl) {
				tiles.push_back({ a_lvl, x, y });
			}
		}
		const float cx = (a_rect.minX + a_rect.maxX) * 0.5f;
		const float cy = (a_rect.minY + a_rect.maxY) * 0.5f;
		const float half = a_lvl * kCellUnits * 0.5f;
		std::ranges::stable_sort(tiles, [&](const TileId& a, const TileId& b) {
			const float ax = a.x * kCellUnits + half - cx, ay = a.y * kCellUnits + half - cy;
			const float bx = b.x * kCellUnits + half - cx, by = b.y * kCellUnits + half - cy;
			return ax * ax + ay * ay < bx * bx + by * by;
		});
		return tiles;
	}

	MapLook LookForStyle(MapStyle a_style) noexcept
	{
		static constexpr std::array<MapLook, static_cast<std::size_t>(MapStyle::kCount)> kLooks{ {
			{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },     // natural
			{ 0.45f, 0.92f, 1.0f, 0.98f, 0.94f }, // muted
			{ 0.0f, 1.15f, 1.0f, 1.0f, 1.0f },    // black and white
			{ 0.0f, 0.9f, 1.08f, 0.95f, 0.74f },  // parchment
		} };
		const auto index = static_cast<std::size_t>(a_style);
		return index < kLooks.size() ? kLooks[index] : kLooks.front();
	}

	float AutoZoomTarget(float a_speed, float a_strength) noexcept
	{
		constexpr float kStartSpeed = 300.0f;  // a run: no change below this
		constexpr float kFullSpeed = 900.0f;   // a horse gallop: full strength
		if (!std::isfinite(a_speed) || !std::isfinite(a_strength) || a_strength <= 0.0f)
			return 1.0f;
		const float t = std::clamp((a_speed - kStartSpeed) / (kFullSpeed - kStartSpeed), 0.0f, 1.0f);
		return 1.0f + a_strength * t * t * (3.0f - 2.0f * t);  // smoothstep
	}

	float EaseAutoZoom(float a_current, float a_target, float a_dt) noexcept
	{
		constexpr float kOutSeconds = 1.0f;  // time constant when zooming out (speeding up)
		constexpr float kInSeconds = 3.0f;   // and back in (slowing down): slow, so stops and turns do not pump the zoom
		if (!std::isfinite(a_current))
			return a_target;
		if (!(a_dt > 0.0f))
			return a_current;
		const float tau = a_target > a_current ? kOutSeconds : kInSeconds;
		return a_current + (a_target - a_current) * (1.0f - std::exp(-a_dt / tau));
	}

	int ChooseLevel(float a_unitsPerPixel) noexcept
	{
		int pick = kLodLevels.front();
		for (const int lvl : kLodLevels) {
			if (NominalUnitsPerTexel(lvl) <= a_unitsPerPixel) {
				pick = lvl;
			}
		}
		return pick;
	}

	int ChooseMaxTextureSize(int a_lvl, float a_unitsPerPixel) noexcept
	{
		const float needed = a_lvl * kCellUnits / std::max(a_unitsPerPixel, 1.0f);
		int size = 256;
		while (size < needed && size < 2048) {
			size *= 2;
		}
		return size;
	}
}
