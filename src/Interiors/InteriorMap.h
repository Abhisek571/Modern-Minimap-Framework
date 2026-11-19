#pragma once

// Interior map built from the cell's navmeshes. Plain data (no game or D3D types): the plugin copies
// the game's navmeshes into NavMeshData on the main thread, the renderer draws it, the test tool
// builds synthetic meshes.

#include <cstdint>
#include <span>
#include <vector>

namespace MM
{
	// Triangle flags as stored in the game's navmesh (BSNavmeshTriangle::triangleFlags).
	inline constexpr std::uint16_t kNavEdge0Link = 1 << 0;  // edge 0 (v0 -> v1) continues in another navmesh
	inline constexpr std::uint16_t kNavDeleted = 1 << 3;
	// Neighbour index meaning "no triangle across this edge". The game stores -1 (0xFFFF) in the 16-bit
	// field; any index past the end of the triangle list is treated the same way.
	inline constexpr std::uint16_t kNavNoNeighbour = 0xFFFF;

	struct NavTriangleIn
	{
		std::uint16_t v[3]{};  // vertex indices into the same navmesh
		std::uint16_t n[3]{};  // neighbour triangle across edge k (v[k] -> v[(k+1)%3])
		std::uint16_t flags{ 0 };
	};

	struct NavMeshData
	{
		std::vector<float>         xyz;    // 3 floats per vertex, world units
		std::vector<std::uint32_t> tris;   // 3 indices per floor triangle
		std::vector<std::uint32_t> walls;  // 2 indices per wall edge
		float                      minX{ 0 }, minY{ 0 }, minZ{ 0 }, maxX{ 0 }, maxY{ 0 }, maxZ{ 0 };
		std::uint32_t              generation{ 0 };  // changes whenever the mesh does

		[[nodiscard]] std::size_t VertexCount() const noexcept { return xyz.size() / 3; }
		[[nodiscard]] bool        Empty() const noexcept { return tris.empty(); }
	};

	// Appends one navmesh. Deleted triangles and triangles with bad vertex indices are skipped.
	// A wall edge is an edge with no neighbour triangle (or a deleted one) and no link to another navmesh.
	void AppendNavmesh(NavMeshData& a_out, std::span<const float> a_xyz, std::span<const NavTriangleIn> a_tris);

	// Floor band: geometry between playerZ - below and playerZ + above is the player's floor.
	inline bool InFloorBand(float a_z, float a_playerZ, float a_below, float a_above) noexcept
	{
		return a_z >= a_playerZ - a_below && a_z <= a_playerZ + a_above;
	}

	// Interior map colours (sRGB, 0..1): background, floor, walls.
	inline constexpr float kNavClear[3]{ 0.07f, 0.08f, 0.09f };
	inline constexpr float kNavFloor[3]{ 0.19f, 0.20f, 0.21f };
	inline constexpr float kNavWall[3]{ 0.78f, 0.74f, 0.62f };

	// Player z must move this far (world units) before the interior map is recomposed.
	inline constexpr float kNavRecomposeZ = 48.0f;
}
