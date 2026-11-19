#include "InteriorMap.h"

#include <algorithm>

namespace MM
{
	void AppendNavmesh(NavMeshData& a_out, std::span<const float> a_xyz, std::span<const NavTriangleIn> a_tris)
	{
		const std::size_t vcount = a_xyz.size() / 3;
		const std::size_t tcount = a_tris.size();
		if (vcount == 0 || tcount == 0)
			return;

		const bool first = a_out.xyz.empty();
		const auto base = static_cast<std::uint32_t>(a_out.VertexCount());
		a_out.xyz.insert(a_out.xyz.end(), a_xyz.begin(), a_xyz.begin() + static_cast<std::ptrdiff_t>(vcount * 3));

		bool any = !first;
		for (std::size_t i = 0; i < vcount; ++i) {
			const float x = a_xyz[i * 3], y = a_xyz[i * 3 + 1], z = a_xyz[i * 3 + 2];
			if (!any) {
				a_out.minX = a_out.maxX = x;
				a_out.minY = a_out.maxY = y;
				a_out.minZ = a_out.maxZ = z;
				any = true;
				continue;
			}
			a_out.minX = std::min(a_out.minX, x);
			a_out.minY = std::min(a_out.minY, y);
			a_out.minZ = std::min(a_out.minZ, z);
			a_out.maxX = std::max(a_out.maxX, x);
			a_out.maxY = std::max(a_out.maxY, y);
			a_out.maxZ = std::max(a_out.maxZ, z);
		}

		auto deleted = [&](std::size_t a_i) { return (a_tris[a_i].flags & kNavDeleted) != 0; };
		for (std::size_t i = 0; i < tcount; ++i) {
			const auto& t = a_tris[i];
			if (deleted(i) || t.v[0] >= vcount || t.v[1] >= vcount || t.v[2] >= vcount)
				continue;
			for (int k = 0; k < 3; ++k)
				a_out.tris.push_back(base + t.v[k]);
			for (int k = 0; k < 3; ++k) {
				if (t.flags & (kNavEdge0Link << k))
					continue;  // continues in a neighbouring navmesh: open floor, not a wall
				const std::uint16_t n = t.n[k];
				if (n != kNavNoNeighbour && n < tcount && !deleted(n))
					continue;  // shared with another floor triangle
				a_out.walls.push_back(base + t.v[k]);
				a_out.walls.push_back(base + t.v[(k + 1) % 3]);
			}
		}
	}
}
