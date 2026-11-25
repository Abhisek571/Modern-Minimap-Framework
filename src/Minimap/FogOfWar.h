#pragma once

// Fog of war: which quarter-cell blocks of each exterior worldspace the player has been near.
// One bit per block, kept in 32 x 32 block chunks. Pure data and maths, shared by the plugin and the test tool.

#include "MapMath.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace MM::Fog
{
	inline constexpr float kBlockUnits = kCellUnits * 0.25f;  // quarter cell
	inline constexpr int   kChunkBlocks = 32;                  // blocks per chunk side
	inline constexpr int   kMaskSize = 256;                    // blocks per side of the mask texture around the player
	inline constexpr int   kMaskSnap = 32;                     // the mask window moves in steps of this many blocks
	inline constexpr float kRevealRadius = kCellUnits;         // blocks within about one cell of the player are explored

	inline int BlockOf(float a_world) noexcept { return static_cast<int>(std::floor(a_world / kBlockUnits)); }

	// Explored blocks of one worldspace.
	class WorldMap
	{
	public:
		bool Get(int a_bx, int a_by) const noexcept;
		bool Set(int a_bx, int a_by);  // true when the bit was not set before

		// Sets every block whose centre lies within a_radius of the point. Returns how many bits flipped.
		int RevealAround(Vec2 a_world, float a_radius);

		// Mask pixels (R8, 0 = unexplored, 255 = explored) for a_size x a_size blocks starting at block
		// (a_originX, a_originY) = the south-west corner. Row 0 is the north edge (like the map tiles).
		void BuildMask(int a_originX, int a_originY, int a_size, std::vector<std::uint8_t>& a_out) const;

		[[nodiscard]] std::size_t ChunkCount() const noexcept { return _chunks.size(); }

		using Chunk = std::array<std::uint64_t, kChunkBlocks * kChunkBlocks / 64>;
		[[nodiscard]] const std::unordered_map<std::uint32_t, Chunk>& Chunks() const noexcept { return _chunks; }
		void PutChunk(std::uint32_t a_key, const Chunk& a_chunk) { _chunks[a_key] = a_chunk; }

		static std::uint32_t ChunkKey(int a_cx, int a_cy) noexcept;

	private:
		std::unordered_map<std::uint32_t, Chunk> _chunks;
	};

	// Every worldspace's map, keyed by worldspace FormID.
	class Store
	{
	public:
		WorldMap&       World(std::uint32_t a_worldId) { return _worlds[a_worldId]; }
		const WorldMap* Find(std::uint32_t a_worldId) const;
		void            Clear() noexcept { _worlds.clear(); }
		[[nodiscard]] std::size_t WorldCount() const noexcept { return _worlds.size(); }

		// Save data (record version kVersion). Load resolves each saved worldspace FormID through a_resolve
		// (load order changes); a world that no longer resolves is dropped. False = broken data (nothing kept).
		static constexpr std::uint32_t kVersion = 1;
		std::vector<std::uint8_t> Serialize() const;
		bool                      Deserialize(std::span<const std::uint8_t> a_data, const std::function<bool(std::uint32_t, std::uint32_t&)>& a_resolve);

	private:
		std::unordered_map<std::uint32_t, WorldMap> _worlds;
	};

	// South-west block of the mask window for a player position: snapped, so it only moves every kMaskSnap blocks.
	inline void MaskOrigin(Vec2 a_player, int& a_originX, int& a_originY) noexcept
	{
		const auto snap = [](int a_block) { return FloorDiv(a_block, kMaskSnap) * kMaskSnap - kMaskSize / 2; };
		a_originX = snap(BlockOf(a_player.x));
		a_originY = snap(BlockOf(a_player.y));
	}

	// World rect the mask window covers.
	inline Rect MaskRect(int a_originX, int a_originY) noexcept
	{
		return { a_originX * kBlockUnits, a_originY * kBlockUnits, (a_originX + kMaskSize) * kBlockUnits, (a_originY + kMaskSize) * kBlockUnits };
	}
}
