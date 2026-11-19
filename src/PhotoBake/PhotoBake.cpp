#include "PCH.h"

#include "PhotoBake.h"

#include "PhotoProcessing.h"
#include "TerrainMesh.h"
#include "Hooks.h"
#include "MapMath.h"
#include "MapRenderer.h"
#include "Minimap.h"
#include "Settings.h"
#include "TileCache.h"

#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <variant>

namespace MM::Bake
{
	namespace
	{
		namespace fs = std::filesystem;
		using Clock = std::chrono::steady_clock;
		using Microsoft::WRL::ComPtr;

		// Waits found while baking the included photos. Each one is there because skipping it crashed the engine at some point.
		constexpr float kJumpCells = 3.0f;      // a move longer than this many cells counts as a long jump
		constexpr float kJumpWait = 12.0f;      // before and after a long jump: let the high camera's LOD queue drain
		constexpr float kCameraTimeout = 20.0f; // most extra seconds to wait for the camera to reach a shot before skipping it
		constexpr float kDrainWait = 12.0f;     // before changing worldspace (BGSQueuedTerrainInitialLoad crash)
		constexpr float kLoadMinWait = 6.0f;    // after MoveTo, before checking that the new area is loaded
		constexpr float kWorldSettle = 15.0f;   // after entering a worldspace
		constexpr float kFogWait = 8.0f;        // camera up, before the first fog test picture
		constexpr float kFogToggleWait = 2.0f;  // after tfog, before the second picture
		constexpr float kLoadTimeout = 600.0f;
		constexpr float kCaptureTimeout = 15.0f;
		constexpr float kPlayerAboveGround = 300.0f;
		constexpr int   kHudFrames = 3;         // frames between hiding the HUD and copying the picture
		constexpr int   kFitShots = 30;         // photos sampled for the colour match
		constexpr int   kRefSize = 128;         // reference render of one 2x2 block (64 px per cell)
		constexpr float kCityMinRadius = 9000.0f;

		constexpr auto kTileDir = "Data/textures/ModernMinimap/tamriel";
		constexpr auto kProgressPath = "Data/SKSE/Plugins/ModernMinimapBake.txt";
		constexpr auto kRootWorld = "Tamriel";
		// Walled cities: own worldspaces sharing Tamriel's coordinates. Tamriel shows only sky inside their walls,
		// so their photos are taken in the city worldspace and stored as Tamriel tiles. Sorted.
		constexpr const char* kCityWorlds[]{ "MarkarthWorld", "RiftenWorld", "SolitudeWorld", "WhiterunWorld", "WindhelmWorld" };

		// ---------------------------------------------------------------- NPC hooks

		std::atomic<bool>          actorsHidden{ false };
		std::atomic<std::uint32_t> actorsDisabled{ 0 };
		std::atomic<std::uint32_t> actorsBlocked{ 0 };

		// The camera moves every few seconds; NPC faces, skeletons and physics loading that fast crashed the engine
		// again and again. Without actor 3D nothing of that kind loads, and the photos have no people in them.
		// Character::Load runs when a placed actor is read from its plugin, before any 3D can be queued for it:
		// marking it disabled there makes the engine treat it like an initially disabled reference.
		struct CharacterLoad
		{
			static bool thunk(RE::Character* a_this, RE::TESFile* a_file)
			{
				const bool ok = func(a_this, a_file);
				if (ok && a_this && !a_this->IsPlayerRef()) {
					a_this->formFlags |= RE::TESForm::RecordFlags::kDisabled;
					const auto n = ++actorsDisabled;
					if (n == 1 || n % 500 == 0)
						logger::info("bake: actors disabled at record load: {}", n);
				}
				return ok;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CharacterLoad3D
		{
			static RE::NiAVObject* thunk(RE::Character* a_this, bool a_backgroundLoading)
			{
				if (a_this && !a_this->IsPlayerRef()) {
					a_this->formFlags |= RE::TESForm::RecordFlags::kDisabled;  // cell, AI and render passes skip it
					const auto n = ++actorsBlocked;
					if (n == 1 || n % 100 == 0)
						logger::info("bake: blocked actor 3D loads: {}", n);
					return nullptr;
				}
				return func(a_this, a_backgroundLoading);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------------------------------------------------------------- shot list

		struct Marker
		{
			RE::FormID  form{ 0 };
			std::string world;
			float       x{ 0 }, y{ 0 };
			int         type{ 0 };
		};

		struct Shot
		{
			std::string                      world;
			int                              bx{ 0 }, by{ 0 };  // south-west cell of the 2x2 block
			float                            x{ 0 }, y{ 0 };    // block centre = shared corner of its 4 cells
			float                            ground{ 0 };
			std::vector<std::pair<int, int>> cells;             // cells this photo writes
			bool                             done{ false };
		};

		std::string ShotKey(const std::string& a_world, int a_bx, int a_by) { return std::format("{} {} {}", a_world, a_bx, a_by); }

		// Marker type -> radius (world units) whose cells get photographed.
		float RadiusFor(int a_type)
		{
			using T = RE::MARKER_TYPE;
			switch (static_cast<T>(a_type)) {
			case T::kCity:
			case T::kDLC02RavenRock:
				return 9000.0f;
			case T::kTown:
			case T::kDLC02TelvanniTower:
				return 6000.0f;
			case T::kSettlement:
			case T::kFort:
			case T::kOrcStronghold:
				return 4500.0f;
			case T::kFarm:
			case T::kDocks:
				return 2500.0f;
			case T::kStable:
			case T::kWoodMill:
			case T::kWheatMill:
			case T::kImperialCamp:
			case T::kStormcloakCamp:
				return 2000.0f;
			case T::kLighthouse:
			case T::kNordicDwelling:
			case T::kMine:
				return 1500.0f;
			case T::kShack:
			case T::kSmelter:
				return 1200.0f;
			default:
				// Every hold castle and capitol marker (35..52).
				if (a_type >= static_cast<int>(T::kRiftenCastle) && a_type <= static_cast<int>(T::kDawnstarCapitol))
					return 6000.0f;
				return 0.0f;
			}
		}

		void CellsNear(float a_x, float a_y, float a_r, std::set<std::pair<int, int>>& a_out)
		{
			const int x0 = static_cast<int>(std::floor((a_x - a_r) / kCellUnits)), x1 = static_cast<int>(std::floor((a_x + a_r) / kCellUnits));
			const int y0 = static_cast<int>(std::floor((a_y - a_r) / kCellUnits)), y1 = static_cast<int>(std::floor((a_y + a_r) / kCellUnits));
			for (int cx = x0; cx <= x1; ++cx) {
				for (int cy = y0; cy <= y1; ++cy) {
					// nearest point of the cell to the marker within r
					const float nx = std::clamp(a_x, cx * kCellUnits, (cx + 1) * kCellUnits);
					const float ny = std::clamp(a_y, cy * kCellUnits, (cy + 1) * kCellUnits);
					if ((nx - a_x) * (nx - a_x) + (ny - a_y) * (ny - a_y) <= a_r * a_r)
						a_out.insert({ cx, cy });
				}
			}
		}

		// 2x2 blocks aligned to even cell numbers, north to south, serpentine so moves stay short.
		std::vector<std::pair<int, int>> Blocks(const std::set<std::pair<int, int>>& a_cells)
		{
			std::set<std::pair<int, int>> set;
			for (const auto& [cx, cy] : a_cells)
				set.insert({ FloorDiv(cx, 2) * 2, FloorDiv(cy, 2) * 2 });
			std::vector<std::pair<int, int>> out(set.begin(), set.end());
			const auto key = [](const std::pair<int, int>& a_b) {
				const int row = -a_b.second / 2;
				return std::pair{ -a_b.second, ((row % 2) + 2) % 2 == 0 ? a_b.first : -a_b.first };
			};
			std::ranges::sort(out, [&](const auto& a, const auto& b) { return key(a) < key(b); });
			return out;
		}

		bool IsCityWorld(const std::string& a_world)
		{
			return std::ranges::any_of(kCityWorlds, [&](const char* w) { return a_world == w; });
		}

		// Main thread: every map marker reference in Tamriel and the walled-city worldspaces, mod-added ones included.
		std::vector<Marker> CollectMarkers()
		{
			std::vector<Marker>     out;
			std::set<RE::FormID>    seen;
			std::vector<std::string> names(std::begin(kCityWorlds), std::end(kCityWorlds));
			names.push_back(kRootWorld);
			auto add = [&](RE::TESObjectREFR* a_ref, const std::string& a_world) {
				if (!a_ref || seen.contains(a_ref->GetFormID()))
					return;
				const auto* extra = a_ref->extraList.GetByType<RE::ExtraMapMarker>();
				const auto* data = extra ? extra->mapData : nullptr;
				if (!data)
					return;
				seen.insert(a_ref->GetFormID());
				const auto pos = a_ref->GetPosition();
				out.push_back({ a_ref->GetFormID(), a_world, pos.x, pos.y, static_cast<int>(data->type.underlying()) });
			};
			for (const auto& name : names) {
				auto* world = RE::TESForm::LookupByEditorID<RE::TESWorldSpace>(name);
				if (!world || !world->persistentCell)
					continue;
				world->persistentCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					add(a_ref, name);
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			// The player's map marker list as well, in case a mod keeps a marker outside the persistent cell.
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				for (const auto& handle : player->GetPlayerRuntimeData().currentMapMarkers) {
					const auto ref = handle.get();
					auto*      world = ref ? ref->GetWorldspace() : nullptr;
					const char* id = world ? world->GetFormEditorID() : nullptr;
					if (id && std::ranges::find(names, std::string(id)) != names.end())
						add(ref.get(), id);
				}
			}
			return out;
		}

		std::vector<Shot> BuildShots(const std::vector<Marker>& a_markers)
		{
			std::map<std::string, std::set<std::pair<int, int>>> city;
			std::set<std::pair<int, int>>                        tam;
			const float scale = Settings::Get()->bake.areaScale;  // player option: photograph a wider area around each marker
			for (const auto& m : a_markers) {
				float r = RadiusFor(m.type) * scale;
				if (IsCityWorld(m.world)) {
					r = std::max(r, kCityMinRadius * scale);
					CellsNear(m.x, m.y, r, city[m.world]);
				} else if (m.world == kRootWorld && r > 0.0f) {
					CellsNear(m.x, m.y, r, tam);
				}
			}
			std::vector<Shot>             shots;
			std::set<std::pair<int, int>> cityCells;  // filled from the city photos; Tamriel shows sky there
			auto                          add = [&](const std::string& a_world, int a_bx, int a_by) {
                Shot s;
                s.world = a_world;
                s.bx = a_bx;
                s.by = a_by;
                s.x = (a_bx + 1) * kCellUnits;
                s.y = (a_by + 1) * kCellUnits;
                for (int dy = 0; dy < BakeImage::kBlockCells; ++dy)
                    for (int dx = 0; dx < BakeImage::kBlockCells; ++dx)
                        if (a_world != kRootWorld || !cityCells.contains({ a_bx + dx, a_by + dy }))
                            s.cells.push_back({ a_bx + dx, a_by + dy });
                if (!s.cells.empty())
                    shots.push_back(std::move(s));
			};
			for (const auto& [world, cells] : city) {
				for (const auto& [bx, by] : Blocks(cells)) {
					add(world, bx, by);
					for (int dy = 0; dy < BakeImage::kBlockCells; ++dy)
						for (int dx = 0; dx < BakeImage::kBlockCells; ++dx)
							cityCells.insert({ bx + dx, by + dy });
				}
			}
			for (const auto& [bx, by] : Blocks(tam))
				add(kRootWorld, bx, by);
			return shots;
		}

		// Ground height from the level-4 terrain LOD mesh. ParseBtr already applies the shape transform
		// (the x4 scale), so vertices are in world units relative to the tile corner; scaling again put every camera
		// four times too high above the ground.
		class GroundReader
		{
		public:
			std::optional<float> At(float a_x, float a_y)
			{
				const TileId id{ 4, FloorDiv(WorldToCell(a_x), 4) * 4, FloorDiv(WorldToCell(a_y), 4) * 4 };
				const auto   key = std::pair{ id.x, id.y };
				auto         it = _cache.find(key);
				if (it == _cache.end()) {
					std::vector<std::array<float, 3>> verts;
					std::vector<std::uint8_t>         data;
					BtrData                           btr;
					std::string                       error;
					if (Minimap::ReadGameFile(TileMeshPath(kRootWorld, id), data) && ParseBtr(data, btr, error) && !btr.land.empty())
						verts = std::move(btr.land[0].verts);
					it = _cache.emplace(key, std::move(verts)).first;
				}
				auto& verts = it->second;
				if (verts.empty())
					return std::nullopt;
				const float lx = a_x - id.x * kCellUnits, ly = a_y - id.y * kCellUnits;
				const auto  d2 = [&](const std::array<float, 3>& v) { return (v[0] - lx) * (v[0] - lx) + (v[1] - ly) * (v[1] - ly); };
				const auto  n = std::min<std::size_t>(6, verts.size());
				std::partial_sort(verts.begin(), verts.begin() + static_cast<std::ptrdiff_t>(n), verts.end(), [&](const auto& a, const auto& b) { return d2(a) < d2(b); });
				float sum = 0;
				for (std::size_t i = 0; i < n; ++i)
					sum += verts[i][2];
				return sum / static_cast<float>(n);
			}

		private:
			std::map<std::pair<int, int>, std::vector<std::array<float, 3>>> _cache;
		};

		fs::path TilePath(int a_cx, int a_cy) { return fs::path(kTileDir) / std::format("{}.{}.dds", a_cx, a_cy); }

		// ---------------------------------------------------------------- progress file

		struct Progress
		{
			bool                  toneKnown{ false };
			BakeImage::Tone       tone;
			std::set<std::string> done;
		};

		Progress LoadProgress()
		{
			Progress      p;
			std::ifstream in(kProgressPath);
			std::string   line;
			while (std::getline(in, line)) {
				std::istringstream s(line);
				std::string        what;
				s >> what;
				if (what == "tone") {
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
			return p;
		}

		void SaveProgress(const Progress& a_p)
		{
			std::error_code ec;
			fs::create_directories(fs::path(kProgressPath).parent_path(), ec);
			const auto tmp = std::string(kProgressPath) + ".tmp";
			{
				std::ofstream out(tmp, std::ios::trunc);
				if (!out)
					return;
				out << "# Modern Minimap town photo bake progress. Delete it (or press Reset) to bake everything again.\n";
				if (a_p.toneKnown) {
					out << "tone " << (a_p.tone.fitted ? "fit" : "grade") << "\n";
					out << "lut";
					for (const float v : a_p.tone.lut)
						out << ' ' << v;
					out << "\ngain " << a_p.tone.gain[0] << ' ' << a_p.tone.gain[1] << ' ' << a_p.tone.gain[2] << "\n";
				}
				for (const auto& key : a_p.done)
					out << "done " << key << "\n";
			}
			fs::rename(tmp, kProgressPath, ec);
			if (ec)
				logger::warn("bake: could not save progress: {}", ec.message());
		}

		// ---------------------------------------------------------------- render thread: captures and reference renders

		struct Photo
		{
			int                       w{ 0 }, h{ 0 };
			DXGI_FORMAT               format{ DXGI_FORMAT_UNKNOWN };
			std::size_t               pitch{ 0 };
			std::vector<std::uint8_t> bytes;
		};

		std::atomic<int> screenW{ 0 }, screenH{ 0 };

		std::mutex                capMutex;  // everything cap*
		int                       capFrames = -1;  // frames to wait before the copy (-1 = no request)
		bool                      capCopied = false;
		bool                      capFailed = false;
		bool                      capUnsupported = false;  // the back buffer cannot be read at all: stop the bake
		ComPtr<ID3D11Texture2D>   capStaging;
		D3D11_TEXTURE2D_DESC      capDesc{};
		std::shared_ptr<Photo>    capResult;

		// Reference: the minimap's own LOD look for one block, rendered with the minimap renderer and read back.
		struct RefRequest
		{
			TileDraw                                tile;
			ComPtr<ID3D11ShaderResourceView>        keepDiffuse, keepNormal, keepWater;
			Vec2                                    centre;
			ShadeParams                             shade;
			std::promise<std::vector<std::uint8_t>> result;
		};
		std::mutex                  refMutex;
		std::shared_ptr<RefRequest> refQueued;   // waiting for the render thread
		std::shared_ptr<RefRequest> refInFlight;  // render thread only
		ComPtr<ID3D11Texture2D>     refStaging;   // render thread only
		MapRenderer*                refRenderer = nullptr;  // render thread only; left alive at exit on purpose
		bool                        refRendererFailed = false;
		D3DStateBlock               refState;

		ID3D11DeviceContext* Context()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11DeviceContext*>(r->GetRuntimeData().context) : nullptr;
		}

		ID3D11Device* Device()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11Device*>(r->GetRuntimeData().forwarder) : nullptr;
		}

		std::size_t BytesPerPixel(DXGI_FORMAT a_f)
		{
			return (a_f == DXGI_FORMAT_R16G16B16A16_FLOAT || a_f == DXGI_FORMAT_R16G16B16A16_TYPELESS) ? 8 : 4;
		}

		void CaptureStep(IDXGISwapChain* a_swap, ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx)
		{
			std::lock_guard lock(capMutex);
			if (capFrames > 0) {
				--capFrames;
				return;
			}
			if (capFrames == 0) {
				capFrames = -1;
				ComPtr<ID3D11Texture2D> back;
				if (FAILED(a_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
					capFailed = true;
					return;
				}
				D3D11_TEXTURE2D_DESC d{};
				back->GetDesc(&d);
				if (d.SampleDesc.Count != 1 || !BakeImage::IsSupportedFormat(d.Format)) {
					logger::error("bake: back buffer format {} with {} samples cannot be read", static_cast<int>(d.Format), d.SampleDesc.Count);
					capFailed = true;
					capUnsupported = true;
					return;
				}
				if (!capStaging || capDesc.Width != d.Width || capDesc.Height != d.Height || capDesc.Format != d.Format) {
					capStaging.Reset();
					D3D11_TEXTURE2D_DESC s = d;
					s.MipLevels = s.ArraySize = 1;
					s.Usage = D3D11_USAGE_STAGING;
					s.BindFlags = 0;
					s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					s.MiscFlags = 0;
					if (FAILED(a_dev->CreateTexture2D(&s, nullptr, &capStaging))) {
						capFailed = true;
						return;
					}
					capDesc = d;
					logger::info("bake: capturing {}x{} back buffer, format {}", d.Width, d.Height, static_cast<int>(d.Format));
				}
				a_ctx->CopySubresourceRegion(capStaging.Get(), 0, 0, 0, 0, back.Get(), 0, nullptr);
				capCopied = true;
				return;
			}
			if (!capCopied)
				return;
			// Read back without stalling the frame: try again next frame until the GPU has finished the copy.
			D3D11_MAPPED_SUBRESOURCE m{};
			const HRESULT            hr = a_ctx->Map(capStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
				return;
			capCopied = false;
			if (FAILED(hr)) {
				capFailed = true;
				return;
			}
			auto photo = std::make_shared<Photo>();
			photo->w = static_cast<int>(capDesc.Width);
			photo->h = static_cast<int>(capDesc.Height);
			photo->format = capDesc.Format;
			photo->pitch = capDesc.Width * BytesPerPixel(capDesc.Format);
			photo->bytes.resize(photo->pitch * capDesc.Height);
			for (UINT y = 0; y < capDesc.Height; ++y)
				std::memcpy(photo->bytes.data() + photo->pitch * y, static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(m.RowPitch) * y, photo->pitch);
			a_ctx->Unmap(capStaging.Get(), 0);
			capResult = std::move(photo);
		}

		void ReferenceStep(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx)
		{
			if (refInFlight) {
				D3D11_MAPPED_SUBRESOURCE m{};
				const HRESULT            hr = a_ctx->Map(refStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
					return;
				std::vector<std::uint8_t> rgba;
				if (SUCCEEDED(hr)) {
					rgba.resize(static_cast<std::size_t>(kRefSize) * kRefSize * 4);
					for (int y = 0; y < kRefSize; ++y)
						std::memcpy(rgba.data() + static_cast<std::size_t>(y) * kRefSize * 4, static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(m.RowPitch) * y, kRefSize * 4);
					a_ctx->Unmap(refStaging.Get(), 0);
				}
				refInFlight->result.set_value(std::move(rgba));
				refInFlight.reset();
				return;
			}
			std::shared_ptr<RefRequest> req;
			{
				std::lock_guard lock(refMutex);
				req = std::exchange(refQueued, nullptr);
			}
			if (!req)
				return;
			if (!refRenderer && !refRendererFailed) {
				refRenderer = new MapRenderer;
				std::string error;
				if (!refRenderer->Init(a_dev, error)) {
					logger::error("bake: reference renderer init failed: {}", error);
					refRendererFailed = true;
				}
			}
			if (!refStaging && !refRendererFailed) {
				D3D11_TEXTURE2D_DESC s{};
				s.Width = s.Height = kRefSize;
				s.MipLevels = s.ArraySize = 1;
				s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // the compose target's format
				s.SampleDesc.Count = 1;
				s.Usage = D3D11_USAGE_STAGING;
				s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(a_dev->CreateTexture2D(&s, nullptr, &refStaging)))
					refRendererFailed = true;
			}
			if (refRendererFailed) {
				req->result.set_value({});
				return;
			}
			refState.Backup(a_ctx);
			const TileDraw draws[]{ req->tile };
			refRenderer->Compose(a_ctx, { req->centre, BakeImage::kBlockCells * kCellUnits / kRefSize, kRefSize, req->shade }, draws);
			const bool ok = refRenderer->ComposeTexture() && refRenderer->ComposeSize() == kRefSize;
			if (ok)
				a_ctx->CopyResource(refStaging.Get(), refRenderer->ComposeTexture());
			refState.Restore(a_ctx);
			if (ok)
				refInFlight = std::move(req);
			else
				req->result.set_value({});
		}

		// ---------------------------------------------------------------- worker: crop, colour, encode, write

		struct BeginJob
		{
			Progress      progress;
			ID3D11Device* device{ nullptr };
			ShadeParams   shade;
		};
		struct ContrastJob
		{
			std::shared_ptr<Photo>              photo;
			std::shared_ptr<std::promise<float>> result;
		};
		struct ShotJob
		{
			std::string                      key;
			int                              bx{ 0 }, by{ 0 };
			std::vector<std::pair<int, int>> cells;
			float                            fov{ BakeImage::kBaseFovDeg };
			std::shared_ptr<Photo>           photo;
		};
		struct FlushJob
		{};
		using Job = std::variant<BeginJob, ContrastJob, ShotJob, FlushJob>;

		std::mutex              jobMutex;
		std::condition_variable jobWake;
		std::deque<Job>         jobs;
		std::atomic<int>        jobsOpen{ 0 };  // queued + running
		std::thread*            worker = nullptr;  // never joined: left running at exit on purpose

		std::mutex  statusMutex;
		Status      status;  // guarded by statusMutex

		void SetMessage(std::string a_msg)
		{
			logger::info("bake: {}", a_msg);
			std::lock_guard lock(statusMutex);
			status.message = std::move(a_msg);
		}

		void SetPhase(std::string a_phase)
		{
			std::lock_guard lock(statusMutex);
			status.phase = std::move(a_phase);
		}

		void SetTone(std::string a_tone)
		{
			std::lock_guard lock(statusMutex);
			status.tone = std::move(a_tone);
		}

		// Worker state (worker thread only).
		struct PendingShot
		{
			std::string                                                  key;
			std::vector<std::pair<std::pair<int, int>, std::vector<std::uint8_t>>> tiles;
		};
		Progress                                                                   wProgress;
		ID3D11Device*                                                              wDevice = nullptr;
		ShadeParams                                                                wShade;
		std::vector<PendingShot>                                                   wPending;
		std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> wSamples;
		int                                                                        wSampledShots = 0;

		std::optional<std::vector<std::uint8_t>> RenderReference(int a_bx, int a_by)
		{
			if (!wDevice)
				return std::nullopt;
			const TileId id{ 4, FloorDiv(a_bx, 4) * 4, FloorDiv(a_by, 4) * 4 };
			std::string  log;
			Tile         tile = LoadTile(wDevice, Minimap::ReadGameFile, kRootWorld, id, 512, &log);
			if (tile.absent || !tile.diffuse)
				return std::nullopt;
			auto req = std::make_shared<RefRequest>();
			req->tile = { id, tile.diffuse.Get(), tile.normal.Get(), tile.water.Get() };
			req->keepDiffuse = tile.diffuse;
			req->keepNormal = tile.normal;
			req->keepWater = tile.water;
			req->centre = { (a_bx + 1) * kCellUnits, (a_by + 1) * kCellUnits };
			req->shade = wShade;
			auto future = req->result.get_future();
			{
				std::lock_guard lock(refMutex);
				refQueued = req;
			}
			if (future.wait_for(10s) != std::future_status::ready) {
				std::lock_guard lock(refMutex);
				if (refQueued == req)
					refQueued.reset();
				return std::nullopt;
			}
			auto rgba = future.get();
			if (rgba.empty())
				return std::nullopt;
			return rgba;
		}

		void AddSamples(int a_bx, int a_by, const PendingShot& a_shot)
		{
			const auto ref = RenderReference(a_bx, a_by);
			if (!ref)
				return;
			constexpr int q = kRefSize / BakeImage::kBlockCells;  // reference px per cell
			static_assert(q == BakeImage::kSampleSize);
			for (const auto& [cell, rgb] : a_shot.tiles) {
				const int                 dx = cell.first - a_bx, dy = cell.second - a_by;
				std::vector<std::uint8_t> refCell(static_cast<std::size_t>(q) * q * 3);
				for (int y = 0; y < q; ++y) {
					for (int x = 0; x < q; ++x) {
						// Reference row 0 is north, so the block's northern cells (dy = 1) are the top half.
						const auto* p = ref->data() + ((static_cast<std::size_t>((BakeImage::kBlockCells - 1 - dy) * q + y)) * kRefSize + static_cast<std::size_t>(dx * q + x)) * 4;
						auto*       o = refCell.data() + (static_cast<std::size_t>(y) * q + x) * 3;
						o[0] = p[0];
						o[1] = p[1];
						o[2] = p[2];
					}
				}
				wSamples.emplace_back(BakeImage::BoxDown(rgb, BakeImage::kTileSize, q), std::move(refCell));
			}
		}

		void WriteShot(PendingShot& a_shot)
		{
			std::error_code ec;
			fs::create_directories(kTileDir, ec);
			for (auto& [cell, rgb] : a_shot.tiles) {
				BakeImage::ApplyTone(wProgress.tone, rgb);
				const auto dds = BakeImage::EncodeDdsBc1(rgb, BakeImage::kTileSize);
				const auto path = TilePath(cell.first, cell.second);
				const auto tmp = fs::path(path).concat(".tmp");
				{
					std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
					out.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
					if (!out) {
						logger::error("bake: could not write {}", tmp.string());
						continue;
					}
				}
				// Written next to the target and renamed, so the minimap never reads half a file.
				fs::rename(tmp, path, ec);
				if (ec) {
					logger::error("bake: could not write {}: {}", path.string(), ec.message());
					continue;
				}
				Minimap::ForgetBakedTile(cell.first, cell.second);
			}
			wProgress.done.insert(a_shot.key);
			SaveProgress(wProgress);
		}

		void DecideTone()
		{
			if (wSamples.size() >= 4) {
				wProgress.tone = BakeImage::FitMatch(wSamples);
			} else {
				wProgress.tone = {};
			}
			wProgress.toneKnown = true;
			if (wProgress.tone.fitted) {
				logger::info("bake: colour match to the terrain LOD from {} cells (tint gain {:.3f} {:.3f} {:.3f})", wSamples.size(), wProgress.tone.gain[0],
					wProgress.tone.gain[1], wProgress.tone.gain[2]);
				SetTone(std::format("matched to your terrain LOD ({} cells)", wSamples.size()));
			} else {
				logger::info("bake: no terrain LOD reference; fixed tone curve");
				SetTone("fixed tone curve (no terrain LOD to match)");
			}
			wSamples.clear();
			for (auto& p : wPending)
				WriteShot(p);
			wPending.clear();
		}

		void DumpPhoto(const BakeImage::RgbImage& a_img, const std::string& a_key);

		void RunJob(Job& a_job)
		{
			if (auto* b = std::get_if<BeginJob>(&a_job)) {
				wProgress = std::move(b->progress);
				wDevice = b->device;
				wShade = b->shade;
				wPending.clear();
				wSamples.clear();
				wSampledShots = 0;
				SetTone(wProgress.toneKnown ? (wProgress.tone.fitted ? "colour match from the earlier run" : "fixed tone curve (earlier run)") : "sampling the first photos");
			} else if (auto* c = std::get_if<ContrastJob>(&a_job)) {
				BakeImage::RgbImage img;
				float               v = -1.0f;
				if (BakeImage::ConvertPhoto(c->photo->bytes.data(), c->photo->w, c->photo->h, c->photo->pitch, c->photo->format, img))
					v = BakeImage::CentreContrast(img);
				c->result->set_value(v);
			} else if (auto* s = std::get_if<ShotJob>(&a_job)) {
				BakeImage::RgbImage img;
				if (!BakeImage::ConvertPhoto(s->photo->bytes.data(), s->photo->w, s->photo->h, s->photo->pitch, s->photo->format, img)) {
					logger::error("bake: could not read the photo for {}", s->key);
					return;
				}
				s->photo.reset();  // the raw copy is not needed any more
				if (Settings::Get()->debugLog)
					DumpPhoto(img, s->key);
				PendingShot shot{ s->key, {} };
				for (const auto& cell : s->cells)
					shot.tiles.push_back({ cell, BakeImage::CropCell(img, s->fov, cell.first - s->bx, cell.second - s->by) });
				if (wProgress.toneKnown) {
					WriteShot(shot);
					return;
				}
				// The colour match needs a sample first: hold the first photos (as 8-bit tiles) until it is fitted.
				if (wSampledShots < kFitShots) {
					AddSamples(s->bx, s->by, shot);
					++wSampledShots;
					SetTone(std::format("sampling the first photos ({}/{})", wSampledShots, kFitShots));
				}
				wPending.push_back(std::move(shot));
				if (static_cast<int>(wPending.size()) >= kFitShots)
					DecideTone();
			} else if (std::holds_alternative<FlushJob>(a_job)) {
				if (!wProgress.toneKnown && !wPending.empty())
					DecideTone();
			}
		}

		// Debug log on: keep a quarter-size BMP of every raw photo, to check camera placement against the tiles.
		void DumpPhoto(const BakeImage::RgbImage& a_img, const std::string& a_key)
		{
			const std::filesystem::path dir = "Data/SKSE/Plugins/ModernMinimapBakeDebug";
			std::error_code             ec;
			std::filesystem::create_directories(dir, ec);
			const int w = a_img.w / 4, h = a_img.h / 4, row = (w * 3 + 3) & ~3;
			std::vector<std::uint8_t> out(54 + static_cast<std::size_t>(row) * h, 0);
			auto put32 = [&](std::size_t o, std::uint32_t v) { std::memcpy(out.data() + o, &v, 4); };
			out[0] = 'B';
			out[1] = 'M';
			put32(2, static_cast<std::uint32_t>(out.size()));
			put32(10, 54);
			put32(14, 40);
			put32(18, static_cast<std::uint32_t>(w));
			put32(22, static_cast<std::uint32_t>(h));
			out[26] = 1;
			out[28] = 24;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) {
					const float* c = &a_img.px[(static_cast<std::size_t>(y * 4) * a_img.w + x * 4) * 3];
					auto* d = &out[54 + static_cast<std::size_t>(h - 1 - y) * row + x * 3];
					for (int k = 0; k < 3; ++k)
						d[2 - k] = static_cast<std::uint8_t>(std::clamp(c[k], 0.0f, 255.0f) + 0.5f);
				}
			std::string name = a_key;
			std::ranges::replace(name, ' ', '_');
			std::ofstream(dir / (name + ".bmp"), std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
		}

		void WorkerLoop()
		{
			for (;;) {
				Job job;
				{
					std::unique_lock lock(jobMutex);
					jobWake.wait(lock, [] { return !jobs.empty(); });
					job = std::move(jobs.front());
					jobs.pop_front();
				}
				// An exception leaving this thread would end the game (std::terminate): log it and go on.
				try {
					RunJob(job);
				} catch (const std::exception& e) {
					logger::error("bake: worker job failed: {}", e.what());
				} catch (...) {
					logger::error("bake: worker job failed");
				}
				if (auto* c = std::get_if<ContrastJob>(&job)) {
					try {
						c->result->set_value(-1.0f);  // throws if already set; only matters when the job failed early
					} catch (...) {
					}
				}
				--jobsOpen;
			}
		}

		void Submit(Job a_job)
		{
			if (!worker)
				worker = new std::thread(WorkerLoop);
			++jobsOpen;
			{
				std::lock_guard lock(jobMutex);
				jobs.push_back(std::move(a_job));
			}
			jobWake.notify_one();
		}

		std::shared_future<float> SubmitContrast(std::shared_ptr<Photo> a_photo)
		{
			auto promise = std::make_shared<std::promise<float>>();
			auto future = promise->get_future().share();
			Submit(ContrastJob{ std::move(a_photo), std::move(promise) });
			return future;
		}

		// ---------------------------------------------------------------- main thread: the bake itself

		enum class Phase
		{
			kIdle,
			kPreparing,
			kNext,
			kDrain,
			kWaitLoad,
			kWorldSettle,
			kFogWait,
			kFogToggleWait,
			kFogDecide,
			kJumpBefore,
			kJumpAfter,
			kSettle,
			kCapturing,
			kFinishing
		};

		enum class CapPurpose
		{
			kShot,
			kFogA,
			kFogB
		};

		enum class CamMode
		{
			kNone,
			kPlayer,  // hold an already free camera just above the player (camera low while the LOD queue drains)
			kShot     // straight down over the shot
		};

		std::atomic<bool> running{ false };
		std::atomic<bool> cancelRequested{ false };
		std::atomic<bool> tickQueued{ false };

		// Main thread only.
		Phase             phase = Phase::kIdle;
		Phase             afterLoad = Phase::kWorldSettle;
		CapPurpose        capPurpose = CapPurpose::kShot;
		CamMode           camMode = CamMode::kNone;
		float             cameraWait = 0.0f;  // extra seconds spent waiting for the camera to reach the shot
		RE::NiPoint3      camTarget;
		float             waitLeft = 0;
		float             loadElapsed = 0;
		float             captureElapsed = 0;
		bool              hudWasShown = true;
		std::vector<Shot> shots;
		std::vector<Marker> markers;
		std::size_t       cur = 0;
		std::string       curWorld;
		bool              havePrev = false;
		float             prevX = 0, prevY = 0;
		float             fov = BakeImage::kBaseFovDeg;
		float             settle = 7.0f;
		bool              tclDone = false;  // console toggles: once per game session, never twice
		bool              tgmDone = false;
		std::shared_future<float> fogA, fogB;
		Clock::time_point lastTick;
		Clock::time_point firstShotTime;
		int               capturedThisRun = 0;
		int               doneBefore = 0;
		int               remainingAtStart = 0;

		// Ground heights are read on a helper thread at start (a few hundred LOD meshes).
		std::atomic<bool>  prepDone{ false };
		std::mutex         prepMutex;
		std::vector<Shot>  prepShots;

		void Console(const std::string& a_cmd)
		{
			auto* script = RE::IFormFactory::Create<RE::Script>();
			if (!script) {
				logger::warn("bake: cannot run console command '{}'", a_cmd);
				return;
			}
			script->SetCommand(a_cmd);
			script->CompileAndRun(RE::PlayerCharacter::GetSingleton());
			delete script;
		}

		bool Loaded()
		{
			auto* ui = RE::UI::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			return ui && player && cell && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) && player->Is3DLoaded() && (cell->IsInteriorCell() || cell->IsAttached());
		}

		void SetFreeCamera(const RE::NiPoint3& a_pos)
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			auto* state = cam ? cam->currentState.get() : nullptr;
			if (!state || state->id != RE::CameraState::kFree)
				return;
			auto* freeCam = static_cast<RE::FreeCameraState*>(state);
			freeCam->translation = a_pos;
			freeCam->rotation.x = std::numbers::pi_v<float> / 2.0f;  // pitch: straight down
			freeCam->rotation.y = 0.0f;                              // yaw: picture top = north
		}

		// Free camera on (never off: switching it off crashed the engine in the player's depth-of-field image space).
		// At most every two seconds, so a camera that refuses to switch is not toggled every frame.
		void EnsureFreeCamera()
		{
			static Clock::time_point lastTry{};
			auto*                    cam = RE::PlayerCamera::GetSingleton();
			if (cam && !cam->IsInFreeCameraMode() && Clock::now() - lastTry > 2s) {
				lastTry = Clock::now();
				cam->ToggleFreeCameraMode(false);
				Console(std::format("fov {:.4f}", fov));  // switching it on resets the field of view
			}
		}

		// Where the rendered camera really is (the free camera state only stores what we asked for).
		std::optional<RE::NiPoint3> CameraPos()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || !cam->cameraRoot)
				return std::nullopt;
			return cam->cameraRoot->world.translate;
		}

		// True when the camera has reached the shot position; logs why not (once per shot) when it has not.
		bool CameraAtTarget(bool a_log)
		{
			const auto pos = CameraPos();
			const bool there = pos && std::hypot(pos->x - camTarget.x, pos->y - camTarget.y) < 1000.0f && std::abs(pos->z - camTarget.z) < 5000.0f;
			if (!there && a_log) {
				auto*       cam = RE::PlayerCamera::GetSingleton();
				auto*       player = RE::PlayerCharacter::GetSingleton();
				auto*       cell = player ? player->GetParentCell() : nullptr;
				const auto* state = cam ? cam->currentState.get() : nullptr;
				const auto  pp = player ? player->GetPosition() : RE::NiPoint3{};
				logger::warn("bake: camera not at the shot yet: camera {:.0f},{:.0f},{:.0f} target {:.0f},{:.0f},{:.0f}; state {} free {}; player {:.0f},{:.0f},{:.0f} cell {} attached {} 3D {}",
					pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f, camTarget.x, camTarget.y, camTarget.z,
					state ? static_cast<int>(state->id) : -1, cam && cam->IsInFreeCameraMode(), pp.x, pp.y, pp.z,
					cell ? cell->GetFormEditorID() : "none", cell && cell->IsAttached(), player && player->Is3DLoaded());
			}
			return there;
		}

		void DriveCamera()
		{
			// Keep steering while cells stream in: waiting for the player's cell to attach left the camera parked
			// at the previous shot after a worldspace change, and every photo there showed the same spot.
			auto* ui = RE::UI::GetSingleton();
			if (camMode == CamMode::kNone || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME))
				return;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (camMode == CamMode::kShot) {
				EnsureFreeCamera();
				SetFreeCamera(camTarget);
			} else if (player) {
				auto p = player->GetPosition();
				p.z += 200.0f;
				SetFreeCamera(p);
			}
		}

		void MovePlayer(const Shot& a_s)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				return;
			// Noon for every shot: game time keeps running during a bake that takes hours.
			if (auto* cal = RE::Calendar::GetSingleton(); cal && cal->gameHour)
				cal->gameHour->value = 12.0f;
			// The reference-level SetPosition is the native behind Papyrus SetPosition and player.setpos.
			player->RE::TESObjectREFR::SetPosition(RE::NiPoint3{ a_s.x, a_s.y, a_s.ground + kPlayerAboveGround });
			havePrev = true;
			prevX = a_s.x;
			prevY = a_s.y;
		}

		void AimAt(const Shot& a_s)
		{
			camMode = CamMode::kShot;
			camTarget = { a_s.x, a_s.y, a_s.ground + BakeImage::kCameraHeight };
		}

		void WorldSetup()
		{
			Console("set gamehour to 12");
			Console("fw 81a");
			if (!std::exchange(tclDone, true))
				Console("tcl");
			// God mode once: a fall or a hostile kill reloads a save, which a save-reload mod turns into a game restart.
			if (!std::exchange(tgmDone, true)) {
				Console("tgm");
				Console("player.modav health 1000000");
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				player->SetAlpha(0.0f);
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (cam && !cam->IsInFreeCameraMode())
				cam->ToggleFreeCameraMode(false);
			Console(std::format("fov {:.4f}", fov));  // after the free camera: switching it on resets the field of view
		}

		RE::TESObjectREFR* EntryMarker(const std::string& a_world, float a_x, float a_y)
		{
			RE::TESObjectREFR* best = nullptr;
			float              bestD = 0;
			for (const auto& m : markers) {
				if (m.world != a_world)
					continue;
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(m.form);
				if (!ref || ref->IsDisabled())
					continue;
				const float d = (m.x - a_x) * (m.x - a_x) + (m.y - a_y) * (m.y - a_y);
				if (!best || d < bestD) {
					best = ref;
					bestD = d;
				}
			}
			return best;
		}

		void CloseTutorial()
		{
			auto* ui = RE::UI::GetSingleton();
			auto* q = RE::UIMessageQueue::GetSingleton();
			if (ui && q && ui->IsMenuOpen(RE::TutorialMenu::MENU_NAME))
				q->AddMessage(RE::TutorialMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		}

		void BeginCapture(CapPurpose a_purpose)
		{
			// Survival / tutorial pop-ups cover the picture and spoil the fog test.
			CloseTutorial();
			if (auto* ui = RE::UI::GetSingleton()) {
				hudWasShown = ui->IsShowingMenus();
				ui->ShowMenus(false);
			}
			Minimap::SetSuppressed(true);
			{
				std::lock_guard lock(capMutex);
				capFrames = kHudFrames;
				capCopied = false;
				capFailed = false;
				capUnsupported = false;
				capResult.reset();
			}
			capPurpose = a_purpose;
			captureElapsed = 0;
			phase = Phase::kCapturing;
		}

		void EndCaptureUi()
		{
			if (auto* ui = RE::UI::GetSingleton())
				ui->ShowMenus(hudWasShown);
			Minimap::SetSuppressed(false);
		}

		void UpdateCounts()
		{
			std::lock_guard lock(statusMutex);
			status.done = doneBefore + capturedThisRun;
			const int left = remainingAtStart - capturedThisRun;
			if (capturedThisRun > 0 && left > 0) {
				const double perShot = std::chrono::duration<double>(Clock::now() - firstShotTime).count() / capturedThisRun;
				status.etaSeconds = perShot * left;
			} else {
				status.etaSeconds = 0;
			}
		}

		void Stop(const std::string& a_msg)
		{
			if (phase == Phase::kCapturing)
				EndCaptureUi();
			Minimap::SetSuppressed(false);
			{
				std::lock_guard lock(capMutex);
				capFrames = -1;
			}
			phase = Phase::kIdle;
			camMode = CamMode::kNone;  // the camera stays free where it is
			running = false;
			cancelRequested = false;
			Submit(FlushJob{});
			if (actorsHidden || Settings::BakeHideActorsRequested())
				Settings::SetBakeHideActors(false);
			Settings::ClearBakeStartOnLoad();
			std::string msg = a_msg + " The camera stays in free mode: load a save now.";
			if (actorsHidden)
				msg += " Restart the game to bring NPCs back.";
			SetMessage(msg);
			SetPhase("stopped");
			RE::DebugMessageBox(("Modern Minimap: " + msg).c_str());
		}

		void StartShot(const Shot& a_s)
		{
			MovePlayer(a_s);
			AimAt(a_s);
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode())
				Console(std::format("fov {:.4f}", fov));
			waitLeft = settle;
			phase = Phase::kSettle;
			SetPhase(std::format("settling at {} {},{}", a_s.world, a_s.bx, a_s.by));
		}

		void OnCaptured(std::shared_ptr<Photo> a_photo)
		{
			const Shot& s = shots[cur];
			switch (capPurpose) {
			case CapPurpose::kFogA:
				fogA = a_photo ? SubmitContrast(std::move(a_photo)) : std::shared_future<float>{};
				Console("tfog");
				waitLeft = kFogToggleWait;
				phase = Phase::kFogToggleWait;
				break;
			case CapPurpose::kFogB:
				fogB = a_photo ? SubmitContrast(std::move(a_photo)) : std::shared_future<float>{};
				phase = Phase::kFogDecide;
				break;
			case CapPurpose::kShot:
				if (a_photo && Settings::Get()->debugLog) {
					const auto pos = CameraPos();
					logger::debug("bake: shot {} {},{} target {:.0f},{:.0f} camera {:.0f},{:.0f},{:.0f}", s.world, s.bx, s.by, camTarget.x, camTarget.y,
						pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f);
				}
				if (a_photo) {
					Submit(ShotJob{ ShotKey(s.world, s.bx, s.by), s.bx, s.by, s.cells, fov, std::move(a_photo) });
					if (capturedThisRun++ == 0)
						firstShotTime = Clock::now();
					shots[cur].done = true;
					UpdateCounts();
				} else {
					logger::warn("bake: capture failed: {} {},{} (taken again on the next run)", s.world, s.bx, s.by);
				}
				++cur;
				phase = Phase::kNext;
				break;
			}
		}

		void Step(float a_dt)
		{
			const bool waited = (waitLeft -= a_dt) <= 0.0f;
			switch (phase) {
			case Phase::kIdle:
				return;
			case Phase::kPreparing:
				if (prepDone) {
					std::lock_guard lock(prepMutex);
					shots = std::move(prepShots);
					cur = 0;
					remainingAtStart = static_cast<int>(std::ranges::count_if(shots, [](const Shot& s) { return !s.done; }));
					doneBefore = static_cast<int>(shots.size()) - remainingAtStart;
					UpdateCounts();
					phase = Phase::kNext;
				}
				return;
			case Phase::kNext:
				{
					while (cur < shots.size() && shots[cur].done)
						++cur;
					if (cur >= shots.size()) {
						Submit(FlushJob{});
						phase = Phase::kFinishing;
						SetPhase("writing the last tiles");
						return;
					}
					const Shot& s = shots[cur];
					if (s.world != curWorld) {
						camMode = CamMode::kPlayer;
						waitLeft = kDrainWait;
						phase = Phase::kDrain;
						SetPhase("waiting before changing worldspace to " + s.world);
					} else if (havePrev && std::hypot(s.x - prevX, s.y - prevY) / kCellUnits > kJumpCells) {
						// Long jumps under the high camera made the engine stream a whole new area plus far LOD at
						// once and crashed it: wait, move, let the new area load. The camera stays where it is.
						waitLeft = kJumpWait;
						phase = Phase::kJumpBefore;
						SetPhase("long jump: waiting");
					} else {
						StartShot(s);
					}
					return;
				}
			case Phase::kDrain:
				if (waited) {
					const Shot& s = shots[cur];
					auto*       ref = EntryMarker(s.world, s.x, s.y);
					auto*       player = RE::PlayerCharacter::GetSingleton();
					if (!ref || !player) {
						Stop("No map marker to enter " + s.world + ".");
						return;
					}
					// MoveTo, never coc/cow: a save-reload mod turns coc into a game restart.
					player->MoveTo(ref);
					loadElapsed = 0;
					waitLeft = kLoadMinWait;
					afterLoad = Phase::kWorldSettle;
					phase = Phase::kWaitLoad;
					SetPhase("loading " + s.world);
				}
				return;
			case Phase::kWaitLoad:
				if (waited && Loaded()) {
					if (afterLoad == Phase::kWorldSettle) {
						waitLeft = kWorldSettle;
						phase = Phase::kWorldSettle;
					} else {
						StartShot(shots[cur]);
					}
				}
				return;
			case Phase::kWorldSettle:
				if (waited) {
					const Shot& s = shots[cur];
					WorldSetup();
					MovePlayer(s);
					AimAt(s);
					curWorld = s.world;
					waitLeft = kFogWait;
					phase = Phase::kFogWait;
					SetPhase("fog test in " + s.world);
				}
				return;
			case Phase::kFogWait:
				if (waited && (CameraAtTarget(false) || (cameraWait += a_dt) > kCameraTimeout)) {
					cameraWait = 0.0f;
					BeginCapture(CapPurpose::kFogA);
				}
				return;
			case Phase::kFogToggleWait:
				if (waited)
					BeginCapture(CapPurpose::kFogB);
				return;
			case Phase::kFogDecide:
				{
					// tfog is a toggle and its state cannot be read: keep the state whose picture has more contrast.
					const auto ready = [](const std::shared_future<float>& f) { return !f.valid() || f.wait_for(0s) == std::future_status::ready; };
					if (!ready(fogA) || !ready(fogB))
						return;
					const float a = fogA.valid() ? fogA.get() : -1.0f, b = fogB.valid() ? fogB.get() : -1.0f;
					if (a > b) {
						Console("tfog");
						logger::info("bake: fog: kept the first state ({:.2f} vs {:.2f})", a, b);
					} else {
						logger::info("bake: fog: toggled ({:.2f} vs {:.2f})", b, a);
					}
					waitLeft = settle;
					phase = Phase::kSettle;
					return;
				}
			case Phase::kJumpBefore:
				if (waited) {
					MovePlayer(shots[cur]);
					waitLeft = kJumpWait;
					phase = Phase::kJumpAfter;
				}
				return;
			case Phase::kJumpAfter:
				if (waited) {
					loadElapsed = 0;
					waitLeft = 0;
					afterLoad = Phase::kSettle;
					phase = Phase::kWaitLoad;
				}
				return;
			case Phase::kSettle:
				if (waited) {
					// Never photograph a spot the camera has not reached: wait up to kCameraTimeout more, then skip
					// the shot (it is taken again on the next run) rather than write a wrong tile.
					if (CameraAtTarget(cameraWait == 0.0f)) {
						cameraWait = 0.0f;
						BeginCapture(CapPurpose::kShot);
					} else if ((cameraWait += a_dt) > kCameraTimeout) {
						const Shot& s = shots[cur];
						logger::warn("bake: camera never reached {} {},{}; skipped (taken again on the next run)", s.world, s.bx, s.by);
						cameraWait = 0.0f;
						++cur;
						phase = Phase::kNext;
					}
				}
				return;
			case Phase::kCapturing:
				return;  // handled in Tick (real time, not game time)
			case Phase::kFinishing:
				if (jobsOpen == 0)
					Stop(std::format("Town photo bake finished: {} photos this run.", capturedThisRun));
				return;
			}
		}

		void Tick()
		{
			tickQueued = false;
			if (!running)
				return;
			const auto now = Clock::now();
			const float realDt = std::min(std::chrono::duration<float>(now - lastTick).count(), 1.0f);
			lastTick = now;
			if (cancelRequested) {
				Stop("Town photo bake cancelled; pressing Start again continues where it stopped.");
				return;
			}
			auto* ui = RE::UI::GetSingleton();
			// Waits only run while the game runs: an open menu or a loading screen pauses them.
			const bool paused = !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			DriveCamera();

			if (phase == Phase::kWaitLoad && (loadElapsed += realDt) > kLoadTimeout) {
				Stop("The new area did not finish loading.");
				return;
			}
			if (phase == Phase::kCapturing) {
				std::shared_ptr<Photo> photo;
				bool                   failed = false, unsupported = false;
				{
					std::lock_guard lock(capMutex);
					photo = std::exchange(capResult, nullptr);
					failed = capFailed;
					unsupported = capUnsupported;
				}
				captureElapsed += realDt;
				if (!photo && !failed && captureElapsed < kCaptureTimeout)
					return;
				EndCaptureUi();
				if (unsupported) {
					Stop("This screen format cannot be read (see ModernMinimap.log).");
					return;
				}
				OnCaptured(std::move(photo));
				return;
			}
			Step(paused ? 0.0f : realDt);
		}

		void QueueTick()
		{
			if (tickQueued.exchange(true))
				return;
			SKSE::GetTaskInterface()->AddTask([] {
				// Never let an exception leave a game task: it would end in std::terminate (fast-fail, no crash log).
				try {
					Tick();
				} catch (const std::exception& e) {
					tickQueued = false;
					logger::error("bake step failed: {}", e.what());
				} catch (...) {
					tickQueued = false;
					logger::error("bake step failed (unknown exception)");
				}
			});
		}

		void Scan()
		{
			markers = CollectMarkers();
			auto       list = BuildShots(markers);
			const auto progress = LoadProgress();
			int        done = 0;
			for (const auto& s : list)
				done += progress.done.contains(ShotKey(s.world, s.bx, s.by)) ? 1 : 0;
			std::lock_guard lock(statusMutex);
			status.found = static_cast<int>(list.size());
			if (!running)
				status.done = done;
			logger::info("bake: {} map markers -> {} photos ({} done in earlier runs)", markers.size(), list.size(), done);
		}

		void Start()
		{
			if (running)
				return;
			if (jobsOpen > 0) {
				SetMessage("Still writing tiles from the last run; try again in a moment.");
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			if (!player || !player->GetParentCell() || !ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
				SetMessage("Load a save first.");
				return;
			}
			Scan();
			auto list = BuildShots(markers);
			if (list.empty()) {
				SetMessage("No settlement map markers found.");
				return;
			}
			const auto           cfg = Settings::Get();
			const int            count = static_cast<int>(list.size());
			auto                 progress = LoadProgress();
			ID3D11Device*        device = Device();
			// The worker creates the reference tile textures from its own thread; a single-threaded device forbids that.
			const bool refsOk = device && !(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED);
			Submit(BeginJob{ progress, refsOk ? device : nullptr, cfg->shade });
			settle = cfg->bake.settleSeconds;
			const bool keep = cfg->bake.keepExisting;
			const int  w = screenW, h = screenH;
			fov = w > 0 && h > 0 ? BakeImage::BakeFovDeg(w, h) : BakeImage::kBaseFovDeg;

			prepDone = false;
			std::thread([list = std::move(list), done = progress.done, keep]() mutable {
				try {
					GroundReader ground;
					for (auto& s : list) {
						s.done = done.contains(ShotKey(s.world, s.bx, s.by));
						if (keep && !s.done) {
							std::erase_if(s.cells, [](const auto& c) {
								std::error_code ec;
								return fs::exists(TilePath(c.first, c.second), ec);
							});
							s.done = s.cells.empty();
						}
						if (s.done)
							continue;
						float sum = 0;
						int   n = 0;
						for (const float dx : { -kCellUnits / 2, kCellUnits / 2 })
							for (const float dy : { -kCellUnits / 2, kCellUnits / 2 })
								if (const auto z = ground.At(s.x + dx, s.y + dy)) {
									sum += *z;
									++n;
								}
						s.ground = n ? sum / static_cast<float>(n) : 0.0f;
					}
				} catch (...) {
					logger::error("bake: ground height pass failed");
				}
				std::lock_guard lock(prepMutex);
				prepShots = std::move(list);
				prepDone = true;
			}).detach();

			Hooks::InstallBakePresent();
			curWorld.clear();
			havePrev = false;
			camMode = CamMode::kNone;
			capturedThisRun = 0;
			cancelRequested = false;
			lastTick = Clock::now();
			phase = Phase::kPreparing;
			running = true;
			SetPhase("reading ground heights");
			SetMessage(std::format("Started: {} photos, FOV {:.2f}, settle {:.0f} s{}. Close this menu.", count, fov, settle,
				actorsHidden ? ", NPCs hidden" : ""));
			QueueTick();  // the first step; the Present hook queues the rest
		}
	}

	void InstallActorHooksIfRequested()
	{
		if (!Settings::BakeHideActorsRequested())
			return;
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
		CharacterLoad::func = vtbl.write_vfunc(0x06, CharacterLoad::thunk);
		CharacterLoad3D::func = vtbl.write_vfunc(0x6A, CharacterLoad3D::thunk);
		actorsHidden = true;
		logger::info("bake: NPCs hidden this session (Character::Load and Load3D hooked)");
	}

	bool ActorsHidden() { return actorsHidden; }

	void RequestScan()
	{
		SKSE::GetTaskInterface()->AddTask([] {
			try {
				Scan();
			} catch (...) {
				logger::error("bake: scan failed");
			}
		});
	}

	void StartOnLoadIfRequested()
	{
		if (!Settings::BakeStartOnLoadRequested())
			return;
		logger::info("bake: bStartOnLoad set; starting once a game is loaded");
		// Poll from a helper thread, but do every game check on the main thread through a task.
		std::thread([] {
			auto ready = std::make_shared<std::atomic<int>>(0);
			constexpr int kNeeded = 15;  // 15 checks 2 s apart: let the area, LOD and start-up pop-ups settle first
			while (ready->load() < kNeeded) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				SKSE::GetTaskInterface()->AddTask([ready] {
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto* ui = RE::UI::GetSingleton();
					const bool ok = player && player->Is3DLoaded() && ui && !ui->GameIsPaused() &&
					                !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
					if (ok)
						ready->fetch_add(1);
					else
						ready->store(0);
				});
			}
			logger::info("bake: game loaded, starting from bStartOnLoad");
			RequestStart();
		}).detach();
	}

	void RequestStart()
	{
		SKSE::GetTaskInterface()->AddTask([] {
			try {
				Start();
			} catch (const std::exception& e) {
				logger::error("bake: start failed: {}", e.what());
			} catch (...) {
				logger::error("bake: start failed");
			}
		});
	}

	void RequestCancel() { cancelRequested = true; }

	void RequestReset()
	{
		if (running || jobsOpen > 0)
			return;
		std::error_code ec;
		fs::remove(kProgressPath, ec);
		{
			std::lock_guard lock(statusMutex);
			status.done = 0;
			status.tone.clear();
		}
		SetMessage("Progress forgotten: the next Start bakes every photo again.");
	}

	void OnPresent(IDXGISwapChain* a_swapChain)
	{
		DXGI_SWAP_CHAIN_DESC d{};
		if (SUCCEEDED(a_swapChain->GetDesc(&d))) {
			screenW = static_cast<int>(d.BufferDesc.Width);
			screenH = static_cast<int>(d.BufferDesc.Height);
		}
		auto* dev = Device();
		auto* ctx = Context();
		if (dev && ctx) {
			CaptureStep(a_swapChain, dev, ctx);
			ReferenceStep(dev, ctx);
		}
		if (running)
			QueueTick();
	}

	Status GetStatus()
	{
		std::lock_guard lock(statusMutex);
		Status s = status;
		s.running = running;
		s.writing = jobsOpen > 0;
		return s;
	}
}
