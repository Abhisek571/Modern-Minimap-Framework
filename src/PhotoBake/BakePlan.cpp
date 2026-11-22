#include "BakePlan.h"

#include <algorithm>
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
			a_out << "done " << key << "\n";
	}
}
