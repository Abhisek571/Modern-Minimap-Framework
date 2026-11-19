#include "Icons.h"

#include <algorithm>
#include <numbers>

namespace MM
{
	namespace
	{
		struct P
		{
			float x, y;
		};

		float Dot(P a_a, P a_b) { return a_a.x * a_b.x + a_a.y * a_b.y; }

		// Signed distance to a simple polygon (negative inside).
		float SdPolygon(P a_p, std::span<const P> a_v)
		{
			P     w0{ a_p.x - a_v[0].x, a_p.y - a_v[0].y };
			float d = Dot(w0, w0);
			float s = 1.0f;
			const std::size_t n = a_v.size();
			for (std::size_t i = 0, j = n - 1; i < n; j = i, ++i) {
				const P     e{ a_v[j].x - a_v[i].x, a_v[j].y - a_v[i].y };
				const P     w{ a_p.x - a_v[i].x, a_p.y - a_v[i].y };
				const float t = std::clamp(Dot(w, e) / Dot(e, e), 0.0f, 1.0f);
				const P     b{ w.x - e.x * t, w.y - e.y * t };
				d = std::min(d, Dot(b, b));
				const bool c1 = a_p.y >= a_v[i].y, c2 = a_p.y < a_v[j].y, c3 = e.x * w.y > e.y * w.x;
				if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
					s = -s;
			}
			return s * std::sqrt(d);
		}

		float SdCircle(P a_p, P a_c, float a_r) { return std::hypot(a_p.x - a_c.x, a_p.y - a_c.y) - a_r; }

		float SdBox(P a_p, P a_c, P a_half)
		{
			const float dx = std::fabs(a_p.x - a_c.x) - a_half.x, dy = std::fabs(a_p.y - a_c.y) - a_half.y;
			const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
			return std::hypot(ox, oy) + std::min(std::max(dx, dy), 0.0f);
		}

		std::vector<P> HalfEllipse(P a_c, float a_rx, float a_ry, int a_steps)
		{
			std::vector<P> v;
			for (int i = 0; i <= a_steps; ++i) {
				const float a = std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(a_steps);
				v.push_back({ a_c.x + a_rx * std::cos(a), a_c.y + a_ry * std::sin(a) });
			}
			return v;
		}

		struct ShapeSd
		{
			float outer;
			float inner;  // < 0: dark detail inside the shape
		};

		ShapeSd Evaluate(IconShape a_shape, P a_p)
		{
			static const P diamond[]{ { 0, 0.8f }, { 0.8f, 0 }, { 0, -0.8f }, { -0.8f, 0 } };
			static const P triangle[]{ { 0, 0.8f }, { 0.75f, -0.6f }, { -0.75f, -0.6f } };
			static const P house[]{ { 0, 0.8f }, { 0.8f, 0.05f }, { 0.55f, 0.05f }, { 0.55f, -0.7f }, { -0.55f, -0.7f }, { -0.55f, 0.05f }, { -0.8f, 0.05f } };
			static const P castle[]{ { -0.75f, -0.7f }, { 0.75f, -0.7f }, { 0.75f, 0.75f }, { 0.45f, 0.75f }, { 0.45f, 0.5f }, { 0.15f, 0.5f }, { 0.15f, 0.75f },
				{ -0.15f, 0.75f }, { -0.15f, 0.5f }, { -0.45f, 0.5f }, { -0.45f, 0.75f }, { -0.75f, 0.75f } };
			static const P arrow[]{ { 0, 0.8f }, { 0.6f, -0.58f }, { 0, -0.27f }, { -0.6f, -0.58f } };
			static const auto mound = HalfEllipse({ 0, -0.62f }, 0.8f, 1.3f, 24);
			static const auto hole = HalfEllipse({ 0, -0.62f }, 0.33f, 0.6f, 16);
			static const auto star = [] {
				std::vector<P> v;
				for (int i = 0; i < 10; ++i) {
					const float r = (i % 2 == 0) ? 0.82f : 0.36f;
					const float a = std::numbers::pi_v<float> * (0.5f + static_cast<float>(i) * 0.2f);
					v.push_back({ r * std::cos(a), r * std::sin(a) - 0.04f });
				}
				return v;
			}();
			constexpr float none = 1.0f;

			switch (a_shape) {
			case IconShape::kCircle:
				return { SdCircle(a_p, { 0, 0 }, 0.72f), none };
			case IconShape::kDiamond:
				return { SdPolygon(a_p, diamond), none };
			case IconShape::kTriangle:
				return { SdPolygon(a_p, triangle), none };
			case IconShape::kHouse:
				return { SdPolygon(a_p, house), SdBox(a_p, { 0, -0.47f }, { 0.15f, 0.23f }) };
			case IconShape::kCity:
				return { SdPolygon(a_p, castle), SdBox(a_p, { 0, -0.45f }, { 0.2f, 0.25f }) };
			case IconShape::kCave:
				return { SdPolygon(a_p, mound), SdPolygon(a_p, hole) };
			case IconShape::kCross:
				{
					const float c = std::numbers::sqrt2_v<float> * 0.5f;
					const P     r1{ (a_p.x + a_p.y) * c, (a_p.y - a_p.x) * c };
					const float d = std::min(SdBox(r1, { 0, 0 }, { 0.78f, 0.16f }), SdBox(r1, { 0, 0 }, { 0.16f, 0.78f }));
					return { d, none };
				}
			case IconShape::kStar:
				return { SdPolygon(a_p, star), none };
			case IconShape::kDoor:
				return { std::min(SdBox(a_p, { 0, -0.25f }, { 0.48f, 0.55f }), SdCircle(a_p, { 0, 0.3f }, 0.48f)), SdCircle(a_p, { 0.24f, -0.2f }, 0.1f) };
			case IconShape::kPin:
				{
					static const P tip[]{ { -0.42f, 0.0f }, { 0.42f, 0.0f }, { 0, -0.78f } };
					return { std::min(SdCircle(a_p, { 0, 0.28f }, 0.5f), SdPolygon(a_p, tip)), SdCircle(a_p, { 0, 0.28f }, 0.18f) };
				}
			case IconShape::kArrow:
				return { SdPolygon(a_p, arrow), none };
			default:
				return { none, none };
			}
		}
	}

	AtlasImage BuildIconAtlas()
	{
		constexpr int   kSS = 4;  // supersamples per axis
		constexpr float kOutline = 0.14f;
		constexpr float kDark = 25.0f / 255.0f;

		AtlasImage img;
		img.size = kAtlasSize;
		std::vector<std::uint8_t> base(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4, 0);
		for (int s = 0; s < static_cast<int>(IconShape::kCount); ++s) {
			const int cx = (s % kAtlasGrid) * kAtlasCellPx, cy = (s / kAtlasGrid) * kAtlasCellPx;
			for (int y = 0; y < kAtlasCellPx; ++y) {
				for (int x = 0; x < kAtlasCellPx; ++x) {
					float rgb = 0.0f, alpha = 0.0f;
					for (int sy = 0; sy < kSS; ++sy) {
						for (int sx = 0; sx < kSS; ++sx) {
							const P p{ ((x + (sx + 0.5f) / kSS) / kAtlasCellPx) * 2.0f - 1.0f, 1.0f - ((y + (sy + 0.5f) / kSS) / kAtlasCellPx) * 2.0f };
							const auto d = Evaluate(static_cast<IconShape>(s), p);
							if (d.outer < 0.0f) {
								rgb += d.inner < 0.0f ? kDark : 1.0f;
								alpha += 1.0f;
							} else if (d.outer < kOutline) {
								rgb += kDark;
								alpha += 1.0f;
							}
						}
					}
					const float n = static_cast<float>(kSS * kSS);
					const float a = alpha / n;
					const float c = alpha > 0.0f ? rgb / alpha : 0.0f;
					auto*       px = &base[(static_cast<std::size_t>(cy + y) * kAtlasSize + cx + x) * 4];
					const auto  cv = static_cast<std::uint8_t>(std::lround(c * 255.0f));
					px[0] = px[1] = px[2] = cv;
					px[3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
				}
			}
		}
		img.mips.push_back(std::move(base));

		// Alpha-weighted box filter so transparent texels do not darken edges.
		for (int m = 1; m < kAtlasMips; ++m) {
			const int                 src = kAtlasSize >> (m - 1), dst = kAtlasSize >> m;
			const auto&               in = img.mips.back();
			std::vector<std::uint8_t> out(static_cast<std::size_t>(dst) * dst * 4, 0);
			for (int y = 0; y < dst; ++y) {
				for (int x = 0; x < dst; ++x) {
					float c[3]{}, a = 0.0f;
					for (int k = 0; k < 4; ++k) {
						const auto* p = &in[(static_cast<std::size_t>(y * 2 + k / 2) * src + x * 2 + k % 2) * 4];
						const float w = p[3] / 255.0f;
						for (int i = 0; i < 3; ++i)
							c[i] += p[i] * w;
						a += w;
					}
					auto* o = &out[(static_cast<std::size_t>(y) * dst + x) * 4];
					for (int i = 0; i < 3; ++i)
						o[i] = static_cast<std::uint8_t>(a > 0.0f ? std::lround(c[i] / a) : 0);
					o[3] = static_cast<std::uint8_t>(std::lround(a / 4.0f * 255.0f));
				}
			}
			img.mips.push_back(std::move(out));
		}
		return img;
	}

	UvRect AtlasUv(IconShape a_shape) noexcept
	{
		const int   s = static_cast<int>(a_shape);
		const float cell = 1.0f / kAtlasGrid;
		const float u = static_cast<float>(s % kAtlasGrid) * cell, v = static_cast<float>(s / kAtlasGrid) * cell;
		return { u, v, u + cell, v + cell };
	}

	MarkerClass ClassifyMapMarker(int a_markerType) noexcept
	{
		switch (a_markerType) {
		case 1:  // city
			return MarkerClass::kCity;
		case 2:   // town
		case 3:   // settlement
		case 13:  // farm
		case 14:  // wood mill
		case 19:  // wheat mill
		case 21:  // stable
		case 28:  // orc stronghold
		case 30:  // shack
		case 32:  // nordic dwelling
		case 33:  // docks
		case 54:  // Raven Rock
		case 56:  // Telvanni tower
			return MarkerClass::kSettlement;
		case 4:   // cave
		case 5:   // camp
		case 6:   // fort
		case 7:   // nordic ruin
		case 8:   // dwemer ruin
		case 9:   // shipwreck
		case 12:  // dragon lair
		case 15:  // mine
		case 22:  // imperial tower
		case 29:  // giant camp
		case 31:  // nordic tower
		case 53:  // Miraak's temple
		case 59:  // Castle Karstaag
			return MarkerClass::kDungeon;
		default:
			// 35..52: the hold capitals' castle / capitol markers.
			return (a_markerType >= 35 && a_markerType <= 52) ? MarkerClass::kCity : MarkerClass::kOther;
		}
	}

	IconShape ShapeForMarkerClass(MarkerClass a_class) noexcept
	{
		switch (a_class) {
		case MarkerClass::kCity:
			return IconShape::kCity;
		case MarkerClass::kSettlement:
			return IconShape::kHouse;
		case MarkerClass::kDungeon:
			return IconShape::kCave;
		default:
			return IconShape::kDiamond;
		}
	}

	void BuildIconInstances(std::span<const IconSource> a_sources, const IconView& a_view, std::vector<IconInstance>& a_out)
	{
		const float half = a_view.sizePx * 0.5f;
		// Anything farther than this many world units cannot be on the map: cheap reject before the rotation.
		const float reach = half * 1.5f * a_view.unitsPerPixel;
		const float reach2 = reach * reach;
		for (const auto& s : a_sources) {
			const float size = s.sizePx * a_view.iconScale;
			const float dx = s.world.x - a_view.player.x, dy = s.world.y - a_view.player.y;
			const bool  far = dx * dx + dy * dy > reach2;
			if (far && !s.clampToRim)
				continue;
			Vec2 q = ProjectToMinimap(s.world, a_view);
			if (s.clampToRim) {
				q = ClampToRim(q, half, a_view.round, size * 0.5f + 1.0f);
			} else if (!InsideMask(q, half, a_view.round)) {
				continue;
			}
			IconInstance inst;
			inst.x = a_view.centerPx.x + q.x;
			inst.y = a_view.centerPx.y + q.y;
			inst.size = size;
			inst.rotation = s.rotation;
			inst.clip = 1.0f;
			inst.uv = AtlasUv(s.shape);
			inst.rgba = s.rgba;
			a_out.push_back(inst);
		}
	}

	IconInstance ArrowInstance(const IconView& a_view, float a_sizePx, float a_rotation, std::uint32_t a_rgba)
	{
		IconInstance inst;
		inst.x = a_view.centerPx.x;
		inst.y = a_view.centerPx.y;
		inst.size = a_sizePx;
		inst.rotation = a_rotation;
		inst.clip = 0.0f;
		inst.uv = AtlasUv(IconShape::kArrow);
		inst.rgba = a_rgba;
		return inst;
	}

	int Schedule::Add(double a_intervalSec)
	{
		if (_count >= kMaxJobs)
			return -1;
		_jobs[static_cast<std::size_t>(_count)] = { a_intervalSec, 0.0, true };
		return _count++;
	}

	void Schedule::SetInterval(int a_job, double a_intervalSec)
	{
		if (a_job >= 0 && a_job < _count)
			_jobs[static_cast<std::size_t>(a_job)].interval = a_intervalSec;
	}

	void Schedule::Force(int a_job)
	{
		if (a_job >= 0 && a_job < _count)
			_jobs[static_cast<std::size_t>(a_job)].forced = true;
	}

	void Schedule::ForceAll()
	{
		for (int i = 0; i < _count; ++i)
			_jobs[static_cast<std::size_t>(i)].forced = true;
	}

	std::uint32_t Schedule::Tick(double a_now)
	{
		std::uint32_t due = 0;
		for (int i = 0; i < _count; ++i) {
			auto& j = _jobs[static_cast<std::size_t>(i)];
			if (!j.forced && a_now < j.next)
				continue;
			due |= 1u << i;
			// Fixed rate, but never a burst to catch up after a pause (menus, loading) or a forced run.
			j.next = j.forced ? a_now + j.interval : j.next + j.interval;
			if (j.next <= a_now)
				j.next = a_now + j.interval;
			j.forced = false;
		}
		return due;
	}
}
