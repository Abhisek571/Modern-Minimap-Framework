#pragma once

// Background loader and cache for LOD terrain tiles of one worldspace.
// Worker thread: file reads, btr parse, water raster, texture creation on the (free-threaded) device.
// Render thread: everything else (Want / Collect / Find / Trim). The immediate context is never used here.

#include "TerrainMesh.h"
#include "MapMath.h"

#include <condition_variable>
#include <cstdint>
#include <d3d11.h>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>

namespace MM
{
	// Reads a whole file given a Data-relative path ("textures\terrain\..."). Must be callable from the worker thread.
	using FileReader = std::function<bool(const std::string& a_path, std::vector<std::uint8_t>& a_out)>;

	inline constexpr int kWaterMaskSize = 512;

	struct Tile
	{
		TileId                                           id;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuse;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normal;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> water;
		std::size_t                                      bytes{ 0 };
		int                                              maxTexSize{ 0 };
		bool                                             capped{ false };  // a larger mip exists than was loaded
		bool                                             absent{ false };  // no diffuse texture for this tile
		std::uint64_t                                    lastUse{ 0 };
	};

	// Data-relative paths of a tile's files. a_world is the worldspace editor ID (any case).
	std::string TileTexturePath(const std::string& a_world, const TileId& a_id, bool a_normal);
	std::string TileMeshPath(const std::string& a_world, const TileId& a_id);

	// Loads one tile synchronously (what the worker runs). Never touches a device context.
	Tile LoadTile(ID3D11Device* a_device, const FileReader& a_reader, const std::string& a_world, const TileId& a_id, int a_maxTexSize, std::string* a_log = nullptr);

	class TileCache
	{
	public:
		TileCache(ID3D11Device* a_device, FileReader a_reader, std::size_t a_budgetBytes);
		~TileCache();
		TileCache(const TileCache&) = delete;
		TileCache& operator=(const TileCache&) = delete;

		// Switches worldspace; drops every tile of the old one. No-op if unchanged.
		void SetWorld(const std::string& a_world);
		[[nodiscard]] const std::string& World() const noexcept { return _world; }

		// Replaces the load queue with the wanted tiles that are not cached yet (order = priority).
		void Want(const std::vector<TileId>& a_tiles, int a_maxTexSize);

		// Moves finished tiles into the cache. True if any tile arrived (absent ones included).
		bool Collect();

		// Cached tile (possibly absent) or nullptr if not loaded yet.
		const Tile* Find(const TileId& a_id);

		// Drops one cached tile (it changed on disk); the next Want loads it again.
		void Forget(const TileId& a_id);

		// Evicts least recently used tiles not in a_keep until under budget.
		void Trim(const std::vector<TileId>& a_keep);

		// Blocks until the queue is empty and the worker is idle (offline test only).
		void WaitIdle();

		[[nodiscard]] std::size_t Bytes() const noexcept { return _bytes; }
		[[nodiscard]] std::size_t Count() const noexcept { return _tiles.size(); }

		// Messages from the worker (missing files, parse errors); render thread drains them into its log.
		std::vector<std::string> TakeLog();

	private:
		struct Job
		{
			std::uint32_t generation;
			std::string   world;
			TileId        id;
			int           maxTexSize;
		};

		static std::uint64_t Key(const TileId& a_id) noexcept;
		void                 Run(std::stop_token a_stop);

		ID3D11Device*                            _device;
		FileReader                               _reader;
		std::size_t                              _budget;
		std::string                              _world;
		std::uint32_t                            _generation{ 1 };
		std::uint64_t                            _clock{ 0 };
		std::size_t                              _bytes{ 0 };
		std::unordered_map<std::uint64_t, Tile>  _tiles;    // render thread only
		std::unordered_set<std::uint64_t>        _pending;  // render thread only

		std::mutex                   _lock;
		std::condition_variable_any  _wake;
		std::condition_variable      _idle;
		std::deque<Job>              _queue;
		std::vector<std::pair<std::uint32_t, Tile>> _done;
		std::vector<std::string>     _log;
		bool                         _busy{ false };
		std::jthread                 _thread;
	};
}
