// Offline checks for Modern Minimap: maths, btr parsing on real LOD files, and the tile/overlay
// passes rendered to PNG through the same D3D11 code the plugin uses.
//
// Usage: ModernMinimapTests <data folder with textures\terrain and meshes\terrain> <output folder>

#include "PhotoProcessing.h"
#include "TerrainMesh.h"
#include "Icons.h"
#include "MapMath.h"
#include "MapRenderer.h"
#include "InteriorMap.h"
#include "TileCache.h"

#include <DDSTextureLoader.h>
#include <ScreenGrab.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <wincodec.h>

using namespace MM;
namespace fs = std::filesystem;

namespace
{
	int failures = 0;
	int checks = 0;

	void Check(bool a_ok, const char* a_what)
	{
		++checks;
		if (!a_ok) {
			++failures;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	bool Near(float a_a, float a_b, float a_eps = 1e-3f) { return std::fabs(a_a - a_b) <= a_eps; }

	void TestMath()
	{
		Check(FloorDiv(-1, 8) == -1 && FloorDiv(-8, 8) == -1 && FloorDiv(-9, 8) == -2 && FloorDiv(7, 8) == 0 && FloorDiv(8, 8) == 1, "FloorDiv");
		Check(WorldToCell(-0.5f) == -1 && WorldToCell(4095.9f) == 0 && WorldToCell(4096.0f) == 1, "WorldToCell");

		Check(TileForWorld(0, 0, 8) == TileId{ 8, 0, 0 }, "tile at origin");
		Check(TileForWorld(-1, -1, 8) == TileId{ 8, -8, -8 }, "tile just south-west of origin");
		Check(TileForWorld(32767.9f, 100, 8) == TileId{ 8, 0, 0 } && TileForWorld(32768.0f, 100, 8) == TileId{ 8, 8, 0 }, "tile edge at 8 cells");
		Check(TileForWorld(-70000, 50000, 16) == TileId{ 16, -32, 0 }, "level 16 picking");
		Check(TileForWorld(19000, -7000, 4) == TileId{ 4, 4, -4 }, "level 4 picking");

		const Rect r = TileWorldRect({ 8, -8, -16 });
		Check(r.minX == -32768 && r.maxX == 0 && r.minY == -65536 && r.maxY == -32768, "tile world rect");

		Check(TilesInRect(TileWorldRect({ 8, 0, 0 }), 8).size() == 1, "rect equal to one tile -> 1 tile");
		const auto four = TilesInRect({ -100, -100, 100, 100 }, 8);
		Check(four.size() == 4, "rect across origin -> 4 tiles");
		const auto many = TilesInRect({ -40000, -10000, 40000, 10000 }, 8);
		Check(many.size() == 8 && (many.front() == TileId{ 8, 0, 0 } || many.front() == TileId{ 8, -8, 0 } || many.front() == TileId{ 8, 0, -8 } || many.front() == TileId{ 8, -8, -8 }),
			"tiles sorted nearest first");

		// Zoom 3000/6000/12000 at 256 px -> 23.4 / 46.9 / 93.8 units per px.
		Check(Near(UnitsPerPixel(3000, 256), 23.4375f), "units per pixel");
		Check(ChooseLevel(23.4375f) == 4 && ChooseLevel(46.875f) == 8 && ChooseLevel(93.75f) == 16 && ChooseLevel(400) == 32 && ChooseLevel(5) == 4, "LOD level choice");
		Check(ChooseMaxTextureSize(4, 23.4375f) == 1024 && ChooseMaxTextureSize(8, 46.875f) == 1024 && ChooseMaxTextureSize(4, 8) == 2048 && ChooseMaxTextureSize(32, 1000) == 256,
			"max texture size");
		Check(ComposeTargetSize(256) == 371, "compose target size");

		// North up: screen right = east, screen up = north.
		Vec2 m = ScreenToMapOffset({ 10, 0 }, 0);
		Check(Near(m.x, 10) && Near(m.y, 0), "north-up: right is east");
		m = ScreenToMapOffset({ 0, -10 }, 0);
		Check(Near(m.x, 0) && Near(m.y, 10), "north-up: up is north");
		// Facing east, rotated map: screen up = east, screen right = south.
		const float east = std::numbers::pi_v<float> / 2;
		m = ScreenToMapOffset({ 0, -10 }, east);
		Check(Near(m.x, 10) && Near(m.y, 0), "rotated: up is heading");
		m = ScreenToMapOffset({ 10, 0 }, east);
		Check(Near(m.x, 0) && Near(m.y, -10), "rotated: right is heading + 90");
		bool roundTrip = true;
		for (float a = -3.0f; a < 3.2f; a += 0.37f) {
			const Vec2 q{ 13.0f, -42.0f };
			const Vec2 back = MapToScreenOffset(ScreenToMapOffset(q, a), a);
			roundTrip = roundTrip && Near(back.x, q.x) && Near(back.y, q.y);
		}
		Check(roundTrip, "screen <-> map round trip");
		Check(Near(HeadingFromForward(1, 0), east) && Near(HeadingFromForward(0, 1), 0) && Near(HeadingFromForward(0, -1), std::numbers::pi_v<float>), "heading from forward");

		const Vec2 c = WorldToTarget({ 1000, 2000 }, { 1000, 2000 }, 32, 400);
		const Vec2 n = WorldToTarget({ 1000, 2000 + 320 }, { 1000, 2000 }, 32, 400);
		Check(Near(c.x, 200) && Near(c.y, 200) && Near(n.x, 200) && Near(n.y, 190), "world -> target (north is up)");

		Check(MaskCoverage({ 0, 0 }, 256, true) == 1.0f && MaskCoverage({ 129, 0 }, 256, true) == 0.0f, "round mask centre/outside");
		Check(MaskCoverage({ 120, 120 }, 256, true) == 0.0f && MaskCoverage({ 120, 120 }, 256, false) == 1.0f, "corner: round out, square in");
		Check(Near(MaskCoverage({ 128, 0 }, 256, true), 0.5f), "mask edge anti-aliased");
	}

	bool ReadFile(const fs::path& a_root, const std::string& a_rel, std::vector<std::uint8_t>& a_out)
	{
		std::ifstream f(a_root / a_rel, std::ios::binary);
		if (!f)
			return false;
		a_out.assign(std::istreambuf_iterator<char>(f), {});
		return !a_out.empty();
	}

	void TestBtr(const fs::path& a_data)
	{
		struct Expect
		{
			TileId id;
			std::size_t landVerts;
			float waterMinZ, waterMaxZ;  // local units, from PyNifly
		};
		const Expect expect[]{
			{ { 8, 0, 0 }, 7272, -759, -631 },
			{ { 8, 8, 0 }, 7382, -631, -631 },
			{ { 4, 12, 0 }, 14331, -1262, -1262 },
		};
		for (const auto& e : expect) {
			std::vector<std::uint8_t> bytes;
			const auto                path = TileMeshPath("tamriel", e.id);
			if (!ReadFile(a_data, path, bytes)) {
				Check(false, ("read " + path).c_str());
				continue;
			}
			BtrData     btr;
			std::string error;
			const bool  ok = ParseBtr(bytes, btr, error);
			Check(ok, ("parse " + path + " " + error).c_str());
			if (!ok)
				continue;
			const float lvl = static_cast<float>(e.id.lvl);
			float       lo[3]{ 1e30f, 1e30f, 1e30f }, hi[3]{ -1e30f, -1e30f, -1e30f };
			for (const auto& v : btr.land[0].verts) {
				for (int i = 0; i < 3; ++i) {
					lo[i] = std::min(lo[i], v[i] / lvl);
					hi[i] = std::max(hi[i], v[i] / lvl);
				}
			}
			float wlo = 1e30f, whi = -1e30f;
			std::size_t wverts = 0;
			for (const auto& w : btr.water) {
				wverts += w.verts.size();
				for (const auto& v : w.verts) {
					wlo = std::min(wlo, v[2] / lvl);
					whi = std::max(whi, v[2] / lvl);
				}
			}
			std::string types;
			for (const auto& t : btr.blockTypes)
				types += t + " ";
			std::printf("%s: blocks %zu [%s]\n  land verts %zu tris %zu  local x %.0f..%.0f y %.0f..%.0f z %.1f..%.1f\n  water shapes %zu verts %zu z %.1f..%.1f\n",
				path.c_str(), btr.blockTypes.size(), types.c_str(), btr.land[0].verts.size(), btr.land[0].tris.size(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
				btr.water.size(), wverts, wlo, whi);
			Check(btr.land.size() == 1 && btr.land[0].verts.size() == e.landVerts, "land vertex count matches PyNifly");
			Check(lo[0] >= -0.5f && hi[0] <= 4096.5f && lo[1] >= -0.5f && hi[1] <= 4096.5f, "land local x/y within 0..4096");
			Check(!btr.water.empty() && Near(wlo, e.waterMinZ, 1.0f) && Near(whi, e.waterMaxZ, 1.0f), "water z matches PyNifly");
		}

		// Garbage must be rejected, not crash.
		std::vector<std::uint8_t> junk(300, 0x41);
		BtrData                   btr;
		std::string               error;
		Check(!ParseBtr(junk, btr, error), "junk rejected");
		std::vector<std::uint8_t> bytes;
		if (ReadFile(a_data, TileMeshPath("tamriel", { 8, 0, 0 }), bytes)) {
			bool allRejectedOrOk = true;
			for (std::size_t cut : { std::size_t(10), std::size_t(200), bytes.size() / 2, bytes.size() - 20 }) {
				std::vector<std::uint8_t> part(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
				allRejectedOrOk = allRejectedOrOk && !ParseBtr(part, btr, error);
			}
			Check(allRejectedOrOk, "truncated btr rejected");
		}
	}

	bool SaveGray(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const std::vector<std::uint8_t>& a_pixels, int a_w, int a_h, const fs::path& a_file)
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = static_cast<UINT>(a_w);
		td.Height = static_cast<UINT>(a_h);
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA init{ a_pixels.data(), static_cast<UINT>(a_w), 0 };
		ComPtr<ID3D11Texture2D>      tex;
		return SUCCEEDED(a_dev->CreateTexture2D(&td, &init, &tex)) &&
		       SUCCEEDED(DirectX::SaveWICTextureToFile(a_ctx, tex.Get(), GUID_ContainerFormatPng, a_file.c_str(), &GUID_WICPixelFormat8bppGray));
	}

	bool SaveColor(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex, const fs::path& a_file)
	{
		return SUCCEEDED(DirectX::SaveWICTextureToFile(a_ctx, a_tex, GUID_ContainerFormatPng, a_file.c_str(), &GUID_WICPixelFormat24bppBGR));
	}

	// Level-8 tiles x -8..16, y -16..8 (the Whiterun area), same as the M1 reference picture.
	std::vector<TileId> WhiterunTiles()
	{
		std::vector<TileId> tiles;
		for (int y = -16; y <= 8; y += 8)
			for (int x = -8; x <= 16; x += 8)
				tiles.push_back({ 8, x, y });
		return tiles;
	}

	void TestWaterMosaic(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const fs::path& a_data, const fs::path& a_out)
	{
		constexpr int             G = 256;
		std::vector<std::uint8_t> mosaic(16 * G * G, 0);
		std::size_t               water = 0;
		for (const auto& id : WhiterunTiles()) {
			std::vector<std::uint8_t> bytes;
			BtrData                   btr;
			std::string               error;
			if (!ReadFile(a_data, TileMeshPath("tamriel", id), bytes) || !ParseBtr(bytes, btr, error)) {
				Check(false, "whiterun btr parse");
				continue;
			}
			const auto mask = RasterizeWater(btr, G, id.lvl * kCellUnits);
			const int  col = (id.x + 8) / 8, row = (8 - id.y) / 8;
			for (int y = 0; y < G; ++y)
				for (int x = 0; x < G; ++x) {
					const auto v = mask[static_cast<std::size_t>(y) * G + x];
					mosaic[static_cast<std::size_t>(row * G + y) * (4 * G) + col * G + x] = v;
					water += v ? 1 : 0;
				}
		}
		std::printf("water mosaic 1024x1024: water fraction %.5f\n", static_cast<double>(water) / mosaic.size());
		Check(water > 0, "some water in the Whiterun area");
		Check(SaveGray(a_dev, a_ctx, mosaic, 4 * G, 4 * G, a_out / "water_mask_1024.png"), "save water mask png");
	}

	void TestRender(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const fs::path& a_data, const fs::path& a_out)
	{
		MapRenderer renderer;
		std::string error;
		Check(renderer.Init(a_dev, error), ("renderer init " + error).c_str());
		if (!renderer.Ready())
			return;

		TileCache cache(a_dev, [&](const std::string& a_path, std::vector<std::uint8_t>& a_bytes) { return ReadFile(a_data, a_path, a_bytes); }, 256u << 20);
		cache.SetWorld("Tamriel");
		auto tiles = WhiterunTiles();
		tiles.push_back({ 8, 4000, 4000 });  // does not exist
		cache.Want(tiles, 1024);
		cache.WaitIdle();
		Check(cache.Collect(), "tiles arrived");
		for (const auto& msg : cache.TakeLog())
			std::printf("loader: %s\n", msg.c_str());

		std::vector<TileDraw> draws;
		int                   normals = 0, waters = 0;
		for (const auto& id : WhiterunTiles()) {
			const Tile* t = cache.Find(id);
			Check(t && !t->absent, "Whiterun tile loaded");
			if (t && !t->absent) {
				draws.push_back({ id, t->diffuse.Get(), t->normal.Get(), t->water.Get() });
				normals += t->normal ? 1 : 0;
				waters += t->water ? 1 : 0;
			}
		}
		const Tile* missing = cache.Find({ 8, 4000, 4000 });
		Check(missing && missing->absent, "missing tile marked absent");
		std::printf("cache: %zu tiles, %.1f MB, %d with normals, %d with water\n", cache.Count(), cache.Bytes() / 1048576.0, normals, waters);
		Check(normals == 16, "all normals loaded");

		// Budget eviction keeps the tiles asked to keep.
		{
			TileCache budgetCache(a_dev, [&](const std::string& a_path, std::vector<std::uint8_t>& a_bytes) { return ReadFile(a_data, a_path, a_bytes); }, 4u << 20);
			budgetCache.SetWorld("tamriel");
			budgetCache.Want(WhiterunTiles(), 1024);
			budgetCache.WaitIdle();
			budgetCache.Collect();
			const std::vector<TileId> keep{ { 8, 0, 0 }, { 8, 8, 0 } };
			budgetCache.Trim(keep);
			Check(budgetCache.Bytes() <= (4u << 20) || budgetCache.Count() <= 3, "trim to budget");
			Check(budgetCache.Find({ 8, 0, 0 }) && budgetCache.Find({ 8, 8, 0 }), "trim keeps wanted tiles");
			std::printf("budget test: %zu tiles left, %.1f MB\n", budgetCache.Count(), budgetCache.Bytes() / 1048576.0);
		}

		// Whole area at 64 units/px into 2048 px: compare with the M1 reference by eye.
		ComposeParams cp;
		cp.center = { 32768.0f, 0.0f };
		cp.unitsPerPixel = 64.0f;
		cp.targetSize = 2048;
		renderer.Compose(a_ctx, cp, draws);
		Check(SaveColor(a_ctx, renderer.ComposeTexture(), a_out / "whiterun_area.png"), "save whiterun_area.png");

		// Minimap-sized compose near Whiterun, then three overlays: north up round, rotated square, radar.
		const float size = 256.0f;
		const float upp = UnitsPerPixel(6000.0f, size);
		cp.center = { 19000.0f, -7000.0f };
		cp.unitsPerPixel = upp;
		cp.targetSize = ComposeTargetSize(static_cast<int>(size));
		renderer.Compose(a_ctx, cp, draws);

		D3D11_TEXTURE2D_DESC td{};
		td.Width = 3 * 300;
		td.Height = 300;
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D>        tex;
		ComPtr<ID3D11RenderTargetView> rtv;
		if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(a_dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
			Check(false, "preview target");
			return;
		}
		const float grey[4]{ 0.35f, 0.35f, 0.38f, 1.0f };
		a_ctx->ClearRenderTargetView(rtv.Get(), grey);
		a_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
		const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(td.Width), static_cast<float>(td.Height), 0, 1 };
		a_ctx->RSSetViewports(1, &vp);

		const float   heading = 0.6f;  // player facing roughly north-east
		OverlayParams op;
		op.size = size;
		op.viewportW = static_cast<float>(td.Width);
		op.viewportH = static_cast<float>(td.Height);
		op.arrowSize = 11.0f;
		op.opacity = 0.9f;
		op.offsetPx = { 0.4f, -0.3f };

		D3DStateBlock* saved = new D3DStateBlock;
		saved->Backup(a_ctx);
		op.left = 22;
		op.top = 22;
		op.mapAngle = 0;
		op.arrowAngle = heading;
		renderer.DrawOverlay(a_ctx, op);
		op.left = 322;
		op.round = false;
		op.mapAngle = heading;
		op.arrowAngle = 0;
		renderer.DrawOverlay(a_ctx, op);
		op.left = 622;
		op.round = true;
		op.hasMap = false;
		renderer.DrawOverlay(a_ctx, op);
		saved->Restore(a_ctx);
		delete saved;

		// The state block must hand back exactly what was bound before.
		ComPtr<ID3D11RenderTargetView> after;
		a_ctx->OMGetRenderTargets(1, &after, nullptr);
		Check(after.Get() == rtv.Get(), "state restored (render target)");

		Check(SaveColor(a_ctx, tex.Get(), a_out / "overlay_preview.png"), "save overlay_preview.png");
	}

	// ---- M4: icons ----

	void TestIconMath()
	{
		const float pi = std::numbers::pi_v<float>;
		IconView    v;
		v.player = { 1000, 1000 };
		v.unitsPerPixel = 10;
		v.sizePx = 200;
		v.centerPx = { 500, 300 };

		// North up: north is screen up, east is screen right.
		Vec2 q = ProjectToMinimap({ 1000, 1100 }, v);
		Check(Near(q.x, 0) && Near(q.y, -10), "icon: north-up, north is up");
		q = ProjectToMinimap({ 1100, 1000 }, v);
		Check(Near(q.x, 10) && Near(q.y, 0), "icon: north-up, east is right");
		// Facing east with a rotating map: east is up, north is left.
		v.mapAngle = pi / 2;
		q = ProjectToMinimap({ 1100, 1000 }, v);
		Check(Near(q.x, 0) && Near(q.y, -10), "icon: rotated, heading is up");
		q = ProjectToMinimap({ 1000, 1100 }, v);
		Check(Near(q.x, -10) && Near(q.y, 0), "icon: rotated, north is left");
		// Facing south-west (225 deg): a point south-west is straight up, at the right distance.
		v.mapAngle = pi * 1.25f;
		q = ProjectToMinimap({ 1000 - 70.7107f, 1000 - 70.7107f }, v);
		Check(Near(q.x, 0, 1e-2f) && Near(q.y, -10, 1e-2f), "icon: rotated 225, heading is up");
		// Same as the map shader: screen -> map -> screen returns the projected point.
		const Vec2 back = ScreenToMapOffset(q, v.mapAngle);
		Check(Near(back.x * 10 + 1000, 1000 - 70.7107f, 0.05f) && Near(back.y * 10 + 1000, 1000 - 70.7107f, 0.05f), "icon projection matches map shader rotation");

		// Mask tests and rim clamping.
		Check(InsideMask({ 70, 70 }, 100, true) && !InsideMask({ 75, 75 }, 100, true), "round mask inside/outside");
		Check(InsideMask({ 99, 99 }, 100, false) && !InsideMask({ 101, 0 }, 100, false), "square mask inside/outside");
		Check(!InsideMask({ 95, 0 }, 100, true, 8) && InsideMask({ 92, 0 }, 100, true, 8), "mask margin");
		Vec2 c = ClampToRim({ 300, 0 }, 100, true, 8);
		Check(Near(c.x, 92) && Near(c.y, 0), "rim clamp round, east");
		c = ClampToRim({ -300, 400 }, 100, true, 10);
		Check(Near(c.x, -54) && Near(c.y, 72), "rim clamp round keeps direction");
		c = ClampToRim({ 300, 150 }, 100, false, 8);
		Check(Near(c.x, 92) && Near(c.y, 46), "rim clamp square");
		c = ClampToRim({ 10, -20 }, 100, true, 8);
		Check(Near(c.x, 10) && Near(c.y, -20), "rim clamp leaves inside points alone");

		// Instance building: far normal icons hide, quest icons clamp to the rim.
		v.mapAngle = 0;
		v.iconScale = 1.5f;
		std::vector<IconSource> src;
		src.push_back({ { 1000, 1500 }, IconShape::kCircle, PackRgba(255, 0, 0), 8, 0, false });     // 50 px north: visible
		src.push_back({ { 1000, 2500 }, IconShape::kCircle, PackRgba(255, 0, 0), 8, 0, false });     // 150 px: off the map, hidden
		src.push_back({ { 1750, 1750 }, IconShape::kCircle, PackRgba(255, 0, 0), 8, 0, false });     // 106 px diagonal: inside square only
		src.push_back({ { 900000, 1000 }, IconShape::kStar, PackRgba(255, 200, 0), 16, 0, true });  // far east quest target
		std::vector<IconInstance> out;
		BuildIconInstances(src, v, out);
		Check(out.size() == 2, "round: 2 of 4 icons visible");
		Check(out.size() == 2 && Near(out[0].x, 500) && Near(out[0].y, 250) && Near(out[0].size, 12), "visible icon placed and scaled");
		Check(out.size() == 2 && Near(out[1].x, 500 + 100 - (16 * 1.5f * 0.5f + 1)) && Near(out[1].y, 300) && out[1].uv.u0 == AtlasUv(IconShape::kStar).u0,
			"quest target clamped to the rim");
		v.round = false;
		out.clear();
		BuildIconInstances(src, v, out);
		Check(out.size() == 3, "square: corner icon visible");
		const auto arrow = ArrowInstance(v, 20, 0.5f, 0xFFFFFFFF);
		Check(arrow.clip == 0.0f && Near(arrow.x, 500) && Near(arrow.y, 300) && Near(arrow.rotation, 0.5f), "arrow instance at centre, unclipped");

		// Marker type -> icon class (MARKER_TYPE values from ExtraMapMarker.h).
		Check(ClassifyMapMarker(1) == MarkerClass::kCity && ClassifyMapMarker(40) == MarkerClass::kCity && ClassifyMapMarker(52) == MarkerClass::kCity, "markers: cities");
		Check(ClassifyMapMarker(2) == MarkerClass::kSettlement && ClassifyMapMarker(13) == MarkerClass::kSettlement && ClassifyMapMarker(28) == MarkerClass::kSettlement,
			"markers: settlements");
		Check(ClassifyMapMarker(4) == MarkerClass::kDungeon && ClassifyMapMarker(7) == MarkerClass::kDungeon && ClassifyMapMarker(12) == MarkerClass::kDungeon &&
				  ClassifyMapMarker(15) == MarkerClass::kDungeon,
			"markers: dungeons");
		Check(ClassifyMapMarker(0) == MarkerClass::kOther && ClassifyMapMarker(11) == MarkerClass::kOther && ClassifyMapMarker(18) == MarkerClass::kOther &&
				  ClassifyMapMarker(99) == MarkerClass::kOther,
			"markers: other");
		Check(ShapeForMarkerClass(MarkerClass::kCity) == IconShape::kCity && ShapeForMarkerClass(MarkerClass::kDungeon) == IconShape::kCave &&
				  ShapeForMarkerClass(MarkerClass::kSettlement) == IconShape::kHouse && ShapeForMarkerClass(MarkerClass::kOther) == IconShape::kDiamond,
			"marker class -> shape");
		Check(PackRgba(1, 2, 3, 4) == 0x04030201u, "rgba packing (R first in memory)");
	}

	void TestSchedule()
	{
		// Fake clock: 60 s at 60 fps with the plugin's intervals. Each job must run at its own rate, never per frame.
		Schedule s;
		const int markers = s.Add(2.0), quest = s.Add(0.25), actors = s.Add(0.2), doors = s.Add(0.5);
		int       runs[4]{};
		int       busyFrames = 0, maxPerFrame = 0;
		const int frames = 3600;
		for (int f = 0; f < frames; ++f) {
			const auto due = s.Tick(f / 60.0);
			int        n = 0;
			for (int j = 0; j < 4; ++j) {
				if (due & (1u << j)) {
					++runs[j];
					++n;
				}
			}
			busyFrames += n ? 1 : 0;
			maxPerFrame = std::max(maxPerFrame, n);
		}
		std::printf("schedule over 60 s @60 fps: markers %d, quest %d, actors %d, doors %d runs; %d of %d frames run any job\n", runs[markers], runs[quest], runs[actors],
			runs[doors], busyFrames, frames);
		Check(runs[markers] >= 30 && runs[markers] <= 31, "markers every 2 s");
		Check(runs[quest] >= 239 && runs[quest] <= 241, "quest targets every 250 ms");
		Check(runs[actors] >= 299 && runs[actors] <= 301, "actors every 200 ms");
		Check(runs[doors] >= 119 && runs[doors] <= 121, "door check every 500 ms");
		Check(busyFrames < frames / 5, "most frames run no job");

		// A long pause (menu, loading) gives one run per job, not a burst of catch-up runs.
		const double t0 = 100.0;
		auto         due = s.Tick(t0);
		Check(due == 0xF, "after a pause every job runs once");
		due = s.Tick(t0 + 1.0 / 60.0);
		Check(due == 0, "and not again on the next frame");
		s.Force(markers);
		due = s.Tick(t0 + 2.0 / 60.0);
		Check(due == (1u << markers), "forced job runs on the next tick");
		due = s.Tick(t0 + 3.0 / 60.0);
		Check((due & (1u << markers)) == 0, "forced job then waits a full interval");
	}

	void TestAtlas()
	{
		const AtlasImage img = BuildIconAtlas();
		Check(img.size == kAtlasSize && img.mips.size() == static_cast<std::size_t>(kAtlasMips), "atlas size and mips");
		bool sizes = true;
		for (std::size_t i = 0; i < img.mips.size(); ++i)
			sizes = sizes && img.mips[i].size() == static_cast<std::size_t>((kAtlasSize >> i) * (kAtlasSize >> i) * 4);
		Check(sizes, "atlas mip byte sizes");
		bool covered = true;
		for (int s = 0; s < static_cast<int>(IconShape::kCount); ++s) {
			const int cx = (s % kAtlasGrid) * kAtlasCellPx, cy = (s / kAtlasGrid) * kAtlasCellPx;
			double    a = 0;
			bool      edgeClear = true;
			for (int y = 0; y < kAtlasCellPx; ++y)
				for (int x = 0; x < kAtlasCellPx; ++x) {
					const auto alpha = img.mips[0][(static_cast<std::size_t>(cy + y) * kAtlasSize + cx + x) * 4 + 3];
					a += alpha / 255.0;
					if ((x == 0 || y == 0 || x == kAtlasCellPx - 1 || y == kAtlasCellPx - 1) && alpha > 8)
						edgeClear = false;
				}
			const double frac = a / (kAtlasCellPx * kAtlasCellPx);
			if (frac < 0.08 || frac > 0.85 || !edgeClear) {
				std::printf("atlas cell %d: coverage %.2f, edge clear %d\n", s, frac, edgeClear);
				covered = false;
			}
		}
		Check(covered, "every icon cell drawn, with a clear border (no bleeding)");
		const auto* centre = &img.mips[0][(static_cast<std::size_t>(32) * kAtlasSize + 32) * 4];
		Check(centre[0] > 240 && centre[3] == 255, "circle centre is opaque white (tintable)");
	}

	struct Pixel
	{
		int r, g, b;
	};

	std::vector<std::uint8_t> ReadBack(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex, UINT& a_pitch)
	{
		D3D11_TEXTURE2D_DESC td{};
		a_tex->GetDesc(&td);
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		td.MiscFlags = 0;
		ComPtr<ID3D11Texture2D> staging;
		std::vector<std::uint8_t> out;
		if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &staging)))
			return out;
		a_ctx->CopyResource(staging.Get(), a_tex);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(a_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
			return out;
		a_pitch = m.RowPitch;
		out.assign(static_cast<const std::uint8_t*>(m.pData), static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(m.RowPitch) * td.Height);
		a_ctx->Unmap(staging.Get(), 0);
		return out;
	}

	void TestIconRender(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const fs::path& a_data, const fs::path& a_out)
	{
		MapRenderer renderer;
		std::string error;
		Check(renderer.Init(a_dev, error), ("renderer init (icons) " + error).c_str());
		if (!renderer.Ready())
			return;
		Check(SaveColor(a_ctx, renderer.AtlasTexture(), a_out / "icon_atlas.png"), "save icon_atlas.png");

		TileCache cache(a_dev, [&](const std::string& a_path, std::vector<std::uint8_t>& a_bytes) { return ReadFile(a_data, a_path, a_bytes); }, 256u << 20);
		cache.SetWorld("Tamriel");
		cache.Want(WhiterunTiles(), 1024);
		cache.WaitIdle();
		cache.Collect();
		std::vector<TileDraw> draws;
		for (const auto& id : WhiterunTiles())
			if (const Tile* t = cache.Find(id); t && !t->absent)
				draws.push_back({ id, t->diffuse.Get(), t->normal.Get(), t->water.Get() });

		const float size = 256.0f;
		const float upp = UnitsPerPixel(6000.0f, size);
		const Vec2  player{ 19000.0f, -7000.0f };
		ComposeParams cp;
		cp.center = player;
		cp.unitsPerPixel = upp;
		cp.targetSize = ComposeTargetSize(static_cast<int>(size));
		renderer.Compose(a_ctx, cp, draws);

		D3D11_TEXTURE2D_DESC td{};
		td.Width = 3 * 300;
		td.Height = 300;
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D>        tex;
		ComPtr<ID3D11RenderTargetView> rtv;
		if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(a_dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
			Check(false, "icon preview target");
			return;
		}
		const float grey[4]{ 0.35f, 0.35f, 0.38f, 1.0f };
		a_ctx->ClearRenderTargetView(rtv.Get(), grey);
		a_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
		const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(td.Width), static_cast<float>(td.Height), 0, 1 };
		a_ctx->RSSetViewports(1, &vp);

		// Made-up sample positions around the compose centre (not read from Skyrim.esm), in world units.
		auto at = [&](float a_eastPx, float a_northPx) { return Vec2{ player.x + a_eastPx * upp, player.y + a_northPx * upp }; };
		const std::uint32_t city = PackRgba(245, 230, 140), town = PackRgba(240, 230, 200), cave = PackRgba(200, 180, 160), other = PackRgba(200, 200, 200);
		const std::uint32_t red = PackRgba(230, 50, 40), green = PackRgba(80, 220, 80), gold = PackRgba(255, 200, 48), door = PackRgba(232, 200, 128);
		std::vector<IconSource> world{
			{ at(55, 25), IconShape::kCity, city, 16, 0, false },
			{ at(-70, -45), IconShape::kHouse, town, 14, 0, false },
			{ at(-30, 85), IconShape::kCave, cave, 13, 0, false },
			{ at(90, -60), IconShape::kDiamond, other, 11, 0, false },
			{ at(-95, 30), IconShape::kDiamond, PackRgba(200, 200, 200, 128), 11, 0, false },  // undiscovered: faded
			{ at(20, -80), IconShape::kPin, PackRgba(100, 200, 255), 15, 0, false },
			{ at(0, 40), IconShape::kCircle, red, 7, 0, false },        // hostile 40 px north
			{ at(40, 0), IconShape::kCircle, green, 7, 0, false },      // follower 40 px east
			{ at(-12, -18), IconShape::kCircle, green, 7, 0, false },
			{ at(0, 140), IconShape::kCircle, red, 7, 0, false },       // off the map: must not draw
			{ at(0, 122), IconShape::kCircle, other, 24, 0, false },     // on the edge: partly clipped by the mask
			{ at(5000, 0), IconShape::kStar, gold, 17, 0, true },       // quest far east: on the rim
		};
		std::vector<IconSource> radar{
			{ at(-50, 40), IconShape::kDoor, door, 11, 0, false },
			{ at(70, -20), IconShape::kDoor, door, 11, 0, false },
			{ at(30, 60), IconShape::kCircle, red, 7, 0, false },
			{ at(-20, -30), IconShape::kCircle, green, 7, 0, false },
			{ at(-40, -70), IconShape::kCross, PackRgba(160, 140, 140), 8, 0, false },
			{ at(-3000, -3000), IconShape::kStar, gold, 17, 0, true },  // quest door south-west
		};

		const float heading = 0.6f;
		auto        panel = [&](float a_left, bool a_round, float a_mapAngle, bool a_hasMap, const std::vector<IconSource>& a_src) {
            OverlayParams op;
            op.left = a_left;
            op.top = 22;
            op.size = size;
            op.viewportW = static_cast<float>(td.Width);
            op.viewportH = static_cast<float>(td.Height);
            op.mapAngle = a_mapAngle;
            op.arrowAngle = heading - a_mapAngle;
            op.opacity = 0.9f;
            op.arrowSize = 0;
            op.round = a_round;
            op.hasMap = a_hasMap;
            renderer.DrawOverlay(a_ctx, op);

            IconView iv;
            iv.player = player;
            iv.unitsPerPixel = upp;
            iv.mapAngle = a_mapAngle;
            iv.sizePx = size;
            iv.round = a_round;
            iv.centerPx = { a_left + size * 0.5f, 22 + size * 0.5f };
            iv.iconScale = 1.0f;
            std::vector<IconInstance> inst;
            BuildIconInstances(a_src, iv, inst);
            inst.push_back(ArrowInstance(iv, 11.0f * 2.5f, op.arrowAngle, PackRgba(255, 247, 230)));
            renderer.DrawIcons(a_ctx, { op.viewportW, op.viewportH, iv.centerPx, size, a_round, op.opacity }, inst);
            return inst.size();
		};

		D3DStateBlock* saved = new D3DStateBlock;
		saved->Backup(a_ctx);
		const auto n1 = panel(22, true, 0.0f, true, world);
		panel(322, false, std::numbers::pi_v<float> / 2, true, world);  // facing east, map turned
		panel(622, true, 0.0f, false, radar);                           // interior radar
		saved->Restore(a_ctx);
		delete saved;
		Check(n1 == world.size() - 1 + 1, "panel 1: every icon but the off-map one, plus the arrow");

		ComPtr<ID3D11RenderTargetView> after;
		a_ctx->OMGetRenderTargets(1, &after, nullptr);
		Check(after.Get() == rtv.Get(), "state restored after icon pass");
		Check(SaveColor(a_ctx, tex.Get(), a_out / "icons_preview.png"), "save icons_preview.png");

		UINT       pitch = 0;
		const auto px = ReadBack(a_dev, a_ctx, tex.Get(), pitch);
		if (px.empty()) {
			Check(false, "read back icon preview");
			return;
		}
		auto get = [&](int a_x, int a_y) {
			const auto* p = &px[static_cast<std::size_t>(a_y) * pitch + static_cast<std::size_t>(a_x) * 4];
			return Pixel{ p[0], p[1], p[2] };
		};
		auto isRed = [](Pixel a_p) { return a_p.r > 150 && a_p.g < 110 && a_p.b < 110; };
		auto isGreen = [](Pixel a_p) { return a_p.g > 150 && a_p.r < 130 && a_p.b < 130; };
		auto isGold = [](Pixel a_p) { return a_p.r > 180 && a_p.g > 130 && a_p.b < 110; };
		auto isGrey = [](Pixel a_p) { return std::abs(a_p.r - 89) <= 3 && std::abs(a_p.g - 89) <= 3 && std::abs(a_p.b - 97) <= 3; };

		// Panel 1, centre (150,150), north up.
		const Pixel hostile = get(150, 110), follower = get(190, 150), rim = get(150 + 128 - 10, 150), arrow = get(150, 150), offMap = get(150, 10), clipped = get(150, 19);
		std::printf("icon pixels: hostile %d,%d,%d follower %d,%d,%d rim %d,%d,%d arrow %d,%d,%d offmap %d,%d,%d clipped %d,%d,%d\n", hostile.r, hostile.g, hostile.b,
			follower.r, follower.g, follower.b, rim.r, rim.g, rim.b, arrow.r, arrow.g, arrow.b, offMap.r, offMap.g, offMap.b, clipped.r, clipped.g, clipped.b);
		Check(isRed(hostile), "hostile dot 40 px north drawn red");
		Check(isGreen(follower), "follower dot 40 px east drawn green");
		Check(isGold(rim), "far quest target drawn on the rim");
		Check(arrow.r > 200 && arrow.g > 200 && arrow.b > 180, "player arrow at the centre");
		Check(isGrey(offMap), "off-map icon not drawn");
		Check(isGrey(clipped), "icon on the edge clipped by the mask");
		// Panel 2, centre (450,150), facing east: the follower (east) is straight up.
		Check(isGreen(get(450, 110)), "rotated map: east follower drawn above the arrow");
		Check(!isGreen(get(490, 150)), "rotated map: nothing left at the north-up spot");
		// Panel 3, radar: the quest target south-west sits on the rim, lower left.
		const float d = (128.0f - 9.5f) / std::numbers::sqrt2_v<float>;
		Check(isGold(get(750 - static_cast<int>(d), 150 + static_cast<int>(d))), "radar: quest door clamped to the rim, south-west");

		// More icons than the instance buffer holds: the last one (the player arrow) must still be drawn.
		D3D11_TEXTURE2D_DESC sd = td;
		sd.Width = sd.Height = 64;
		ComPtr<ID3D11Texture2D>        ovfTex;
		ComPtr<ID3D11RenderTargetView> ovfRtv;
		if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &ovfTex)) || FAILED(a_dev->CreateRenderTargetView(ovfTex.Get(), nullptr, &ovfRtv))) {
			Check(false, "overflow test target");
			return;
		}
		a_ctx->ClearRenderTargetView(ovfRtv.Get(), grey);
		a_ctx->OMSetRenderTargets(1, ovfRtv.GetAddressOf(), nullptr);
		const D3D11_VIEWPORT svp{ 0, 0, 64, 64, 0, 1 };
		a_ctx->RSSetViewports(1, &svp);
		IconView sv;
		sv.centerPx = { 32, 32 };
		sv.sizePx = 64;
		IconInstance filler = ArrowInstance(sv, 6, 0, PackRgba(230, 50, 40));
		filler.x = filler.y = 8;
		std::vector<IconInstance> many(MapRenderer::kMaxIcons + 50, filler);
		many.push_back(ArrowInstance(sv, 24, 0, PackRgba(255, 247, 230)));
		renderer.DrawIcons(a_ctx, { 64, 64, sv.centerPx, 64, true, 1.0f }, many);
		UINT        spitch = 0;
		const auto  spx = ReadBack(a_dev, a_ctx, ovfTex.Get(), spitch);
		const auto* sc = spx.empty() ? nullptr : &spx[static_cast<std::size_t>(34) * spitch + 32 * 4];
		Check(sc && sc[0] > 200 && sc[1] > 200 && sc[2] > 180, "icon overflow: player arrow still drawn");
	}

	// ---- Interior map from the navmesh ----

	void TestNavMesh()
	{
		// Square 0..100 split into two triangles sharing the diagonal 2-0.
		const std::vector<float> sq{ 0, 0, 0, 100, 0, 0, 100, 100, 0, 0, 100, 0 };
		const std::uint16_t      none = kNavNoNeighbour;
		std::vector<NavTriangleIn> tris{
			{ { 0, 1, 2 }, { none, none, 1 }, 0 },  // edge 2 (2 -> 0) borders triangle 1
			{ { 0, 2, 3 }, { 0, none, none }, 0 },  // edge 0 (0 -> 2) borders triangle 0
		};
		NavMeshData d;
		AppendNavmesh(d, sq, tris);
		Check(d.tris.size() == 6 && d.walls.size() == 8, "nav: two triangles sharing an edge -> 4 wall edges");
		bool diagonal = false;
		for (std::size_t i = 0; i + 1 < d.walls.size(); i += 2) {
			const auto a = d.walls[i], b = d.walls[i + 1];
			diagonal |= (a == 0 && b == 2) || (a == 2 && b == 0);
		}
		Check(!diagonal, "nav: shared edge is not a wall");
		Check(d.minX == 0 && d.maxX == 100 && d.minY == 0 && d.maxY == 100, "nav: bounds");

		tris[0].flags = kNavEdge0Link;  // edge 0 -> 1 continues in another navmesh
		NavMeshData linked;
		AppendNavmesh(linked, sq, tris);
		Check(linked.walls.size() == 6, "nav: link flag suppresses a wall");

		tris[0].flags = 0;
		tris[1].flags = kNavDeleted;
		NavMeshData del;
		AppendNavmesh(del, sq, tris);
		Check(del.tris.size() == 3 && del.walls.size() == 6, "nav: deleted triangle skipped, its neighbour edge becomes a wall");

		tris[1].flags = 0;
		tris[0].n[2] = 7;  // past the end of the list: no neighbour
		tris[1].v[2] = 9;  // bad vertex index: triangle skipped
		NavMeshData bad;
		AppendNavmesh(bad, sq, tris);
		Check(bad.tris.size() == 3 && bad.walls.size() == 6, "nav: out-of-range neighbour is a wall, bad vertex index skipped");

		// Second navmesh appended: indices offset by the first one's vertex count.
		NavMeshData two;
		const std::vector<NavTriangleIn> one{ { { 0, 1, 2 }, { none, none, none }, 0 } };
		AppendNavmesh(two, sq, one);
		AppendNavmesh(two, sq, one);
		Check(two.VertexCount() == 8 && two.tris[3] == 4 && two.walls.size() == 12, "nav: second navmesh indices offset");

		// Floor band: default 120 below, 220 above.
		Check(InFloorBand(0, 0, 120, 220) && InFloorBand(-120, 0, 120, 220) && InFloorBand(220, 0, 120, 220), "floor band: inside and edges");
		Check(!InFloorBand(-121, 0, 120, 220) && !InFloorBand(221, 0, 120, 220) && !InFloorBand(400, 0, 120, 220), "floor band: outside");
		Check(InFloorBand(510, 400, 120, 220) && !InFloorBand(0, 400, 120, 220), "floor band follows the player");
	}

	void AddQuad(std::vector<float>& a_xyz, std::vector<NavTriangleIn>& a_tris, float a_x0, float a_y0, float a_x1, float a_y1, float a_z)
	{
		const auto b = static_cast<std::uint16_t>(a_xyz.size() / 3);
		for (const float v : { a_x0, a_y0, a_z, a_x1, a_y0, a_z, a_x1, a_y1, a_z, a_x0, a_y1, a_z })
			a_xyz.push_back(v);
		const auto     t = static_cast<std::uint16_t>(a_tris.size());
		const auto     none = kNavNoNeighbour;
		a_tris.push_back({ { b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 2) }, { none, none, static_cast<std::uint16_t>(t + 1) }, 0 });
		a_tris.push_back({ { b, static_cast<std::uint16_t>(b + 2), static_cast<std::uint16_t>(b + 3) }, { t, none, none }, 0 });
	}

	void TestNavRender(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const fs::path& a_out)
	{
		MapRenderer renderer;
		std::string error;
		Check(renderer.Init(a_dev, error), ("renderer init (nav) " + error).c_str());
		Check(renderer.NavReady(), ("nav pipeline ready " + renderer.NavError()).c_str());
		if (!renderer.Ready() || !renderer.NavReady())
			return;

		// Two floors: ground floor (z 0) west of x = 0 plus a corridor going east at the south; upper floor (z 400) east of x = 0.
		std::vector<float>         xyz;
		std::vector<NavTriangleIn> tris;
		AddQuad(xyz, tris, -800, -800, 0, 800, 0);
		AddQuad(xyz, tris, 0, -800, 800, 800, 400);
		NavMeshData mesh;
		AppendNavmesh(mesh, xyz, tris);
		mesh.generation = 1;
		Check(mesh.walls.size() == 16, "nav: two separate rooms -> 8 wall edges");
		Check(renderer.SetNavMesh(mesh) && renderer.HasNavMesh(1), "nav mesh uploaded");
		Check(renderer.SetNavMesh(mesh), "nav mesh same generation: kept");

		const float size = 300.0f;
		const int   target = ComposeTargetSize(static_cast<int>(size));
		NavComposeParams p;
		p.center = { 0, 0 };
		p.unitsPerPixel = 8.0f;  // 800 units = 100 px
		p.targetSize = target;
		p.playerZ = 10.0f;
		p.wallWidthPx = 2.0f;
		renderer.ComposeNav(a_ctx, p);
		Check(SaveColor(a_ctx, renderer.ComposeTexture(), a_out / "nav_compose.png"), "save nav_compose.png");

		UINT pitch = 0;
		auto px = ReadBack(a_dev, a_ctx, renderer.ComposeTexture(), pitch);
		if (px.empty()) {
			Check(false, "read back nav compose");
			return;
		}
		auto get = [&](int a_x, int a_y) {
			const auto* q = &px[static_cast<std::size_t>(a_y) * pitch + static_cast<std::size_t>(a_x) * 4];
			return Pixel{ q[0], q[1], q[2] };
		};
		auto is = [](Pixel a_p, const float (&a_c)[3]) {
			return std::abs(a_p.r - static_cast<int>(a_c[0] * 255 + 0.5f)) <= 3 && std::abs(a_p.g - static_cast<int>(a_c[1] * 255 + 0.5f)) <= 3 &&
			       std::abs(a_p.b - static_cast<int>(a_c[2] * 255 + 0.5f)) <= 3;
		};
		const int  c = target / 2;
		const auto west = get(c - 50, c), east = get(c + 50, c), westWall = get(c - 100, c), outside = get(c - 130, c);
		std::printf("nav pixels: own floor %d,%d,%d other floor %d,%d,%d wall %d,%d,%d outside %d,%d,%d\n", west.r, west.g, west.b, east.r, east.g, east.b, westWall.r,
			westWall.g, westWall.b, outside.r, outside.g, outside.b);
		Check(is(west, kNavFloor), "nav: player's floor drawn");
		Check(is(east, kNavClear), "nav: other floor hidden");
		Check(is(westWall, kNavWall), "nav: outer wall drawn");
		Check(is(outside, kNavClear), "nav: outside the navmesh is background");
		Check(is(get(c - 50, c - 100), kNavWall) && is(get(c - 50, c + 100), kNavWall), "nav: north and south walls (north up)");
		Check(!is(get(c + 50, c - 100), kNavWall), "nav: upper floor walls hidden");

		// Player upstairs: the other floor swaps in.
		p.playerZ = 380.0f;
		renderer.ComposeNav(a_ctx, p);
		px = ReadBack(a_dev, a_ctx, renderer.ComposeTexture(), pitch);
		Check(!px.empty() && is(get(c + 50, c), kNavFloor) && is(get(c - 50, c), kNavClear), "nav: upstairs shows only the upper floor");

		// Faint other floors.
		p.playerZ = 10.0f;
		p.otherFloorAlpha = 0.5f;
		renderer.ComposeNav(a_ctx, p);
		px = ReadBack(a_dev, a_ctx, renderer.ComposeTexture(), pitch);
		const auto faint = px.empty() ? Pixel{} : get(c + 50, c);
		Check(!px.empty() && faint.r > static_cast<int>(kNavClear[0] * 255) + 3 && faint.r < static_cast<int>(kNavFloor[0] * 255) - 3 && is(get(c - 50, c), kNavFloor),
			"nav: other floor drawn faint when asked");

		// Preview: own floor only, through the real overlay pass (rotated) with a door and the arrow on top.
		p.otherFloorAlpha = 0.0f;
		p.center = { -150.0f, 100.0f };
		renderer.ComposeNav(a_ctx, p);

		D3D11_TEXTURE2D_DESC td{};
		td.Width = 2 * 340;
		td.Height = 340;
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D>        tex;
		ComPtr<ID3D11RenderTargetView> rtv;
		if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(a_dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
			Check(false, "nav preview target");
			return;
		}
		const float grey[4]{ 0.35f, 0.35f, 0.38f, 1.0f };
		a_ctx->ClearRenderTargetView(rtv.Get(), grey);
		a_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
		const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(td.Width), static_cast<float>(td.Height), 0, 1 };
		a_ctx->RSSetViewports(1, &vp);
		for (int i = 0; i < 2; ++i) {
			OverlayParams op;
			op.left = 20.0f + 340.0f * i;
			op.top = 20;
			op.size = size;
			op.viewportW = static_cast<float>(td.Width);
			op.viewportH = static_cast<float>(td.Height);
			op.mapAngle = i == 0 ? 0.0f : 0.6f;
			op.arrowAngle = 0.6f - op.mapAngle;
			op.opacity = 0.9f;
			op.arrowSize = 0;
			renderer.DrawOverlay(a_ctx, op);
			IconView iv;
			iv.player = p.center;
			iv.unitsPerPixel = p.unitsPerPixel;
			iv.mapAngle = op.mapAngle;
			iv.sizePx = size;
			iv.centerPx = { op.left + size * 0.5f, op.top + size * 0.5f };
			std::vector<IconInstance> inst;
			const std::vector<IconSource> src{ { Vec2{ -800.0f, 0.0f }, IconShape::kDoor, PackRgba(232, 200, 128), 11, 0, false } };
			BuildIconInstances(src, iv, inst);
			inst.push_back(ArrowInstance(iv, std::max(7.0f, size * 0.045f) * 2.5f, op.arrowAngle, PackRgba(255, 247, 230)));
			renderer.DrawIcons(a_ctx, { op.viewportW, op.viewportH, iv.centerPx, size, true, op.opacity }, inst);
		}
		Check(SaveColor(a_ctx, tex.Get(), a_out / "nav_preview.png"), "save nav_preview.png");

		// Empty mesh: nothing to draw.
		NavMeshData empty;
		empty.generation = 2;
		Check(!renderer.SetNavMesh(empty) && !renderer.HasNavMesh(2), "empty nav mesh -> fall back to the radar disc");
	}

	void TestWorkerException(ID3D11Device* a_dev)
	{
		// A reader that throws must not kill the process (std::terminate on the worker); the tile comes back absent.
		TileCache cache(a_dev, [](const std::string&, std::vector<std::uint8_t>&) -> bool { throw std::runtime_error("disk gone"); }, 16u << 20);
		cache.SetWorld("Tamriel");
		cache.Want({ TileId{ 8, 0, 0 } }, 1024);
		cache.WaitIdle();
		const bool  arrived = cache.Collect();
		const Tile* t = cache.Find({ 8, 0, 0 });
		const auto  log = cache.TakeLog();
		Check(arrived && t && t->absent && log.size() == 1, "worker exception -> tile absent and logged");
	}

	// In-game photo bake: crop geometry, colour fit, BC1 encoder and the .dds it writes.
	void TestBakeImage()
	{
		namespace BI = BakeImage;
		Check(Near(BI::BakeFovDeg(1920, 1080), 7.7f) && Near(BI::BakeFovDeg(1920, 1200), 7.7f) && BI::BakeFovDeg(2560, 1080) > 7.7f, "bake FOV by aspect");

		// Synthetic 1920x1080 block photo: each cell of the 2x2 block a flat colour, placed as the bake crops it.
		BI::RgbImage img;
		img.w = 1920;
		img.h = 1080;
		img.px.assign(static_cast<std::size_t>(img.w) * img.h * 3, 0.0f);
		const double cellPx = kCellUnits / BI::UnitsPerPixel(img.w, 7.7f);
		Check(cellPx > 500 && cellPx < 520, "about 508 photo px per cell at 1080p");
		const float colours[2][2][3]{ { { 200, 0, 0 }, { 0, 200, 0 } }, { { 0, 0, 200 }, { 200, 200, 0 } } };  // [dy][dx]
		for (int y = 0; y < img.h; ++y) {
			for (int x = 0; x < img.w; ++x) {
				const int dx = x < img.w / 2 ? 0 : 1, dy = y < img.h / 2 ? 1 : 0;  // top half = north = dy 1
				for (int c = 0; c < 3; ++c)
					img.px[(static_cast<std::size_t>(y) * img.w + x) * 3 + c] = colours[dy][dx][c];
			}
		}
		bool cropOk = true;
		for (int dy = 0; dy < 2; ++dy) {
			for (int dx = 0; dx < 2; ++dx) {
				const auto  tile = BI::CropCell(img, 7.7f, dx, dy);
				const auto* mid = tile.data() + (static_cast<std::size_t>(256) * 512 + 256) * 3;
				for (int c = 0; c < 3; ++c)
					cropOk &= std::abs(mid[c] - static_cast<int>(colours[dy][dx][c])) <= 1;
			}
		}
		Check(cropOk, "bake crop picks the right quarter for each cell");

		// Identical photo and reference: the fit is an identity curve with no tint.
		std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> pairs;
		std::vector<std::uint8_t> sample(64 * 64 * 3);
		for (std::size_t i = 0; i < sample.size(); ++i)
			sample[i] = static_cast<std::uint8_t>((i * 37) % 256);
		pairs.emplace_back(sample, sample);
		const auto tone = BI::FitMatch(pairs);
		Check(tone.fitted && Near(tone.lut[128], 128.0f, 2.0f) && Near(tone.gain[0], 1.0f) && Near(tone.gain[2], 1.0f), "colour fit of identical images is identity");

		// BC1 round trip on a smooth gradient block.
		float px[16][3];
		for (int i = 0; i < 16; ++i)
			for (int c = 0; c < 3; ++c)
				px[i][c] = static_cast<float>(40 + i * 3 + c * 20);  // 45 levels across the block, like ground texture
		std::uint8_t block[8], back[16][3];
		BI::EncodeBc1Block(px, block);
		BI::DecodeBc1Block(block, back);
		int worst = 0;
		for (int i = 0; i < 16; ++i)
			for (int c = 0; c < 3; ++c)
				worst = std::max(worst, std::abs(back[i][c] - static_cast<int>(px[i][c])));
		std::printf("bake: BC1 gradient worst error %d\n", worst);
		Check(worst <= 9, "BC1 encoder error on a gradient");
	}

	void TestBakeDds(ID3D11Device* a_dev)
	{
		std::vector<std::uint8_t> rgb(512 * 512 * 3);
		for (std::size_t i = 0; i < rgb.size(); ++i)
			rgb[i] = static_cast<std::uint8_t>(i % 251);
		const auto dds = BakeImage::EncodeDdsBc1(rgb, 512);
		Check(dds.size() == 128 + 174776, "BC1 dds size with 10 mips");
		ComPtr<ID3D11ShaderResourceView> srv;
		const HRESULT                    hr = DirectX::CreateDDSTextureFromMemory(a_dev, dds.data(), dds.size(), nullptr, &srv);
		D3D11_SHADER_RESOURCE_VIEW_DESC  d{};
		if (srv)
			srv->GetDesc(&d);
		Check(SUCCEEDED(hr) && d.Format == DXGI_FORMAT_BC1_UNORM && d.Texture2D.MipLevels == 10, "bake dds loads as BC1 with 10 mips (DirectXTK)");
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: ModernMinimapTests <data folder> [output folder]\n");
		return 2;
	}
	const fs::path data = argv[1];
	const fs::path out = argc > 2 ? fs::path(argv[2]) : fs::current_path();
	fs::create_directories(out);

	TestMath();
	TestIconMath();
	TestSchedule();
	TestAtlas();
	TestNavMesh();
	TestBakeImage();
	TestBtr(data);

	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		Check(false, "CoInitializeEx");
	ComPtr<ID3D11Device>        dev;
	ComPtr<ID3D11DeviceContext> ctx;
	const D3D_FEATURE_LEVEL     levels[]{ D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	const char* driver = "hardware";
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		driver = "WARP";
	}
	Check(SUCCEEDED(hr), "create D3D11 device");
	if (SUCCEEDED(hr)) {
		std::printf("D3D11 device: %s\n", driver);
		TestWaterMosaic(dev.Get(), ctx.Get(), data, out);
		TestRender(dev.Get(), ctx.Get(), data, out);
		TestIconRender(dev.Get(), ctx.Get(), data, out);
		TestNavRender(dev.Get(), ctx.Get(), out);
		TestWorkerException(dev.Get());
		TestBakeDds(dev.Get());
	}

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
