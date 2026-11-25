#include "BakePlan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <istream>
#include <ostream>
#include <sstream>

namespace MM::BakePlan
{
	int ClampGrid(int a_grid) { return std::clamp(a_grid, kMinGrid, kMaxGrid); }

	std::vector<std::pair<int, int>> Blocks(const std::set<std::pair<int, int>>& a_cells, int a_grid)
	{
		std::set<std::pair<int, int>> origins;
		for (const auto& [cx, cy] : a_cells)
			origins.insert({ FloorDiv(cx, a_grid) * a_grid, FloorDiv(cy, a_grid) * a_grid });
		std::vector<std::pair<int, int>> out(origins.begin(), origins.end());
		const auto key = [a_grid](const std::pair<int, int>& a_block) {
			const int row = -a_block.second / a_grid;  // exact: origins are multiples of the grid
			return std::pair{ -a_block.second, ((row % 2) + 2) % 2 == 0 ? a_block.first : -a_block.first };
		};
		std::ranges::sort(out, [&](const auto& a_lhs, const auto& a_rhs) { return key(a_lhs) < key(a_rhs); });
		return out;
	}

	float BlockCentre(int a_origin, int a_grid) { return (static_cast<float>(a_origin) + static_cast<float>(a_grid) / 2.0f) * kCellUnits; }

	std::vector<TileId> ReferenceTiles(int a_bx, int a_by, int a_grid)
	{
		constexpr int       kLevel = 4;
		std::vector<TileId> tiles;
		for (int ty = FloorDiv(a_by, kLevel) * kLevel; ty <= a_by + a_grid - 1; ty += kLevel)
			for (int tx = FloorDiv(a_bx, kLevel) * kLevel; tx <= a_bx + a_grid - 1; tx += kLevel)
				tiles.push_back({ kLevel, tx, ty });
		return tiles;
	}

	std::set<std::pair<int, int>> LandCells(const BtrData& a_btr, const TileId& a_tile)
	{
		constexpr int                 kSamplesPerCell = 8;
		std::set<std::pair<int, int>> cells;
		if (a_tile.lvl <= 0 || a_btr.land.empty())
			return cells;
		const int   size = a_tile.lvl * kSamplesPerCell;
		const float tileUnits = static_cast<float>(a_tile.lvl) * kCellUnits;
		const auto  heights = RasterizeHeights(a_btr.land, size, tileUnits);
		const auto  water = RasterizeWater(a_btr, size, tileUnits);
		for (int py = 0; py < size; ++py) {
			for (int px = 0; px < size; ++px) {
				const auto at = static_cast<std::size_t>(py) * size + px;
				if (!std::isfinite(heights[at]) || water[at] != 0)
					continue;
				// Row 0 is the tile's north edge.
				cells.insert({ a_tile.x + px / kSamplesPerCell, a_tile.y + (size - 1 - py) / kSamplesPerCell });
			}
		}
		return cells;
	}

	namespace
	{
		using Point = std::pair<float, float>;

		// Even-odd rule.
		bool PointInPolygon(const Polygon& a_polygon, float a_x, float a_y)
		{
			bool inside = false;
			for (std::size_t i = 0, j = a_polygon.size() - 1; i < a_polygon.size(); j = i++) {
				const auto [xi, yi] = a_polygon[i];
				const auto [xj, yj] = a_polygon[j];
				if ((yi > a_y) != (yj > a_y) && a_x < xi + (a_y - yi) * (xj - xi) / (yj - yi))
					inside = !inside;
			}
			return inside;
		}

		// Liang-Barsky: does segment a_from-a_to pass through the rectangle?
		bool SegmentHitsRect(Point a_from, Point a_to, float a_x0, float a_y0, float a_x1, float a_y1)
		{
			const float dx = a_to.first - a_from.first, dy = a_to.second - a_from.second;
			const std::array<float, 4> p{ -dx, dx, -dy, dy };
			const std::array<float, 4> q{ a_from.first - a_x0, a_x1 - a_from.first, a_from.second - a_y0, a_y1 - a_from.second };
			float                      enter = 0.0f, leave = 1.0f;
			for (std::size_t k = 0; k < 4; ++k) {
				if (p[k] == 0.0f) {
					if (q[k] < 0.0f)
						return false;  // parallel and outside
					continue;
				}
				const float t = q[k] / p[k];
				if (p[k] < 0.0f)
					enter = std::max(enter, t);
				else
					leave = std::min(leave, t);
				if (enter > leave)
					return false;
			}
			return true;
		}
	}

	bool CellTouchesPolygon(const Polygon& a_polygon, int a_cx, int a_cy)
	{
		if (a_polygon.size() < 3)
			return false;
		const float x0 = static_cast<float>(a_cx) * kCellUnits, y0 = static_cast<float>(a_cy) * kCellUnits;
		const float x1 = x0 + kCellUnits, y1 = y0 + kCellUnits;
		for (const auto& [x, y] : { Point{ x0, y0 }, Point{ x1, y0 }, Point{ x0, y1 }, Point{ x1, y1 } })
			if (PointInPolygon(a_polygon, x, y))
				return true;
		for (std::size_t i = 0, j = a_polygon.size() - 1; i < a_polygon.size(); j = i++)
			if (SegmentHitsRect(a_polygon[j], a_polygon[i], x0, y0, x1, y1))
				return true;  // also covers a polygon point inside the cell
		return false;
	}

	std::set<std::pair<int, int>> ClipToBorders(const std::set<std::pair<int, int>>& a_cells, const std::vector<Polygon>& a_borders)
	{
		if (std::ranges::none_of(a_borders, [](const Polygon& a_border) { return a_border.size() >= 3; }))
			return a_cells;
		std::set<std::pair<int, int>> kept;
		for (const auto& [cx, cy] : a_cells)
			if (std::ranges::any_of(a_borders, [&](const Polygon& a_border) { return CellTouchesPolygon(a_border, cx, cy); }))
				kept.insert({ cx, cy });
		return kept;
	}

	std::string ShotKey(const std::string& a_world, int a_bx, int a_by) { return std::format("{} {} {}", a_world, a_bx, a_by); }

	Progress ParseProgress(std::istream& a_in, int a_grid)
	{
		Progress    p;
		int         savedGrid = kDefaultGrid;
		std::string line;
		while (std::getline(a_in, line)) {
			std::istringstream s(line);
			std::string        what;
			s >> what;
			if (what == "grid") {
				s >> savedGrid;
			} else if (what == "tone") {
				std::string kind;
				s >> kind;
				p.toneKnown = true;
				p.tone.fitted = kind == "fit";
			} else if (what == "lut") {
				for (auto& v : p.tone.lut)
					s >> v;
			} else if (what == "gain") {
				s >> p.tone.gain[0] >> p.tone.gain[1] >> p.tone.gain[2];
			} else if (what == "done") {
				std::string world;
				int         bx = 0, by = 0;
				if (s >> world >> bx >> by)
					p.done.insert(ShotKey(world, bx, by));
			}
		}
		p.grid = a_grid;
		if (savedGrid != a_grid && !p.done.empty()) {
			p.droppedGrid = savedGrid;
			p.done.clear();
		}
		return p;
	}

	void WriteProgress(std::ostream& a_out, const Progress& a_p)
	{
		a_out << "# Modern Minimap town photo bake progress. Delete it (or press Reset) to bake everything again.\n";
		a_out << "grid " << a_p.grid << "\n";
		if (a_p.toneKnown) {
			a_out << "tone " << (a_p.tone.fitted ? "fit" : "grade") << "\n";
			a_out << "lut";
			for (const float v : a_p.tone.lut)
				a_out << ' ' << v;
			a_out << "\ngain " << a_p.tone.gain[0] << ' ' << a_p.tone.gain[1] << ' ' << a_p.tone.gain[2] << "\n";
		}
		for (const auto& key : a_p.done)
			AppendDone(a_out, key);
	}

	void AppendDone(std::ostream& a_out, const std::string& a_key) { a_out << "done " << a_key << "\n"; }

	bool SettleDone(float a_elapsed, std::span<const float> a_diffs, const SettleRule& a_rule)
	{
		if (a_elapsed >= a_rule.maxSeconds)
			return true;
		if (!a_rule.adaptive || a_elapsed < a_rule.minSeconds)
			return false;
		const auto needed = static_cast<std::size_t>(std::max(a_rule.calmLooks, 1));
		if (a_diffs.size() < needed)
			return false;
		return std::ranges::all_of(a_diffs.last(needed), [&](float a_diff) { return a_diff >= 0.0f && a_diff < a_rule.threshold; });
	}

	void WriteToneText(std::ostream& a_out, const BakeImage::Tone& a_tone)
	{
		// Full float precision: the Photo Baker app must tone the masters exactly as the tiles were toned.
		const auto oldPrecision = a_out.precision(9);
		a_out << "fitted " << (a_tone.fitted ? 1 : 0) << "\n";
		a_out << "gain " << a_tone.gain[0] << ' ' << a_tone.gain[1] << ' ' << a_tone.gain[2] << "\n";
		a_out << "lut";
		for (const float v : a_tone.lut)
			a_out << ' ' << v;
		a_out << "\n";
		a_out.precision(oldPrecision);
	}

	BakeImage::Tone ParseToneText(std::istream& a_in)
	{
		BakeImage::Tone tone;
		std::string     line;
		while (std::getline(a_in, line)) {
			std::istringstream s(line);
			std::string        what;
			s >> what;
			if (what == "fitted") {
				int flag = 0;
				if (s >> flag)
					tone.fitted = flag != 0;
			} else if (what == "gain") {
				std::array<float, 3> gain{};
				if (s >> gain[0] >> gain[1] >> gain[2])
					tone.gain = gain;
			} else if (what == "lut") {
				std::array<float, 256> lut{};
				bool                   whole = true;
				for (auto& v : lut)
					whole = whole && static_cast<bool>(s >> v);
				if (whole)
					tone.lut = lut;
			}
		}
		return tone;
	}

	namespace
	{
		// One pick per square of a a_side x a_side grid over the points' bounding box, serpentine from the north row.
		std::vector<std::size_t> PickPerSquare(const std::vector<std::pair<float, float>>& a_points, int a_side)
		{
			float minX = a_points[0].first, maxX = minX, minY = a_points[0].second, maxY = minY;
			for (const auto& [x, y] : a_points) {
				minX = std::min(minX, x);
				maxX = std::max(maxX, x);
				minY = std::min(minY, y);
				maxY = std::max(maxY, y);
			}
			const float spanX = std::max(maxX - minX, 1.0f), spanY = std::max(maxY - minY, 1.0f);
			const auto  squareOf = [&](float a_v, float a_min, float a_span) { return std::clamp(static_cast<int>((a_v - a_min) / a_span * static_cast<float>(a_side)), 0, a_side - 1); };
			constexpr std::size_t kNone = static_cast<std::size_t>(-1);
			std::vector<std::size_t> nearest(static_cast<std::size_t>(a_side) * a_side, kNone);
			std::vector<float>       nearestDist(nearest.size(), 0.0f);
			for (std::size_t i = 0; i < a_points.size(); ++i) {
				const auto [x, y] = a_points[i];
				const int   sx = squareOf(x, minX, spanX), sy = squareOf(y, minY, spanY);
				const float cx = minX + (static_cast<float>(sx) + 0.5f) * spanX / static_cast<float>(a_side);
				const float cy = minY + (static_cast<float>(sy) + 0.5f) * spanY / static_cast<float>(a_side);
				const float dist = (x - cx) * (x - cx) + (y - cy) * (y - cy);
				const auto  at = static_cast<std::size_t>(sy) * a_side + sx;
				if (nearest[at] == kNone || dist < nearestDist[at]) {
					nearest[at] = i;
					nearestDist[at] = dist;
				}
			}
			std::vector<std::size_t> picked;
			for (int row = 0; row < a_side; ++row) {
				const int sy = a_side - 1 - row;  // north first
				for (int step = 0; step < a_side; ++step) {
					const int sx = row % 2 == 0 ? step : a_side - 1 - step;
					if (const auto idx = nearest[static_cast<std::size_t>(sy) * a_side + sx]; idx != kNone)
						picked.push_back(idx);
				}
			}
			return picked;
		}
	}

	std::vector<std::size_t> SpreadSample(const std::vector<std::pair<float, float>>& a_points, std::size_t a_count)
	{
		if (a_points.empty() || a_count == 0)
			return {};
		if (a_points.size() <= a_count) {
			std::vector<std::size_t> all(a_points.size());
			for (std::size_t i = 0; i < all.size(); ++i)
				all[i] = i;
			return all;
		}
		// Squares over open sea stay empty: grow the grid until there are enough picks.
		constexpr int            kMaxSide = 256;
		std::vector<std::size_t> picked;
		for (int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(a_count)))); side <= kMaxSide; ++side) {
			picked = PickPerSquare(a_points, side);
			if (picked.size() >= a_count)
				break;
		}
		if (picked.size() <= a_count)
			return picked;
		std::vector<std::size_t> thinned;
		for (std::size_t k = 0; k < a_count; ++k)
			thinned.push_back(picked[k * picked.size() / a_count]);
		return thinned;
	}

	bool RestartDue(int a_shotsThisRun, int a_restartEvery) { return a_restartEvery > 0 && a_shotsThisRun >= a_restartEvery; }

	void RetryBook::Fail(std::size_t a_shot, int a_cells)
	{
		_failed.push_back(a_shot);
		_failedCells += std::max(a_cells, 0);
	}

	std::vector<std::size_t> RetryBook::StartRetry()
	{
		if (_retryStarted)
			return {};
		_retryStarted = true;
		_failedCells = 0;
		return std::exchange(_failed, {});
	}

	void WriteMissing(std::ostream& a_out, const std::vector<std::string>& a_keys)
	{
		for (const auto& key : a_keys)
			a_out << key << "\n";
	}
}
