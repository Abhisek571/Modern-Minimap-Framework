#pragma once

// Planning for the in-game town photo bake: the photo grid, the block list and the progress file format.
// Pure code (no game types) so the offline test tool can check it.

#include "MapMath.h"
#include "PhotoProcessing.h"

#include <iosfwd>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace MM::BakePlan
{
	// Cells per photo side ([Bake] iPhotoGrid): 1x1 sharpest ... 4x4 fastest. 2x2 is the original bake.
	inline constexpr int kMinGrid = 1;
	inline constexpr int kMaxGrid = 4;
	inline constexpr int kDefaultGrid = 2;

	int ClampGrid(int a_grid);

	// South-west cells of the a_grid x a_grid blocks covering a_cells, aligned to multiples of a_grid,
	// north to south, serpentine so moves stay short.
	std::vector<std::pair<int, int>> Blocks(const std::set<std::pair<int, int>>& a_cells, int a_grid);

	// World coordinate of a block's centre along one axis (a_origin = its south-west cell on that axis).
	float BlockCentre(int a_origin, int a_grid);

	// Level-4 terrain LOD tiles under a block (one for 1x1, 2x2 and 4x4; up to four for 3x3).
	std::vector<TileId> ReferenceTiles(int a_bx, int a_by, int a_grid);

	std::string ShotKey(const std::string& a_world, int a_bx, int a_by);

	// Data\SKSE\Plugins\ModernMinimapBake.txt. Files without a grid line are from 2x2 bakes.
	struct Progress
	{
		int                   grid{ kDefaultGrid };
		bool                  toneKnown{ false };
		BakeImage::Tone       tone;
		std::set<std::string> done;
		int                   droppedGrid{ 0 };  // > 0: the file's finished shots were for this other grid and were left out
	};

	// Reads the file for a bake with a_grid. Finished shots made with another grid are dropped (and named in
	// droppedGrid), so photos of different grids never mix; the colour match does not depend on the grid and is kept.
	Progress ParseProgress(std::istream& a_in, int a_grid);
	void     WriteProgress(std::ostream& a_out, const Progress& a_progress);
}
