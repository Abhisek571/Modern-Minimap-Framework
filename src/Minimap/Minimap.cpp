#include "PCH.h"

#include "Minimap.h"

#include "FogState.h"
#include "IconData.h"
#include "MapRenderer.h"
#include "InteriorMap.h"
#include "Settings.h"
#include "TileCache.h"

namespace MM::Minimap
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::atomic<std::uint32_t> zoomIndex{ 0 };
		std::atomic<bool>          hidden{ false };
		std::atomic<bool>          holdZoom{ false };    // hold-to-zoom-out key is down
		std::atomic<bool>          suppressed{ false };  // town photo bake is taking a picture

		std::mutex          forgetMutex;
		std::vector<TileId> forgetTiles;  // baked tiles rewritten on disk, dropped from the cache on the next draw

		// Render thread only. Heap objects left alive at exit on purpose: no D3D releases or thread joins during shutdown.
		MapRenderer&               renderer = *new MapRenderer;
		TileCache*                 cache = nullptr;
		bool                       initTried = false;
		bool                       firstDrawLogged = false;
		DXGI_FORMAT                targetFormat = DXGI_FORMAT_UNKNOWN;  // HUD target format, read at the first draw
		std::atomic<DWORD>         mainThread{ 0 };
		std::atomic<DWORD>         renderThread{ 0 };  // last thread that drew (log only)
		std::mutex                 drawMutex;          // Draw never runs twice at once
		std::atomic<bool>          offThreadLogged{ false };
		std::atomic<bool>          tilesOn{ false };     // tile cache exists (set on the render thread)
		std::atomic<bool>          tickQueued{ false };  // a GameTick task is waiting on the main thread
		std::atomic<std::int64_t>  tickTicks{ 0 };       // GameTick cost since the last draw (perf log)
		D3DStateBlock              state;

		// Game state read on the main thread (GameTick) and drawn on the render thread (Draw).
		// HUDMenu::PostDisplay runs on the render thread on SE 1.6, where game objects must not be touched.
		struct Snapshot
		{
			bool                    visible{ false };
			bool                    interior{ false };
			RE::NiPoint3            pos;
			float                   heading{ 0 };
			float                   view{ 0 };
			float                   radius{ 0 };
			std::string             world;
			std::vector<IconSource> icons;
			std::shared_ptr<const NavMeshData> nav;  // interiors: the cell's navmesh (null = radar disc)
			std::shared_ptr<const FogState::Mask> fog;  // fog of war mask (null = off)
			std::shared_ptr<const std::vector<GlyphPlacement>> infoLine;  // clock / location text (null = none)
		};
		std::mutex snapMutex;
		Snapshot   snapShared;  // guarded by snapMutex
		Snapshot   snapRender;  // render thread's copy (keeps its vector capacity)

		struct Composed
		{
			bool        valid{ false };
			std::string world;
			Vec2        center;
			float       upp{ 0 };
			int         lvl{ 0 };
			int         size{ 0 };
			std::uint32_t gen{ 0 };  // settings generation (shading changes need a new compose)
			std::uint32_t navGen{ 0 };  // interior map: navmesh generation drawn (0 = tile map)
			float       z{ 0 };       // interior map: player z the floor band was drawn for
		} composed;
		Clock::time_point lastCompose{};
		bool              tilesArrived = false;  // a tile came in since the last compose

		// Whole-file read through the game's resource system (loose files and BSAs). Runs on the tile worker.
		std::atomic<bool> firstReadLogged{ false };
		std::atomic<bool> firstMissLogged{ false };
		std::atomic<bool> sizeMismatchLogged{ false };

		bool ReadFile(const std::string& a_path, std::vector<std::uint8_t>& a_out)
		{
			a_out.clear();
			RE::BSResourceNiBinaryStream stream(a_path);
			if (!stream.good()) {
				// Missing tiles are normal (open sea); if this is the only line and no "first tile file read" follows, paths are wrong.
				if (!firstMissLogged.exchange(true))
					logger::info("first tile file not found: {}", a_path);
				return false;
			}
			const std::uint32_t total = stream.stream ? stream.stream->totalSize : 0;
			std::vector<char>   chunk(1 << 16);
			for (;;) {
				const auto before = stream.tell();
				const bool full = stream.read(chunk.data(), static_cast<std::uint32_t>(chunk.size()));
				// tell() is the game's streamPos; never trust it past the chunk we handed in.
				const auto n = std::min<std::size_t>(stream.tell() - before, chunk.size());
				a_out.insert(a_out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
				if (!full || n == 0)
					break;
			}
			if (!firstReadLogged.exchange(true))
				logger::info("first tile file read (worker thread): {} -> {} bytes (stream size {})", a_path, a_out.size(), total);
			if (total != 0 && a_out.size() != total && !sizeMismatchLogged.exchange(true))
				logger::warn("file read size mismatch: {} read {} of {} bytes", a_path, a_out.size(), total);
			return !a_out.empty();
		}

		std::vector<IconInstance> iconInstances;  // reused every frame
		std::uint32_t             fogUploaded = 0;  // render thread: fog mask generation in the texture

		// Cost of our draw + timers, logged once a minute at debug level.
		struct Perf
		{
			double        freq{ 0 };
			std::int64_t  start{ 0 };
			std::int64_t  total{ 0 };
			std::int64_t  worst{ 0 };
			std::uint64_t frames{ 0 };
		} perf;

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcFreq()
		{
			static const double f = [] {
				LARGE_INTEGER v;
				QueryPerformanceFrequency(&v);
				return static_cast<double>(v.QuadPart);
			}();
			return f;
		}

		double Seconds(std::int64_t a_ticks) { return static_cast<double>(a_ticks) / QpcFreq(); }

		void PerfFrame(std::int64_t a_begin)
		{
			const auto now = Qpc();
			const auto cost = now - a_begin + tickTicks.exchange(0);
			perf.total += cost;
			perf.worst = std::max(perf.worst, cost);
			++perf.frames;
			if (Seconds(now - perf.start) >= 60.0) {
				const auto c = IconData::GetCounts();
				logger::debug("perf: {} frames, avg {:.1f} us, worst {:.1f} us per frame (draw + game tick); icons: {} markers, {} doors, {} actors, {} quest",
					perf.frames, Seconds(perf.total) * 1e6 / static_cast<double>(perf.frames), Seconds(perf.worst) * 1e6, c.markers, c.doors, c.actors, c.quests);
				perf.start = now;
				perf.total = perf.worst = 0;
				perf.frames = 0;
			}
		}

		// Worldspace whose LOD tiles show this one: city worlds use their parent's (same coordinates).
		std::string LodWorldName(RE::TESWorldSpace* a_world)
		{
			a_world = IconData::MapRootWorld(a_world);
			const char* id = a_world ? a_world->GetFormEditorID() : nullptr;
			return id ? std::string(id) : std::string{};
		}

		// Kill moves (VATS-style camera) and the player's death camera.
		bool InKillCam(RE::PlayerCharacter* a_player)
		{
			if (a_player->IsInKillMove() || a_player->IsDead())
				return true;
			const auto* cam = RE::PlayerCamera::GetSingleton();
			const auto* camState = cam ? cam->currentState.get() : nullptr;
			return camState && camState->id == RE::CameraState::kVATS;
		}

		// Heading of the game camera (its local X axis is the view direction), clockwise from north.
		float CameraHeading(float a_fallback)
		{
			const auto* cam = RE::Main::WorldRootCamera();
			if (!cam)
				return a_fallback;
			const auto& m = cam->world.rotate;
			const float east = m.entry[0][0], north = m.entry[1][0];
			return (east * east + north * north) > 1e-6f ? HeadingFromForward(east, north) : a_fallback;
		}

		DXGI_FORMAT TargetFormat(ID3D11RenderTargetView* a_rtv)
		{
			D3D11_RENDER_TARGET_VIEW_DESC d{};
			a_rtv->GetDesc(&d);
			return d.Format;
		}

		bool IsSrgb(DXGI_FORMAT a_f)
		{
			return a_f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || a_f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || a_f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
		}

		bool IsFloat(DXGI_FORMAT a_f)
		{
			return a_f == DXGI_FORMAT_R16G16B16A16_FLOAT || a_f == DXGI_FORMAT_R11G11B10_FLOAT || a_f == DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		bool TargetSize(ID3D11RenderTargetView* a_rtv, float& a_w, float& a_h)
		{
			Microsoft::WRL::ComPtr<ID3D11Resource> res;
			a_rtv->GetResource(&res);
			Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
			if (!res || FAILED(res.As(&tex)))
				return false;
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			a_w = static_cast<float>(d.Width);
			a_h = static_cast<float>(d.Height);
			return true;
		}

		constexpr float kCompassGlyphPx = 17.0f;  // at 1080p, before the icon scale (the ink is about half the glyph cell)

		constexpr float kBakedMaxUnitsPerPixel = 64.0f;  // baked tiles are 8 units/px at 512 px (4 at 1024); past this zoom the LOD terrain alone is enough

		void Compose(ID3D11DeviceContext* a_ctx, const Settings::Values& cfg, const std::string& a_world, Vec2 a_center, float a_upp, int a_lvl, int a_size,
			const FogParams& a_fog)
		{
			const float half = a_size * 0.5f * a_upp;
			const Rect  view{ a_center.x - half, a_center.y - half, a_center.x + half, a_center.y + half };
			const Rect  ahead{ view.minX - half * 0.5f, view.minY - half * 0.5f, view.maxX + half * 0.5f, view.maxY + half * 0.5f };
			auto        wanted = TilesInRect(ahead, a_lvl);
			// Baked top-down pictures (if the bake output is installed) go over the terrain when zoomed in.
			const bool  baked = a_upp <= kBakedMaxUnitsPerPixel;
			if (baked) {
				const auto b = TilesInRect(ahead, kBakedLevel);
				wanted.insert(wanted.end(), b.begin(), b.end());
			}
			cache->Want(wanted, ChooseMaxTextureSize(a_lvl, a_upp));

			std::vector<TileDraw> draws;
			auto                  add = [&](const std::vector<TileId>& a_ids) {
                for (const auto& id : a_ids) {
                    const Tile* t = cache->Find(id);
                    if (t && !t->absent)
                        draws.push_back({ id, t->diffuse.Get(), t->normal.Get(), t->water.Get() });
                }
			};
			// After a zoom step, cached tiles of the previous level fill the gaps until the new level arrives.
			if (composed.valid && composed.world == a_world && composed.lvl != a_lvl)
				add(TilesInRect(view, composed.lvl));
			add(TilesInRect(view, a_lvl));
			if (baked)
				add(TilesInRect(view, kBakedLevel));

			renderer.Compose(a_ctx, { a_center, a_upp, a_size, cfg.shade, a_fog }, draws);
			cache->Trim(wanted);
			composed = { true, a_world, a_center, a_upp, a_lvl, a_size, Settings::Generation() };
		}

		void ComposeNav(ID3D11DeviceContext* a_ctx, const Settings::Values& cfg, const NavMeshData& a_nav, const RE::NiPoint3& a_pos, float a_upp, int a_size, float a_mapSize)
		{
			NavComposeParams p;
			p.center = { a_pos.x, a_pos.y };
			p.unitsPerPixel = a_upp;
			p.targetSize = a_size;
			p.playerZ = a_pos.z;
			p.floorBelow = cfg.floorBelow;
			p.floorAbove = cfg.floorAbove;
			p.otherFloorAlpha = cfg.otherFloorAlpha;
			p.wallWidthPx = std::clamp(a_mapSize / 150.0f, 1.5f, 4.0f);
			renderer.ComposeNav(a_ctx, p);
			composed = { true, std::string{}, p.center, a_upp, 0, a_size, Settings::Generation(), a_nav.generation, a_pos.z };
		}
	}

	bool HudIsClear()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui || !ui->IsShowingMenus() || ui->GameIsPaused())
			return false;
		for (const auto name : { RE::LoadingMenu::MENU_NAME, RE::MainMenu::MENU_NAME, RE::DialogueMenu::MENU_NAME }) {
			if (ui->IsMenuOpen(name))
				return false;
		}
		const auto cursor = ui->GetMenu(RE::CursorMenu::MENU_NAME);
		for (const auto& menu : ui->menuStack) {
			if (!menu || menu.get() == cursor.get() || menu->AlwaysOpen())
				continue;
			// UpdateUsesCursor / InventoryItemMenu: crafting and item screens that neither pause nor flag kUsesCursor.
			if (menu->PausesGame() || menu->Modal() || menu->UsesCursor() || menu->UpdateUsesCursor() || menu->InventoryItemMenu() || menu->FreezeFrameBackground())
				return false;
		}
		return true;
	}

	namespace
	{
		void Publish(Snapshot&& a_snap)
		{
			std::lock_guard lock(snapMutex);
			std::swap(snapShared, a_snap);
		}

		// Interior map source, main thread only. Rebuilt when the cell or its navmeshes change.
		struct NavIdentity
		{
			RE::FormID               form{ 0 };
			const RE::TESObjectCELL* cell{ nullptr };
			std::uint32_t            meshes{ 0 };
			std::uint64_t            verts{ 0 };
			std::uint64_t            tris{ 0 };

			bool operator==(const NavIdentity&) const = default;
		};
		NavIdentity                        navId;
		std::shared_ptr<const NavMeshData> navCurrent;
		std::uint32_t                      navGeneration = 0;

		NavIdentity NavIdOf(RE::TESObjectCELL* a_cell)
		{
			NavIdentity id{ a_cell->GetFormID(), a_cell };
			if (const auto* arr = a_cell->GetRuntimeData().navMeshes) {
				for (const auto& m : arr->navMeshes) {
					if (!m)
						continue;
					++id.meshes;
					id.verts += m->vertices.size();
					id.tris += m->triangles.size();
				}
			}
			return id;
		}

		// One copy per cell load: the game navmeshes into plain arrays the render thread can keep.
		std::shared_ptr<const NavMeshData> BuildNav(RE::TESObjectCELL* a_cell, const NavIdentity& a_id)
		{
			const auto begin = Qpc();
			auto       data = std::make_shared<NavMeshData>();
			if (++navGeneration == 0)
				navGeneration = 1;
			data->generation = navGeneration;
			std::vector<float>         xyz;
			std::vector<NavTriangleIn> tris;
			if (const auto* arr = a_cell->GetRuntimeData().navMeshes) {
				for (const auto& m : arr->navMeshes) {
					if (!m)
						continue;
					xyz.clear();
					tris.clear();
					xyz.reserve(m->vertices.size() * 3);
					tris.reserve(m->triangles.size());
					for (const auto& v : m->vertices) {
						xyz.push_back(v.location.x);
						xyz.push_back(v.location.y);
						xyz.push_back(v.location.z);
					}
					for (const auto& t : m->triangles) {
						tris.push_back({ { t.vertices[0], t.vertices[1], t.vertices[2] }, { t.triangles[0], t.triangles[1], t.triangles[2] },
							t.triangleFlags.underlying() });
					}
					AppendNavmesh(*data, xyz, tris);
				}
			}
			const char*  name = a_cell->GetName();
			const double ms = Seconds(Qpc() - begin) * 1e3;
			if (data->Empty()) {
				logger::info("interior map: cell {:08X} ({}) has no usable navmesh ({} navmeshes); showing the radar disc", a_id.form, name ? name : "", a_id.meshes);
				return nullptr;
			}
			logger::info("interior map: cell {:08X} ({}): {} navmeshes, {} vertices, {} floor triangles, {} wall edges, z {:.0f}..{:.0f}, copied in {:.2f} ms", a_id.form,
				name ? name : "", a_id.meshes, data->VertexCount(), data->tris.size() / 3, data->walls.size() / 2, data->minZ, data->maxZ, ms);
			return data;
		}

		// Auto zoom state (main thread only): the player's ground speed and the eased zoom factor.
		struct AutoZoomState
		{
			bool         havePos{ false };
			RE::NiPoint3 lastPos;
			double       lastTime{ 0 };
			float        speed{ 0 };   // smoothed, world units per second
			float        factor{ 1 };  // eased, multiplies the zoom radius
		} autoZoom;

		// Zoom radius multiplier for the player's speed. Off, indoors or after a teleport the factor eases back to 1.
		float AutoZoomFactor(const Settings::Values& a_cfg, bool a_active, const RE::NiPoint3& a_pos, double a_now)
		{
			constexpr float kTeleportSpeed = 5000.0f;  // faster than any horse: a load door, fast travel or setpos
			constexpr float kSpeedSeconds = 0.4f;      // speed smoothing time constant
			constexpr float kMaxSampleSeconds = 0.5f;  // a longer gap (menu, hidden map, load) is no speed sample
			auto&       z = autoZoom;
			const float dt = z.havePos ? static_cast<float>(a_now - z.lastTime) : 0.0f;
			float       target = 1.0f;
			if (a_cfg.autoZoom && a_active && z.havePos && dt > 0.0f) {
				const float dist = std::hypot(a_pos.x - z.lastPos.x, a_pos.y - z.lastPos.y);
				const float raw = dist / dt;
				if (raw < kTeleportSpeed && dt < kMaxSampleSeconds)
					z.speed += (raw - z.speed) * (1.0f - std::exp(-dt / kSpeedSeconds));
				else
					z.speed = 0.0f;
				target = AutoZoomTarget(z.speed, a_cfg.autoZoomStrength);
			} else if (!a_active) {
				z.speed = 0.0f;
			}
			z.factor = EaseAutoZoom(z.factor, target, std::min(dt, 0.25f));
			z.havePos = a_active;
			z.lastPos = a_pos;
			z.lastTime = a_now;
			return QuantizeZoom(z.factor);
		}

		// Clock / location line (main thread). Checked on a short timer, laid out again only when the text changes.
		struct InfoLineState
		{
			double                                             nextCheck{ 0 };
			std::uint32_t                                      gen{ 0 };  // settings generation of the text
			std::string                                        text;
			std::shared_ptr<const std::vector<GlyphPlacement>> glyphs;
		} infoLine;

		constexpr double kInfoLineInterval = 0.25;
		constexpr float  kInfoGlyphPx = 18.0f;  // at 1080p, before the icon scale

		std::string LocationName(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_cell)
		{
			const auto* loc = a_player->GetCurrentLocation();
			const char* name = loc ? loc->GetFullName() : nullptr;
			if ((!name || !*name) && a_cell && a_cell->IsInteriorCell())
				name = a_cell->GetFullName();
			return name ? std::string(name) : std::string{};
		}

		std::shared_ptr<const std::vector<GlyphPlacement>> InfoLine(const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_cell, double a_now)
		{
			auto& st = infoLine;
			if (!a_cfg.clock && !a_cfg.locationName) {
				st.glyphs.reset();
				st.text.clear();
				return nullptr;
			}
			if (a_now < st.nextCheck && st.gen == Settings::Generation())
				return st.glyphs;
			st.nextCheck = a_now + kInfoLineInterval;
			std::string text;
			if (a_cfg.clock) {
				const auto* calendar = RE::Calendar::GetSingleton();
				text = FormatClock(calendar ? calendar->GetHour() : 0.0f, a_cfg.clock24h);
			}
			if (a_cfg.locationName) {
				// A name with letters the glyph set lacks (non-Latin) is left out: the clock alone.
				if (auto name = LocationName(a_player, a_cell); !name.empty() && AllCharsHaveGlyphs(name))
					text += (text.empty() ? "" : "  ") + name;
			}
			if (text == st.text && st.gen == Settings::Generation())
				return st.glyphs;
			st.text = std::move(text);
			st.gen = Settings::Generation();
			auto glyphs = std::make_shared<std::vector<GlyphPlacement>>();
			LayoutText(st.text, kInfoGlyphPx, TextAlign::kCenter, *glyphs);
			st.glyphs = glyphs->empty() ? nullptr : std::move(glyphs);
			return st.glyphs;
		}

		// Main thread, once per frame (queued from Draw): reads every game object the minimap needs.
		void GameTick()
		{
			tickQueued = false;
			const auto begin = Qpc();
			Snapshot   s;
			const auto cfgPtr = Settings::Get();
			const auto& cfg = *cfgPtr;
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (cfg.enabled && !hidden && cell && HudIsClear()) {
				s.interior = cell->IsInteriorCell();
				if (!s.interior) {
					navId = {};
					navCurrent.reset();
				}
				const bool hide = (s.interior && cfg.hideInInteriors) || (cfg.hideInKillCam && InKillCam(player)) || (cfg.hideInCombat && player->IsInCombat());
				if (!hide) {
					s.pos = player->GetPosition();
					s.heading = player->GetAngleZ();
					s.view = cfg.useCameraYaw ? CameraHeading(s.heading) : s.heading;
					s.world = s.interior ? std::string{} : LodWorldName(player->GetWorldspace());
					const bool hasMap = !s.world.empty() && tilesOn;
					const auto steps = static_cast<std::uint32_t>(cfg.zoomRadii.size());
					s.radius = cfg.zoomRadii[zoomIndex % steps] * (hasMap ? 1.0f : cfg.radarZoomScale);
					const double nowSec = Seconds(begin);
					const float zoomFactor = AutoZoomFactor(cfg, hasMap, s.pos, nowSec);
					if (hasMap)
						s.radius *= zoomFactor;
					s.radius = HoldZoomRadius(s.radius, holdZoom && cfg.holdZoomKey != 0, cfg.holdZoomFactor);
					IconData::Update(nowSec, cfg, player, s.radius);
					s.infoLine = InfoLine(cfg, player, cell, nowSec);
					if (hasMap)
						s.fog = FogState::Update(nowSec, cfg, player, IconData::MapRootWorld(player->GetWorldspace()));
					if (cfg.icons.enabled)
						IconData::Collect(nowSec, s.icons);
					if (s.interior) {
						if (const auto id = NavIdOf(cell); id != navId) {
							navId = id;
							navCurrent = BuildNav(cell, id);
						}
						s.nav = navCurrent;
					}
					s.visible = true;
				}
			}
			Publish(std::move(s));
			tickTicks += Qpc() - begin;
		}
	}

	void Draw(ID3D11DeviceContext* a_ctx, ID3D11Device* a_device, ID3D11RenderTargetView* a_target)
	{
		const auto  cfgPtr = Settings::Get();
		const auto& cfg = *cfgPtr;
		if (!cfg.enabled || hidden || suppressed || !a_ctx || !a_device)
			return;
		// The game calls the HUD hook from whichever thread renders the UI that frame (it changes), and never from
		// two at once. Never draw concurrently: a call that overlaps a running draw (e.g. a frame-generation
		// layer presenting from its own thread) is skipped.
		std::unique_lock drawLock(drawMutex, std::try_to_lock);
		if (!drawLock.owns_lock()) {
			if (!offThreadLogged.exchange(true))
				logger::warn("draw hook called while another draw was running (thread {}); skipping overlapping calls", GetCurrentThreadId());
			return;
		}
		if (const DWORD self = GetCurrentThreadId(); renderThread.exchange(self) == 0)
			logger::info("first draw thread id {} (main thread {})", self, mainThread.load());
		// Game state is read on the main thread for the next frame; this frame draws the latest snapshot.
		if (!tickQueued.exchange(true))
			SKSE::GetTaskInterface()->AddTask([] {
				// Never let an exception leave a game task: it would end in std::terminate (fast-fail, no crash log).
				try {
					GameTick();
				} catch (const std::exception& e) {
					tickQueued = false;
					static std::atomic<bool> logged{ false };
					if (!logged.exchange(true))
						logger::error("game tick failed: {}", e.what());
				} catch (...) {
					tickQueued = false;
					static std::atomic<bool> logged{ false };
					if (!logged.exchange(true))
						logger::error("game tick failed (unknown exception)");
				}
			});
		{
			std::lock_guard lock(snapMutex);
			snapRender.visible = snapShared.visible;
			snapRender.interior = snapShared.interior;
			snapRender.pos = snapShared.pos;
			snapRender.heading = snapShared.heading;
			snapRender.view = snapShared.view;
			snapRender.radius = snapShared.radius;
			snapRender.world = snapShared.world;
			snapRender.icons.assign(snapShared.icons.begin(), snapShared.icons.end());
			snapRender.nav = snapShared.nav;
			snapRender.fog = snapShared.fog;
			snapRender.infoLine = snapShared.infoLine;
		}
		const Snapshot& snap = snapRender;
		if (!snap.visible)
			return;

		const auto begin = Qpc();
		if (!initTried) {
			initTried = true;
			perf.start = begin;
			std::string error;
			const UINT  flags = a_device->GetCreationFlags();
			logger::info("D3D11 device: creation flags 0x{:X}, feature level 0x{:X}", flags, static_cast<unsigned>(a_device->GetFeatureLevel()));
			if (renderer.Init(a_device, error)) {
				if (!renderer.NavReady())
					logger::error("interior map off (radar disc in interiors): {}", renderer.NavError());
				// The tile worker creates textures on the device from its own thread; a single-threaded device forbids that.
				if (flags & D3D11_CREATE_DEVICE_SINGLETHREADED)
					logger::error("D3D11 device is single-threaded: map tiles off (radar mode only)");
				else {
					cache = new TileCache(a_device, ReadFile, static_cast<std::size_t>(cfg.cacheMB) << 20);
					tilesOn = true;
				}
				logger::info("renderer ready");
			} else {
				logger::error("renderer init failed: {}", error);
			}
		}
		if (!renderer.Ready())
			return;

		state.Backup(a_ctx);
		float W = 0, H = 0;
		if (a_target ? !TargetSize(a_target, W, H) : state.viewportCount == 0) {
			state.Restore(a_ctx);
			return;
		}
		if (!a_target) {
			W = state.viewports[0].Width;
			H = state.viewports[0].Height;
		}
		if (!firstDrawLogged) {
			firstDrawLogged = true;
			float rtW = 0, rtH = 0;
			const bool rt = state.rtv[0] && TargetSize(state.rtv[0], rtW, rtH);
			auto*      fmtView = a_target ? a_target : state.rtv[0];
			targetFormat = fmtView ? TargetFormat(fmtView) : DXGI_FORMAT_UNKNOWN;
			logger::info("HUD target format {} ({})", static_cast<int>(targetFormat), IsSrgb(targetFormat) ? "sRGB: writing linear colour" : IsFloat(targetFormat) ? "float/HDR: writing linear colour" : "UNORM: writing sRGB colour as is");
			logger::info("first draw via {}: drawing into {}x{}; bound target {}x{} ({} viewports, first at {},{})", a_target ? "Present" : "HUDMenu::PostDisplay", W, H,
				rt ? rtW : 0.0f, rt ? rtH : 0.0f, state.viewportCount, state.viewportCount ? state.viewports[0].TopLeftX : 0.0f,
				state.viewportCount ? state.viewports[0].TopLeftY : 0.0f);
		}
		if (W < 64.0f || H < 64.0f) {  // no usable viewport bound (size would be 0 -> divide by zero below)
			state.Restore(a_ctx);
			return;
		}

		const float scale = cfg.scaleWithResolution ? H / 1080.0f : 1.0f;
		const float size = std::round(cfg.sizePx * scale);
		const float offX = cfg.offsetX * scale, offY = cfg.offsetY * scale;
		const bool  right = cfg.corner == Settings::Corner::kTopRight || cfg.corner == Settings::Corner::kBottomRight;
		const bool  bottom = cfg.corner == Settings::Corner::kBottomLeft || cfg.corner == Settings::Corner::kBottomRight;

		const auto  pos = snap.pos;
		const float heading = snap.heading;
		const float view = snap.view;

		OverlayParams op;
		op.left = right ? W - size - offX : offX;
		op.top = bottom ? H - size - offY : offY;
		op.size = size;
		op.viewportW = W;
		op.viewportH = H;
		op.mapAngle = cfg.rotateWithPlayer ? view : 0.0f;
		op.arrowAngle = heading - op.mapAngle;
		op.opacity = cfg.opacity;
		op.arrowSize = std::max(7.0f, size * 0.045f);
		op.round = cfg.round;
		op.linearOut = cfg.colorMode == 2 || (cfg.colorMode == 0 && (IsSrgb(targetFormat) || IsFloat(targetFormat)));
		op.outScale = cfg.brightness * (IsFloat(targetFormat) && op.linearOut ? cfg.hdrBrightness : 1.0f);
		op.look = LookForStyle(cfg.style);

		const std::string& world = snap.world;
		const bool  tileMap = !world.empty() && cache;
		const bool  navMap = !tileMap && snap.interior && snap.nav && renderer.SetNavMesh(*snap.nav);
		op.hasMap = tileMap || navMap;
		const float radius = snap.radius;
		const float upp = UnitsPerPixel(radius, size);
		if (tileMap) {
			cache->SetWorld(world);  // evicts the tiles of any other worldspace
			{
				std::lock_guard lock(forgetMutex);
				for (const auto& id : forgetTiles) {
					cache->Forget(id);
					composed.valid = false;
				}
				forgetTiles.clear();
			}
			tilesArrived = cache->Collect() || tilesArrived;
			for (const auto& msg : cache->TakeLog())
				logger::warn("{}", msg);

			const int   lvl = ChooseLevel(upp);
			const int   targetSize = ComposeTargetSize(static_cast<int>(size));
			// Fog of war: upload the mask only when it changed (a block explored or the window moved), then redraw once.
			FogParams fog;
			bool      fogChanged = false;
			if (snap.fog) {
				if (snap.fog->generation != fogUploaded && renderer.SetFogMask(a_ctx, snap.fog->pixels, Fog::kMaskSize)) {
					fogUploaded = snap.fog->generation;
					fogChanged = true;
				}
				fog = { fogUploaded == snap.fog->generation, snap.fog->world, cfg.fogDarkness, cfg.fogInTowns };
			}
			const bool  must = !composed.valid || composed.navGen != 0 || composed.world != cache->World() || composed.upp != upp || composed.size != targetSize ||
			                  composed.gen != Settings::Generation() || fogChanged;
			const float moved = composed.valid ? std::max(std::fabs(pos.x - composed.center.x), std::fabs(pos.y - composed.center.y)) / upp : 1e9f;
			const auto  now = Clock::now();
			if ((must || tilesArrived || moved >= 1.0f) && (must || now - lastCompose >= 33ms)) {
				Compose(a_ctx, cfg, cache->World(), { pos.x, pos.y }, upp, lvl, targetSize, fog);
				lastCompose = now;
				tilesArrived = false;
				state.BindTargets(a_ctx);
			}
			op.offsetPx = { (pos.x - composed.center.x) / upp, (pos.y - composed.center.y) / upp };
		} else if (navMap) {
			// Interior map: same triggers as the tile map, plus a floor-band redraw when the player climbs or descends.
			const int   targetSize = ComposeTargetSize(static_cast<int>(size));
			const bool  must = !composed.valid || composed.navGen != snap.nav->generation || composed.upp != upp || composed.size != targetSize ||
			                  composed.gen != Settings::Generation();
			const float moved = composed.valid ? std::max(std::fabs(pos.x - composed.center.x), std::fabs(pos.y - composed.center.y)) / upp : 1e9f;
			const bool  climbed = std::fabs(pos.z - composed.z) > kNavRecomposeZ;
			const auto  now = Clock::now();
			if ((must || climbed || moved >= 1.0f) && (must || now - lastCompose >= 33ms)) {
				ComposeNav(a_ctx, cfg, *snap.nav, pos, upp, targetSize, size);
				lastCompose = now;
				state.BindTargets(a_ctx);
			}
			op.offsetPx = { (pos.x - composed.center.x) / upp, (pos.y - composed.center.y) / upp };
		}

		// Icons: the main thread gathered them into the snapshot; here they are only projected.
		const bool icons = cfg.icons.enabled;
		IconView   iv;
		iconInstances.clear();
		if (icons) {
			op.arrowSize = 0.0f;  // the arrow is drawn as the last icon instead
			iv.player = { pos.x, pos.y };
			iv.unitsPerPixel = upp;
			iv.mapAngle = op.mapAngle;
			iv.sizePx = size;
			iv.round = op.round;
			iv.centerPx = { op.left + size * 0.5f, op.top + size * 0.5f };
			iv.iconScale = scale * cfg.icons.scale;
			AppendCompassLetters(iv, kCompassGlyphPx, cfg.icons.colors.compass, cfg.icons.colors.textOutline, cfg.icons.compassLetters, iconInstances);
			BuildIconInstances(snap.icons, iv, iconInstances);
			iconInstances.push_back(ArrowInstance(iv, std::max(7.0f, size * 0.045f) * 2.5f, op.arrowAngle, cfg.icons.colors.arrow));
		}
		if (snap.infoLine) {
			// One line centred under the minimap (above it in the bottom corners), drawn whole (not masked).
			const float k = scale * cfg.icons.scale;
			const float glyph = kInfoGlyphPx * k;
			// Outside compass letters sit in the first band past the rim: the line goes one letter further out.
			const bool  lettersOutside = cfg.icons.enabled && cfg.icons.compassLetters == CompassMode::kOutside;
			const float clearance = glyph * 0.6f + (lettersOutside ? kCompassGlyphPx * 1.2f * k + 2.0f : 0.0f);
			const float lineY = bottom ? op.top - clearance : op.top + size + clearance;
			for (const auto& g : *snap.infoLine) {
				IconInstance inst;
				inst.x = op.left + size * 0.5f + g.x * k;
				inst.y = lineY;
				inst.size = glyph;
				inst.clip = 0.0f;
				inst.uv = AtlasUv(g.shape);
				inst.rgba = cfg.icons.colors.infoText;
				AppendOutlinedGlyph(inst, cfg.icons.colors.textOutline, iconInstances);
			}
		}

		if (a_target) {
			a_ctx->OMSetRenderTargets(1, &a_target, nullptr);
			const D3D11_VIEWPORT vp{ 0, 0, W, H, 0, 1 };
			a_ctx->RSSetViewports(1, &vp);
		}
		renderer.DrawOverlay(a_ctx, op);
		if (!iconInstances.empty()) {
			const Vec2 centre{ op.left + size * 0.5f, op.top + size * 0.5f };
			renderer.DrawIcons(a_ctx, { W, H, centre, size, op.round, cfg.opacity, op.linearOut, op.outScale }, iconInstances);
		}
		state.Restore(a_ctx);
		PerfFrame(begin);
	}

	void CycleZoom()
	{
		const auto cfg = Settings::Get();
		const auto steps = static_cast<std::uint32_t>(cfg->zoomRadii.size());
		const auto next = (zoomIndex.load() + 1) % steps;
		zoomIndex = next;
		logger::info("zoom step {} ({} units)", next, cfg->zoomRadii[next]);
	}

	void SetHoldZoom(bool a_held) { holdZoom = a_held; }

	void SetMainThread()
	{
		mainThread = GetCurrentThreadId();
		logger::info("main thread id {}", mainThread.load());
	}

	void SetSuppressed(bool a_suppressed) { suppressed = a_suppressed; }

	void ForgetBakedTile(int a_cellX, int a_cellY)
	{
		std::lock_guard lock(forgetMutex);
		forgetTiles.push_back({ kBakedLevel, a_cellX, a_cellY });
	}

	bool ReadGameFile(const std::string& a_path, std::vector<std::uint8_t>& a_out) { return ReadFile(a_path, a_out); }

	void ToggleHidden()
	{
		hidden = !hidden;
		logger::info("minimap {}", hidden ? "hidden" : "shown");
	}
}
