// Offline checks for Modern Minimap: maths, btr parsing on real LOD files, and the tile/overlay
// passes rendered to PNG through the same D3D11 code the plugin uses.
//
// Usage: ModernMinimapTests <data folder with textures\terrain and meshes\terrain> <output folder>

#include "BakePlan.h"
#include "BakeRules.h"
#include "FogOfWar.h"
#include "MasterTone.h"
#include "PhotoProcessing.h"
#include "TerrainMesh.h"
#include "Icons.h"
#include "MapMath.h"
#include "MapRenderer.h"
#include "InteriorMap.h"
#include "TileCache.h"
#include "TileFormats.h"

#include <DDSTextureLoader.h>
#include <ScreenGrab.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
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

		// Map styles: natural is unchanged, black and white and parchment have no colour, a bad value falls back.
		const MapLook natural = LookForStyle(MapStyle::kNatural);
		Check(natural.saturation == 1.0f && natural.contrast == 1.0f && natural.tintR == 1.0f && natural.tintB == 1.0f, "natural style is unchanged");
		Check(LookForStyle(MapStyle::kBlackWhite).saturation == 0.0f && LookForStyle(MapStyle::kParchment).saturation == 0.0f, "grey styles");
		Check(LookForStyle(MapStyle::kParchment).tintR > LookForStyle(MapStyle::kParchment).tintB, "parchment is warm");
		Check(LookForStyle(static_cast<MapStyle>(99)).saturation == 1.0f, "unknown style falls back to natural");

		// Auto zoom: no change up to a run, full strength at a gallop, never below 1, bad input = no change.
		Check(AutoZoomTarget(0, 1.5f) == 1.0f && AutoZoomTarget(300, 1.5f) == 1.0f, "auto zoom: standing and running unchanged");
		Check(Near(AutoZoomTarget(900, 1.5f), 2.5f) && Near(AutoZoomTarget(5000, 1.5f), 2.5f), "auto zoom: gallop = 1 + strength");
		Check(AutoZoomTarget(600, 1.0f) > 1.0f && AutoZoomTarget(600, 1.0f) < 2.0f, "auto zoom: sprint in between");
		Check(AutoZoomTarget(900, 0.0f) == 1.0f && AutoZoomTarget(NAN, 1.0f) == 1.0f && AutoZoomTarget(900, NAN) == 1.0f, "auto zoom: off or bad input");
		const float out = EaseAutoZoom(1.0f, 2.0f, 0.1f), back = EaseAutoZoom(2.0f, 1.0f, 0.1f);
		Check(out > 1.0f && out < 2.0f && back < 2.0f && back > 1.0f && (out - 1.0f) > (2.0f - back), "auto zoom eases, out faster than in");
		Check(EaseAutoZoom(1.3f, 2.0f, 0.0f) == 1.3f && EaseAutoZoom(NAN, 2.0f, 0.1f) == 2.0f, "auto zoom: no time or bad state");
		float eased = 1.0f;
		for (int frame = 0; frame < 600; ++frame)
			eased = EaseAutoZoom(eased, 2.0f, 1.0f / 60.0f);
		Check(Near(eased, 2.0f, 0.01f), "auto zoom reaches its target");
		Check(QuantizeZoom(1.01f) == 1.0f && QuantizeZoom(1.03f) == 1.03125f, "zoom factor in 1/32 steps");
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

	// Whole-map bake on the real terrain LOD in the data folder: how many cells count as land.
	void TestFullMapOnLod(const fs::path& a_data)
	{
		std::set<std::pair<int, int>> land;
		int                           meshes = 0;
		for (int ty = -64; ty < 64; ty += 4) {
			for (int tx = -64; tx < 64; tx += 4) {
				const TileId              id{ 4, tx, ty };
				std::vector<std::uint8_t> bytes;
				BtrData                   btr;
				std::string               error;
				if (!ReadFile(a_data, TileMeshPath("tamriel", id), bytes) || !ParseBtr(bytes, btr, error))
					continue;
				++meshes;
				land.merge(BakePlan::LandCells(btr, id));
			}
		}
		std::printf("whole map: %d level-4 meshes, %zu cells with land (of %d)\n", meshes, land.size(), meshes * 16);
		if (meshes < 300)
			return;  // not a full Tamriel terrain LOD set
		Check(land.size() > 5000 && land.size() < static_cast<std::size_t>(meshes) * 16, "whole map: thousands of land cells, open sea left out");
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

		// The four map styles side by side (natural, muted, black and white, parchment), same compose.
		a_ctx->ClearRenderTargetView(rtv.Get(), grey);
		D3D11_TEXTURE2D_DESC sd = td;
		sd.Width = 4 * 300;
		ComPtr<ID3D11Texture2D>        styleTex;
		ComPtr<ID3D11RenderTargetView> styleRtv;
		if (FAILED(a_dev->CreateTexture2D(&sd, nullptr, &styleTex)) || FAILED(a_dev->CreateRenderTargetView(styleTex.Get(), nullptr, &styleRtv))) {
			Check(false, "style preview target");
			return;
		}
		a_ctx->ClearRenderTargetView(styleRtv.Get(), grey);
		a_ctx->OMSetRenderTargets(1, styleRtv.GetAddressOf(), nullptr);
		const D3D11_VIEWPORT svp{ 0, 0, static_cast<float>(sd.Width), static_cast<float>(sd.Height), 0, 1 };
		a_ctx->RSSetViewports(1, &svp);
		op.viewportW = static_cast<float>(sd.Width);
		op.hasMap = true;
		op.round = true;
		op.mapAngle = 0;
		op.arrowAngle = heading;
		for (int style = 0; style < static_cast<int>(MapStyle::kCount); ++style) {
			op.left = 22.0f + 300.0f * style;
			op.look = LookForStyle(static_cast<MapStyle>(style));
			renderer.DrawOverlay(a_ctx, op);
		}
		Check(SaveColor(a_ctx, styleTex.Get(), a_out / "style_preview.png"), "save style_preview.png");
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
		Check(CountsAsEnemy(true, 4.0f) && CountsAsEnemy(true, 1.0f), "actors: hostile and not cowardly is an enemy (wolf, bear)");
		Check(!CountsAsEnemy(true, 0.0f), "actors: hostile but cowardly prey is not an enemy (rabbit, deer)");
		Check(!CountsAsEnemy(false, 4.0f) && !CountsAsEnemy(false, 0.0f), "actors: not hostile is never an enemy");
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
			const double frac = a / (kAtlasInnerPx * kAtlasInnerPx);
			const double minFrac = s >= static_cast<int>(IconShape::kGlyphFirst) ? 0.005 : 0.08;  // glyphs such as '.' are small
			if (frac < minFrac || frac > 0.85 || !edgeClear) {
				std::printf("atlas cell %d: coverage %.2f, edge clear %d\n", s, frac, edgeClear);
				covered = false;
			}
		}
		Check(covered, "every icon cell drawn, with a clear border (no bleeding)");
		const auto* centre = &img.mips[0][(static_cast<std::size_t>(kAtlasGutterPx + 32) * kAtlasSize + kAtlasGutterPx + 32) * 4];
		Check(centre[0] > 240 && centre[3] == 255, "circle centre is opaque white (tintable)");
	}

	// ---- timed features: pure helpers ----

	void TestFeatureHelpers()
	{
		// Distance labels: 70 units per metre, km from 1000 m.
		Check(FormatDistance(0) == "0m" && FormatDistance(70.0f * 45) == "45m", "distance: metres");
		Check(FormatDistance(70.0f * 999.4f) == "999m" && FormatDistance(70.0f * 999.6f) == "1.0km", "distance: switches to km at 1000 m");
		Check(FormatDistance(70.0f * 1234) == "1.2km" && FormatDistance(70.0f * 25260) == "25.3km", "distance: km with one decimal");
		Check(FormatDistance(-50) == "0m", "distance: negative clamps to 0");

		// Dead body mode and the old bDead key.
		Check(DeadModeFromIni(-1, false) == DeadMode::kOff && DeadModeFromIni(-1, true) == DeadMode::kAll, "iDead missing: bDead decides (true = all)");
		Check(DeadModeFromIni(1, true) == DeadMode::kPlayerKills && DeadModeFromIni(0, true) == DeadMode::kOff, "iDead present wins over bDead");
		Check(DeadModeFromIni(9, false) == DeadMode::kAll, "iDead too large clamps to all");
		Check(!ShowsDeadBody(DeadMode::kOff, true) && ShowsDeadBody(DeadMode::kAll, false), "dead: off hides, all shows");
		Check(ShowsDeadBody(DeadMode::kPlayerKills, true) && !ShowsDeadBody(DeadMode::kPlayerKills, false), "dead: player kills only");

		// Hold to zoom out.
		Check(Near(HoldZoomRadius(3000, false, 3.0f), 3000) && Near(HoldZoomRadius(3000, true, 3.0f), 9000), "hold zoom: factor only while held");
		Check(Near(HoldZoomRadius(1000, true, 10.0f), 6000) && Near(HoldZoomRadius(1000, true, 1.0f), 1500), "hold zoom: factor clamped 1.5..6");
		Check(Near(HoldZoomRadius(1000, true, std::numeric_limits<float>::quiet_NaN()), 1500), "hold zoom: NaN factor is safe");

		// Trail: spacing, ring buffer order, wrap-around, fading.
		TrailBuffer trail;
		Check(trail.Offer({ 0, 0 }, 256) && !trail.Offer({ 100, 100 }, 256) && trail.Offer({ 300, 0 }, 256), "trail: points closer than the spacing are skipped");
		Check(trail.Size() == 2 && Near(trail.At(0).x, 0) && Near(trail.At(1).x, 300), "trail: oldest first");
		for (int i = 2; i < TrailBuffer::kCapacity + 10; ++i)
			trail.Offer({ 300.0f * static_cast<float>(i), 0 }, 256);
		Check(trail.Size() == TrailBuffer::kCapacity, "trail: ring buffer holds its capacity");
		Check(Near(trail.At(0).x, 300.0f * 10) && Near(trail.At(TrailBuffer::kCapacity - 1).x, 300.0f * (TrailBuffer::kCapacity + 9)), "trail: oldest dropped on wrap");
		Check(TrailBuffer::AgeAlpha(0, 4) < TrailBuffer::AgeAlpha(3, 4) && Near(TrailBuffer::AgeAlpha(3, 4), 1.0f), "trail: older points fainter, newest full");
		trail.Clear();
		Check(trail.Size() == 0 && trail.Offer({ 5, 5 }, 256), "trail: clear starts fresh");

		// Glyphs and text layout.
		IconShape g;
		Check(GlyphShape('0', g) && g == IconShape::kGlyphFirst, "glyph: '0' is the first glyph cell");
		Check(GlyphShape('z', g) && g == static_cast<IconShape>(static_cast<int>(IconShape::kCount) - 1), "glyph: 'z' is the last cell");
		Check(!GlyphShape(' ', g) && !GlyphShape('\xE9', g) && !GlyphShape('@', g), "glyph: space and unknown characters have none");
		// Per-glyph advances: wide m / w, narrow i / l / '.', a space as wide as an 'n'.
		auto advOf = [](char a_c) {
			IconShape shape;
			GlyphShape(a_c, shape);
			return 10.0f * GlyphAdvanceEm(shape);
		};
		Check(advOf('m') > advOf('n') * 1.25f && advOf('w') > advOf('v') * 1.25f && advOf('i') < advOf('n') * 0.6f && advOf('.') < advOf('n') * 0.6f,
			"text: m and w wider, i and . narrower than n");
		Check(advOf('o') == advOf('n') && Near(10.0f * SpaceAdvanceEm(), advOf('n')), "text: space advances like an n");
		Check(advOf('n') >= 10.0f * 0.55f, "text: n advance leaves a gap clear of the outline");
		std::vector<GlyphPlacement> line;
		const float w = LayoutText("45m", 10.0f, TextAlign::kCenter, line);
		const float a4 = advOf('4'), a5 = advOf('5'), am = advOf('m');
		Check(line.size() == 3 && Near(w, a4 + a5 + am) && Near(line[0].x, -w * 0.5f + a4 * 0.5f) && Near(line[1].x, -w * 0.5f + a4 + a5 * 0.5f) &&
				  Near(line[2].x, w * 0.5f - am * 0.5f),
			"text: centred line");
		line.clear();
		LayoutText("a b", 10.0f, TextAlign::kLeft, line);
		Check(line.size() == 2 && Near(line[0].x, advOf('a') * 0.5f) && Near(line[1].x, advOf('a') + advOf('n') + advOf('b') * 0.5f), "text: space advances without a glyph");
		line.clear();
		const float wr = LayoutText("x@y", 10.0f, TextAlign::kRight, line);
		Check(line.size() == 2 && Near(wr, advOf('x') + advOf('y')) && Near(line[1].x, -advOf('y') * 0.5f), "text: unknown characters skipped, right aligned");

		// Compass letters: N at the top with a north-up map, on the right when facing west; square edge.
		const float pi = std::numbers::pi_v<float>;
		IconView    v;
		v.sizePx = 200;
		v.centerPx = { 500, 300 };
		// Each letter: kOutlineCopies outline copies, then the letter itself.
		constexpr std::size_t perLetter = kOutlineCopies + 1;
		const auto            text = PackRgba(240, 240, 240), outline = PackRgba(0, 0, 0, 200);
		std::vector<IconInstance> letters;
		AppendCompassLetters(v, 10.0f, text, outline, CompassMode::kInside, letters);
		IconShape n;
		GlyphShape('N', n);
		const auto& nIn = letters.size() == 4 * perLetter ? letters[perLetter - 1] : IconInstance{};
		const auto& eIn = letters.size() == 4 * perLetter ? letters[2 * perLetter - 1] : IconInstance{};
		Check(letters.size() == 4 * perLetter && nIn.uv.u0 == AtlasUv(n).u0 && nIn.uv.v0 == AtlasUv(n).v0 && nIn.rgba == text, "compass: four outlined letters, N first");
		Check(Near(nIn.x, 500) && Near(nIn.y, 300 - (100 - 6 - 2)) && Near(nIn.size, 12) && nIn.clip == 1.0f, "compass inside: N just inside the top, larger, clipped");
		Check(Near(eIn.x, 500 + (100 - 5 - 2)) && Near(eIn.y, 300) && (eIn.rgba >> 24) < 255, "compass inside: E on the right, dimmer");
		Check(letters.size() == 4 * perLetter && letters[0].rgba == outline && letters[0].uv.u0 == nIn.uv.u0 && Near(letters[0].x, 499) && Near(letters[0].y, nIn.y - 1),
			"compass: outline copies first, in the outline colour, 1 px off");
		letters.clear();
		AppendCompassLetters(v, 10.0f, text, outline, CompassMode::kOutside, letters);
		const auto& nOut = letters.size() == 4 * perLetter ? letters[perLetter - 1] : IconInstance{};
		Check(Near(nOut.y, 300 - (100 + 6 + 2)) && nOut.clip == 0.0f, "compass outside: N just beyond the ring, not clipped");
		letters.clear();
		AppendCompassLetters(v, 10.0f, text, outline, CompassMode::kOff, letters);
		Check(letters.empty(), "compass off: nothing drawn");
		v.mapAngle = -pi / 2;  // facing west: north is to the right
		AppendCompassLetters(v, 10.0f, text, outline, CompassMode::kInside, letters);
		Check(letters.size() == 4 * perLetter && Near(letters[perLetter - 1].x, 500 + 92, 1e-2f) && Near(letters[perLetter - 1].y, 300, 1e-2f), "compass: N turns with the map");
		letters.clear();
		v.mapAngle = pi / 4;  // square map: the letter sits on the square edge, not a circle
		v.round = false;
		AppendCompassLetters(v, 10.0f, text, outline, CompassMode::kInside, letters);
		Check(letters.size() == 4 * perLetter && Near(std::fmax(std::fabs(letters[perLetter - 1].x - 500), std::fabs(letters[perLetter - 1].y - 300)), 92, 1e-2f),
			"compass: square shape uses the square edge");
		letters.clear();
		v.mapAngle = 0;
		AppendCompassLetters(v, 10.0f, text, outline, CompassMode::kOutside, letters);
		Check(letters.size() == 4 * perLetter && Near(letters[2 * perLetter - 1].x, 500 + 100 + 5 + 2) && Near(letters[2 * perLetter - 1].y, 300),
			"compass outside, square: E centred beyond the right side");
		Check(CompassModeFromIni(-1, -1) == CompassMode::kOutside && CompassModeFromIni(-1, 0) == CompassMode::kOff && CompassModeFromIni(-1, 1) == CompassMode::kOutside,
			"iCompassLetters missing: bCompassLetters false = off, true or missing = outside");
		Check(CompassModeFromIni(1, 0) == CompassMode::kInside && CompassModeFromIni(7, 1) == CompassMode::kOutside, "iCompassLetters present wins, clamped");
		Check(Near(CompassLetterDistance(100, 10, CompassMode::kInside), 93) && Near(CompassLetterDistance(100, 10, CompassMode::kOutside), 107), "compass letter distance");

		// Outlines: dark text on a light outline works the same way.
		std::vector<IconInstance> outlined;
		IconInstance              glyph;
		glyph.x = 10;
		glyph.y = 20;
		glyph.size = 28;
		glyph.rgba = PackRgba(20, 20, 20);
		AppendOutlinedGlyph(glyph, PackRgba(255, 255, 255, 200), outlined);
		Check(outlined.size() == perLetter && outlined.back().rgba == glyph.rgba && outlined[0].rgba == PackRgba(255, 255, 255, 200) && Near(outlined[3].x, 12) &&
				  Near(outlined[3].y, 22),
			"outline: copies scale with big glyphs, glyph on top");
		outlined.clear();
		AppendOutlinedGlyph(glyph, PackRgba(0, 0, 0, 0), outlined);
		Check(outlined.size() == 1, "outline: alpha 0 = no outline");

		// Dead body log from the death events.
		DeadBodyLog deaths;
		Check(deaths.Add(0x10, false) && !deaths.Add(0x10, true) && deaths.Entries().size() == 1 && deaths.Entries()[0].killedByPlayer,
			"dead log: second event for the same body merges the killer flag");
		for (std::uint32_t id = 0x100; id < 0x100 + DeadBodyLog::kCapacity; ++id)
			deaths.Add(id, false);
		Check(deaths.Entries().size() == static_cast<std::size_t>(DeadBodyLog::kCapacity) && deaths.Entries()[0].formId == 0x100, "dead log: capped, oldest out");
		deaths.RemoveIf([](const DeadBodyLog::Entry& a_e) { return a_e.formId < 0x110; });
		Check(deaths.Entries().size() == static_cast<std::size_t>(DeadBodyLog::kCapacity) - 16, "dead log: unloaded bodies removed");
		deaths.Clear();
		Check(deaths.Entries().empty(), "dead log: cleared on load");

		// Labels: offset under the icon, clamp like the icon, flip above near the bottom rim.
		IconView lv;
		lv.player = { 0, 0 };
		lv.unitsPerPixel = 10;
		lv.sizePx = 200;
		lv.centerPx = { 500, 300 };
		IconSource label;
		label.world = { 0, 200 };  // 20 px north
		label.shape = IconShape::kGlyphFirst;
		label.sizePx = 10;
		label.offsetPx = { 4, 12 };
		label.flipOffsetInside = true;
		std::vector<IconInstance> out;
		BuildIconInstances(std::span(&label, 1), lv, out);
		Check(out.size() == 1 && Near(out[0].x, 504) && Near(out[0].y, 300 - 20 + 12), "label: offset below its point");
		label.world = { 0, -900 };  // 90 px south: below would leave the map
		out.clear();
		BuildIconInstances(std::span(&label, 1), lv, out);
		Check(out.size() == 1 && Near(out[0].y, 300 + 90 - 12), "label: flips above near the bottom rim");
		label.world = { 90000, 0 };  // far east, clamps like a 20 px star
		label.clampToRim = true;
		label.anchorSizePx = 20;
		label.offsetPx = { 0, 12 };
		out.clear();
		BuildIconInstances(std::span(&label, 1), lv, out);
		Check(out.size() == 1 && Near(out[0].x, 500 + 100 - 11) && Near(out[0].y, 312), "label: clamps with its icon's margin");
		std::vector<IconSource> labelSrc;
		AppendTextLabel(labelSrc, "1.2km", { 90000, 0 }, 17, 10, true, 0xFFFFFFFF, 0);
		std::vector<GlyphPlacement> labelLine;
		const float                 labelWidth = LayoutText("1.2km", 10, TextAlign::kCenter, labelLine);
		Check(labelSrc.size() == 5 && labelSrc[0].anchorSizePx > labelWidth + 2 && Near(labelSrc[0].offsetPx.x, labelLine[0].x) &&
				  Near(labelSrc[0].offsetPx.x + labelSrc[4].offsetPx.x, labelLine[0].x + labelLine[4].x) && labelSrc[0].offsetPx.x < 0 && labelSrc[4].offsetPx.x > 0 &&
				  labelSrc[0].flipOffsetInside,
			"label: wide label clamps farther in, centred under its icon");
		out.clear();
		BuildIconInstances(labelSrc, lv, out);
		bool inside = out.size() == 5;
		for (const auto& inst : out)
			inside = inside && InsideMask({ inst.x - 500, inst.y - 300 }, 100, true, inst.size * 0.5f - 0.01f);
		Check(inside, "label: every glyph of a rim label stays inside the map");
		labelSrc.clear();
		AppendTextLabel(labelSrc, "45m", { 0, 0 }, 17, 10, false, PackRgba(255, 200, 48), PackRgba(0, 0, 0, 190));
		out.clear();
		BuildIconInstances(labelSrc, lv, out);
		Check(labelSrc.size() == 3 && labelSrc[0].outlineRgba == PackRgba(0, 0, 0, 190) && out.size() == 3 * perLetter && out[perLetter - 1].rgba == PackRgba(255, 200, 48),
			"label: quest distance glyphs drawn with their outline");
	}

	void TestBatch2Helpers()
	{
		// Vendor classification from the buy/sell list.
		Check(ClassifyVendor(true, 0) == ServiceKind::kGeneralGoods && ClassifyVendor(true, kGoodsWeaponsArmor) == ServiceKind::kGeneralGoods,
			"vendor: sells all but its list = general goods");
		Check(ClassifyVendor(false, kGoodsWeaponsArmor) == ServiceKind::kBlacksmith, "vendor: weapons/armour = blacksmith");
		Check(ClassifyVendor(false, kGoodsPotions) == ServiceKind::kAlchemist, "vendor: potions = alchemist");
		Check(ClassifyVendor(false, kGoodsFood) == ServiceKind::kInnkeeper, "vendor: food = innkeeper");
		Check(ClassifyVendor(false, kGoodsSpells | kGoodsPotions) == ServiceKind::kSpells, "vendor: spell tomes win over potions (court wizard)");
		Check(ClassifyVendor(false, kGoodsWeaponsArmor | kGoodsFood) == ServiceKind::kBlacksmith, "vendor: blacksmith wins over food");
		Check(ClassifyVendor(false, 0) == ServiceKind::kNone, "vendor: empty list = no service");
		Check(ShapeForService(ServiceKind::kBlacksmith) == IconShape::kAnvil && ShapeForService(ServiceKind::kInnkeeper) == IconShape::kMug &&
				  ShapeForService(ServiceKind::kAlchemist) == IconShape::kFlask && ShapeForService(ServiceKind::kGeneralGoods) == IconShape::kBag &&
				  ShapeForService(ServiceKind::kSpells) == IconShape::kBook,
			"service -> shape");

		// Clock.
		Check(FormatClock(13.09f, true) == "13:05" && FormatClock(7.5f, true) == "07:30" && FormatClock(0.0f, true) == "00:00", "clock: 24 h");
		Check(FormatClock(13.09f, false) == "1:05 PM" && FormatClock(0.25f, false) == "12:15 AM" && FormatClock(12.0f, false) == "12:00 PM", "clock: 12 h");
		Check(FormatClock(23.999f, true) == "23:59" && FormatClock(24.0f, true) == "00:00" && FormatClock(-1.0f, true) == "23:00", "clock: wraps around midnight");
		Check(FormatClock(std::numeric_limits<float>::quiet_NaN(), true) == "00:00", "clock: NaN is safe");
		Check(AllCharsHaveGlyphs("Dragon's Bridge") && AllCharsHaveGlyphs("Fort Dawnguard - East"), "location names: Latin letters, spaces, apostrophe, dash");
		Check(!AllCharsHaveGlyphs("Vinterh\xE4lla") && !AllCharsHaveGlyphs("Ruins (old)"), "location names: other characters -> clock only");

		// Fog of war bits.
		Fog::WorldMap map;
		Check(!map.Get(0, 0) && map.Set(0, 0) && map.Get(0, 0) && !map.Set(0, 0), "fog: set once, then already set");
		Check(map.Set(-1, -33) && map.Get(-1, -33) && !map.Get(-1, -32) && !map.Get(0, -33), "fog: negative blocks across chunk edges");
		Check(Fog::BlockOf(-0.5f) == -1 && Fog::BlockOf(1023.9f) == 0 && Fog::BlockOf(1024.0f) == 1, "fog: quarter-cell blocks");
		Fog::WorldMap reveal;
		const int flipped = reveal.RevealAround({ 512, 512 }, Fog::kRevealRadius);
		Check(flipped > 40 && flipped < 60 && reveal.Get(0, 0) && reveal.Get(4, 0) && !reveal.Get(5, 0) && !reveal.Get(4, 4), "fog: about one cell around the player");
		Check(reveal.RevealAround({ 512, 512 }, Fog::kRevealRadius) == 0, "fog: standing still flips nothing");

		// Mask window: snapped, row 0 = north.
		int ox = 0, oy = 0;
		Fog::MaskOrigin({ 512, 512 }, ox, oy);
		Check(ox == -Fog::kMaskSize / 2 && oy == -Fog::kMaskSize / 2, "fog mask: window centred on the snapped block");
		int ox2 = 0, oy2 = 0;
		Fog::MaskOrigin({ 512 + 31 * Fog::kBlockUnits, 512 }, ox2, oy2);
		Check(ox2 == ox, "fog mask: window does not move within a snap step");
		Fog::MaskOrigin({ 512 + 32 * Fog::kBlockUnits, 512 }, ox2, oy2);
		Check(ox2 == ox + Fog::kMaskSnap, "fog mask: window moves by one snap step");
		std::vector<std::uint8_t> mask;
		Fog::WorldMap one;
		one.Set(ox + 3, oy + Fog::kMaskSize - 1);  // north-west corner area
		one.BuildMask(ox, oy, Fog::kMaskSize, mask);
		Check(mask.size() == static_cast<std::size_t>(Fog::kMaskSize * Fog::kMaskSize) && mask[3] == 255 && std::count(mask.begin(), mask.end(), 255) == 1,
			"fog mask: north row first, one pixel per block");
		const Rect mr = Fog::MaskRect(ox, oy);
		Check(Near(mr.minX, ox * Fog::kBlockUnits) && Near(mr.maxY - mr.minY, Fog::kMaskSize * Fog::kBlockUnits), "fog mask: world rect");

		// Co-save round trip, load order changes and broken data.
		Fog::Store store;
		store.World(0x3C).Set(10, -20);
		store.World(0x0200ABCD).Set(-5, 7);
		const auto bytes = store.Serialize();
		Fog::Store loaded;
		auto       remap = [](std::uint32_t a_old, std::uint32_t& a_new) {
            if (a_old == 0x0200ABCD) {
                a_new = 0x0300ABCD;  // its plugin moved one slot
                return true;
            }
            a_new = a_old;
            return a_old == 0x3C;
		};
		Check(loaded.Deserialize(bytes, remap) && loaded.WorldCount() == 2 && loaded.Find(0x3C) && loaded.Find(0x3C)->Get(10, -20) && loaded.Find(0x0300ABCD) &&
				  loaded.Find(0x0300ABCD)->Get(-5, 7) && !loaded.Find(0x0200ABCD),
			"fog save: round trip with a moved plugin");
		auto dropSecond = [](std::uint32_t a_old, std::uint32_t& a_new) {
			a_new = a_old;
			return a_old == 0x3C;
		};
		Fog::Store dropped;
		Check(dropped.Deserialize(bytes, dropSecond) && dropped.WorldCount() == 1 && dropped.Find(0x3C), "fog save: a removed worldspace is dropped");
		Fog::Store broken;
		broken.World(1).Set(0, 0);
		const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
		Check(!broken.Deserialize(cut, remap) && broken.Find(1), "fog save: cut data is refused and keeps the old state");
		Check(loaded.Deserialize({}, remap) == false, "fog save: empty record refused");
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
			{ at(-60, 60), IconShape::kHorse, PackRgba(200, 160, 110), 14, 0, true },
		};
		// Distance label for the rim star, built the way IconData builds it.
		const std::size_t worldBeforeLabel = world.size();
		AppendTextLabel(world, FormatDistance(5000 * upp), at(5000, 0), 17, 13, true, gold, PackRgba(0, 0, 0, 190));
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
            AppendCompassLetters(iv, 13.0f, PackRgba(240, 240, 240), PackRgba(0, 0, 0, 190), CompassMode::kInside, inst);
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
		const std::size_t labelGlyphs = world.size() - worldBeforeLabel;
		Check(n1 == 4 * (kOutlineCopies + 1) + (world.size() - 1 - labelGlyphs) + labelGlyphs * (kOutlineCopies + 1) + 1 && labelGlyphs >= 3,
			"panel 1: outlined compass letters, every icon but the off-map one, the outlined label, the arrow");

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

	// Every glyph and some real place names at the sizes the plugin uses, dark-on-light and light-on-dark, for checking
	// readability by eye (text_preview.png).
	void TestTextPreview(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const fs::path& a_out)
	{
		MapRenderer renderer;
		std::string error;
		Check(renderer.Init(a_dev, error), ("renderer init (text) " + error).c_str());
		if (!renderer.Ready())
			return;
		constexpr UINT kW = 1000, kPanelH = 330;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = kW;
		td.Height = kPanelH;
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC sheetDesc = td;
		sheetDesc.Height = kPanelH * 2;
		sheetDesc.BindFlags = 0;
		ComPtr<ID3D11Texture2D> sheet;
		if (FAILED(a_dev->CreateTexture2D(&sheetDesc, nullptr, &sheet))) {
			Check(false, "text preview sheet");
			return;
		}
		struct Panel
		{
			float         bg[4];
			std::uint32_t text, outline;
		};
		const Panel panels[]{
			{ { 0.78f, 0.80f, 0.82f, 1 }, PackRgba(20, 20, 20), PackRgba(255, 255, 255, 200) },  // default: dark text, light outline (snow)
			{ { 0.16f, 0.17f, 0.15f, 1 }, PackRgba(240, 240, 240), PackRgba(0, 0, 0, 190) },     // light text, dark outline
		};
		const char* lines[]{ "0123456789.?:'-", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz", "Riverwood  Pinepeak Cavern  Whiterun",
			"Bleak Falls Barrow  Mzinchaleft  High Hrothgar", "12:04  1.2km  45m  N E S W" };
		constexpr float kSizes[]{ 17.0f, 18.0f };  // quest labels / compass, the clock line
		D3DStateBlock* saved = new D3DStateBlock;
		saved->Backup(a_ctx);
		for (UINT p = 0; p < 2; ++p) {
			ComPtr<ID3D11Texture2D>        tex;
			ComPtr<ID3D11RenderTargetView> rtv;
			if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(a_dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
				Check(false, "text preview target");
				break;
			}
			a_ctx->ClearRenderTargetView(rtv.Get(), panels[p].bg);
			a_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
			const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kW), static_cast<float>(kPanelH), 0, 1 };
			a_ctx->RSSetViewports(1, &vp);
			std::vector<IconInstance> inst;
			float                     y = 20.0f;
			for (const float size : kSizes) {
				for (const char* text : lines) {
					std::vector<GlyphPlacement> glyphs;
					LayoutText(text, size, TextAlign::kLeft, glyphs);
					for (const auto& g : glyphs) {
						IconInstance i;
						i.x = 16.0f + g.x;
						i.y = y;
						i.size = size;
						i.clip = 0.0f;
						i.uv = AtlasUv(g.shape);
						i.rgba = panels[p].text;
						AppendOutlinedGlyph(i, panels[p].outline, inst);
					}
					y += size * 0.75f;
				}
				y += 8.0f;
			}
			renderer.DrawIcons(a_ctx, { static_cast<float>(kW), static_cast<float>(kPanelH), { 0, 0 }, 10.0f, true, 1.0f }, inst);
			a_ctx->CopySubresourceRegion(sheet.Get(), 0, 0, p * kPanelH, 0, tex.Get(), 0, nullptr);
		}
		saved->Restore(a_ctx);
		delete saved;
		Check(SaveColor(a_ctx, sheet.Get(), a_out / "text_preview.png"), "save text_preview.png");
	}

	// Fog of war in the compose: unexplored land darker, explored land and town photos unchanged.
	void TestFogRender(ID3D11Device* a_dev, ID3D11DeviceContext* a_ctx, const fs::path& a_data, const fs::path& a_out)
	{
		MapRenderer renderer;
		std::string error;
		Check(renderer.Init(a_dev, error), ("renderer init (fog) " + error).c_str());
		if (!renderer.Ready())
			return;
		TileCache cache(a_dev, [&](const std::string& a_path, std::vector<std::uint8_t>& a_bytes) { return ReadFile(a_data, a_path, a_bytes); }, 256u << 20);
		cache.SetWorld("Tamriel");
		cache.Want(WhiterunTiles(), 1024);
		cache.WaitIdle();
		cache.Collect();
		std::vector<TileDraw> draws;
		for (const auto& id : WhiterunTiles())
			if (const Tile* t = cache.Find(id); t && !t->absent)
				draws.push_back({ id, t->diffuse.Get(), t->normal.Get(), t->water.Get() });

		ComposeParams cp;
		cp.center = { 19000.0f, -7000.0f };
		cp.unitsPerPixel = 64.0f;
		cp.targetSize = 512;
		renderer.Compose(a_ctx, cp, draws);
		UINT       pitch = 0;
		const auto plain = ReadBack(a_dev, a_ctx, renderer.ComposeTexture(), pitch);

		Fog::WorldMap map;
		map.RevealAround(cp.center, 6000.0f);
		int ox = 0, oy = 0;
		Fog::MaskOrigin(cp.center, ox, oy);
		std::vector<std::uint8_t> mask;
		map.BuildMask(ox, oy, Fog::kMaskSize, mask);
		Check(renderer.SetFogMask(a_ctx, mask, Fog::kMaskSize) && renderer.SetFogMask(a_ctx, mask, Fog::kMaskSize), "fog mask created, then updated in place");
		cp.fog = { true, Fog::MaskRect(ox, oy), 0.6f, false };
		renderer.Compose(a_ctx, cp, draws);
		Check(SaveColor(a_ctx, renderer.ComposeTexture(), a_out / "fog_preview.png"), "save fog_preview.png");
		const auto fogged = ReadBack(a_dev, a_ctx, renderer.ComposeTexture(), pitch);
		if (plain.empty() || fogged.size() != plain.size()) {
			Check(false, "read back fog compose");
			return;
		}
		auto lum = [&](const std::vector<std::uint8_t>& a_px, int a_x, int a_y) {
			const auto* p = &a_px[static_cast<std::size_t>(a_y) * pitch + static_cast<std::size_t>(a_x) * 4];
			return (p[0] + p[1] + p[2]) / 3.0f;
		};
		// Centre: explored. Corner (16 000 units out): unexplored, about 40 % brightness.
		const float centrePlain = lum(plain, 256, 256), centreFog = lum(fogged, 256, 256);
		const float cornerPlain = lum(plain, 20, 20), cornerFog = lum(fogged, 20, 20);
		std::printf("fog: centre %.0f -> %.0f, corner %.0f -> %.0f\n", centrePlain, centreFog, cornerPlain, cornerFog);
		Check(std::fabs(centrePlain - centreFog) <= 2.0f, "fog: explored land unchanged");
		Check(cornerPlain > 10.0f && std::fabs(cornerFog - cornerPlain * 0.4f) <= 4.0f, "fog: unexplored land darkened by fFogDarkness");
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
		const double cellPx = kCellUnits / BI::UnitsPerPixel(img.w, 7.7f, 2);
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
				const auto  tile = BI::CropCell(img, 7.7f, 2, dx, dy, BI::kDefaultTileSize);
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

	// Photo grid N x N: camera height, crop geometry and the colour reference for N = 1..4 (2 = the original bake).
	void TestBakeGridGeometry()
	{
		namespace BI = BakeImage;
		Check(BI::CameraHeight(2) == 115000.0f && BI::CameraHeight(1) == 57500.0f && BI::CameraHeight(3) == 172500.0f && BI::CameraHeight(4) == 230000.0f,
			"grid: camera height 115000 * N / 2");
		Check(BI::UnitsPerPixel(1920, 7.7f, 2) == 2.0 * 115000.0f * std::tan(7.7f * std::numbers::pi / 360.0) / 1920, "grid 2: units per pixel same as before");

		for (int grid = 1; grid <= 4; ++grid) {
			// Synthetic 1920x1080 photo: every cell of the block a flat colour, grey around it.
			BI::RgbImage img;
			img.w = 1920;
			img.h = 1080;
			img.px.assign(static_cast<std::size_t>(img.w) * img.h * 3, 128.0f);
			const double cellPx = kCellUnits / BI::UnitsPerPixel(img.w, 7.7f, grid);
			const double blockPx = cellPx * grid;
			const auto   colour = [](int a_dx, int a_dy, int a_c) { return static_cast<float>((a_dx * 61 + a_dy * 101 + a_c * 37) % 200 + 20); };
			for (int y = 0; y < img.h; ++y) {
				for (int x = 0; x < img.w; ++x) {
					const double ex = x + 0.5 - (img.w / 2.0 - blockPx / 2), ny = (img.h / 2.0 + blockPx / 2) - (y + 0.5);  // east / north of the SW corner
					if (ex < 0 || ny < 0 || ex >= blockPx || ny >= blockPx)
						continue;
					const int dx = static_cast<int>(ex / cellPx), dy = static_cast<int>(ny / cellPx);
					for (int c = 0; c < 3; ++c)
						img.px[(static_cast<std::size_t>(y) * img.w + x) * 3 + c] = colour(dx, dy, c);
				}
			}
			Check(blockPx > 1000 && blockPx <= 1080, std::format("grid {}: the block fits the 1080 px picture height", grid).c_str());
			bool cropOk = true;
			for (int dy = 0; dy < grid; ++dy) {
				for (int dx = 0; dx < grid; ++dx) {
					const auto tile = BI::CropCell(img, 7.7f, grid, dx, dy, BI::kDefaultTileSize);
					cropOk &= tile.size() == static_cast<std::size_t>(BI::kDefaultTileSize) * BI::kDefaultTileSize * 3;
					for (const auto& [tx, ty] : { std::pair{ 256, 256 }, std::pair{ 128, 128 }, std::pair{ 384, 384 } }) {
						const auto* px = tile.data() + (static_cast<std::size_t>(ty) * BI::kDefaultTileSize + tx) * 3;
						for (int c = 0; c < 3; ++c)
							cropOk &= std::abs(px[c] - static_cast<int>(colour(dx, dy, c))) <= 1;
					}
				}
			}
			Check(cropOk, std::format("grid {}: crop picks the right cell of the block", grid).c_str());

			// Colour reference: kSampleSize px per cell whatever the grid.
			const int refSize = BI::ReferenceSize(grid);
			Check(refSize == BI::kSampleSize * grid, std::format("grid {}: reference size {} px", grid, refSize).c_str());
			std::vector<std::uint8_t> rgba(static_cast<std::size_t>(refSize) * refSize * 4);
			for (int y = 0; y < refSize; ++y)
				for (int x = 0; x < refSize; ++x)
					for (int c = 0; c < 4; ++c)
						rgba[(static_cast<std::size_t>(y) * refSize + x) * 4 + c] =
							static_cast<std::uint8_t>(colour(x / BI::kSampleSize, grid - 1 - y / BI::kSampleSize, c % 3));  // row 0 = north
			bool refOk = true;
			for (int dy = 0; dy < grid; ++dy) {
				for (int dx = 0; dx < grid; ++dx) {
					const auto cell = BI::ReferenceCell(rgba, grid, dx, dy);
					refOk &= cell.size() == static_cast<std::size_t>(BI::kSampleSize) * BI::kSampleSize * 3;
					for (std::size_t i = 0; refOk && i < cell.size(); ++i)
						refOk &= cell[i] == static_cast<std::uint8_t>(colour(dx, dy, static_cast<int>(i % 3)));
				}
			}
			Check(refOk, std::format("grid {}: reference cell picks the right part of the render", grid).c_str());
		}
		Check(BI::ReferenceSize(2) == 128, "grid 2: reference render 128 px as before");
		const std::vector<std::uint8_t> shortRender(16);
		const auto                      blank = BI::ReferenceCell(shortRender, 2, 0, 0);
		Check(blank.size() == static_cast<std::size_t>(BI::kSampleSize) * BI::kSampleSize * 3 && std::ranges::all_of(blank, [](std::uint8_t a_v) { return a_v == 0; }),
			"reference cell of a too small render is blank, not a read past the end");
	}

	// Photo grid: block list alignment, block centres, reference tiles, and the progress file across grid changes.
	void TestBakePlan()
	{
		namespace BP = BakePlan;
		Check(BP::ClampGrid(0) == 1 && BP::ClampGrid(-5) == 1 && BP::ClampGrid(9) == 4 && BP::ClampGrid(3) == 3, "grid clamped to 1..4");
		Check(BP::BlockCentre(4, 2) == 5 * kCellUnits && BP::BlockCentre(-2, 2) == -1 * kCellUnits, "grid 2: block centre = shared corner, as before");
		Check(BP::BlockCentre(3, 3) == 4.5f * kCellUnits && BP::BlockCentre(-3, 1) == -2.5f * kCellUnits && BP::BlockCentre(-4, 4) == -2 * kCellUnits, "block centre for grids 1, 3, 4");

		const std::set<std::pair<int, int>> cells{ { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 0 }, { -1, -1 }, { 5, 7 }, { -3, 8 } };
		const auto                          blocks3 = BP::Blocks(cells, 3);
		const bool aligned3 = std::ranges::all_of(blocks3, [](const auto& a_b) { return FloorDiv(a_b.first, 3) * 3 == a_b.first && FloorDiv(a_b.second, 3) * 3 == a_b.second; });
		Check(aligned3, "grid 3: blocks at multiples of 3");
		const std::vector<std::pair<int, int>> want3{ { -3, 6 }, { 3, 6 }, { 0, 0 }, { 3, 0 }, { -3, -3 } };  // north to south, serpentine by row
		Check(blocks3 == want3, "grid 3: block list covers every cell once, north to south, serpentine");
		bool covered3 = true;
		for (const auto& [cx, cy] : cells)
			covered3 &= std::ranges::count_if(blocks3, [&](const auto& a_b) { return cx >= a_b.first && cx < a_b.first + 3 && cy >= a_b.second && cy < a_b.second + 3; }) == 1;
		Check(covered3, "grid 3: each cell in exactly one block");
		const std::vector<std::pair<int, int>> want2{ { -4, 8 }, { 4, 6 }, { 2, 2 }, { 0, 0 }, { 2, 0 }, { -2, -2 } };
		Check(BP::Blocks(cells, 2) == want2, "grid 2: even-aligned block list in the original order");
		const auto blocks1 = BP::Blocks(cells, 1);
		Check(blocks1.size() == cells.size(), "grid 1: one block per cell");
		Check(BP::Blocks({}, 3).empty(), "no cells -> no blocks");

		Check(BP::ReferenceTiles(2, -2, 2) == std::vector<TileId>{ { 4, 0, -4 } }, "grid 2: one reference LOD tile");
		Check(BP::ReferenceTiles(-4, 4, 4) == std::vector<TileId>{ { 4, -4, 4 } }, "grid 4: one reference LOD tile");
		Check(BP::ReferenceTiles(3, 3, 3) == std::vector<TileId>{ { 4, 0, 0 }, { 4, 4, 0 }, { 4, 0, 4 }, { 4, 4, 4 } }, "grid 3: block across four LOD tiles");
		Check(BP::ReferenceTiles(0, -3, 3).size() == 1 && BP::ReferenceTiles(-3, 0, 1).size() == 1, "grid 3 inside one tile / grid 1: one tile");

		// Progress file.
		BP::Progress saved;
		saved.grid = 2;
		saved.toneKnown = true;
		saved.tone.fitted = true;
		saved.tone.lut.fill(10.0f);
		saved.tone.gain = { 1.25f, 1.0f, 0.75f };
		saved.done = { BP::ShotKey("Tamriel", 4, -2), BP::ShotKey("WhiterunWorld", 6, 0) };
		std::ostringstream text;
		BP::WriteProgress(text, saved);
		Check(text.str().find("\ngrid 2\n") != std::string::npos, "progress file stores the grid");

		std::istringstream same(text.str());
		const auto         kept = BP::ParseProgress(same, 2);
		Check(kept.grid == 2 && kept.droppedGrid == 0 && kept.done == saved.done && kept.toneKnown && kept.tone.fitted && Near(kept.tone.gain[0], 1.25f) && Near(kept.tone.lut[200], 10.0f),
			"progress: same grid keeps the finished shots and the colour match");

		std::istringstream other(text.str());
		const auto         fresh = BP::ParseProgress(other, 3);
		Check(fresh.grid == 3 && fresh.droppedGrid == 2 && fresh.done.empty(), "progress: another grid drops the finished shots and names the old grid");
		Check(fresh.toneKnown && fresh.tone.fitted && Near(fresh.tone.gain[2], 0.75f), "progress: another grid keeps the colour match");
		std::ostringstream rewritten;
		BP::WriteProgress(rewritten, fresh);
		Check(rewritten.str().find("\ngrid 3\n") != std::string::npos && rewritten.str().find("done ") == std::string::npos, "progress: rewritten for the new grid, no old shots");

		const std::string  legacyText = "# old file\ndone Tamriel 4 -2\ndone Tamriel 6 -2\n";
		std::istringstream legacy2(legacyText), legacy4(legacyText);
		Check(BP::ParseProgress(legacy2, 2).done.size() == 2, "progress: a file without a grid line is a 2x2 bake");
		const auto legacyOther = BP::ParseProgress(legacy4, 4);
		Check(legacyOther.done.empty() && legacyOther.droppedGrid == 2, "progress: a file without a grid line does not match 4x4");

		std::istringstream empty;
		const auto         none = BP::ParseProgress(empty, 3);
		Check(none.grid == 3 && none.done.empty() && none.droppedGrid == 0 && !none.toneKnown, "progress: no file -> nothing dropped, nothing to report");
		std::istringstream junk("grid x\ndone Tamriel\nlut 1 2\n");
		const auto         junkRead = BP::ParseProgress(junk, 2);
		Check(junkRead.done.empty() && junkRead.droppedGrid == 0, "progress: broken lines are ignored");
	}


	// [Bake] iTileSize: clamping, a 1024 px crop, the lossless master .bmp, and photo pixels per cell.
	void TestBakeTileSize()
	{
		namespace BI = BakeImage;
		Check(BI::ClampTileSize(512) == 512 && BI::ClampTileSize(1024) == 1024 && BI::ClampTileSize(2048) == 2048, "tile size: 512, 1024 and 2048 kept");
		Check(BI::ClampTileSize(0) == 512 && BI::ClampTileSize(-7) == 512 && BI::ClampTileSize(256) == 512, "tile size: below 512 -> 512");
		Check(BI::ClampTileSize(700) == 512 && BI::ClampTileSize(1500) == 1024 && BI::ClampTileSize(2047) == 1024, "tile size: rounded down to a power of two");
		Check(BI::ClampTileSize(4096) == 2048 && BI::ClampTileSize(1 << 30) == 2048, "tile size: above 2048 -> 2048");
		Check(BI::ClampPhotoExposure(1.0f) == 1.0f && BI::ClampPhotoExposure(0.7f) == 0.7f && BI::ClampPhotoExposure(0.25f) == 0.25f, "photo exposure: 0.25..1 kept");
		Check(BI::ClampPhotoExposure(0.1f) == 0.25f && BI::ClampPhotoExposure(-1.0f) == 0.25f && BI::ClampPhotoExposure(0.0f) == 0.25f, "photo exposure: below 0.25 -> 0.25");
		Check(BI::ClampPhotoExposure(1.5f) == 1.0f && BI::ClampPhotoExposure(std::numeric_limits<float>::infinity()) == 1.0f, "photo exposure: above 1 -> 1");
		Check(BI::ClampPhotoExposure(std::numeric_limits<float>::quiet_NaN()) == 1.0f, "photo exposure: unreadable -> 1");

		const double px1 = BI::PhotoPixelsPerCell(1920, 7.7f, 1), px2 = BI::PhotoPixelsPerCell(1920, 7.7f, 2);
		Check(px1 > 1000 && px1 < 1080 && Near(static_cast<float>(px1 / px2), 2.0f), "photo px per cell: about 1016 at 1080p with the 1x1 grid, twice the 2x2 grid");
		Check(px1 < 1024 && px2 > 500 && px2 < 512, "photo px per cell: below both tile sizes at 1080p (the start warning fires)");

		// The 2x2 photo of TestBakeImage, cropped to 1024 px tiles.
		BI::RgbImage img;
		img.w = 1920;
		img.h = 1080;
		img.px.assign(static_cast<std::size_t>(img.w) * img.h * 3, 0.0f);
		const float colours[2][2][3]{ { { 200, 0, 0 }, { 0, 200, 0 } }, { { 0, 0, 200 }, { 200, 200, 0 } } };  // [dy][dx]
		for (int y = 0; y < img.h; ++y)
			for (int x = 0; x < img.w; ++x)
				for (int c = 0; c < 3; ++c)
					img.px[(static_cast<std::size_t>(y) * img.w + x) * 3 + c] = colours[y < img.h / 2 ? 1 : 0][x < img.w / 2 ? 0 : 1][c];
		bool cropOk = true;
		for (int dy = 0; dy < 2; ++dy) {
			for (int dx = 0; dx < 2; ++dx) {
				const auto tile = BI::CropCell(img, 7.7f, 2, dx, dy, 1024);
				cropOk &= tile.size() == std::size_t{ 1024 } * 1024 * 3;
				for (const auto& [tx, ty] : { std::pair{ 512, 512 }, std::pair{ 256, 768 }, std::pair{ 768, 256 } }) {
					const auto* p = tile.data() + (static_cast<std::size_t>(ty) * 1024 + tx) * 3;
					for (int c = 0; c < 3; ++c)
						cropOk &= std::abs(p[c] - static_cast<int>(colours[dy][dx][c])) <= 1;
				}
			}
		}
		Check(cropOk, "1024 px crop: right size and the right cell");

		// Master .bmp: 3x2 RGB image, rows padded to 4 bytes, bottom row first, BGR.
		const std::vector<std::uint8_t> rgb{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };  // top row: 3 px, bottom row: 3 px
		const auto                      bmp = BI::EncodeBmp24(rgb, 3, 2);
		const auto                      u32 = [&](std::size_t a_at) {
            std::uint32_t v = 0;
            std::memcpy(&v, bmp.data() + a_at, 4);
            return v;
		};
		Check(bmp.size() == 54 + 12 * 2 && bmp[0] == 'B' && bmp[1] == 'M' && u32(2) == bmp.size() && u32(10) == 54 && u32(14) == 40 && u32(18) == 3 && u32(22) == 2 &&
				  bmp[28] == 24 && u32(30) == 0,
			"master bmp: 24-bit BITMAPINFOHEADER file, padded rows");
		Check(bmp[54] == 12 && bmp[55] == 11 && bmp[56] == 10 && bmp[54 + 12] == 3 && bmp[54 + 12 + 2] == 1 && bmp[54 + 9] == 0, "master bmp: bottom row first, BGR, zero padding");
		Check(BI::EncodeBmp24(rgb, 4, 2).empty() && BI::EncodeBmp24(rgb, 0, 2).empty(), "master bmp: too little data or no size -> nothing");
		std::vector<std::uint8_t> big(std::size_t{ 1024 } * 1024 * 3, 77);
		Check(BI::EncodeBmp24(big, 1024, 1024).size() == 54 + std::size_t{ 1024 } * 1024 * 3, "master bmp: 1024 px tile is 3 MB, no padding");
	}

	BtrMesh FlatSquare(float a_x0, float a_y0, float a_x1, float a_y1, float a_z)
	{
		BtrMesh m;
		m.verts = { { a_x0, a_y0, a_z }, { a_x1, a_y0, a_z }, { a_x1, a_y1, a_z }, { a_x0, a_y1, a_z } };
		m.tris = { { 0, 1, 2 }, { 0, 2, 3 } };
		return m;
	}

	// Whole-map bake: which cells of a terrain LOD tile count as land.
	void TestFullMapCells()
	{
		namespace BP = BakePlan;
		const TileId tile{ 4, 8, -4 };
		const float  edge = 4 * kCellUnits;
		BtrData      btr;
		btr.land.push_back(FlatSquare(0, 0, edge, edge, 100.0f));
		Check(BP::LandCells(btr, tile).size() == 16, "land cells: dry tile -> all 16 cells");

		// Sea over the western half (above the land), and a lake under the land in the east (land wins).
		btr.water.push_back(FlatSquare(0, 0, 2 * kCellUnits, edge, 200.0f));
		btr.water.push_back(FlatSquare(2 * kCellUnits, 0, edge, edge, 50.0f));
		const auto cells = BP::LandCells(btr, tile);
		std::set<std::pair<int, int>> want;
		for (int cy = -4; cy < 0; ++cy)
			for (int cx = 10; cx < 12; ++cx)
				want.insert({ cx, cy });
		Check(cells == want, "land cells: open water is left out, land above water stays, cell coordinates from the tile corner");

		// An island one cell big in the sea: only that cell (and no neighbour) counts.
		BtrData sea;
		sea.land.push_back(FlatSquare(0, 0, edge, edge, -500.0f));
		sea.land.push_back(FlatSquare(kCellUnits + 1024, 2 * kCellUnits + 1024, kCellUnits + 3072, 2 * kCellUnits + 3072, 300.0f));
		sea.water.push_back(FlatSquare(0, 0, edge, edge, 0.0f));
		Check(BP::LandCells(sea, tile) == std::set<std::pair<int, int>>{ { 9, -2 } }, "land cells: a small island marks only its own cell");

		BtrData nothing;
		Check(BP::LandCells(nothing, tile).empty() && BP::LandCells(btr, TileId{ 0, 0, 0 }).empty(), "land cells: no land mesh or a bad tile -> none");

		// The full map goes through Blocks: serpentine rows, every land cell once.
		std::set<std::pair<int, int>> map;
		for (int cy = -3; cy <= 3; ++cy)
			for (int cx = -5 + (cy & 1); cx <= 6; ++cx)
				map.insert({ cx, cy });
		const auto blocks = BP::Blocks(map, 1);
		bool       shortMoves = blocks.size() == map.size();
		for (std::size_t i = 1; i < blocks.size(); ++i)
			shortMoves &= std::abs(blocks[i].first - blocks[i - 1].first) + std::abs(blocks[i].second - blocks[i - 1].second) <= 2;
		Check(shortMoves, "full map, 1x1 grid: every cell once and only short moves along the serpentine rows");
	}

	// Photo bake quality guards: flat photo verdict with the LOD reference, highlight cap of the tone curve,
	// clipped white metric, the photo px per cell warning and the map border clip.
	void TestBakeQualityGuards()
	{
		namespace BI = BakeImage;
		namespace BP = BakePlan;
		using V = BI::ContrastVerdict;

		// Flat verdict. Live values: sea / map edge 0.0-2.6, a plain snowfield 13.8 over a plain reference, ground 30+.
		Check(BI::JudgeContrast(0.0f, -1.0f) == V::kEmpty && BI::JudgeContrast(2.6f, 1.0f) == V::kEmpty && BI::JudgeContrast(4.9f, 0.5f) == V::kEmpty,
			"flat verdict: near-zero contrast is empty whatever the reference says");
		Check(BI::JudgeContrast(13.8f, 3.3f) == V::kKeep && BI::JudgeContrast(5.0f, 2.5f) == V::kKeep, "flat verdict: plain snow over a plain reference is kept");
		Check(BI::JudgeContrast(13.8f, 10.0f) == V::kFlat && BI::JudgeContrast(8.0f, 6.0f) == V::kFlat, "flat verdict: low contrast over a detailed reference is fog");
		Check(BI::JudgeContrast(13.8f, -1.0f) == V::kFlat, "flat verdict: no reference keeps the old rule (below 15 = flat)");
		Check(BI::JudgeContrast(30.0f, -1.0f) == V::kKeep && BI::JudgeContrast(15.0f, 40.0f) == V::kKeep && BI::JudgeContrast(-1.0f, -1.0f) == V::kKeep,
			"flat verdict: normal photos and unmeasured photos are kept");
		Check(BI::NeedsReferenceContrast(13.8f) && BI::NeedsReferenceContrast(5.0f) && !BI::NeedsReferenceContrast(4.9f) && !BI::NeedsReferenceContrast(15.0f) &&
				  !BI::NeedsReferenceContrast(-1.0f),
			"flat verdict: the reference is rendered only for the 5..15 band");
		Check(BI::kClearlyEmptyContrast < BI::kEmptyContrast, "flat verdict: clearly empty is a subset of empty");

		// Reference contrast from an RGBA render.
		const int                 refSize = BI::ReferenceSize(1);
		std::vector<std::uint8_t> plain(static_cast<std::size_t>(refSize) * refSize * 4, 200);
		std::vector<std::uint8_t> checker = plain;
		for (int y = 0; y < refSize; ++y)
			for (int x = 0; x < refSize; ++x)
				for (int c = 0; c < 3; ++c)
					checker[(static_cast<std::size_t>(y) * refSize + x) * 4 + c] = ((x + y) & 1) ? 220 : 180;
		Check(Near(BI::ReferenceContrast(plain, refSize), 0.0f) && Near(BI::ReferenceContrast(checker, refSize), 20.0f, 0.5f),
			"reference contrast: plain render 0, +-20 checker about 20");
		Check(BI::ReferenceContrast(std::span<const std::uint8_t>(plain).first(10), refSize) < 0.0f && BI::ReferenceContrast(plain, 0) < 0.0f,
			"reference contrast: short data or no size -> -1");

		// Clipped white share.
		BI::RgbImage half;
		half.w = 4;
		half.h = 2;
		half.px.assign(4 * 2 * 3, 255.0f);
		for (int i = 0; i < 4 * 3; ++i)
			half.px[static_cast<std::size_t>(i)] = 120.0f;  // top row grey
		half.px[12] = 200.0f;                                  // one bottom pixel bright but below the limit (luma about 239)
		Check(Near(BI::ClippedWhiteFraction(half), 3.0f / 8.0f), "clipped white: share of pixels with luma 250 or more");
		Check(BI::ClippedWhiteFraction(BI::RgbImage{}) == 0.0f, "clipped white: empty image -> 0");

		// Highlight cap: identity and darkening curves untouched; a lifting curve no longer clips bright input.
		std::array<float, 256> identity{}, darker{}, lifted{};
		for (int v = 0; v < 256; ++v) {
			identity[v] = static_cast<float>(v);
			darker[v] = static_cast<float>(v) * 0.8f;
			lifted[v] = std::min(255.0f, static_cast<float>(v) * 1.3f);
		}
		auto capped = identity;
		BI::CapHighlights(capped);
		Check(capped == identity, "highlight cap: identity curve unchanged");
		capped = darker;
		BI::CapHighlights(capped);
		Check(capped == darker, "highlight cap: a darkening curve is unchanged");
		capped = lifted;
		BI::CapHighlights(capped);
		bool monotonic = true, belowWhite = true, notRaised = true;
		for (int v = 1; v < 256; ++v)
			monotonic &= capped[v] >= capped[v - 1];
		for (int v = 0; v < 256; ++v)
			notRaised &= capped[v] <= lifted[v];
		for (int v = BI::kHighlightKnee; v <= 240; ++v)
			belowWhite &= capped[v] < BI::kClippedLuma;
		Check(monotonic && notRaised && belowWhite && Near(capped[255], 255.0f) && capped[100] == lifted[100],
			"highlight cap: lifting curve stays monotonic, only lowered, bright input 192..240 stays below clipped white, shadows untouched");
		std::printf("bake: lifted curve 230 -> %.1f before, %.1f after the highlight cap\n", lifted[230], capped[230]);

		// FitMatch applies the cap: bright photo cells matched to an even brighter reference.
		std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> pairs;
		std::vector<std::uint8_t>                                                    photo(64 * 64 * 3), ref(64 * 64 * 3);
		for (std::size_t i = 0; i < photo.size(); ++i) {
			photo[i] = static_cast<std::uint8_t>(150 + (i / 3) % 90);                             // 150..239
			ref[i] = static_cast<std::uint8_t>(std::min<std::size_t>(255, 190 + (i / 3) % 90));  // 190..255
		}
		pairs.emplace_back(photo, ref);
		const auto tone = BI::FitMatch(pairs);
		Check(tone.fitted && tone.lut[230] < BI::kClippedLuma && tone.lut[200] > 200.0f, "colour fit: lifts a bright photo but keeps 230 below clipped white");

		// Photo px per cell warning (1016 px per cell at 1080p with the 1x1 grid).
		Check(BI::TooFewPixelsPerCell(1920, 7.7f, 1, 1024) && !BI::TooFewPixelsPerCell(1920, 7.7f, 1, 512) && BI::TooFewPixelsPerCell(1920, 7.7f, 2, 512),
			"detail warning: 1080p 1x1 below 1024 px tiles, enough for 512; 2x2 below 512");
		Check(!BI::TooFewPixelsPerCell(0, 7.7f, 1, 1024) && !BI::TooFewPixelsPerCell(3840, 7.7f, 1, 1024), "detail warning: no screen size yet, or a 4K screen -> no warning");

		// Map border clip.
		const float       c = kCellUnits;
		const BP::Polygon square{ { 0.5f * c, 0.5f * c }, { 2.5f * c, 0.5f * c }, { 2.5f * c, 2.5f * c }, { 0.5f * c, 2.5f * c } };
		bool              squareOk = true;
		for (int cy = -1; cy <= 3; ++cy)
			for (int cx = -1; cx <= 3; ++cx)
				squareOk &= BP::CellTouchesPolygon(square, cx, cy) == (cx >= 0 && cx <= 2 && cy >= 0 && cy <= 2);
		Check(squareOk, "border: cells overlapping a square border, partly covered edge cells included");
		const BP::Polygon spike{ { 0.0f, 0.4f * c }, { 10.0f * c, 0.5f * c }, { 0.0f, 0.6f * c } };  // thin, no cell corner inside
		Check(BP::CellTouchesPolygon(spike, 5, 0) && !BP::CellTouchesPolygon(spike, 5, 1) && !BP::CellTouchesPolygon(spike, 11, 0),
			"border: a cell an edge passes through counts with no corner or point inside it");
		Check(!BP::CellTouchesPolygon(BP::Polygon{ { 0.0f, 0.0f }, { c, c } }, 0, 0), "border: fewer than 3 points -> no polygon");
		const std::set<std::pair<int, int>> cells{ { 0, 0 }, { 1, 1 }, { 3, 3 }, { -50, -21 }, { 53, 9 } };
		Check(BP::ClipToBorders(cells, { square }) == std::set<std::pair<int, int>>{ { 0, 0 }, { 1, 1 } }, "border: cells past the border are dropped");
		Check(BP::ClipToBorders(cells, {}) == cells && BP::ClipToBorders(cells, { BP::Polygon{ { 0.0f, 0.0f } } }) == cells, "border: no usable border -> cells unchanged");
		const BP::Polygon farSquare{ { 50.0f * c, 0.0f }, { 60.0f * c, 0.0f }, { 60.0f * c, 20.0f * c }, { 50.0f * c, 20.0f * c } };
		Check(BP::ClipToBorders(cells, { square, farSquare }) == std::set<std::pair<int, int>>{ { 0, 0 }, { 1, 1 }, { 53, 9 } }, "border: several borders -> union");
	}

	// The progress file must hold a whole-map bake (~10000 shots) and read old files as before.
	void TestBakeProgressFullMap()
	{
		namespace BP = BakePlan;
		BP::Progress saved;
		saved.grid = 1;
		for (int cy = -50; cy < 50; ++cy)
			for (int cx = -60; cx < 60; cx += 1)
				if ((cx + cy) % 7 != 0)
					saved.done.insert(BP::ShotKey("Tamriel", cx, cy));
		saved.done.insert(BP::ShotKey("WhiterunWorld", 3, -2));
		std::ostringstream text;
		BP::WriteProgress(text, saved);
		std::istringstream in(text.str());
		const auto         back = BP::ParseProgress(in, 1);
		Check(saved.done.size() > 10000 && back.done == saved.done && back.droppedGrid == 0, "progress: 10000+ shots with negative cells round trip");
		Check(text.str().size() < 400000, "progress: a whole-map file stays small (rewritten after every photo)");
		std::istringstream oldFile("# Modern Minimap town photo bake progress.\ngrid 1\ntone grade\ndone Tamriel -12 7\n");
		const auto         old = BP::ParseProgress(oldFile, 1);
		Check(old.done.contains(BP::ShotKey("Tamriel", -12, 7)) && old.toneKnown && !old.tone.fitted, "progress: a file from before the whole-map option reads the same");
	}

	// Pre-bake improvements: adaptive settle, tone.txt, spread colour fit, planned restarts, retry pass, append-only progress.
	void TestBakePrebake()
	{
		namespace BP = BakePlan;
		namespace BI = BakeImage;

		// Adaptive settle decision.
		const BP::SettleRule rule{ true, 1.5f, 15.0f, 0.75f };
		const std::vector<float> calm{ 3.0f, 0.4f, 0.3f }, busy{ 0.3f, 0.4f, 2.0f }, oneCalm{ 0.3f }, noisy{ 0.6f, 0.7f };
		Check(!BP::SettleDone(1.0f, calm, rule), "settle: never before the minimum, even when calm");
		Check(BP::SettleDone(1.5f, calm, rule) && BP::SettleDone(2.0f, noisy, rule), "settle: two calm changes in a row after the minimum (small motion tolerated)");
		Check(!BP::SettleDone(5.0f, busy, rule) && !BP::SettleDone(5.0f, oneCalm, rule) && !BP::SettleDone(5.0f, {}, rule), "settle: a big last change, one look or no looks keep waiting");
		Check(BP::SettleDone(15.0f, busy, rule) && BP::SettleDone(15.0f, {}, rule), "settle: the longest wait always ends it");
		const std::vector<float> failedLook{ -1.0f, -1.0f };
		Check(!BP::SettleDone(5.0f, failedLook, rule), "settle: looks that could not be compared do not count as calm");
		const BP::SettleRule fixedWait{ false, 1.5f, 7.0f, 0.75f };
		Check(!BP::SettleDone(6.9f, calm, fixedWait) && BP::SettleDone(7.0f, calm, fixedWait), "settle: adaptive off waits the full time");

		// Thumbnails: identical pictures do not differ; a moved bright square does; formats agree.
		const int                 w = 160, h = 90;
		std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 100), moved = rgba;
		for (int y = 30; y < 60; ++y)
			for (int x = 40; x < 70; ++x)
				for (int c = 0; c < 3; ++c)
					moved[(static_cast<std::size_t>(y) * w + x) * 4 + c] = 250;
		const auto still = BI::LumaThumbnail(rgba.data(), w, h, w * 4, DXGI_FORMAT_R8G8B8A8_UNORM, 16);
		const auto shifted = BI::LumaThumbnail(moved.data(), w, h, w * 4, DXGI_FORMAT_R8G8B8A8_UNORM, 16);
		const auto bgra = BI::LumaThumbnail(rgba.data(), w, h, w * 4, DXGI_FORMAT_B8G8R8A8_UNORM, 16);
		Check(still.size() == 256 && Near(still[0], 100.0f, 0.01f) && BI::MeanAbsDiff(still, bgra) < 0.01f, "look thumbnail: grey picture, any byte order");
		Check(BI::MeanAbsDiff(still, still) == 0.0f && BI::MeanAbsDiff(still, shifted) > 5.0f, "look thumbnail: no change -> 0, a new bright patch -> a clear change");
		Check(BI::LumaThumbnail(nullptr, w, h, w * 4, DXGI_FORMAT_R8G8B8A8_UNORM, 16).empty() && BI::LumaThumbnail(rgba.data(), w, h, w * 4, DXGI_FORMAT_BC1_UNORM, 16).empty() &&
				  BI::MeanAbsDiff(still, std::vector<float>(3, 0.0f)) < 0.0f,
			"look thumbnail: bad input -> empty, different sizes -> -1");

		// tone.txt round trip.
		BI::Tone tone;
		tone.fitted = true;
		for (int v = 0; v < 256; ++v)
			tone.lut[v] = static_cast<float>(v) * 0.987654321f + 1.0f / 3.0f;
		tone.gain = { 1.0123457f, 0.98765432f, 1.1f };
		std::ostringstream toneText;
		BP::WriteToneText(toneText, tone);
		Check(toneText.str().starts_with("fitted 1\ngain ") && toneText.str().find("\nlut ") != std::string::npos, "tone.txt: fitted, gain and lut lines");
		std::istringstream toneIn(toneText.str());
		const auto         toneBack = BP::ParseToneText(toneIn);
		Check(toneBack.fitted && toneBack.gain == tone.gain && toneBack.lut == tone.lut, "tone.txt: round trip keeps every float exactly");
		std::ostringstream fallbackText;
		BP::WriteToneText(fallbackText, BI::Tone{});
		std::istringstream fallbackIn(fallbackText.str()), emptyIn, brokenIn("fitted 1\nlut 1 2 3\ngain x\n");
		const auto         fallback = BP::ParseToneText(fallbackIn), none = BP::ParseToneText(emptyIn), broken = BP::ParseToneText(brokenIn);
		Check(!fallback.fitted && fallback.gain[0] == 1.0f && !none.fitted && none.gain[1] == 1.0f, "tone.txt: the fallback tone and a missing file read as Tone{}");
		Check(broken.fitted && broken.lut[2] == 0.0f && broken.gain[2] == 1.0f, "tone.txt: a short lut or a broken gain line is ignored");

		// Spread sample: a serpentine whole-map list starting in the north.
		std::vector<std::pair<float, float>> points;
		for (int cy = 40; cy >= -40; --cy)
			for (int cx = -50; cx <= 50; ++cx)
				points.emplace_back(cx * kCellUnits, cy * kCellUnits);
		const auto picks = BP::SpreadSample(points, 30);
		int        north = 0, south = 0, west = 0, east = 0;
		for (const auto idx : picks) {
			north += points[idx].second > 0 ? 1 : 0;
			south += points[idx].second < 0 ? 1 : 0;
			west += points[idx].first < 0 ? 1 : 0;
			east += points[idx].first > 0 ? 1 : 0;
		}
		const std::set<std::size_t> uniquePicks(picks.begin(), picks.end());
		Check(picks.size() == 30 && uniquePicks.size() == 30, "spread sample: 30 different shots");
		Check(north >= 10 && south >= 10 && west >= 10 && east >= 10, "spread sample: north, south, west and east all sampled (not just the first rows)");
		Check(BP::SpreadSample(points, 30) == picks, "spread sample: the same list gives the same picks (resume)");
		// An L-shaped land mass with empty sea squares still yields the full count.
		std::vector<std::pair<float, float>> coast;
		for (int i = 0; i < 400; ++i)
			coast.emplace_back(static_cast<float>(i % 20) * kCellUnits, i < 200 ? 0.0f : static_cast<float>(i / 20) * kCellUnits);
		Check(BP::SpreadSample(coast, 30).size() == 30, "spread sample: empty sea squares are made up with a finer grid");
		const std::vector<std::pair<float, float>> few{ { 0.0f, 0.0f }, { 1.0f, 1.0f } };
		Check(BP::SpreadSample(few, 30).size() == 2 && BP::SpreadSample({}, 30).empty() && BP::SpreadSample(points, 0).empty(), "spread sample: fewer shots than wanted -> all; none -> none");

		// Planned restarts.
		Check(!BP::RestartDue(499, 500) && BP::RestartDue(500, 500) && BP::RestartDue(501, 500), "restart: due once this run has taken iRestartEvery photos");
		Check(!BP::RestartDue(100000, 0) && !BP::RestartDue(0, 500) && !BP::RestartDue(5, -3), "restart: 0 or less = off; nothing taken = never");

		// Retry pass bookkeeping.
		BP::RetryBook book;
		Check(book.FailedCells() == 0 && book.Failed().empty() && !book.InRetry(), "retry: starts empty");
		book.Fail(3, 4);
		book.Fail(9, 2);
		Check(book.FailedCells() == 6 && book.Failed() == std::vector<std::size_t>{ 3, 9 }, "retry: failed shots and their cells are counted");
		const auto again = book.StartRetry();
		Check(again == std::vector<std::size_t>{ 3, 9 } && book.InRetry() && book.Failed().empty() && book.FailedCells() == 0, "retry: the failed shots are handed out once");
		book.Fail(12, 4);
		Check(book.StartRetry().empty() && book.Failed() == std::vector<std::size_t>{ 12 } && book.FailedCells() == 4, "retry: only one retry pass; failures in it stay as missing");
		BP::RetryBook clean;
		Check(clean.StartRetry().empty() && clean.InRetry(), "retry: nothing failed -> no retry pass");
		std::ostringstream missing;
		BP::WriteMissing(missing, { BP::ShotKey("Tamriel", -3, 7), BP::ShotKey("WhiterunWorld", 1, 2) });
		Check(missing.str() == "Tamriel -3 7\nWhiterunWorld 1 2\n", "missing list: one cell key per line");

		// Append-only progress: a whole file plus appended lines (and a torn last line after a crash) reads back.
		BP::Progress start;
		start.grid = 1;
		start.toneKnown = true;
		start.tone = tone;
		start.done = { BP::ShotKey("Tamriel", 0, 0) };
		std::ostringstream progressText;
		BP::WriteProgress(progressText, start);
		BP::AppendDone(progressText, BP::ShotKey("Tamriel", 1, 0));
		BP::AppendDone(progressText, BP::ShotKey("Tamriel", -2, 5));
		std::istringstream progressIn(progressText.str() + "done Tamr");
		const auto         progressBack = BP::ParseProgress(progressIn, 1);
		Check(progressBack.done.size() == 3 && progressBack.done.contains(BP::ShotKey("Tamriel", -2, 5)) && progressBack.toneKnown && progressBack.droppedGrid == 0,
			"progress: appended shots read back, a torn last line is ignored");
		std::ostringstream appendedOnly;
		BP::AppendDone(appendedOnly, BP::ShotKey("Tamriel", 4, 4));
		Check(appendedOnly.str() == "done Tamriel 4 4\n", "progress: one appended line per shot, same format as the whole file");
	}

	// Photo Baker app: restart budget, stall watchdog, missing list, tone.txt reading and master colour.
	void TestPhotoBakerRules()
	{
		using namespace PhotoBaker;

		// DecideRestart.
		const auto crash = DecideRestart(GameExit::kCrash, 3, 3, 10, 10);
		Check(crash.relaunch && crash.restartsLeft == 2 && crash.usedBudget && !crash.budgetRefilled, "restart budget: a crash without progress uses one restart");
		const auto refilled = DecideRestart(GameExit::kCrash, 1, 3, 10, 20);
		Check(refilled.relaunch && refilled.budgetRefilled && refilled.usedBudget && refilled.restartsLeft == 2, "restart budget: progress refills the budget, then the crash uses one");
		const auto empty = DecideRestart(GameExit::kCrash, 0, 3, 10, 10);
		Check(!empty.relaunch && empty.restartsLeft == 0 && !empty.usedBudget, "restart budget: none left and no progress -> no relaunch");
		const auto planned = DecideRestart(GameExit::kPlannedRestart, 2, 3, 10, 10), plannedAtZero = DecideRestart(GameExit::kPlannedRestart, 0, 3, 10, 10);
		Check(planned.relaunch && planned.restartsLeft == 2 && !planned.usedBudget && plannedAtZero.relaunch && plannedAtZero.restartsLeft == 0,
			"restart budget: a planned restart always relaunches and never uses the budget");
		const auto stuck = DecideRestart(GameExit::kStuck, 3, 3, 10, 10), stuckEmpty = DecideRestart(GameExit::kStuck, 0, 3, 10, 10);
		Check(stuck.relaunch == crash.relaunch && stuck.restartsLeft == crash.restartsLeft && stuck.usedBudget && !stuckEmpty.relaunch, "restart budget: a stuck exit follows the crash rules");
		Check(!DecideRestart(GameExit::kCrash, 0, 0, 10, 20).relaunch, "restart budget: 0 restarts allowed -> no relaunch, even after progress");

		// IsStalled.
		constexpr std::uint64_t launch = 1000, limit = 300'000, stallAt = launch + kStartAllowanceMs + limit;
		Check(!IsStalled("running", stallAt * 10, 0, launch, 0), "stall: limit 0 = never");
		Check(!IsStalled("running", launch + 100'000, 0, launch, limit) && !IsStalled("running", stallAt - 1, 0, launch, limit), "stall: not inside the start allowance plus the limit");
		Check(IsStalled("running", stallAt, 0, launch, limit) && IsStalled("", stallAt, 0, launch, limit), "stall: true at exactly the allowance plus the limit");
		bool quietStates = true;
		for (const auto* state : { "writing", "done", "cancelled", "error", "restart" })
			quietStates &= !IsStalled(state, stallAt * 2, 0, launch, limit);
		Check(quietStates, "stall: never while writing, finished, cancelled, failed or restarting");
		Check(!IsStalled("running", stallAt, stallAt - 10, launch, limit) && IsStalled("running", stallAt - 10 + limit, stallAt - 10, launch, limit), "stall: progress resets the clock");
		Check(!IsStalled("running", 5, 50'000'000, launch, limit), "stall: a clock before the last progress is not a stall");

		// MissingCellKeys.
		Check(MissingCellKeys("Tamriel 1 2\r\n\r\n  WhiterunWorld 3 4  \r\nTamriel -1 -2") == std::vector<std::string>{ "Tamriel 1 2", "WhiterunWorld 3 4", "Tamriel -1 -2" },
			"missing list: CRLF, blank lines, spaces and no final newline");
		Check(MissingCellKeys("").empty() && MissingCellKeys("\r\n \n").empty(), "missing list: empty text -> no keys");
		std::ostringstream pluginMissing;
		BakePlan::WriteMissing(pluginMissing, { "Tamriel 5 -6", "SolitudeWorld -60 26" });
		Check(MissingCellKeys(pluginMissing.str()) == std::vector<std::string>{ "Tamriel 5 -6", "SolitudeWorld -60 26" }, "missing list: reads the plugin's file");

		// ParseToneText, against the plugin's tone.txt writer.
		BakeImage::Tone tone;
		tone.fitted = true;
		for (int v = 0; v < 256; ++v)
			tone.lut[v] = static_cast<float>(v) * 0.913579f + 2.0f / 7.0f;
		tone.gain = { 1.0432101f, 0.97654f, 1.0f / 3.0f };
		std::ostringstream written;
		BakePlan::WriteToneText(written, tone);
		BakeImage::Tone read;
		Check(ParseToneText(written.str(), read) && read.fitted && read.lut == tone.lut && read.gain == tone.gain, "app tone.txt: reads the plugin's file exactly");
		std::ostringstream fallbackWritten;
		BakePlan::WriteToneText(fallbackWritten, BakeImage::Tone{});
		Check(ParseToneText(fallbackWritten.str(), read) && !read.fitted, "app tone.txt: reads the plugin's fallback tone");
		Check(ParseToneText("fitted 0", read) && !read.fitted && read.gain[0] == 1.0f, "app tone.txt: fitted 0 alone is the fallback tone");
		std::string lut256 = "lut";
		for (int v = 0; v < 256; ++v)
			lut256 += " " + std::to_string(v);
		const std::string lut255 = lut256.substr(0, lut256.rfind(' ')), lut257 = lut256 + " 256";
		Check(!ParseToneText("fitted 1\ngain 1 1 1\n", read) && !ParseToneText("fitted 1\n" + lut256, read), "app tone.txt: fitted 1 without lut or gain fails");
		Check(!ParseToneText("fitted 1\ngain 1 1 1\n" + lut255, read) && !ParseToneText("fitted 1\ngain 1 1 1\n" + lut257, read), "app tone.txt: 255 or 257 lut numbers fail");
		Check(!ParseToneText("fitted 1\ngain 1 x 1\n" + lut256, read) && !ParseToneText("fitted 2", read), "app tone.txt: a bad number fails");
		Check(ParseToneText("fitted 0\ncolour 5 5\n", read) && !ParseToneText("gain 1 1 1\n" + lut256, read), "app tone.txt: unknown keys ignored, no fitted line fails");
		read = tone;
		Check(!ParseToneText("fitted 1\ngain 1 1\n", read) && !read.fitted && read.gain[1] == 1.0f && read.lut[100] == 0.0f, "app tone.txt: a failure leaves the unfitted fallback");
		Check(ParseToneText("fitted 1\r\ngain 0.5 0.5 0.5\r\n" + lut256 + "\r\n", read) && read.gain[2] == 0.5f && read.lut[255] == 255.0f, "app tone.txt: \"0.5\" reads as 0.5 (no locale), CRLF");

		// ApplyRecolour.
		std::vector<std::uint8_t> pixels{ 100, 50, 0, 250, 250, 250, 77, 77, 77 }, same = pixels;
		ApplyRecolour({}, same);
		Check(same == pixels, "recolour: neutral leaves the bytes alone");
		std::vector<std::uint8_t> brighter = pixels;
		ApplyRecolour({ 1.2f, 1.0f }, brighter);
		Check(brighter[3] == 255 && brighter[0] == 120 && brighter[1] == 60 && brighter[2] == 0, "recolour: brightness 1.2 scales and clamps at 255");
		std::vector<std::uint8_t> duller = pixels;
		ApplyRecolour({ 1.0f, 0.8f }, duller);
		Check(duller[6] == 77 && duller[7] == 77 && duller[8] == 77, "recolour: saturation 0.8 leaves grey alone");
		Check(duller[0] == 90 && duller[1] == 50 && duller[2] == 10, "recolour: saturation 0.8 of 100,50,0 -> 90,50,10");

		// ToneMasterRgba == the plugin's ApplyTone with a neutral recolour; alpha untouched.
		bool sameAsPlugin = true;
		for (const auto& useTone : { tone, BakeImage::Tone{} }) {
			std::vector<std::uint8_t> rgba(64 * 4), rgb(64 * 3);
			for (std::size_t p = 0; p < 64; ++p) {
				for (int c = 0; c < 3; ++c)
					rgba[p * 4 + c] = rgb[p * 3 + c] = static_cast<std::uint8_t>((p * 37 + c * 91) % 256);
				rgba[p * 4 + 3] = 0x37;
			}
			ToneMasterRgba(useTone, {}, rgba);
			BakeImage::ApplyTone(useTone, rgb);
			for (std::size_t p = 0; p < 64; ++p)
				sameAsPlugin &= rgba[p * 4] == rgb[p * 3] && rgba[p * 4 + 1] == rgb[p * 3 + 1] && rgba[p * 4 + 2] == rgb[p * 3 + 2] && rgba[p * 4 + 3] == 0x37;
		}
		Check(sameAsPlugin, "master tone: same bytes as the plugin's ApplyTone (fitted and fallback), alpha untouched");

		// ColourStamp.
		const auto stamp = ColourStamp(written.str(), {});
		Check(stamp == ColourStamp(written.str(), {}) && stamp != ColourStamp(written.str(), { 1.1f, 1.0f }) && stamp != ColourStamp(written.str(), { 1.0f, 0.9f }) &&
				  stamp != ColourStamp(fallbackWritten.str(), {}),
			"colour stamp: changes with brightness, saturation and tone text only");
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

	// A BC7 .dds as texconv writes it: DX10 header, DXGI_FORMAT_BC7_UNORM, a_mips levels (block content does not matter here).
	std::vector<std::uint8_t> MakeBc7Dds(int a_size, int a_mips)
	{
		std::vector<std::uint8_t> out(4 + 124 + 20, 0);
		const auto                put = [&](std::size_t a_at, std::uint32_t a_v) { std::memcpy(out.data() + a_at, &a_v, 4); };
		std::memcpy(out.data(), "DDS ", 4);
		const auto blocks = static_cast<std::uint32_t>(a_size / 4);
		put(4, 124);
		put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);
		put(12, static_cast<std::uint32_t>(a_size));
		put(16, static_cast<std::uint32_t>(a_size));
		put(20, blocks * blocks * 16);
		put(28, static_cast<std::uint32_t>(a_mips));
		put(76, 32);
		put(80, 0x4);
		std::memcpy(out.data() + 84, "DX10", 4);
		put(108, 0x1000 | (a_mips > 1 ? 0x400000 | 0x8 : 0));
		put(128, 98);  // DXGI_FORMAT_BC7_UNORM
		put(132, 3);   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
		put(140, 1);   // array size
		for (int m = 0, size = a_size; m < a_mips; ++m, size = std::max(1, size / 2)) {
			const std::size_t n = static_cast<std::size_t>(std::max(1, size / 4)) * std::max(1, size / 4);
			for (std::size_t b = 0; b < n; ++b) {
				std::uint8_t block[16]{ 0x40 };  // mode 6
				out.insert(out.end(), block, block + 16);
			}
		}
		return out;
	}

	struct LoadedTile
	{
		bool        loaded{ false };
		UINT        width{ 0 };
		UINT        mips{ 0 };
		DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
		bool        capped{ false };
	};

	LoadedTile LoadBakedTile(ID3D11Device* a_dev, const std::vector<std::uint8_t>& a_dds, int a_maxTexSize)
	{
		const TileId id{ kBakedLevel, -12, 7 };
		const auto   want = TileTexturePath("Tamriel", id, false);
		const Tile   tile = LoadTile(
            a_dev,
            [&](const std::string& a_path, std::vector<std::uint8_t>& a_out) {
                if (a_path != want)
                    return false;
                a_out = a_dds;
                return true;
            },
            "Tamriel", id, a_maxTexSize);
		LoadedTile r;
		if (tile.absent || !tile.diffuse)
			return r;
		ComPtr<ID3D11Resource> res;
		tile.diffuse->GetResource(&res);
		ComPtr<ID3D11Texture2D> tex;
		if (FAILED(res.As(&tex)))
			return r;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		return { true, d.Width, d.MipLevels, d.Format, tile.capped };
	}

	// The minimap's baked tiles: BC1 from the bake at any tile size, and BC7 tiles converted from the masters.
	void TestBakedTileFormats(ID3D11Device* a_dev)
	{
		std::vector<std::uint8_t> rgb(std::size_t{ 1024 } * 1024 * 3);
		for (std::size_t i = 0; i < rgb.size(); ++i)
			rgb[i] = static_cast<std::uint8_t>(i % 253);
		const auto bc1 = BakeImage::EncodeDdsBc1(rgb, 1024);
		Check(bc1.size() == 128 + 699064, "BC1 1024 px dds: 11 mips");

		const auto full = LoadBakedTile(a_dev, bc1, 2048);
		Check(full.loaded && full.width == 1024 && full.mips == 11 && full.format == DXGI_FORMAT_BC1_UNORM && !full.capped, "baked tile: BC1 1024 px loads whole when zoomed in");
		const auto zoomedOut = LoadBakedTile(a_dev, bc1, 512);
		Check(zoomedOut.loaded && zoomedOut.width == 512 && zoomedOut.capped, "baked tile: BC1 1024 px loads its 512 px mip when zoomed out");
		const auto old = LoadBakedTile(a_dev, BakeImage::EncodeDdsBc1(std::span(rgb).first(std::size_t{ 512 } * 512 * 3), 512), 2048);
		Check(old.loaded && old.width == 512 && !old.capped, "baked tile: BC1 512 px still loads (mixed sizes in one folder)");

		const auto bc7 = LoadBakedTile(a_dev, MakeBc7Dds(1024, 11), 2048);
		Check(bc7.loaded && bc7.width == 1024 && bc7.mips == 11 && bc7.format == DXGI_FORMAT_BC7_UNORM, "baked tile: BC7 1024 px with mips loads (DX10 header)");
		const auto bc7ZoomedOut = LoadBakedTile(a_dev, MakeBc7Dds(1024, 11), 256);
		Check(bc7ZoomedOut.loaded && bc7ZoomedOut.width == 256 && bc7ZoomedOut.capped, "baked tile: BC7 loads a smaller mip when zoomed out");
		const auto bc7One = LoadBakedTile(a_dev, MakeBc7Dds(512, 1), 256);
		Check(bc7One.loaded && bc7One.width == 512, "baked tile: BC7 without mips loads whole even when zoomed out");
		auto srgb = MakeBc7Dds(512, 10);
		const std::uint32_t kSrgb = 99;  // DXGI_FORMAT_BC7_UNORM_SRGB
		std::memcpy(srgb.data() + 128, &kSrgb, 4);
		const auto bc7Srgb = LoadBakedTile(a_dev, srgb, 1024);
		Check(bc7Srgb.loaded && bc7Srgb.format == DXGI_FORMAT_BC7_UNORM, "baked tile: BC7 sRGB loads as UNORM (sRGB flag ignored, like BC1)");
	}

	// Photo Baker app: tile formats, the [Bake] switches it writes, the quality slider, and reading the masters back.
	void TestPhotoBakerFormats()
	{
		using namespace PhotoBaker;
		Check(kTileFormats[0].format == TileFormat::kBc7Max && kTileFormats[1].format == TileFormat::kBc7 && kTileFormats[2].format == TileFormat::kBc7Quick &&
				  kTileFormats[3].format == TileFormat::kBc1,
			"baker: formats listed slowest encode first, BC1 last");
		bool roundTrip = true;
		for (int i = 0; i < static_cast<int>(kTileFormats.size()); ++i) {
			const auto format = TileFormatAt(i);
			roundTrip &= TileFormatIndex(format) == i && TileFormatFromKey(kTileFormats[static_cast<std::size_t>(i)].key) == format;
		}
		Check(roundTrip, "baker: format index and settings key round trip");
		Check(TileFormatAt(-1) == TileFormat::kBc1 && TileFormatAt(4) == TileFormat::kBc1 && TileFormatFromKey(L"") == TileFormat::kBc1 &&
				  TileFormatFromKey(L"bc7") == TileFormat::kBc1 && kDefaultTileFormat == TileFormat::kBc1,
			"baker: bad index or key -> BC1, the default");
		Check(IsBc7(TileFormat::kBc7Max) && IsBc7(TileFormat::kBc7) && IsBc7(TileFormat::kBc7Quick) && !IsBc7(TileFormat::kBc1), "baker: which formats are BC7");

		const auto value = [](const std::vector<std::pair<std::string, std::string>>& a_switches, const std::string& a_key) {
			const auto it = std::ranges::find(a_switches, a_key, &std::pair<std::string, std::string>::first);
			return it == a_switches.end() ? std::string("<missing>") : it->second;
		};
		bool mastersOk = true;
		for (const auto& choice : kTileFormats) {
			BakeChoices c;
			c.format = choice.format;
			mastersOk &= value(BakeSwitches(c), "bSaveMasters") == (IsBc7(choice.format) ? "true" : "false");
		}
		Check(mastersOk, "baker: bSaveMasters on for the three BC7 formats, off for BC1");
		BakeChoices c;
		c.area = 1.25f;
		c.seconds = 9;
		c.hideNpcs = false;
		c.keepExisting = true;
		c.grid = 1;
		c.fullMap = true;
		c.tileSize = 1024;
		const auto sw = BakeSwitches(c);
		Check(value(sw, "bFullMap") == "true" && value(sw, "iTileSize") == "1024" && value(sw, "iPhotoGrid") == "1" && value(sw, "fAreaScale") == "1.25" &&
				  value(sw, "fSettleSeconds") == "9.0" && value(sw, "bHideActorsOnNextStart") == "false" && value(sw, "bKeepExistingTiles") == "true",
			"baker: switches pass the choices through in the plugin's ini format");
		c.area = 2.0f / 3.0f;
		c.fullMap = false;
		Check(value(BakeSwitches(c), "fAreaScale") == "0.67" && value(BakeSwitches(c), "bFullMap") == "false", "baker: area with two decimals, towns-only area");

		Check(GridFromQuality(1) == 4 && GridFromQuality(4) == 1 && GridFromQuality(2) == 3 && GridFromQuality(3) == 2, "baker: quality slider left = 4x4, right = 1x1");
		Check(QualityFromGrid(4) == 1 && QualityFromGrid(1) == 4 && QualityFromGrid(3) == 2 && QualityFromGrid(2) == 3, "baker: grid back to slider position");
		Check(GridFromQuality(0) == 4 && GridFromQuality(9) == 1 && QualityFromGrid(-3) == 4 && QualityFromGrid(7) == 1, "baker: quality slider positions clamped");
		Check(TileSizeIndex(512) == 0 && TileSizeIndex(1024) == 1 && TileSizeIndex(2048) == 0 && TileSizeIndex(0) == 0, "baker: tile size choice index, other sizes -> 512");
	}

	// Photo Baker app: the plugin's master .bmp read back, and the checks that decide which tiles still need BC7 encoding.
	void TestPhotoBakerEncodeChecks()
	{
		using namespace PhotoBaker;
		// The plugin writes the master; the app must read the same pixels back. 3x2: padded rows.
		const std::vector<std::uint8_t> rgb{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
		auto                            bmp = BakeImage::EncodeBmp24(rgb, 3, 2);
		int                             w = 0, h = 0;
		std::vector<std::uint8_t>       rgba;
		const bool                      read = DecodeBmp24(bmp, w, h, rgba);
		bool                            same = read && w == 3 && h == 2 && rgba.size() == 24;
		for (std::size_t i = 0; same && i < 6; ++i)
			same &= rgba[i * 4] == rgb[i * 3] && rgba[i * 4 + 1] == rgb[i * 3 + 1] && rgba[i * 4 + 2] == rgb[i * 3 + 2] && rgba[i * 4 + 3] == 255;
		Check(same, "baker: master bmp read back: top row first, RGB, alpha 255");
		const auto rejects = [&](std::size_t a_at, std::uint8_t a_byte) {
			auto bad = bmp;
			bad[a_at] = a_byte;
			int                       bw = 0, bh = 0;
			std::vector<std::uint8_t> out;
			return !DecodeBmp24(bad, bw, bh, out);
		};
		Check(rejects(28, 32) && rejects(30, 1) && rejects(25, 0xFF) && rejects(0, 'X'), "baker: 32-bit, compressed, top-down and non-bmp files rejected");
		Check(!DecodeBmp24(std::span(bmp).first(60), w, h, rgba) && !DecodeBmp24({}, w, h, rgba), "baker: short master files rejected");

		Check(BmpWidth(bmp) == 3 && BmpWidth(std::span(bmp).first(25)) == 0 && BmpWidth({}) == 0, "baker: bmp width, short heads -> 0");
		auto wrongMagic = bmp;
		wrongMagic[1] = 'X';
		auto negative = bmp;
		const std::uint32_t minusOne = 0xFFFFFFFF, huge = 20000;
		std::memcpy(negative.data() + 18, &minusOne, 4);
		auto oversized = bmp;
		std::memcpy(oversized.data() + 18, &huge, 4);
		Check(BmpWidth(wrongMagic) == 0 && BmpWidth(negative) == 0 && BmpWidth(oversized) == 0, "baker: bmp width of a wrong magic, negative or oversized width -> 0");

		const auto bc7 = MakeBc7Dds(1024, 1);
		std::vector<std::uint8_t> rgb512(std::size_t{ 512 } * 512 * 3, 90);
		const auto                bc1 = BakeImage::EncodeDdsBc1(rgb512, 512);
		const auto                bc7Head = std::span(bc7).first(kDdsHeadBytes), bc1Head = std::span(bc1).first(kDdsHeadBytes);
		Check(DdsWidth(bc7Head) == 1024 && DdsWidth(bc1Head) == 512 && DdsWidth(std::span(bc7).first(19)) == 0 && DdsWidth(bmp) == 0, "baker: dds width, short or non-dds heads -> 0");

		auto srgb = bc7;
		const std::uint32_t kSrgb = 99, kBc1Dx10 = 71;
		std::memcpy(srgb.data() + 128, &kSrgb, 4);
		auto dx10Bc1 = bc7;
		std::memcpy(dx10Bc1.data() + 128, &kBc1Dx10, 4);
		Check(IsBc7Dds(bc7Head) && IsBc7Dds(std::span(srgb).first(kDdsHeadBytes)) && !IsBc7Dds(std::span(dx10Bc1).first(kDdsHeadBytes)) && !IsBc7Dds(bc1Head) &&
				  !IsBc7Dds(std::span(bc7).first(100)),
			"baker: BC7 dds = DX10 header with format 98 or 99 only");

		Check(EncodedSide(1024, 512) == 512 && EncodedSide(512, 512) == 512 && EncodedSide(1024, 0) == 1024 && EncodedSide(512, 1024) == 512, "baker: encoded side");
		Check(ShouldSkipEncode(bc7Head, 1024, 200, 100), "baker: BC7 tile of the right size, newer than its master -> skipped");
		Check(!ShouldSkipEncode(bc1Head, 512, 200, 100) && !ShouldSkipEncode(bc7Head, 512, 200, 100) && !ShouldSkipEncode(bc7Head, 1024, 100, 100) &&
				  !ShouldSkipEncode(bc7Head, 1024, 99, 100) && !ShouldSkipEncode({}, 1024, 200, 100) && !ShouldSkipEncode(std::span(bc7).first(50), 1024, 200, 100),
			"baker: BC1 tile, other size, not newer than the master or a short head -> encoded again");
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
	TestFeatureHelpers();
	TestBatch2Helpers();
	TestNavMesh();
	TestBakeImage();
	TestBakeGridGeometry();
	TestBakePlan();
	TestBakeTileSize();
	TestFullMapCells();
	TestBakeQualityGuards();
	TestBakeProgressFullMap();
	TestBakePrebake();
	TestPhotoBakerRules();
	TestPhotoBakerFormats();
	TestPhotoBakerEncodeChecks();
	TestBtr(data);
	TestFullMapOnLod(data);

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
		TestFogRender(dev.Get(), ctx.Get(), data, out);
		TestTextPreview(dev.Get(), ctx.Get(), out);
		TestNavRender(dev.Get(), ctx.Get(), out);
		TestWorkerException(dev.Get());
		TestBakeDds(dev.Get());
		TestBakedTileFormats(dev.Get());
	}

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
