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
		constexpr float kPlayerAboveGround = 300.0f;
		constexpr int   kHudFrames = 3;         // frames between hiding the HUD and copying the picture
		constexpr int   kFitShots = 30;         // photos sampled for the colour match
		constexpr float kCityMinRadius = 9000.0f;
		constexpr float kLookInterval = 0.5f;   // adaptive settle: seconds between two looks at the screen
		constexpr int   kLookSize = 64;         // adaptive settle: side of the luma thumbnail compared between looks
		constexpr int   kSettleLogEvery = 25;   // photos between two average-settle log lines
		constexpr int   kHighlightLogEvery = 25;  // photos between two clipped-white log lines
		constexpr auto  kRestartState = "restart";  // status file: a planned restart, not a crash and not the end

		constexpr auto kTileDir = "Data/textures/ModernMinimap/tamriel";
		// [Bake] bSaveMasters: lossless copies of the tiles, outside textures so the game never loads them.
		constexpr auto kMasterDir = "Data/ModernMinimapMasters/tamriel";
		// The tone the BC1 tiles got; the masters are untoned and the Photo Baker app applies it to them.
		constexpr auto kToneFile = "tone.txt";
		constexpr auto kProgressPath = "Data/SKSE/Plugins/ModernMinimapBake.txt";
		// Cells still without a photo after the retry pass, one key per line (read by the Photo Baker app).
		constexpr auto kMissingPath = "Data/SKSE/Plugins/ModernMinimapMissing.txt";
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

		// The playable map area: the point lists of the worldspace's border regions (REGN with the border flag, the
		// invisible wall; vanilla Tamriel has one, BorderRegionSkyrim). The world map data (MNAM) is no help here: in
		// Skyrim.esm it says usable 0 x 0 with NW -30,15 and SE 40,-40, nothing like the real border.
		std::vector<BakePlan::Polygon> BorderPolygons(const RE::TESWorldSpace* a_world)
		{
			std::vector<BakePlan::Polygon> borders;
			auto*                          dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler)
				return borders;
			for (auto* region : dataHandler->GetFormArray<RE::TESRegion>()) {
				if (!region || region->worldSpace != a_world || !(region->formFlags & RE::TESRegion::RecordFlags::kBorderRegion) || !region->pointLists)
					continue;
				for (auto* list : *region->pointLists) {
					if (!list)
						continue;
					BakePlan::Polygon polygon;
					for (auto* point : *list)
						if (point)
							polygon.emplace_back(point->point.x, point->point.y);
					logger::info("bake: whole map: border region {:08X} {}: {} points, cells {:.1f},{:.1f} to {:.1f},{:.1f}", region->GetFormID(),
						region->GetFormEditorID(), polygon.size(), list->minimums.x / kCellUnits, list->minimums.y / kCellUnits, list->maximums.x / kCellUnits,
						list->maximums.y / kCellUnits);
					if (polygon.size() >= 3)
						borders.push_back(std::move(polygon));
				}
			}
			return borders;
		}

		// Main thread, once per session: every Tamriel cell with land, from the level-4 terrain LOD meshes inside the
		// worldspace's bounds (the same meshes the ground heights come from), clipped to the playable map area.
		// Tiles without a mesh have no land.
		const std::set<std::pair<int, int>>& FullMapCells()
		{
			static std::optional<std::set<std::pair<int, int>>> cached;
			if (cached)
				return *cached;
			cached.emplace();
			constexpr int kLevel = 4;
			auto*         world = RE::TESForm::LookupByEditorID<RE::TESWorldSpace>(kRootWorld);
			if (!world) {
				logger::warn("bake: whole map: no {} worldspace", kRootWorld);
				return *cached;
			}
			// The worldspace's object bounds (NAM0 / NAM9); without usable ones, a square well past vanilla Skyrim's edges.
			constexpr int kFallbackCells = 96;
			const bool    bounded = world->maximumCoords.x > world->minimumCoords.x && world->maximumCoords.y > world->minimumCoords.y;
			const int     minX = FloorDiv(bounded ? WorldToCell(world->minimumCoords.x) : -kFallbackCells, kLevel) * kLevel;
			const int     minY = FloorDiv(bounded ? WorldToCell(world->minimumCoords.y) : -kFallbackCells, kLevel) * kLevel;
			const int     maxX = bounded ? WorldToCell(world->maximumCoords.x) : kFallbackCells;
			const int     maxY = bounded ? WorldToCell(world->maximumCoords.y) : kFallbackCells;
			int       meshes = 0;
			for (int ty = minY; ty <= maxY; ty += kLevel) {
				for (int tx = minX; tx <= maxX; tx += kLevel) {
					const TileId              id{ kLevel, tx, ty };
					std::vector<std::uint8_t> data;
					BtrData                   btr;
					std::string               error;
					if (!Minimap::ReadGameFile(TileMeshPath(kRootWorld, id), data) || !ParseBtr(data, btr, error))
						continue;
					++meshes;
					cached->merge(BakePlan::LandCells(btr, id));
				}
			}
			logger::info("bake: whole map: cells {},{} to {},{}: {} terrain LOD meshes, {} cells with land", minX, minY, maxX, maxY, meshes, cached->size());
			const auto& mapData = world->worldMapData;
			logger::info("bake: whole map: world map data (MNAM) usable {}x{}, NW cell {},{}, SE cell {},{} (not used)", mapData.usableWidth, mapData.usableHeight,
				mapData.nwCellX, mapData.nwCellY, mapData.seCellX, mapData.seCellY);
			const auto borders = BorderPolygons(world);
			if (borders.empty()) {
				logger::warn("bake: whole map: no border region for {}; every cell with land inside the worldspace bounds is photographed", kRootWorld);
				return *cached;
			}
			const std::size_t landCells = cached->size();
			*cached = BakePlan::ClipToBorders(*cached, borders);
			logger::info("bake: whole map: {} of {} cells with land are inside the map border ({} border polygons)", cached->size(), landCells, borders.size());
			return *cached;
		}

		std::vector<Shot> BuildShots(const std::vector<Marker>& a_markers, int a_grid)
		{
			std::map<std::string, std::set<std::pair<int, int>>> city;
			std::set<std::pair<int, int>>                        tam;
			const auto  cfg = Settings::Get();
			const float scale = cfg->bake.areaScale;  // player option: photograph a wider area around each marker
			for (const auto& m : a_markers) {
				float r = RadiusFor(m.type) * scale;
				if (IsCityWorld(m.world)) {
					r = std::max(r, kCityMinRadius * scale);
					CellsNear(m.x, m.y, r, city[m.world]);
				} else if (m.world == kRootWorld && r > 0.0f && !cfg->bake.fullMap) {
					CellsNear(m.x, m.y, r, tam);
				}
			}
			if (cfg->bake.fullMap)
				tam = FullMapCells();
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
		fs::path MasterPath(int a_cx, int a_cy) { return fs::path(kMasterDir) / std::format("{}.{}.bmp", a_cx, a_cy); }

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

		// After every photo: one line added, not the whole file (10000 lines in a whole-map bake) rewritten.
		// SaveProgress rewrites it whole at the start, when the colour match is decided and at the end.
		void AppendProgress(const std::string& a_key)
		{
			std::ofstream out(kProgressPath, std::ios::app);
			if (!out) {
				logger::warn("bake: could not add {} to the progress file", a_key);
				return;
			}
			BakePlan::AppendDone(out, a_key);
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

		// The swap chain's back buffer size, read straight from the renderer (the same swap chain the hooks use).
		void ReadScreenSize()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			auto* swap = r ? reinterpret_cast<IDXGISwapChain*>(r->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
			DXGI_SWAP_CHAIN_DESC d{};
			if (!swap || FAILED(swap->GetDesc(&d)))
				return;
			screenW = static_cast<int>(d.BufferDesc.Width);
			screenH = static_cast<int>(d.BufferDesc.Height);
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
			int           tileSize{ BakeImage::kDefaultTileSize };
			bool          saveMasters{ false };
		};
		struct ContrastJob
		{
			std::shared_ptr<Photo>              photo;
			std::shared_ptr<std::promise<float>> result;
			// Shot photos only: the block, and the terrain LOD reference's contrast (set to -1 when not needed or not known).
			std::optional<std::pair<int, int>>   block;
			std::shared_ptr<std::promise<float>> reference;
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
		int                                                                        wTileSize = BakeImage::kDefaultTileSize;
		bool                                                                       wSaveMasters = false;
		bool                                                                       wBegun = false;  // a BeginJob ran and its progress is not yet saved whole at the end
		std::vector<PendingShot>                                                   wPending;
		std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> wSamples;
		int                                                                        wSampledShots = 0;
		double                                                                     wClippedSum = 0;   // clipped white share summed over this run's photos
		int                                                                        wClippedShots = 0;

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
				wSamples.emplace_back(BakeImage::BoxDown(rgb, wTileSize, BakeImage::kSampleSize), BakeImage::ReferenceCell(*ref, wGrid, dx, dy));
			}
		}

		// Contrast of the block's terrain LOD reference render; -1 without one.
		float ReferenceContrastOf(int a_bx, int a_by)
		{
			const auto ref = RenderReference(a_bx, a_by);
			return ref ? BakeImage::ReferenceContrast(*ref, BakeImage::ReferenceSize(wGrid)) : -1.0f;
		}

		// Written next to the target and renamed, so nothing (the minimap, the Photo Baker app) ever reads half a file.
		bool WriteFileWhole(const fs::path& a_path, std::span<const std::uint8_t> a_bytes)
		{
			const auto tmp = fs::path(a_path).concat(".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out.write(reinterpret_cast<const char*>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
				if (!out) {
					logger::error("bake: could not write {}", tmp.string());
					return false;
				}
			}
			std::error_code ec;
			fs::rename(tmp, a_path, ec);
			if (ec) {
				logger::error("bake: could not write {}: {}", a_path.string(), ec.message());
				return false;
			}
			return true;
		}

		void WriteShot(PendingShot& a_shot)
		{
			std::error_code ec;
			fs::create_directories(kTileDir, ec);
			if (wSaveMasters)
				fs::create_directories(kMasterDir, ec);
			for (auto& [cell, rgb] : a_shot.tiles) {
				// Masters untoned: the Photo Baker app applies tone.txt to them, as ApplyTone does here for the BC1 tile.
				if (wSaveMasters)
					WriteFileWhole(MasterPath(cell.first, cell.second), BakeImage::EncodeBmp24(rgb, wTileSize, wTileSize));
				BakeImage::ApplyTone(wProgress.tone, rgb);
				if (WriteFileWhole(TilePath(cell.first, cell.second), BakeImage::EncodeDdsBc1(rgb, wTileSize)))
					Minimap::ForgetBakedTile(cell.first, cell.second);
			}
			wProgress.done.insert(a_shot.key);
			AppendProgress(a_shot.key);
		}

		// Masters only: the tone the BC1 tiles got, whenever it is decided or taken over from an earlier run.
		void WriteToneFile(const BakeImage::Tone& a_tone)
		{
			if (!wSaveMasters)
				return;
			std::ostringstream text;
			BakePlan::WriteToneText(text, a_tone);
			const std::string bytes = text.str();
			std::error_code   ec;
			fs::create_directories(kMasterDir, ec);
			WriteFileWhole(fs::path(kMasterDir) / kToneFile, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
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
			SaveProgress(wProgress);  // whole: the tone lines change
			WriteToneFile(wProgress.tone);
			for (auto& p : wPending)
				WriteShot(p);
			wPending.clear();
		}

		void DumpPhoto(const BakeImage::RgbImage& a_img, const std::string& a_key);

		// Blown-out white in the raw photos (before the colour match): per photo at debug level, an average every few photos.
		void LogHighlights(const BakeImage::RgbImage& a_img, const std::string& a_key)
		{
			const float clipped = BakeImage::ClippedWhiteFraction(a_img);
			logger::debug("bake: photo {} clipped white {:.1f}%", a_key, clipped * 100.0f);
			wClippedSum += clipped;
			if (++wClippedShots % kHighlightLogEvery == 0)
				logger::info("bake: {} photos this run, clipped white {:.1f}% average", wClippedShots, wClippedSum / wClippedShots * 100.0);
		}

		void RunJob(Job& a_job)
		{
			if (auto* b = std::get_if<BeginJob>(&a_job)) {
				wProgress = std::move(b->progress);
				wDevice = b->device;
				wShade = b->shade;
				wGrid = b->grid;
				wTileSize = b->tileSize;
				wSaveMasters = b->saveMasters;
				wPending.clear();
				wSamples.clear();
				wSampledShots = 0;
				wClippedSum = 0;
				wClippedShots = 0;
				wBegun = true;
				SaveProgress(wProgress);  // whole, with the grid line, before any shot is appended
				if (wProgress.toneKnown)
					WriteToneFile(wProgress.tone);
				SetTone(wProgress.toneKnown ? (wProgress.tone.fitted ? "colour match from the earlier run" : "fixed tone curve (earlier run)") : "sampling the first photos");
			} else if (auto* c = std::get_if<ContrastJob>(&a_job)) {
				BakeImage::RgbImage img;
				float               v = -1.0f;
				if (BakeImage::ConvertPhoto(c->photo->bytes.data(), c->photo->w, c->photo->h, c->photo->pitch, c->photo->format, img))
					v = BakeImage::CentreContrast(img);
				c->result->set_value(v);
				if (c->reference)
					c->reference->set_value(c->block && BakeImage::NeedsReferenceContrast(v) ? ReferenceContrastOf(c->block->first, c->block->second) : -1.0f);
			} else if (auto* s = std::get_if<ShotJob>(&a_job)) {
				BakeImage::RgbImage img;
				if (!BakeImage::ConvertPhoto(s->photo->bytes.data(), s->photo->w, s->photo->h, s->photo->pitch, s->photo->format, img)) {
					logger::error("bake: could not read the photo for {}", s->key);
					return;
				}
				s->photo.reset();  // the raw copy is not needed any more
				LogHighlights(img, s->key);
				if (Settings::Get()->debugLog)
					DumpPhoto(img, s->key);
				PendingShot shot{ s->key, {} };
				for (const auto& cell : s->cells)
					shot.tiles.push_back({ cell, BakeImage::CropCell(img, s->fov, s->grid, cell.first - s->bx, cell.second - s->by, wTileSize) });
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
				if (std::exchange(wBegun, false))
					SaveProgress(wProgress);  // whole again: folds the appended lines in
			}
		}

		// Debug log on: keep a quarter-size BMP of every raw photo, to check camera placement against the tiles.
		void DumpPhoto(const BakeImage::RgbImage& a_img, const std::string& a_key)
		{
			const std::filesystem::path dir = "Data/SKSE/Plugins/ModernMinimapBakeDebug";
			std::error_code             ec;
			std::filesystem::create_directories(dir, ec);
			const int                 w = a_img.w / 4, h = a_img.h / 4;
			std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) {
					const float* c = &a_img.px[(static_cast<std::size_t>(y * 4) * a_img.w + x * 4) * 3];
					for (int k = 0; k < 3; ++k)
						rgb[(static_cast<std::size_t>(y) * w + x) * 3 + k] = static_cast<std::uint8_t>(std::clamp(c[k], 0.0f, 255.0f) + 0.5f);
				}
			const auto  out = BakeImage::EncodeBmp24(rgb, w, h);
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
					for (const auto& promise : { c->result, c->reference }) {
						try {
							if (promise)
								promise->set_value(-1.0f);  // throws if already set; only matters when the job failed early
						} catch (...) {
						}
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

		// A shot photo's contrast plus, when it is low, its block reference's contrast.
		struct ShotContrast
		{
			std::shared_future<float> photo;
			std::shared_future<float> reference;
		};
		ShotContrast SubmitShotContrast(std::shared_ptr<Photo> a_photo, int a_bx, int a_by)
		{
			auto photoPromise = std::make_shared<std::promise<float>>();
			auto referencePromise = std::make_shared<std::promise<float>>();
			ShotContrast futures{ photoPromise->get_future().share(), referencePromise->get_future().share() };
			Submit(ContrastJob{ std::move(a_photo), std::move(photoPromise), std::pair{ a_bx, a_by }, std::move(referencePromise) });
			return futures;
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
			kFogB,
			kSettleLook  // adaptive settle: a thumbnail to compare with the last one, no photo
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
		ShotContrast              checkContrast;
		bool                      flatRetried = false;  // this shot already got a new fog test after a flat photo
		bool                      detailChecked = false;  // the photo px per cell warning was decided at the first capture
		int                       runTileSize = BakeImage::kDefaultTileSize;  // tile side of the running bake, taken at Start
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

		// Adaptive settle.
		BakePlan::SettleRule settleRule;          // taken at Start
		float                settleElapsed = 0;   // unpaused seconds since the camera was sent to the shot
		float                lookWait = 0;        // seconds to the next look
		std::vector<float>   lastLook;            // the last look's luma thumbnail
		std::vector<float>   lookDiffs;           // changes between consecutive looks, oldest first
		double               settleTotal = 0;     // settle seconds summed over this run's photos
		int                  settledShots = 0;

		// Spread colour fit, planned restarts, retry pass (taken at Start).
		bool                spreadFit = false;     // move a spread sample of the shots to the front for the colour match
		bool                restartAllowed = false;  // Photo Baker app run: planned restarts may quit the game
		int                 restartEvery = 0;
		bool                retryFailed = true;
		BakePlan::RetryBook retryBook;             // indices into shots
		int                 retryCellsLeft = 0;    // retry pass: cells of the retry shots not tried yet

		// [Bake] bLockExposure: image spaces whose eye adaptation strength was set to 0, with the original value.
		std::vector<std::pair<RE::TESImageSpace*, float>> lockedEyeAdapt;
		// [Bake] fPhotoExposure: image spaces whose brightness was scaled down, with the original value.
		std::vector<std::pair<RE::TESImageSpace*, float>> scaledBrightness;

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
			const auto text = std::format(
				"{{\"state\": {}, \"found\": {}, \"done\": {}, \"doneCount\": {}, \"failedCells\": {}, \"etaSeconds\": {:.0f}, \"phase\": {}, \"message\": {}, \"pid\": {}}}\n",
				JsonText(state), s.found, s.done, s.done, s.failedCells, s.etaSeconds, JsonText(s.phase), JsonText(s.message), GetCurrentProcessId());
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

		// Eye adaptation would brighten dark spots and darken bright ones from photo to photo. Strength 0 in every
		// image space record (the HDR data the weather and cell lighting blend from) keeps the exposure fixed.
		// Form data only, never saved in the game; restored when the bake ends, and gone anyway when the game quits.
		void LockExposure()
		{
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!lockedEyeAdapt.empty() || !dataHandler)
				return;
			for (auto* space : dataHandler->GetFormArray<RE::TESImageSpace>()) {
				if (!space || space->data.hdr.eyeAdaptStrength == 0.0f)
					continue;
				lockedEyeAdapt.emplace_back(space, space->data.hdr.eyeAdaptStrength);
				space->data.hdr.eyeAdaptStrength = 0.0f;
			}
			logger::info("bake: eye adaptation off in {} image spaces", lockedEyeAdapt.size());
		}

		void UnlockExposure()
		{
			if (lockedEyeAdapt.empty())
				return;
			for (const auto& [space, strength] : lockedEyeAdapt)
				space->data.hdr.eyeAdaptStrength = strength;
			logger::info("bake: eye adaptation restored in {} image spaces", lockedEyeAdapt.size());
			lockedEyeAdapt.clear();
		}

		// Snow at noon in clear weather comes out pure white even after eye adaptation has settled. The image space
		// brightness is applied after adaptation and tone mapping, so scaling it down keeps the bright ground below
		// white; the colour match brightens the photos back. Form data only, restored when the bake ends.
		void ScaleExposure(float a_factor)
		{
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!scaledBrightness.empty() || !dataHandler || a_factor >= 1.0f)
				return;
			for (auto* space : dataHandler->GetFormArray<RE::TESImageSpace>()) {
				if (!space)
					continue;
				scaledBrightness.emplace_back(space, space->data.cinematic.brightness);
				space->data.cinematic.brightness *= a_factor;
			}
			logger::info("bake: picture brightness x{:.2f} in {} image spaces", a_factor, scaledBrightness.size());
		}

		void RestoreExposure()
		{
			if (scaledBrightness.empty())
				return;
			for (const auto& [space, brightness] : scaledBrightness)
				space->data.cinematic.brightness = brightness;
			logger::info("bake: picture brightness restored in {} image spaces", scaledBrightness.size());
			scaledBrightness.clear();
		}

		double AverageSettle() { return settledShots > 0 ? settleTotal / settledShots : 0.0; }

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
			UnlockExposure();
			RestoreExposure();
			if (settledShots > 0)
				logger::info("bake: {} photos this run, average settle {:.1f} s", settledShots, AverageSettle());
			// A planned restart keeps the start keys (and hidden NPCs): the next game start carries on with the bake.
			if (std::string_view(a_state) != kRestartState) {
				if (actorsHidden || Settings::BakeHideActorsRequested())
					Settings::SetBakeHideActors(false);
				Settings::ClearBakeStartOnLoad();
			}
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

		void BeginSettle()
		{
			settleElapsed = 0.0f;
			lookWait = 0.0f;
			lastLook.clear();
			lookDiffs.clear();
			phase = Phase::kSettle;
		}

		// Adaptive settle: a look at the screen every kLookInterval once the camera is over the shot.
		void LookIfDue(float a_dt)
		{
			if (!settleRule.adaptive || (lookWait -= a_dt) > 0.0f || !CameraAtTarget(false))
				return;
			lookWait = kLookInterval;
			BeginCapture(CapPurpose::kSettleLook);
		}

		void AddLook(const std::shared_ptr<Photo>& a_photo)
		{
			if (!a_photo)
				return;
			auto look = BakeImage::LumaThumbnail(a_photo->bytes.data(), a_photo->w, a_photo->h, a_photo->pitch, a_photo->format, kLookSize);
			if (look.empty())
				return;
			if (!lastLook.empty()) {
				lookDiffs.push_back(BakeImage::MeanAbsDiff(lastLook, look));
				logger::debug("bake: settle look at {:.1f} s: change {:.2f}", settleElapsed, lookDiffs.back());
			}
			lastLook = std::move(look);
		}

		// A failed photo: taken again in the retry pass at the end (or on the next run), else named as missing.
		// Status failedCells: before the retry pass, the cells of the failed shots; during it, the cells still to be
		// tried again, counted down after every retry shot whatever its outcome (so the Photo Baker app sees the bake
		// moving); after it, the cells still missing (WriteMissingList).
		void CountRetryShot()
		{
			if (!retryBook.InRetry())
				return;
			retryCellsLeft = std::max(0, retryCellsLeft - static_cast<int>(shots[cur].cells.size()));
			std::lock_guard lock(statusMutex);
			status.failedCells = retryCellsLeft;
		}

		void MarkFailed()
		{
			const bool inRetry = retryBook.InRetry();
			retryBook.Fail(cur, static_cast<int>(shots[cur].cells.size()));
			if (inRetry) {
				CountRetryShot();
			} else {
				std::lock_guard lock(statusMutex);
				status.failedCells = retryBook.FailedCells();
			}
			WriteStatusFile();
		}

		void StartShot(const Shot& a_s)
		{
			MovePlayer(a_s);
			AimAt(a_s);
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode())
				Console(std::format("fov {:.4f}", fov));
			BeginSettle();
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
			CountRetryShot();
			UpdateCounts();
			if (settledShots > 0 && settledShots % kSettleLogEvery == 0)
				logger::info("bake: {} photos this run, {} done of {}, average settle {:.1f} s", capturedThisRun, doneBefore + capturedThisRun, shots.size(), AverageSettle());
			++cur;
			phase = Phase::kNext;
		}

		// Spread colour fit: the colour match is fitted from the first photos taken, so take a sample spread over
		// the whole list first (a whole-map list starts in the snowy north). Tamriel shots only: the walled cities
		// are few and would add a worldspace change each.
		void MoveSpreadShotsFirst()
		{
			std::vector<std::size_t>            candidates;
			std::vector<std::pair<float, float>> points;
			for (std::size_t i = 0; i < shots.size(); ++i) {
				if (shots[i].done || shots[i].world != kRootWorld)
					continue;
				candidates.push_back(i);
				points.emplace_back(shots[i].x, shots[i].y);
			}
			const auto picks = BakePlan::SpreadSample(points, kFitShots);
			if (picks.empty())
				return;
			std::vector<bool> picked(shots.size(), false);
			std::vector<Shot> ordered;
			ordered.reserve(shots.size());
			for (const auto pick : picks) {
				picked[candidates[pick]] = true;
				ordered.push_back(shots[candidates[pick]]);
			}
			for (std::size_t i = 0; i < shots.size(); ++i)
				if (!picked[i])
					ordered.push_back(std::move(shots[i]));
			shots = std::move(ordered);
			logger::info("bake: colour match from {} photos spread over the map, taken first", picks.size());
		}

		// End of the list: failed shots once more, appended to the list. False when there is nothing (left) to retry.
		bool QueueRetryPass()
		{
			const auto again = retryBook.StartRetry();
			retryCellsLeft = 0;
			for (const auto idx : again) {
				Shot copy = shots[idx];
				copy.done = false;
				retryCellsLeft += static_cast<int>(copy.cells.size());
				shots.push_back(std::move(copy));
			}
			{
				std::lock_guard lock(statusMutex);
				status.failedCells = retryCellsLeft;
			}
			if (again.empty())
				return false;
			curWorld.clear();  // the retry pass starts with a fresh entry and fog test
			havePrev = false;
			logger::info("bake: taking {} failed photos once more", again.size());
			SetPhase(std::format("taking {} failed photos once more", again.size()));
			return true;
		}

		// Cells still without a photo: named in the log and in kMissingPath (removed when there are none).
		void WriteMissingList()
		{
			std::vector<std::string> keys;
			for (const auto idx : retryBook.Failed())
				for (const auto& [cx, cy] : shots[idx].cells)
					keys.push_back(ShotKey(shots[idx].world, cx, cy));
			{
				std::lock_guard lock(statusMutex);
				status.failedCells = static_cast<int>(keys.size());
			}
			std::error_code ec;
			if (keys.empty()) {
				fs::remove(kMissingPath, ec);
				return;
			}
			std::ostringstream text;
			BakePlan::WriteMissing(text, keys);
			const std::string bytes = text.str();
			WriteFileWhole(kMissingPath, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
			logger::warn("bake: {} cells still have no photo (listed in {}):", keys.size(), kMissingPath);
			for (const auto& key : keys)
				logger::warn("bake:   missing {}", key);
		}

		// A nearly flat photo (one colour): the fog came back on, or something covers the spot. Plain snow or ice
		// is low-contrast too, so a low photo is compared with the block's terrain LOD reference first. Test the fog
		// again and take the photo once more; if it is still flat (or clearly empty from the start), save nothing for it.
		void CheckShot()
		{
			const auto ready = [](const std::shared_future<float>& f) { return !f.valid() || f.wait_for(0s) == std::future_status::ready; };
			if (!ready(checkContrast.photo) || !ready(checkContrast.reference))
				return;
			const float contrast = checkContrast.photo.valid() ? checkContrast.photo.get() : -1.0f;
			const float reference = checkContrast.reference.valid() ? checkContrast.reference.get() : -1.0f;
			checkContrast = {};
			const auto verdict = BakeImage::JudgeContrast(contrast, reference);
			const bool clearlyEmpty = verdict == BakeImage::ContrastVerdict::kEmpty && contrast < BakeImage::kClearlyEmptyContrast;
			if (verdict != BakeImage::ContrastVerdict::kKeep && (flatRetried || clearlyEmpty)) {
				// Flat again after a new fog test, or nothing at all there (sea, map edge): no tile is better than a
				// grey one; the minimap shows the player's terrain there.
				const Shot& s = shots[cur];
				logger::warn("bake: photo at {} {},{} {} (contrast {:.1f}, reference {:.1f}); not saved, your terrain shows there", s.world, s.bx, s.by,
					flatRetried ? "still flat" : "empty", contrast, reference);
				checkPhoto.reset();
				flatRetried = false;
				MarkFailed();
				++cur;
				phase = Phase::kNext;
				return;
			}
			if (verdict == BakeImage::ContrastVerdict::kKeep && BakeImage::NeedsReferenceContrast(contrast)) {
				const Shot& s = shots[cur];
				logger::info("bake: low-contrast photo at {} {},{} kept: plain ground (contrast {:.1f}, reference {:.1f})", s.world, s.bx, s.by, contrast, reference);
			}
			if (verdict != BakeImage::ContrastVerdict::kKeep) {
				const Shot& s = shots[cur];
				logger::warn("bake: flat photo at {} {},{} (contrast {:.1f}, reference {:.1f}); testing the fog again", s.world, s.bx, s.by, contrast, reference);
				checkPhoto.reset();
				flatRetried = true;
				waitLeft = 0.0f;
				phase = Phase::kFogWait;  // fog test, then settle and take the photo again
				SetPhase("fog test again in " + s.world);
				return;
			}
			AcceptShot(std::move(checkPhoto));
		}

		// Once per run, at the first capture: the real back buffer size is known only now.
		void CheckPhotoDetail(const Photo& a_photo)
		{
			if (std::exchange(detailChecked, true) || !BakeImage::TooFewPixelsPerCell(a_photo.w, fov, grid, runTileSize))
				return;
			const double cellPx = BakeImage::PhotoPixelsPerCell(a_photo.w, fov, grid);
			logger::warn("bake: {:.0f} photo px per cell with the {} grid on this {}x{} screen, less than the {} px tiles: they are upscaled", cellPx, GridName(grid),
				a_photo.w, a_photo.h, runTileSize);
			SetMessage(std::format("Only {:.0f} photo px per cell for {} px tiles on this {}x{} screen: the tiles are upscaled. Use a smaller grid or tile size.", cellPx,
				runTileSize, a_photo.w, a_photo.h));
		}

		void OnCaptured(std::shared_ptr<Photo> a_photo)
		{
			if (a_photo)
				CheckPhotoDetail(*a_photo);
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
			case CapPurpose::kSettleLook:
				AddLook(a_photo);
				phase = Phase::kSettle;
				break;
			case CapPurpose::kShot:
				if (a_photo && Settings::Get()->debugLog) {
					const auto pos = CameraPos();
					logger::debug("bake: shot {} {},{} target {:.0f},{:.0f} camera {:.0f},{:.0f},{:.0f}", s.world, s.bx, s.by, camTarget.x, camTarget.y,
						pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f);
				}
				if (!a_photo) {
					logger::warn("bake: capture failed: {} {},{} (taken again in the retry pass)", s.world, s.bx, s.by);
					MarkFailed();
					++cur;
					phase = Phase::kNext;
					break;
				}
				checkContrast = SubmitShotContrast(a_photo, s.bx, s.by);
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
				logger::warn("bake: area at {} {},{} did not finish loading; photo skipped (tried again in the retry pass)", s.world, s.bx, s.by);
				MarkFailed();
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
					if (spreadFit)
						MoveSpreadShotsFirst();
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
						if (retryFailed && QueueRetryPass())
							return;  // the failed shots are now at the end of the list
						WriteMissingList();
						Submit(FlushJob{});
						phase = Phase::kFinishing;
						SetPhase("writing the last tiles");
						return;
					}
					if (restartAllowed && BakePlan::RestartDue(capturedThisRun, restartEvery)) {
						Stop(std::format("Planned restart after {} photos; the bake carries on when the game starts again.", capturedThisRun), kRestartState);
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
					BeginSettle();
					return;
				}
			case Phase::kSettle:
				settleElapsed += a_dt;
				if (!BakePlan::SettleDone(settleElapsed, lookDiffs, settleRule)) {
					LookIfDue(a_dt);
					return;
				}
				// Never photograph a spot the camera has not reached: wait up to kCameraTimeout more, then skip
				// the shot (it is taken again in the retry pass) rather than write a wrong tile.
				if (CameraAtTarget(cameraWait == 0.0f)) {
					cameraWait = 0.0f;
					settleTotal += settleElapsed;
					++settledShots;
					BeginCapture(CapPurpose::kShot);
				} else if ((cameraWait += a_dt) > kCameraTimeout) {
					const Shot& s = shots[cur];
					logger::warn("bake: camera never reached {} {},{}; skipped (taken again in the retry pass)", s.world, s.bx, s.by);
					cameraWait = 0.0f;
					MarkFailed();
					++cur;
					phase = Phase::kNext;
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
			logger::info("bake: {} map markers -> {} photos with the {} grid{} ({} done in earlier runs)", markers.size(), list.size(), GridName(scanGrid),
				Settings::Get()->bake.fullMap ? ", whole map" : "", done);
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
				FailUnattended(cfg->bake.fullMap ? "No land found for the whole map (no terrain LOD meshes for Tamriel?)." : "No settlement map markers found.");
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
			const int tileSize = BakeImage::ClampTileSize(cfg->bake.tileSize);
			Submit(BeginJob{ progress, refsOk ? device : nullptr, cfg->shade, grid, tileSize, cfg->bake.saveMasters });
			settle = cfg->bake.settleSeconds;
			settleRule = { cfg->bake.adaptiveSettle, cfg->bake.settleMinSeconds, cfg->bake.settleSeconds, cfg->bake.settleThreshold };
			settleTotal = 0;
			settledShots = 0;
			spreadFit = cfg->bake.spreadToneFit && !progress.toneKnown;
			restartAllowed = statusFileOn && quitWhenDone;  // never quit a game a player started the bake in by hand
			restartEvery = cfg->bake.restartEvery;
			retryFailed = cfg->bake.retryFailed;
			retryBook = {};
			retryCellsLeft = 0;
			{
				std::lock_guard lock(statusMutex);
				status.failedCells = 0;
			}
			if (cfg->bake.lockExposure)
				LockExposure();
			ScaleExposure(BakeImage::ClampPhotoExposure(cfg->bake.photoExposure));
			const bool keep = cfg->bake.keepExisting;
			ReadScreenSize();  // the bake's Present hook (which keeps it current) is not installed yet on the first start
			const int  w = screenW, h = screenH;
			fov = w > 0 && h > 0 ? BakeImage::BakeFovDeg(w, h) : BakeImage::kBaseFovDeg;
			runTileSize = tileSize;
			detailChecked = false;  // the photo px per cell warning comes with the first capture

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
			const std::string settleNote = settleRule.adaptive ? std::format("settle {:.1f} to {:.0f} s", settleRule.minSeconds, settle) : std::format("settle {:.0f} s", settle);
			SetMessage(std::format("{}{}Started: {} photos ({} grid{}, {} px tiles), FOV {:.2f}, {}{}. Close this menu.", gridNote, gridNote.empty() ? "" : " ", count,
				GridName(grid), cfg->bake.fullMap ? ", whole map" : "", tileSize, fov, settleNote, actorsHidden ? ", NPCs hidden" : ""));
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
