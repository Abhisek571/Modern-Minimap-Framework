#include "TileCache.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace MM
{
	namespace
	{
		std::string Lower(std::string a_s)
		{
			for (auto& c : a_s)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return a_s;
		}

		std::string TileStem(const std::string& a_world, const TileId& a_id)
		{
			const std::string w = Lower(a_world);
			return w + "\\" + w + "." + std::to_string(a_id.lvl) + "." + std::to_string(a_id.x) + "." + std::to_string(a_id.y);
		}

		std::size_t BitsPerPixel(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				return 4;
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
			case DXGI_FORMAT_R8_UNORM:
				return 8;
			default:
				return 32;
			}
		}

		// GPU bytes of a texture behind an SRV, plus its top mip width.
		std::size_t TextureBytes(ID3D11ShaderResourceView* a_srv, UINT* a_width = nullptr)
		{
			if (!a_srv)
				return 0;
			Microsoft::WRL::ComPtr<ID3D11Resource> res;
			a_srv->GetResource(&res);
			Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
			if (FAILED(res.As(&tex)))
				return 0;
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			if (a_width)
				*a_width = d.Width;
			std::size_t total = 0;
			for (UINT m = 0; m < d.MipLevels; ++m) {
				const std::size_t w = std::max<UINT>(d.Width >> m, 1), h = std::max<UINT>(d.Height >> m, 1);
				total += std::max<std::size_t>(w * h * BitsPerPixel(d.Format) / 8, 8);
			}
			return total;
		}

		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadDDS(ID3D11Device* a_device, const std::vector<std::uint8_t>& a_data, int a_maxSize)
		{
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
			auto load = [&](std::size_t a_max) {
				return DirectX::CreateDDSTextureFromMemoryEx(a_device, a_data.data(), a_data.size(), a_max, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0,
					DirectX::DDS_LOADER_IGNORE_SRGB, nullptr, srv.ReleaseAndGetAddressOf());
			};
			if (FAILED(load(static_cast<std::size_t>(a_maxSize))) && a_maxSize > 0) {
				// No mip small enough (e.g. single-mip far tiles): take the full image.
				if (FAILED(load(0)))
					srv.Reset();
			}
			return srv;
		}

		UINT DDSWidth(const std::vector<std::uint8_t>& a_data)
		{
			std::uint32_t w = 0;
			if (a_data.size() >= 20 && std::memcmp(a_data.data(), "DDS ", 4) == 0)
				std::memcpy(&w, a_data.data() + 16, 4);
			return w;
		}
	}

	std::string TileTexturePath(const std::string& a_world, const TileId& a_id, bool a_normal)
	{
		if (a_id.lvl == kBakedLevel)  // baked top-down picture, one per cell (made by the bake tools)
			return "textures\\ModernMinimap\\" + Lower(a_world) + "\\" + std::to_string(a_id.x) + "." + std::to_string(a_id.y) + ".dds";
		return "textures\\terrain\\" + TileStem(a_world, a_id) + (a_normal ? "_n.dds" : ".dds");
	}

	std::string TileMeshPath(const std::string& a_world, const TileId& a_id)
	{
		return "meshes\\terrain\\" + TileStem(a_world, a_id) + ".btr";
	}

	Tile LoadTile(ID3D11Device* a_device, const FileReader& a_reader, const std::string& a_world, const TileId& a_id, int a_maxTexSize, std::string* a_log)
	{
		Tile tile;
		tile.id = a_id;
		tile.maxTexSize = a_maxTexSize;

		std::vector<std::uint8_t> data;
		const std::string         diffusePath = TileTexturePath(a_world, a_id, false);
		if (!a_reader(diffusePath, data)) {
			tile.absent = true;
			return tile;
		}
		tile.diffuse = LoadDDS(a_device, data, a_maxTexSize);
		if (!tile.diffuse) {
			if (a_log)
				*a_log = "cannot create texture from " + diffusePath;
			tile.absent = true;
			return tile;
		}
		UINT loadedWidth = 0;
		tile.bytes += TextureBytes(tile.diffuse.Get(), &loadedWidth);
		tile.capped = DDSWidth(data) > loadedWidth;
		if (a_id.lvl == kBakedLevel)  // finished picture: no normal map, no water mask
			return tile;

		// Relief is low frequency: half the diffuse resolution is plenty.
		if (a_reader(TileTexturePath(a_world, a_id, true), data)) {
			tile.normal = LoadDDS(a_device, data, std::max(256, a_maxTexSize / 2));
			tile.bytes += TextureBytes(tile.normal.Get());
		}

		const std::string meshPath = TileMeshPath(a_world, a_id);
		if (a_reader(meshPath, data)) {
			BtrData     btr;
			std::string error;
			if (!ParseBtr(data, btr, error)) {
				if (a_log)
					*a_log = meshPath + ": " + error;
			} else if (!btr.water.empty()) {
				const auto mask = RasterizeWater(btr, kWaterMaskSize, a_id.lvl * kCellUnits);
				if (std::ranges::any_of(mask, [](std::uint8_t v) { return v != 0; })) {
					D3D11_TEXTURE2D_DESC td{};
					td.Width = td.Height = kWaterMaskSize;
					td.MipLevels = td.ArraySize = 1;
					td.Format = DXGI_FORMAT_R8_UNORM;
					td.SampleDesc.Count = 1;
					td.Usage = D3D11_USAGE_IMMUTABLE;
					td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					const D3D11_SUBRESOURCE_DATA init{ mask.data(), kWaterMaskSize, 0 };
					Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
					if (SUCCEEDED(a_device->CreateTexture2D(&td, &init, &tex)) && SUCCEEDED(a_device->CreateShaderResourceView(tex.Get(), nullptr, &tile.water)))
						tile.bytes += mask.size();
				}
			}
		}
		return tile;
	}

	TileCache::TileCache(ID3D11Device* a_device, FileReader a_reader, std::size_t a_budgetBytes) :
		_device(a_device),
		_reader(std::move(a_reader)),
		_budget(a_budgetBytes)
	{
		_thread = std::jthread([this](std::stop_token a_stop) { Run(a_stop); });
	}

	TileCache::~TileCache()
	{
		_thread.request_stop();
		_wake.notify_all();
		if (_thread.joinable())
			_thread.join();
	}

	std::uint64_t TileCache::Key(const TileId& a_id) noexcept
	{
		return (static_cast<std::uint64_t>(a_id.lvl) << 48) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_id.x + 0x800000) & 0xFFFFFF) << 24) |
		       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_id.y + 0x800000) & 0xFFFFFF));
	}

	void TileCache::SetWorld(const std::string& a_world)
	{
		const std::string w = Lower(a_world);
		if (w == _world)
			return;
		{
			std::scoped_lock l{ _lock };
			_queue.clear();
			_pending.clear();
			_done.clear();
			++_generation;
		}
		_world = w;
		_tiles.clear();
		_bytes = 0;
	}

	void TileCache::Want(const std::vector<TileId>& a_tiles, int a_maxTexSize)
	{
		if (_world.empty())
			return;
		std::scoped_lock l{ _lock };
		for (const auto& job : _queue)
			_pending.erase(Key(job.id));
		_queue.clear();
		for (const auto& id : a_tiles) {
			const auto key = Key(id);
			if (_pending.contains(key))
				continue;  // in flight
			const auto it = _tiles.find(key);
			if (it != _tiles.end() && !(it->second.capped && it->second.maxTexSize < a_maxTexSize))
				continue;
			_queue.push_back({ _generation, _world, id, a_maxTexSize });
			_pending.insert(key);
		}
		if (!_queue.empty())
			_wake.notify_one();
	}

	bool TileCache::Collect()
	{
		std::vector<std::pair<std::uint32_t, Tile>> done;
		{
			std::scoped_lock l{ _lock };
			done.swap(_done);
			for (const auto& [gen, tile] : done) {
				if (gen == _generation)
					_pending.erase(Key(tile.id));
			}
		}
		bool any = false;
		for (auto& [gen, tile] : done) {
			if (gen != _generation)
				continue;
			auto& slot = _tiles[Key(tile.id)];
			_bytes -= slot.bytes;
			tile.lastUse = ++_clock;
			slot = std::move(tile);
			_bytes += slot.bytes;
			any = true;
		}
		return any;
	}

	const Tile* TileCache::Find(const TileId& a_id)
	{
		const auto it = _tiles.find(Key(a_id));
		if (it == _tiles.end())
			return nullptr;
		it->second.lastUse = ++_clock;
		return &it->second;
	}

	void TileCache::Forget(const TileId& a_id)
	{
		const auto it = _tiles.find(Key(a_id));
		if (it == _tiles.end())
			return;
		_bytes -= it->second.bytes;
		_tiles.erase(it);
	}

	void TileCache::Trim(const std::vector<TileId>& a_keep)
	{
		while (_bytes > _budget) {
			auto victim = _tiles.end();
			for (auto it = _tiles.begin(); it != _tiles.end(); ++it) {
				if (it->second.bytes == 0 || std::ranges::find(a_keep, it->second.id) != a_keep.end())
					continue;
				if (victim == _tiles.end() || it->second.lastUse < victim->second.lastUse)
					victim = it;
			}
			if (victim == _tiles.end())
				break;  // everything left is in view
			_bytes -= victim->second.bytes;
			_tiles.erase(victim);
		}
	}

	void TileCache::WaitIdle()
	{
		std::unique_lock l{ _lock };
		_idle.wait(l, [&] { return _queue.empty() && !_busy; });
	}

	std::vector<std::string> TileCache::TakeLog()
	{
		std::scoped_lock l{ _lock };
		return std::exchange(_log, {});
	}

	void TileCache::Run(std::stop_token a_stop)
	{
		while (!a_stop.stop_requested()) {
			Job job;
			{
				std::unique_lock l{ _lock };
				_wake.wait(l, a_stop, [&] { return !_queue.empty(); });
				if (a_stop.stop_requested())
					return;
				job = std::move(_queue.front());
				_queue.pop_front();
				_busy = true;
			}
			std::string msg;
			Tile        tile;
			// An exception leaving this thread would call std::terminate and take the game down: mark the tile absent.
			try {
				tile = LoadTile(_device, _reader, job.world, job.id, job.maxTexSize, &msg);
			} catch (const std::exception& e) {
				tile = {};
				tile.id = job.id;
				tile.absent = true;
				msg = TileTexturePath(job.world, job.id, false) + ": load failed: " + e.what();
			} catch (...) {
				tile = {};
				tile.id = job.id;
				tile.absent = true;
				msg = TileTexturePath(job.world, job.id, false) + ": load failed";
			}
			{
				std::scoped_lock l{ _lock };
				if (!msg.empty())
					_log.push_back(std::move(msg));
				if (job.generation == _generation)
					_done.emplace_back(job.generation, std::move(tile));
				_busy = false;
			}
			_idle.notify_all();
		}
	}
}
