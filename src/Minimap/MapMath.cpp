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
