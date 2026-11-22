#include "PCH.h"

#include "PhotoBake.h"

#include "BakePlan.h"
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
		constexpr float kJumpCells = 3.0f;      // a move longer than this many cells counts as a long jump (2x2 grid; scales with the grid)
		constexpr float kJumpWait = 12.0f;      // before a long jump: let the high camera's LOD queue drain
		constexpr float kCameraTimeout = 20.0f; // most extra seconds to wait for the camera to reach a shot before skipping it
		constexpr float kDrainWait = 12.0f;     // before changing worldspace (BGSQueuedTerrainInitialLoad crash)
		constexpr float kLoadMinWait = 6.0f;    // after MoveTo, before checking that the new area is loaded
		constexpr float kWorldSettle = 15.0f;   // after entering a worldspace
		constexpr float kFogWait = 8.0f;        // camera up, before the first fog test picture
		constexpr float kFogToggleWait = 2.0f;  // after tfog, before the second picture
		constexpr float kLoadTimeout = 240.0f;  // unpaused seconds before a spot that never loads is skipped
		constexpr float kCaptureTimeout = 15.0f;
		constexpr float kFlatContrast = 15.0f;  // centre luma deviation below this: a flat, fogged-out photo (normal ones are 40+)
		constexpr float kPlayerAboveGround = 300.0f;
		constexpr int   kHudFrames = 3;         // frames between hiding the HUD and copying the picture
		constexpr int   kFitShots = 30;         // photos sampled for the colour match
		constexpr float kCityMinRadius = 9000.0f;

		constexpr auto kTileDir = "Data/textures/ModernMinimap/tamriel";
		constexpr auto kProgressPath = "Data/SKSE/Plugins/ModernMinimapBake.txt";
		// Read by the Modern Minimap Photo Baker app, which starts the game with bStartOnLoad and watches the bake.
		constexpr auto kStatusPath = "Data/SKSE/Plugins/ModernMinimapBakeStatus.json";
		constexpr auto kCancelPath = "Data/SKSE/Plugins/ModernMinimapBake.cancel";  // created by the app's Cancel button
		constexpr auto kMenuCell = "WhiterunOrigin";  // bEnterFromMainMenu: coc target, next to the first shots
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
			int                              bx{ 0 }, by{ 0 };  // south-west cell of the block (photo grid x grid cells)
			float                            x{ 0 }, y{ 0 };    // block centre
			float                            ground{ 0 };
			std::vector<std::pair<int, int>> cells;             // cells this photo writes
			bool                             done{ false };
		};

		using BakePlan::ShotKey;

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

		std::vector<Shot> BuildShots(const std::vector<Marker>& a_markers, int a_grid)
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
                s.x = BakePlan::BlockCentre(a_bx, a_grid);
                s.y = BakePlan::BlockCentre(a_by, a_grid);
                for (int dy = 0; dy < a_grid; ++dy)
                    for (int dx = 0; dx < a_grid; ++dx)
                        if (a_world != kRootWorld || !cityCells.contains({ a_bx + dx, a_by + dy }))
                            s.cells.push_back({ a_bx + dx, a_by + dy });
                if (!s.cells.empty())
                    shots.push_back(std::move(s));
			};
			for (const auto& [world, cells] : city) {
				for (const auto& [bx, by] : BakePlan::Blocks(cells, a_grid)) {
					add(world, bx, by);
					for (int dy = 0; dy < a_grid; ++dy)
						for (int dx = 0; dx < a_grid; ++dx)
							cityCells.insert({ bx + dx, by + dy });
				}
			}
			for (const auto& [bx, by] : BakePlan::Blocks(tam, a_grid))
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

		using Progress = BakePlan::Progress;

		Progress LoadProgress(int a_grid)
		{
			std::ifstream in(kProgressPath);
			return BakePlan::ParseProgress(in, a_grid);
		}

		std::string GridName(int a_grid) { return std::format("{}x{}", a_grid, a_grid); }

		// Said when earlier progress was for another photo grid and is not used.
		std::string DroppedGridNote(const Progress& a_p)
		{
			if (a_p.droppedGrid <= 0)
				return {};
			return std::format("Earlier progress was for the {} photo grid, not {}: those photos are baked again.", GridName(a_p.droppedGrid), GridName(a_p.grid));
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
				BakePlan::WriteProgress(out, a_p);
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
			std::vector<TileDraw>                         tiles;
			std::vector<ComPtr<ID3D11ShaderResourceView>> keep;  // holds the tiles' views until the render is done
			Vec2                                          centre;
			int                                           size{ 0 };  // render side, px (BakeImage::ReferenceSize)
			int                                           grid{ BakePlan::kDefaultGrid };
			ShadeParams                             shade;
			std::promise<std::vector<std::uint8_t>> result;
		};
		std::mutex                  refMutex;
		std::shared_ptr<RefRequest> refQueued;   // waiting for the render thread
		std::shared_ptr<RefRequest> refInFlight;  // render thread only
		ComPtr<ID3D11Texture2D>     refStaging;   // render thread only
		int                         refStagingSize = 0;  // render thread only
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
					const int size = refInFlight->size;
					rgba.resize(static_cast<std::size_t>(size) * size * 4);
					for (int y = 0; y < size; ++y)
						std::memcpy(rgba.data() + static_cast<std::size_t>(y) * size * 4, static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(m.RowPitch) * y,
							static_cast<std::size_t>(size) * 4);
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
			if (refStaging && refStagingSize != req->size)
				refStaging.Reset();  // another photo grid than the last bake this session
			if (!refStaging && !refRendererFailed) {
				D3D11_TEXTURE2D_DESC s{};
				s.Width = s.Height = static_cast<UINT>(req->size);
				s.MipLevels = s.ArraySize = 1;
				s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // the compose target's format
				s.SampleDesc.Count = 1;
				s.Usage = D3D11_USAGE_STAGING;
				s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(a_dev->CreateTexture2D(&s, nullptr, &refStaging)))
					refRendererFailed = true;
				else
					refStagingSize = req->size;
			}
			if (refRendererFailed) {
				req->result.set_value({});
				return;
			}
			refState.Backup(a_ctx);
			refRenderer->Compose(a_ctx, { req->centre, req->grid * kCellUnits / static_cast<float>(req->size), req->size, req->shade }, req->tiles);
			const bool ok = refRenderer->ComposeTexture() && refRenderer->ComposeSize() == req->size;
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
			int           grid{ BakePlan::kDefaultGrid };
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
			int                              grid{ BakePlan::kDefaultGrid };
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

		// Only for a bake started through bStartOnLoad (the Photo Baker app or a hand-edited ini): a bake started
		// from the settings page writes no status file and never quits the game.
		std::atomic<bool> statusFileOn{ false };
		std::atomic<bool> unattended{ false };    // bEnterFromMainMenu or bQuitWhenDone: nobody is at the keyboard
		std::atomic<bool> quitWhenDone{ false };  // bQuitWhenDone, read once at kDataLoaded

		void WriteStatusFile();  // main thread

		void SetMessage(std::string a_msg)
		{
			logger::info("bake: {}", a_msg);
			std::lock_guard lock(statusMutex);
			status.message = std::move(a_msg);
		}

		void SetPhase(std::string a_phase)
		{
			{
				std::lock_guard lock(statusMutex);
				status.phase = std::move(a_phase);
			}
			WriteStatusFile();
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
		int                                                                        wGrid = BakePlan::kDefaultGrid;
		std::vector<PendingShot>                                                   wPending;
		std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> wSamples;
		int                                                                        wSampledShots = 0;

		std::optional<std::vector<std::uint8_t>> RenderReference(int a_bx, int a_by)
		{
			if (!wDevice)
				return std::nullopt;
			auto req = std::make_shared<RefRequest>();
			for (const TileId& id : BakePlan::ReferenceTiles(a_bx, a_by, wGrid)) {
				std::string log;
				Tile        tile = LoadTile(wDevice, Minimap::ReadGameFile, kRootWorld, id, 512, &log);
				if (tile.absent || !tile.diffuse)
					return std::nullopt;
				req->tiles.push_back({ id, tile.diffuse.Get(), tile.normal.Get(), tile.water.Get() });
				req->keep.insert(req->keep.end(), { tile.diffuse, tile.normal, tile.water });
			}
			req->centre = { BakePlan::BlockCentre(a_bx, wGrid), BakePlan::BlockCentre(a_by, wGrid) };
			req->size = BakeImage::ReferenceSize(wGrid);
			req->grid = wGrid;
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
			for (const auto& [cell, rgb] : a_shot.tiles) {
				const int dx = cell.first - a_bx, dy = cell.second - a_by;
				wSamples.emplace_back(BakeImage::BoxDown(rgb, BakeImage::kTileSize, BakeImage::kSampleSize), BakeImage::ReferenceCell(*ref, wGrid, dx, dy));
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
				wGrid = b->grid;
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
					shot.tiles.push_back({ cell, BakeImage::CropCell(img, s->fov, s->grid, cell.first - s->bx, cell.second - s->by) });
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
			kSettle,
			kShotCheck,
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
		std::shared_ptr<Photo>    checkPhoto;     // kShotCheck: the photo waiting for its contrast check
		std::shared_future<float> checkContrast;
		bool                      flatRetried = false;  // this shot already got a new fog test after a flat photo
		CapPurpose        capPurpose = CapPurpose::kShot;
		CamMode           camMode = CamMode::kNone;
		float             cameraWait = 0.0f;  // extra seconds spent waiting for the camera to reach the shot
		RE::NiPoint3      camTarget;
		float             waitLeft = 0;
		float             loadElapsed = 0;
		bool              reentering = false;  // a spot did not load: entering again at a map marker, no photo since
		float             captureElapsed = 0;
		bool              hudWasShown = true;
		std::vector<Shot> shots;
		std::vector<Marker> markers;
		std::size_t       cur = 0;
		std::string       curWorld;
		bool              havePrev = false;
		float             prevX = 0, prevY = 0;
		float             fov = BakeImage::kBaseFovDeg;
		int               grid = BakePlan::kDefaultGrid;  // photo grid of the running bake, taken at Start
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

		// ---------------------------------------------------------------- Photo Baker app: status, cancel, quit

		std::string JsonText(std::string_view a_s)
		{
			std::string out = "\"";
			for (const char c : a_s) {
				if (c == '"' || c == '\\') {
					out += '\\';
					out += c;
				} else if (static_cast<unsigned char>(c) < 0x20) {
					out += std::format("\\u{:04x}", static_cast<int>(c));
				} else {
					out += c;
				}
			}
			return out + "\"";
		}

		void WriteStatusFile()
		{
			if (!statusFileOn)
				return;
			const Status s = GetStatus();
			std::string  state = s.state;
			if (state.empty())
				state = (phase == Phase::kFinishing || (!s.running && s.writing)) ? "writing" : "running";
			const auto text = std::format("{{\"state\": {}, \"found\": {}, \"done\": {}, \"etaSeconds\": {:.0f}, \"phase\": {}, \"message\": {}, \"pid\": {}}}\n",
				JsonText(state), s.found, s.done, s.etaSeconds, JsonText(s.phase), JsonText(s.message), GetCurrentProcessId());
			std::error_code ec;
			fs::create_directories(fs::path(kStatusPath).parent_path(), ec);
			// Temp file and rename: the app polls this file and must never read half of it.
			const auto tmp = std::string(kStatusPath) + ".tmp";
			{
				std::ofstream out(tmp, std::ios::trunc);
				if (!out)
					return;
				out << text;
			}
			fs::rename(tmp, kStatusPath, ec);
			if (ec)
				logger::warn("bake: could not write the status file: {}", ec.message());
		}

		bool TakeCancelFlag()
		{
			std::error_code ec;
			if (!fs::exists(kCancelPath, ec))
				return false;
			fs::remove(kCancelPath, ec);
			logger::info("bake: cancel requested by the Photo Baker app");
			return true;
		}

		// bQuitWhenDone: leaving the game running after a bake ends in a crash sooner or later (the free camera
		// cannot be switched off safely), so quit the way the main menu's Quit button does: the main loop sees
		// Main::quitGame and shuts the game down normally. If that has not happened after 90 s (a stuck shutdown),
		// end the process: by then every tile and the progress file are already on disk.
		void QuitGame()
		{
			logger::info("bake: bQuitWhenDone set, quitting the game");
			if (auto* main = RE::Main::GetSingleton())
				main->GetRuntimeData().quitGame = true;
			std::thread([] {
				std::this_thread::sleep_for(90s);
				logger::warn("bake: the game did not quit within 90 s; ending it");
				spdlog::default_logger()->flush();
				TerminateProcess(GetCurrentProcess(), 0);
			}).detach();
		}

		// Unattended runs only: a pop-up message box (a mod's first-start question, survival mode and so on)
		// pauses the game, and every bake wait with it. After it has stayed open for 5 s, answer it the way a
		// player who wants to be left alone would: a "No / Later / Cancel"-type button if there is one, else
		// the last button (usually the declining one).
		void AnswerStuckPopup()
		{
			static RE::MessageBoxData* seen = nullptr;
			static Clock::time_point   since{};
			auto*                      ui = RE::UI::GetSingleton();
			if (!ui || !ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
				seen = nullptr;
				return;
			}
			auto* data = RE::MessageBoxMenu::GetCurrentMessageBoxData();
			if (!data || data->buttonText.empty())
				return;
			if (data != seen) {
				seen = data;
				since = Clock::now();
				return;
			}
			if (Clock::now() - since < 5s)
				return;
			const auto lower = [](std::string a_s) {
				for (auto& c : a_s)
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				return a_s;
			};
			constexpr std::string_view kDecline[]{ "no", "later", "not now", "maybe later", "cancel", "skip", "close", "ok" };
			const auto                 count = static_cast<std::int32_t>(data->buttonText.size());
			std::int32_t               pick = count - 1;
			for (std::int32_t i = 0; i < count; ++i) {
				const auto* t = data->buttonText[static_cast<std::uint32_t>(i)].c_str();
				if (t && std::ranges::find(kDecline, lower(t)) != std::end(kDecline)) {
					pick = i;
					break;
				}
			}
			const auto* body = data->bodyText.c_str();
			const auto* button = data->buttonText[static_cast<std::uint32_t>(pick)].c_str();
			logger::info("bake: answered a pop-up with '{}': {}", button ? button : "", body ? body : "");
			seen = nullptr;
			RE::MessageBoxMenu::SelectOption(pick);
		}

		// The final status ("done", "cancelled", "error") only once every queued tile is on disk: the app moves
		// the tiles away when it reads it. Then quit if asked.
		void FinishAfterWrites(const char* a_state)
		{
			if (!statusFileOn)
				return;
			std::thread([a_state] {
				for (int i = 0; i < 1200 && jobsOpen > 0; ++i)  // at most 5 minutes
					std::this_thread::sleep_for(250ms);
				SKSE::GetTaskInterface()->AddTask([a_state] {
					{
						std::lock_guard lock(statusMutex);
						status.state = a_state;
					}
					WriteStatusFile();
					if (quitWhenDone)
						QuitGame();
				});
			}).detach();
		}

		// A bake started through bStartOnLoad that could not start at all: switch the ini keys off (or the
		// app would start the game again and again) and tell the app.
		void FailUnattended(const std::string& a_msg, const char* a_state = "error")
		{
			SetMessage(a_msg);
			if (!statusFileOn)
				return;
			Settings::ClearBakeStartOnLoad();
			if (Settings::BakeHideActorsRequested())
				Settings::SetBakeHideActors(false);
			{
				std::lock_guard lock(statusMutex);
				status.state = a_state;
			}
			SetPhase("stopped");
			FinishAfterWrites(a_state);
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
			camTarget = { a_s.x, a_s.y, a_s.ground + BakeImage::CameraHeight(grid) };
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
			WriteStatusFile();  // after every photo
		}

		// a_state: what the status file says once the last tiles are written.
		void Stop(const std::string& a_msg, const char* a_state = "error")
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
			// With bQuitWhenDone (the Photo Baker app) the game quits by itself: no advice about the camera or NPCs.
			std::string msg = a_msg;
			if (!quitWhenDone) {
				msg += " The camera stays in free mode: load a save now.";
				if (actorsHidden)
					msg += " Restart the game to bring NPCs back.";
			}
			SetMessage(msg);
			{
				std::lock_guard lock(statusMutex);
				status.state = statusFileOn ? "writing" : "";
			}
			SetPhase("stopped");
			FinishAfterWrites(a_state);
			if (!quitWhenDone)  // the game quits by itself in a moment: no message box to answer
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

		void AcceptShot(std::shared_ptr<Photo> a_photo)
		{
			const Shot& s = shots[cur];
			Submit(ShotJob{ ShotKey(s.world, s.bx, s.by), s.bx, s.by, s.cells, fov, grid, std::move(a_photo) });
			if (capturedThisRun++ == 0)
				firstShotTime = Clock::now();
			shots[cur].done = true;
			reentering = false;
			flatRetried = false;
			UpdateCounts();
			++cur;
			phase = Phase::kNext;
		}

		// A nearly flat photo (one colour): the fog came back on, or something covers the spot. Test the fog again
		// and take the photo once more; if it is still flat, save nothing for it.
		void CheckShot()
		{
			if (checkContrast.valid() && checkContrast.wait_for(0s) != std::future_status::ready)
				return;
			const float contrast = checkContrast.valid() ? checkContrast.get() : -1.0f;
			checkContrast = {};
			if (contrast >= 0.0f && contrast < kFlatContrast && flatRetried) {
				// Flat again after a new fog test: something covers this spot. No tile is better than a grey one:
				// the minimap shows the player's terrain there.
				const Shot& s = shots[cur];
				logger::warn("bake: photo at {} {},{} still flat (contrast {:.1f}); not saved, your terrain shows there", s.world, s.bx, s.by, contrast);
				checkPhoto.reset();
				flatRetried = false;
				++cur;
				phase = Phase::kNext;
				return;
			}
			if (contrast >= 0.0f && contrast < kFlatContrast) {
				const Shot& s = shots[cur];
				logger::warn("bake: flat photo at {} {},{} (contrast {:.1f}); testing the fog again", s.world, s.bx, s.by, contrast);
				checkPhoto.reset();
				flatRetried = true;
				waitLeft = 0.0f;
				phase = Phase::kFogWait;  // fog test, then settle and take the photo again
				SetPhase("fog test again in " + s.world);
				return;
			}
			AcceptShot(std::move(checkPhoto));
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
				if (!a_photo) {
					logger::warn("bake: capture failed: {} {},{} (taken again on the next run)", s.world, s.bx, s.by);
					++cur;
					phase = Phase::kNext;
					break;
				}
				checkContrast = SubmitContrast(a_photo);
				checkPhoto = std::move(a_photo);
				phase = Phase::kShotCheck;
				break;
			}
		}

		// A spot that never finished loading: skip its photo and enter again at the map marker nearest the next one
		// (MoveTo with a loading screen). Stop only when that fails too.
		void OnLoadTimeout()
		{
			if (reentering) {
				Stop("The new area did not finish loading, even after moving to a map marker.");
				return;
			}
			reentering = true;
			if (cur < shots.size()) {
				const Shot& s = shots[cur];
				logger::warn("bake: area at {} {},{} did not finish loading; photo skipped (tried again on the next run)", s.world, s.bx, s.by);
				++cur;
			}
			curWorld.clear();  // the next photo enters its worldspace again through a map marker
			havePrev = false;
			phase = Phase::kNext;
			SetPhase("area did not load: moving to the next place");
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
					} else if (havePrev && std::hypot(s.x - prevX, s.y - prevY) / kCellUnits > kJumpCells * static_cast<float>(grid) / 2.0f) {
						// Long jumps go through the map marker nearest the photo (MoveTo, loading screen), like a worldspace
						// change. Moving the player there with SetPosition left the player's cell detached: the area never loaded.
						// Under the high camera such a jump also streamed a whole new area plus far LOD at once and crashed.
						camMode = CamMode::kPlayer;
						waitLeft = kJumpWait;
						phase = Phase::kDrain;
						SetPhase("long jump: moving to a map marker");
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
					phase = Phase::kWaitLoad;
					SetPhase("loading " + s.world);
				}
				return;
			case Phase::kWaitLoad:
				if (waited && Loaded()) {
					waitLeft = kWorldSettle;
					phase = Phase::kWorldSettle;
				}
				return;
			case Phase::kWorldSettle:
				if (waited) {
					const Shot& s = shots[cur];
					// Fog test after every loading screen, long jumps included: a loading screen can turn the fog back on
					// (whole runs of flat dark grey photos near Falkreath when the test was skipped).
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
			case Phase::kShotCheck:
				CheckShot();
				return;
			case Phase::kFinishing:
				if (jobsOpen == 0)
					Stop(std::format("Town photo bake finished: {} photos this run.", capturedThisRun), "done");
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
			if (statusFileOn) {
				static Clock::time_point lastFlagCheck{};
				if (now - lastFlagCheck > 1s) {  // a file check every frame would be wasted work
					lastFlagCheck = now;
					if (TakeCancelFlag())
						cancelRequested = true;
				}
			}
			if (cancelRequested) {
				Stop("Town photo bake cancelled; pressing Start again continues where it stopped.", "cancelled");
				return;
			}
			if (unattended)
				AnswerStuckPopup();
			auto* ui = RE::UI::GetSingleton();
			// Waits only run while the game runs: an open menu or a loading screen pauses them.
			const bool paused = !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			DriveCamera();

			// An open menu pauses the game and cell loading with it: that time does not count. A loading screen does.
			const bool menuPaused = ui && ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			if (phase == Phase::kWaitLoad && !menuPaused && (loadElapsed += realDt) > kLoadTimeout) {
				OnLoadTimeout();
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

		// The photo grid a scan or start uses: the running bake's, otherwise the current setting.
		int ActiveGrid() { return running ? grid : BakePlan::ClampGrid(Settings::Get()->bake.photoGrid); }

		void Scan()
		{
			markers = CollectMarkers();
			const int  scanGrid = ActiveGrid();
			auto       list = BuildShots(markers, scanGrid);
			const auto progress = LoadProgress(scanGrid);
			int        done = 0;
			for (const auto& s : list)
				done += progress.done.contains(ShotKey(s.world, s.bx, s.by)) ? 1 : 0;
			logger::info("bake: {} map markers -> {} photos with the {} grid ({} done in earlier runs)", markers.size(), list.size(), GridName(scanGrid), done);
			if (const auto note = DroppedGridNote(progress); !note.empty() && !running)
				SetMessage(note);
			std::lock_guard lock(statusMutex);
			status.found = static_cast<int>(list.size());
			if (!running)
				status.done = done;
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
				FailUnattended("Load a save first.");
				return;
			}
			Scan();
			const auto cfg = Settings::Get();
			grid = BakePlan::ClampGrid(cfg->bake.photoGrid);
			auto list = BuildShots(markers, grid);
			if (list.empty()) {
				FailUnattended("No settlement map markers found.");
				return;
			}
			{
				std::lock_guard lock(statusMutex);
				status.state.clear();  // running; also clears a final state from an earlier bake this session
			}
			const int            count = static_cast<int>(list.size());
			auto                 progress = LoadProgress(grid);
			const std::string    gridNote = DroppedGridNote(progress);
			ID3D11Device*        device = Device();
			// The worker creates the reference tile textures from its own thread; a single-threaded device forbids that.
			const bool refsOk = device && !(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED);
			Submit(BeginJob{ progress, refsOk ? device : nullptr, cfg->shade, grid });
			settle = cfg->bake.settleSeconds;
			const bool keep = cfg->bake.keepExisting;
			const int  w = screenW, h = screenH;
			fov = w > 0 && h > 0 ? BakeImage::BakeFovDeg(w, h) : BakeImage::kBaseFovDeg;

			prepDone = false;
			std::thread([list = std::move(list), done = progress.done, keep, runGrid = grid]() mutable {
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
						// Every cell centre of the block.
						for (int dx = 0; dx < runGrid; ++dx)
							for (int dy = 0; dy < runGrid; ++dy)
								if (const auto z = ground.At((static_cast<float>(s.bx + dx) + 0.5f) * kCellUnits, (static_cast<float>(s.by + dy) + 0.5f) * kCellUnits)) {
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
			reentering = false;
			cancelRequested = false;
			lastTick = Clock::now();
			phase = Phase::kPreparing;
			running = true;
			SetPhase("reading ground heights");
			SetMessage(std::format("{}{}Started: {} photos ({} grid), FOV {:.2f}, settle {:.0f} s{}. Close this menu.", gridNote, gridNote.empty() ? "" : " ", count,
				GridName(grid), fov, settle, actorsHidden ? ", NPCs hidden" : ""));
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
		const bool fromMenu = Settings::BakeEnterFromMainMenuRequested();
		quitWhenDone = Settings::BakeQuitWhenDoneRequested();
		unattended = fromMenu || quitWhenDone;
		statusFileOn = true;
		logger::info("bake: bStartOnLoad set; starting once a game is loaded (enter from main menu {}, quit when done {})", fromMenu, quitWhenDone.load());
		{
			std::lock_guard lock(statusMutex);
			status.state = "starting";
		}
		SetPhase(fromMenu ? "waiting for the main menu" : "waiting for a save to be loaded");

		// Poll from a helper thread, but do every game check on the main thread through a task.
		struct Wait
		{
			std::atomic<int>  ready{ 0 };
			std::atomic<int>  menuIdle{ 0 };
			std::atomic<bool> entered{ false };
			std::atomic<bool> cancelled{ false };
		};
		std::thread([fromMenu] {
			auto          w = std::make_shared<Wait>();
			constexpr int kNeeded = 30;   // 30 checks 1 s apart: let the area, LOD and start-up pop-ups settle first
			constexpr int kMenuIdle = 5;  // main menu open and quiet this many checks in a row before the coc
			while (w->ready.load() < kNeeded && !w->cancelled) {
				std::this_thread::sleep_for(1s);
				SKSE::GetTaskInterface()->AddTask([w, fromMenu] {
					if (w->cancelled || w->ready.load() >= kNeeded)
						return;
					if (TakeCancelFlag()) {
						w->cancelled = true;
						return;
					}
					if (unattended)
						AnswerStuckPopup();
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto* ui = RE::UI::GetSingleton();
					// bEnterFromMainMenu: coc is only safe from the main menu. In game a save-reload mod turns it into
					// a game restart, which is why the bake itself moves with MoveTo. Wait until the menu has
					// finished fading in and nothing else (a pop-up, a loading screen) is open.
					if (fromMenu && !w->entered && ui) {
						const bool idle = ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) &&
						                  !ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME);
						if (!idle) {
							w->menuIdle = 0;
						} else if (++w->menuIdle >= kMenuIdle) {
							w->entered = true;
							SetPhase(std::format("entering the world from the main menu (coc {})", kMenuCell));
							Console(std::format("coc {}", kMenuCell));
						}
					}
					const bool ok = player && player->Is3DLoaded() && ui && !ui->GameIsPaused() &&
					                !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
					if (ok) {
						if (w->ready.fetch_add(1) == 0)
							SetPhase("in the world, waiting for it to settle");
					} else {
						w->ready.store(0);
					}
				});
			}
			if (w->cancelled) {
				SKSE::GetTaskInterface()->AddTask([] { FailUnattended("Town photo bake cancelled before it started.", "cancelled"); });
				return;
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
