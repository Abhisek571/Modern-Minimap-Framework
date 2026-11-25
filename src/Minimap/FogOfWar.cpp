#include "FogOfWar.h"

#include <cstring>

namespace MM::Fog
{
	namespace
	{
		constexpr int kChunkBits = kChunkBlocks * kChunkBlocks;

		struct BitPos
		{
			std::uint32_t key;
			int           index;  // bit inside the chunk
		};

		BitPos Locate(int a_bx, int a_by) noexcept
		{
			const int cx = FloorDiv(a_bx, kChunkBlocks), cy = FloorDiv(a_by, kChunkBlocks);
			const int lx = a_bx - cx * kChunkBlocks, ly = a_by - cy * kChunkBlocks;
			return { WorldMap::ChunkKey(cx, cy), ly * kChunkBlocks + lx };
		}

		template <class T>
		void Put(std::vector<std::uint8_t>& a_out, T a_value)
		{
			const auto at = a_out.size();
			a_out.resize(at + sizeof(T));
			std::memcpy(a_out.data() + at, &a_value, sizeof(T));
		}

		template <class T>
		bool Take(std::span<const std::uint8_t>& a_in, T& a_value)
		{
			if (a_in.size() < sizeof(T))
				return false;
			std::memcpy(&a_value, a_in.data(), sizeof(T));
			a_in = a_in.subspan(sizeof(T));
			return true;
		}
	}

	std::uint32_t WorldMap::ChunkKey(int a_cx, int a_cy) noexcept
	{
		return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(a_cx)) << 16) | static_cast<std::uint16_t>(a_cy);
	}

	bool WorldMap::Get(int a_bx, int a_by) const noexcept
	{
		const auto pos = Locate(a_bx, a_by);
		const auto it = _chunks.find(pos.key);
		return it != _chunks.end() && (it->second[static_cast<std::size_t>(pos.index / 64)] >> (pos.index % 64) & 1u);
	}

	bool WorldMap::Set(int a_bx, int a_by)
	{
		const auto pos = Locate(a_bx, a_by);
		auto&      word = _chunks[pos.key][static_cast<std::size_t>(pos.index / 64)];
		const auto bit = std::uint64_t{ 1 } << (pos.index % 64);
		if (word & bit)
			return false;
		word |= bit;
		return true;
	}

	int WorldMap::RevealAround(Vec2 a_world, float a_radius)
	{
		const int minX = BlockOf(a_world.x - a_radius), maxX = BlockOf(a_world.x + a_radius);
		const int minY = BlockOf(a_world.y - a_radius), maxY = BlockOf(a_world.y + a_radius);
		int       flipped = 0;
		for (int by = minY; by <= maxY; ++by) {
			for (int bx = minX; bx <= maxX; ++bx) {
				const float dx = (static_cast<float>(bx) + 0.5f) * kBlockUnits - a_world.x;
				const float dy = (static_cast<float>(by) + 0.5f) * kBlockUnits - a_world.y;
				if (dx * dx + dy * dy <= a_radius * a_radius && Set(bx, by))
					++flipped;
			}
		}
		return flipped;
	}

	void WorldMap::BuildMask(int a_originX, int a_originY, int a_size, std::vector<std::uint8_t>& a_out) const
	{
		a_out.assign(static_cast<std::size_t>(a_size) * static_cast<std::size_t>(a_size), 0);
		// Walk the chunks that touch the window, not every block through the hash map.
		const int c0x = FloorDiv(a_originX, kChunkBlocks), c1x = FloorDiv(a_originX + a_size - 1, kChunkBlocks);
		const int c0y = FloorDiv(a_originY, kChunkBlocks), c1y = FloorDiv(a_originY + a_size - 1, kChunkBlocks);
		for (int cy = c0y; cy <= c1y; ++cy) {
			for (int cx = c0x; cx <= c1x; ++cx) {
				const auto it = _chunks.find(ChunkKey(cx, cy));
				if (it == _chunks.end())
					continue;
				for (int i = 0; i < kChunkBits; ++i) {
					if (!(it->second[static_cast<std::size_t>(i / 64)] >> (i % 64) & 1u))
						continue;
					const int bx = cx * kChunkBlocks + i % kChunkBlocks - a_originX;
					const int by = cy * kChunkBlocks + i / kChunkBlocks - a_originY;
					if (bx < 0 || by < 0 || bx >= a_size || by >= a_size)
						continue;
					a_out[static_cast<std::size_t>(a_size - 1 - by) * static_cast<std::size_t>(a_size) + static_cast<std::size_t>(bx)] = 255;
				}
			}
		}
	}

	const WorldMap* Store::Find(std::uint32_t a_worldId) const
	{
		const auto it = _worlds.find(a_worldId);
		return it != _worlds.end() ? &it->second : nullptr;
	}

	std::vector<std::uint8_t> Store::Serialize() const
	{
		std::vector<std::uint8_t> out;
		Put(out, static_cast<std::uint32_t>(_worlds.size()));
		for (const auto& [worldId, map] : _worlds) {
			Put(out, worldId);
			Put(out, static_cast<std::uint32_t>(map.Chunks().size()));
			for (const auto& [key, chunk] : map.Chunks()) {
				Put(out, key);
				for (const auto word : chunk)
					Put(out, word);
			}
		}
		return out;
	}

	bool Store::Deserialize(std::span<const std::uint8_t> a_data, const std::function<bool(std::uint32_t, std::uint32_t&)>& a_resolve)
	{
		std::unordered_map<std::uint32_t, WorldMap> loaded;
		std::uint32_t                               worlds = 0;
		if (!Take(a_data, worlds))
			return false;
		for (std::uint32_t w = 0; w < worlds; ++w) {
			std::uint32_t savedId = 0, chunks = 0;
			if (!Take(a_data, savedId) || !Take(a_data, chunks))
				return false;
			std::uint32_t worldId = 0;
			const bool    keep = a_resolve(savedId, worldId);
			for (std::uint32_t c = 0; c < chunks; ++c) {
				std::uint32_t   key = 0;
				WorldMap::Chunk chunk{};
				if (!Take(a_data, key))
					return false;
				for (auto& word : chunk) {
					if (!Take(a_data, word))
						return false;
				}
				if (keep)
					loaded[worldId].PutChunk(key, chunk);
			}
		}
		_worlds = std::move(loaded);
		return true;
	}
}
