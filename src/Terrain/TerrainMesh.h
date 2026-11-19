#pragma once

// Minimal reader for terrain LOD meshes (.btr, Skyrim SE NIF with BSTriShape blocks) and the
// water mask built from them. Only what the minimap needs: vertex positions and triangles of the
// land shape and of the shapes under the WATER node.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MM
{
	struct BtrMesh
	{
		std::string                             name;
		std::vector<std::array<float, 3>>         verts;  // tile space: x/y 0..lvl*4096, after the shape's transforms
		std::vector<std::array<std::uint16_t, 3>> tris;
	};

	struct BtrData
	{
		std::vector<std::string> blockTypes;  // one entry per block, in file order
		std::vector<BtrMesh>     land;
		std::vector<BtrMesh>     water;
	};

	// Returns false (and sets a_error) when the data is not a readable SSE terrain mesh.
	bool ParseBtr(std::span<const std::uint8_t> a_data, BtrData& a_out, std::string& a_error);

	// Water mask, a_size x a_size bytes, row 0 = north edge. 255 where a water shape covers the pixel
	// centre and lies above the land surface there, else 0. a_tileUnits = lvl * 4096.
	std::vector<std::uint8_t> RasterizeWater(const BtrData& a_btr, int a_size, float a_tileUnits);

	// Height field of one set of meshes at pixel centres; -inf where nothing covers the pixel.
	std::vector<float> RasterizeHeights(const std::vector<BtrMesh>& a_meshes, int a_size, float a_tileUnits);
}
