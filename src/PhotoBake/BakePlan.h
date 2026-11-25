#pragma once

// Planning for the in-game town photo bake: the photo grid, the block list and the progress file format.
// Pure code (no game types) so the offline test tool can check it.

#include "MapMath.h"
#include "PhotoProcessing.h"
#include "TerrainMesh.h"

#include <iosfwd>
#include <set>
#include <span>
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

	// Whole-map bake ([Bake] bFullMap): the cells of one terrain LOD tile that show land. A cell counts when any
	// sample point in it (8 x 8 per cell) has a land surface that no water shape covers; open sea is left out.
	std::set<std::pair<int, int>> LandCells(const BtrData& a_btr, const TileId& a_tile);

	// A closed polygon in world units (x east, y north), e.g. a border region's point list.
	using Polygon = std::vector<std::pair<float, float>>;

	// True when cell (a_cx, a_cy) overlaps the polygon: a cell corner inside it, a polygon point inside the cell,
	// or a polygon edge crossing the cell. Fewer than 3 points: false.
	bool CellTouchesPolygon(const Polygon& a_polygon, int a_cx, int a_cy);

	// Whole-map bake: the cells that overlap at least one of a_borders (the playable map area). No usable border
	// (none with 3+ points): the cells unchanged.
	std::set<std::pair<int, int>> ClipToBorders(const std::set<std::pair<int, int>>& a_cells, const std::vector<Polygon>& a_borders);

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
	// One finished shot, appended to the file between full rewrites (ParseProgress reads it like the rest).
	void AppendDone(std::ostream& a_out, const std::string& a_key);

	// Adaptive settle: a photo is taken once the picture has stopped changing.
	struct SettleRule
	{
		bool  adaptive{ true };
		float minSeconds{ 1.5f };   // never sooner
		float maxSeconds{ 15.0f };  // never later (the whole wait when adaptive is off)
		float threshold{ 0.75f };   // mean absolute luma change (0..255) between two looks that counts as still
		int   calmLooks{ 2 };       // consecutive still changes needed
	};
	// a_diffs: changes between consecutive looks, oldest first. True when the photo can be taken.
	bool SettleDone(float a_elapsed, std::span<const float> a_diffs, const SettleRule& a_rule);

	// Data\ModernMinimapMasters\<world>\tone.txt: "fitted 1|0", "gain r g b", "lut <256 floats>".
	void            WriteToneText(std::ostream& a_out, const BakeImage::Tone& a_tone);
	BakeImage::Tone ParseToneText(std::istream& a_in);  // missing or broken lines leave Tone{} values

	// Spread colour fit: up to a_count indices of a_points spread evenly over their bounding box (one per square of
	// a grid laid over it, the point nearest that square's centre), in serpentine row order.
	std::vector<std::size_t> SpreadSample(const std::vector<std::pair<float, float>>& a_points, std::size_t a_count);

	// Planned restarts ([Bake] iRestartEvery, 0 = off): true once this run has taken that many photos.
	bool RestartDue(int a_shotsThisRun, int a_restartEvery);

	// Shots that failed in one bake run: taken once more at the end of the list, then named as missing.
	class RetryBook
	{
	public:
		void Fail(std::size_t a_shot, int a_cells);
		// End of the list: the shots to take again. Empty when nothing failed or the retry pass already began.
		std::vector<std::size_t> StartRetry();
		[[nodiscard]] bool                            InRetry() const { return _retryStarted; }
		[[nodiscard]] int                             FailedCells() const { return _failedCells; }  // cells of the failed shots listed now
		[[nodiscard]] const std::vector<std::size_t>& Failed() const { return _failed; }

	private:
		std::vector<std::size_t> _failed;
		int                      _failedCells{ 0 };
		bool                     _retryStarted{ false };
	};

	// Data\SKSE\Plugins\ModernMinimapMissing.txt: one cell key per line.
	void WriteMissing(std::ostream& a_out, const std::vector<std::string>& a_keys);
}
