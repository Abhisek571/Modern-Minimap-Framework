#include "Icons.h"

#include <algorithm>
#include <cstdio>
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

		// Distance from a point to a polyline (a single point is a dot).
		float DistToPolyline(P a_p, std::span<const P> a_v)
		{
			if (a_v.size() == 1)
				return std::hypot(a_p.x - a_v[0].x, a_p.y - a_v[0].y);
			float d2 = 1e30f;
			for (std::size_t i = 1; i < a_v.size(); ++i) {
				const P     e{ a_v[i].x - a_v[i - 1].x, a_v[i].y - a_v[i - 1].y };
				const P     w{ a_p.x - a_v[i - 1].x, a_p.y - a_v[i - 1].y };
				const float len2 = Dot(e, e);
				const float t = len2 > 0.0f ? std::clamp(Dot(w, e) / len2, 0.0f, 1.0f) : 0.0f;
				const P     b{ w.x - e.x * t, w.y - e.y * t };
				d2 = std::min(d2, Dot(b, b));
			}
			return std::sqrt(d2);
		}

		// Stroke font for kGlyphChars, same order. Points "x,y" on a grid x 0..6, y 0..8 (2 = baseline,
		// 6 = x-height, 8 = cap height, 0 = descender); '|' starts a new stroke. Most letters are 4 units wide,
		// m / w / M / W 6; each glyph is centred in its cell and gets its own advance (GlyphAdvanceEm).
		constexpr const char* kGlyphStrokes[]{
			"1,8 3,8 4,7 4,3 3,2 1,2 0,3 0,7 1,8",                              // 0
			"1,7 2,8 2,2|1,2 3,2",                                              // 1
			"0,7 1,8 3,8 4,7 4,6 0,2 4,2",                                      // 2
			"0,8 4,8 2,5 3,5 4,4 4,3 3,2 1,2 0,3",                              // 3
			"3,2 3,8 0,4 4,4",                                                  // 4
			"4,8 0,8 0,5 3,5 4,4 4,3 3,2 0,2",                                  // 5
			"3,8 1,8 0,7 0,3 1,2 3,2 4,3 4,4 3,5 0,5",                          // 6
			"0,8 4,8 1,2",                                                      // 7
			"1,5 0,6 0,7 1,8 3,8 4,7 4,6 3,5 1,5 0,4 0,3 1,2 3,2 4,3 4,4 3,5",  // 8
			"4,5 1,5 0,6 0,7 1,8 3,8 4,7 4,3 3,2 1,2",                          // 9
			"0,2",                                                              // .
			"0,7 1,8 3,8 4,7 4,6 2,5 2,4|2,2",                                  // ?
			"0,3|0,6",                                                          // :
			"0,8 0,6",                                                          // apostrophe
			"0,5 3,5",                                                          // -
			"0,2 0,6 2,8 4,6 4,2|0,5 4,5",                                      // A
			"0,2 0,8 3,8 4,7 4,6 3,5 0,5|3,5 4,4 4,3 3,2 0,2",                  // B
			"4,7 3,8 1,8 0,7 0,3 1,2 3,2 4,3",                                  // C
			"0,2 0,8 2,8 4,6 4,4 2,2 0,2",                                      // D
			"4,8 0,8 0,2 4,2|0,5 3,5",                                          // E
			"4,8 0,8 0,2|0,5 3,5",                                              // F
			"4,7 3,8 1,8 0,7 0,3 1,2 3,2 4,3 4,5 2,5",                          // G
			"0,2 0,8|4,2 4,8|0,5 4,5",                                          // H
			"0,8 2,8|1,8 1,2|0,2 2,2",                                          // I
			"4,8 4,3 3,2 1,2 0,3",                                              // J
			"0,2 0,8|4,8 0,4|1,5 4,2",                                          // K
			"0,8 0,2 4,2",                                                      // L
			"0,2 0,8 3,4 6,8 6,2",                                              // M
			"0,2 0,8 4,2 4,8",                                                  // N
			"1,8 3,8 4,7 4,3 3,2 1,2 0,3 0,7 1,8",                              // O
			"0,2 0,8 3,8 4,7 4,6 3,5 0,5",                                      // P
			"1,8 3,8 4,7 4,3 3,2 1,2 0,3 0,7 1,8|2,4 4,2",                      // Q
			"0,2 0,8 3,8 4,7 4,6 3,5 0,5|2,5 4,2",                              // R
			"4,7 3,8 1,8 0,7 0,6 1,5 3,5 4,4 4,3 3,2 1,2 0,3",                  // S
			"0,8 4,8|2,8 2,2",                                                  // T
			"0,8 0,3 1,2 3,2 4,3 4,8",                                          // U
			"0,8 2,2 4,8",                                                      // V
			"0,8 1,2 3,6 5,2 6,8",                                              // W
			"0,8 4,2|4,8 0,2",                                                  // X
			"0,8 2,5 4,8|2,5 2,2",                                              // Y
			"0,8 4,8 0,2 4,2",                                                  // Z
			"4,6 4,2|4,5 3,6 1,6 0,5 0,3 1,2 3,2 4,3",                          // a (single storey, closed bowl)
			"0,8 0,2|0,5 1,6 3,6 4,5 4,3 3,2 1,2 0,3",                          // b
			"4,5 3,6 1,6 0,5 0,3 1,2 3,2 4,3",                                  // c
			"4,8 4,2|4,5 3,6 1,6 0,5 0,3 1,2 3,2 4,3",                          // d
			"0,4 4,4 4,5 3,6 1,6 0,5 0,3 1,2 3,2 4,3",                          // e
			"3,8 2,8 1,7 1,2|0,6 3,6",                                          // f
			"4,6 4,1 3,0 1,0|4,5 3,6 1,6 0,5 0,4 1,3 3,3 4,4",                  // g
			"0,8 0,2|0,5 1,6 3,6 4,5 4,2",                                      // h
			"0,6 0,2|0,8",                                                      // i
			"1,6 1,1 0,0|1,8",                                                  // j
			"0,8 0,2|4,6 0,3|1,4 4,2",                                          // k
			"0,8 0,3 1,2 2,2",                                                  // l
			"0,6 0,2|0,5 1,6 2,6 3,5 3,2|3,5 4,6 5,6 6,5 6,2",                  // m
			"0,6 0,2|0,5 1,6 3,6 4,5 4,2",                                      // n
			"1,6 3,6 4,5 4,3 3,2 1,2 0,3 0,5 1,6",                              // o
			"0,6 0,0|0,5 1,6 3,6 4,5 4,3 3,2 1,2 0,3",                          // p
			"4,6 4,0|4,5 3,6 1,6 0,5 0,3 1,2 3,2 4,3",                          // q
			"0,6 0,2|0,4 2,6 4,6",                                              // r
			"4,6 1,6 0,5 1,4 3,4 4,3 3,2 0,2",                                  // s
			"1,8 1,3 2,2 3,2|0,6 3,6",                                          // t
			"0,6 0,3 1,2 3,2 4,3|4,6 4,2",                                      // u
			"0,6 2,2 4,6",                                                      // v
			"0,6 1,2 3,5 5,2 6,6",                                              // w
			"0,6 4,2|4,6 0,2",                                                  // x
			"0,6 0,3 1,2 3,2 4,3|4,6 4,1 3,0 1,0",                              // y
			"0,6 4,6 0,2 4,2",                                                  // z
		};
		static_assert(std::size(kGlyphStrokes) == kGlyphChars.size());

		constexpr float kGlyphGridScale = 0.18f;   // grid unit -> cell units ([-1, 1] across the cell)
		constexpr float kGlyphGridMidY = 4.3f;     // vertical centre of the grid in the cell
		constexpr float kGlyphStrokeHalf = 0.11f;  // half stroke width, cell units
		constexpr float kGlyphOutline = 0.0f;      // no baked dark edge: the drawn outline copies take the user's outline colour
		// Advance = ink width + both stroke halves + a gap clear of the outline copies, in grid units.
		constexpr float kGlyphAdvancePad = 2.0f * kGlyphStrokeHalf / kGlyphGridScale + 1.4f;

		using Strokes = std::vector<std::vector<P>>;

		struct Glyph
		{
			Strokes strokes;  // centred horizontally on the cell
			float   width{ 0.0f };  // ink width between stroke centres, grid units
		};

		Glyph ParseGlyph(std::string_view a_text)
		{
			std::vector<std::vector<P>> grid(1);  // grid units first, centred afterwards
			for (std::size_t i = 0; i < a_text.size();) {
				const char c = a_text[i];
				if (c == '|') {
					grid.emplace_back();
					++i;
				} else if (c == ' ') {
					++i;
				} else {
					// "x,y", one digit each.
					grid.back().push_back({ static_cast<float>(a_text[i] - '0'), static_cast<float>(a_text[i + 2] - '0') });
					i += 3;
				}
			}
			float minX = 1e9f, maxX = -1e9f;
			for (const auto& stroke : grid) {
				for (const auto& p : stroke) {
					minX = std::min(minX, p.x);
					maxX = std::max(maxX, p.x);
				}
			}
			Glyph glyph;
			glyph.width = maxX - minX;
			const float midX = (minX + maxX) * 0.5f;
			for (const auto& stroke : grid) {
				auto& out = glyph.strokes.emplace_back();
				for (const auto& p : stroke)
					out.push_back({ (p.x - midX) * kGlyphGridScale, (p.y - kGlyphGridMidY) * kGlyphGridScale });
			}
			return glyph;
		}

		const std::vector<Glyph>& Font()
		{
			static const auto font = [] {
				std::vector<Glyph> all;
				for (const char* text : kGlyphStrokes)
					all.push_back(ParseGlyph(text));
				return all;
			}();
			return font;
		}

		float SdGlyph(int a_glyph, P a_p)
		{
			float d = 1e30f;
			for (const auto& stroke : Font()[static_cast<std::size_t>(a_glyph)].strokes)
				d = std::min(d, DistToPolyline(a_p, stroke));
			return d - kGlyphStrokeHalf;
		}

		float AdvanceEmForWidth(float a_gridWidth) { return (a_gridWidth + kGlyphAdvancePad) * kGlyphGridScale * 0.5f; }

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
			case IconShape::kHorse:
				{
					// Horseshoe: a thick arc open at the bottom, with two nail holes.
					static const auto shoe = [] {
						std::vector<P> v;
						for (int i = 0; i <= 20; ++i) {
							const float a = (-0.28f + 1.56f * static_cast<float>(i) / 20.0f) * std::numbers::pi_v<float>;
							v.push_back({ 0.5f * std::cos(a), 0.08f + 0.55f * std::sin(a) });
						}
						return v;
					}();
					const float holes = std::min(SdCircle(a_p, { -0.47f, 0.25f }, 0.06f), SdCircle(a_p, { 0.47f, 0.25f }, 0.06f));
					return { DistToPolyline(a_p, shoe) - 0.17f, holes };
				}
			case IconShape::kAnvil:
				{
					static const P anvil[]{ { -0.78f, 0.35f }, { 0.72f, 0.35f }, { 0.72f, 0.1f }, { 0.35f, 0.0f }, { 0.22f, -0.3f }, { 0.55f, -0.6f }, { -0.55f, -0.6f },
						{ -0.22f, -0.3f }, { -0.35f, 0.0f }, { -0.55f, 0.08f }, { -0.78f, 0.25f } };
					return { SdPolygon(a_p, anvil), none };
				}
			case IconShape::kMug:
				{
					const float handle = std::fabs(SdCircle(a_p, { 0.38f, 0.0f }, 0.3f)) - 0.09f;
					return { std::min(SdBox(a_p, { -0.12f, -0.02f }, { 0.4f, 0.58f }), handle), SdBox(a_p, { -0.12f, 0.42f }, { 0.3f, 0.06f }) };
				}
			case IconShape::kFlask:
				{
					const float body = std::min(SdCircle(a_p, { 0, -0.25f }, 0.5f), SdBox(a_p, { 0, 0.42f }, { 0.15f, 0.25f }));
					return { std::min(body, SdBox(a_p, { 0, 0.7f }, { 0.25f, 0.07f })), SdBox(a_p, { 0, -0.38f }, { 0.35f, 0.08f }) };
				}
			case IconShape::kBag:
				{
					static const P neck[]{ { -0.18f, 0.25f }, { 0.18f, 0.25f }, { 0.42f, 0.75f }, { -0.42f, 0.75f } };
					return { std::min(SdCircle(a_p, { 0, -0.22f }, 0.56f), SdPolygon(a_p, neck)), SdBox(a_p, { 0, 0.3f }, { 0.22f, 0.05f }) };
				}
			case IconShape::kBook:
				{
					static const P left[]{ { -0.7f, 0.5f }, { -0.06f, 0.4f }, { -0.06f, -0.55f }, { -0.7f, -0.45f } };
					static const P right[]{ { 0.7f, 0.5f }, { 0.7f, -0.45f }, { 0.06f, -0.55f }, { 0.06f, 0.4f } };
					return { std::min(SdPolygon(a_p, left), SdPolygon(a_p, right)), none };
				}
			default:
				if (a_shape >= IconShape::kGlyphFirst && a_shape < IconShape::kCount)
					return { SdGlyph(static_cast<int>(a_shape) - static_cast<int>(IconShape::kGlyphFirst), a_p), none };
				return { none, none };
			}
		}
	}

	AtlasImage BuildIconAtlas()
	{
		constexpr int   kShapeSS = 4;  // supersamples per axis
		constexpr int   kGlyphSS = 3;  // glyphs: fewer (start-up cost), still smooth after the mips
		constexpr float kOutline = 0.14f;
		constexpr float kDark = 25.0f / 255.0f;

		AtlasImage img;
		img.size = kAtlasSize;
		std::vector<std::uint8_t> base(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4, 0);
		for (int s = 0; s < static_cast<int>(IconShape::kCount); ++s) {
			const int cx = (s % kAtlasGrid) * kAtlasCellPx + kAtlasGutterPx, cy = (s / kAtlasGrid) * kAtlasCellPx + kAtlasGutterPx;
			const bool  isGlyph = s >= static_cast<int>(IconShape::kGlyphFirst);
			const int   kSS = isGlyph ? kGlyphSS : kShapeSS;
			const float outline = isGlyph ? kGlyphOutline : kOutline;
			for (int y = 0; y < kAtlasInnerPx; ++y) {
				for (int x = 0; x < kAtlasInnerPx; ++x) {
					float rgb = 0.0f, alpha = 0.0f;
					for (int sy = 0; sy < kSS; ++sy) {
						for (int sx = 0; sx < kSS; ++sx) {
							const P p{ ((x + (sx + 0.5f) / kSS) / kAtlasInnerPx) * 2.0f - 1.0f, 1.0f - ((y + (sy + 0.5f) / kSS) / kAtlasInnerPx) * 2.0f };
							const auto d = Evaluate(static_cast<IconShape>(s), p);
							if (d.outer < 0.0f) {
								rgb += d.inner < 0.0f ? kDark : 1.0f;
								alpha += 1.0f;
							} else if (d.outer < outline) {
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
		// The drawn area only, not the gutter.
		constexpr float cell = 1.0f / kAtlasGrid;
		constexpr float gutter = static_cast<float>(kAtlasGutterPx) / kAtlasSize;
		constexpr float inner = static_cast<float>(kAtlasInnerPx) / kAtlasSize;
		const float     u = static_cast<float>(s % kAtlasGrid) * cell + gutter, v = static_cast<float>(s / kAtlasGrid) * cell + gutter;
		return { u, v, u + inner, v + inner };
	}

	bool GlyphShape(char a_char, IconShape& a_out) noexcept
	{
		const auto pos = kGlyphChars.find(a_char);
		if (pos == std::string_view::npos)
			return false;
		a_out = static_cast<IconShape>(static_cast<int>(IconShape::kGlyphFirst) + static_cast<int>(pos));
		return true;
	}

	float GlyphAdvanceEm(IconShape a_glyph) noexcept
	{
		const int index = static_cast<int>(a_glyph) - static_cast<int>(IconShape::kGlyphFirst);
		if (index < 0 || index >= static_cast<int>(kGlyphChars.size()))
			return SpaceAdvanceEm();
		return AdvanceEmForWidth(Font()[static_cast<std::size_t>(index)].width);
	}

	float SpaceAdvanceEm() noexcept
	{
		static const float space = [] {
			IconShape n;
			GlyphShape('n', n);
			return GlyphAdvanceEm(n);
		}();
		return space;
	}

	float LayoutText(std::string_view a_text, float a_glyphPx, TextAlign a_align, std::vector<GlyphPlacement>& a_out)
	{
		const auto first = a_out.size();
		float      pen = 0.0f;
		for (const char c : a_text) {
			IconShape shape;
			if (GlyphShape(c, shape)) {
				const float advance = a_glyphPx * GlyphAdvanceEm(shape);
				a_out.push_back({ shape, pen + advance * 0.5f });
				pen += advance;
			} else if (c == ' ') {
				pen += a_glyphPx * SpaceAdvanceEm();
			}
		}
		static constexpr float kAlignShift[]{ 0.0f, 0.5f, 1.0f };  // by TextAlign
		const float shift = pen * kAlignShift[static_cast<int>(a_align)];
		for (auto i = first; i < a_out.size(); ++i)
			a_out[i].x -= shift;
		return pen;
	}

	void AppendOutlinedGlyph(const IconInstance& a_glyph, std::uint32_t a_outlineRgba, std::vector<IconInstance>& a_out)
	{
		if ((a_outlineRgba >> 24) != 0) {
			static constexpr Vec2 kDirs[kOutlineCopies]{ { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
			const float           offset = std::fmax(1.0f, a_glyph.size / 14.0f);
			for (const auto& dir : kDirs) {
				IconInstance copy = a_glyph;
				copy.x += dir.x * offset;
				copy.y += dir.y * offset;
				copy.rgba = a_outlineRgba;
				a_out.push_back(copy);
			}
		}
		a_out.push_back(a_glyph);
	}

	void AppendTextLabel(std::vector<IconSource>& a_out, std::string_view a_text, Vec2 a_world, float a_iconSizePx, float a_glyphPx, bool a_clampToRim,
		std::uint32_t a_rgba, std::uint32_t a_outlineRgba)
	{
		std::vector<GlyphPlacement> glyphs;  // a few characters, on a data timer only
		const float                 width = LayoutText(a_text, a_glyphPx, TextAlign::kCenter, glyphs);
		const float                 offsetY = a_iconSizePx * 0.5f + a_glyphPx * 0.3f;
		// Rim margin that keeps the label's far corner inside a round or square edge.
		const float                 reach = std::hypot(width * 0.5f + 1.0f, offsetY + a_glyphPx * 0.5f);
		for (const auto& g : glyphs) {
			IconSource src;
			src.world = a_world;
			src.shape = g.shape;
			src.rgba = a_rgba;
			src.sizePx = a_glyphPx;
			src.clampToRim = a_clampToRim;
			src.anchorSizePx = std::max(a_iconSizePx, reach * 2.0f);
			src.offsetPx = { g.x, offsetY };
			src.flipOffsetInside = true;
			src.outlineRgba = a_outlineRgba;
			a_out.push_back(src);
		}
	}

	CompassMode CompassModeFromIni(long a_iniValue, int a_legacyBool) noexcept
	{
		if (a_iniValue >= 0)
			return static_cast<CompassMode>(std::min<long>(a_iniValue, kCompassModeCount - 1));
		return a_legacyBool == 0 ? CompassMode::kOff : CompassMode::kOutside;
	}

	float CompassLetterDistance(float a_half, float a_letterPx, CompassMode a_mode) noexcept
	{
		constexpr float kGap = 2.0f;
		return a_mode == CompassMode::kOutside ? a_half + a_letterPx * 0.5f + kGap : a_half - a_letterPx * 0.5f - kGap;
	}

	void AppendCompassLetters(const IconView& a_view, float a_glyphPx, std::uint32_t a_rgba, std::uint32_t a_outlineRgba, CompassMode a_mode,
		std::vector<IconInstance>& a_out)
	{
		if (a_mode == CompassMode::kOff)
			return;
		struct Letter
		{
			char  ch;
			Vec2  dir;     // world direction (x east, y north)
			float sizeK;
			float alphaK;
		};
		static constexpr Letter kLetters[]{
			{ 'N', { 0, 1 }, 1.2f, 1.0f },
			{ 'E', { 1, 0 }, 1.0f, 0.8f },
			{ 'S', { 0, -1 }, 1.0f, 0.8f },
			{ 'W', { -1, 0 }, 1.0f, 0.8f },
		};
		const float half = a_view.sizePx * 0.5f;
		for (const auto& letter : kLetters) {
			IconShape shape;
			if (!GlyphShape(letter.ch, shape))
				continue;
			const float size = a_glyphPx * letter.sizeK * a_view.iconScale;
			const Vec2  dir = MapToScreenOffset(letter.dir, a_view.mapAngle);
			// Far out along the direction, then pulled onto a round or square edge at the letter's distance.
			const float edge = CompassLetterDistance(half, size, a_mode);
			const Vec2  q = ClampToRim({ dir.x * a_view.sizePx * 2.0f, dir.y * a_view.sizePx * 2.0f }, edge, a_view.round, 0.0f);
			const auto  alpha = static_cast<std::uint32_t>(std::lround(static_cast<float>(a_rgba >> 24) * letter.alphaK));
			IconInstance inst;
			inst.x = a_view.centerPx.x + q.x;
			inst.y = a_view.centerPx.y + q.y;
			inst.size = size;
			inst.clip = a_mode == CompassMode::kInside ? 1.0f : 0.0f;
			inst.uv = AtlasUv(shape);
			inst.rgba = (a_rgba & 0x00FFFFFF) | (alpha << 24);
			// Outlined, so the letter reads on snow and bright photos as well as on dark ground.
			const auto outlineAlpha = static_cast<std::uint32_t>(std::lround(static_cast<float>(a_outlineRgba >> 24) * letter.alphaK));
			AppendOutlinedGlyph(inst, (a_outlineRgba & 0x00FFFFFF) | (outlineAlpha << 24), a_out);
		}
	}

	bool DeadBodyLog::Add(std::uint32_t a_formId, bool a_killedByPlayer)
	{
		for (auto& entry : _entries) {
			if (entry.formId == a_formId) {
				entry.killedByPlayer = entry.killedByPlayer || a_killedByPlayer;  // the event comes twice (dying, dead)
				return false;
			}
		}
		if (_entries.size() >= static_cast<std::size_t>(kCapacity))
			_entries.erase(_entries.begin());  // oldest out
		_entries.push_back({ a_formId, a_killedByPlayer });
		return true;
	}

	std::string FormatDistance(float a_units)
	{
		const float metres = std::fmax(a_units, 0.0f) / kUnitsPerMetre;
		const long  rounded = std::lround(metres);
		char        text[24];
		if (rounded < 1000)
			std::snprintf(text, sizeof(text), "%ldm", rounded);
		else
			std::snprintf(text, sizeof(text), "%.1fkm", std::round(metres / 100.0f) / 10.0f);
		return text;
	}

	std::string FormatClock(float a_gameHour, bool a_24h)
	{
		const float hour = std::isfinite(a_gameHour) ? a_gameHour : 0.0f;
		const long  minuteOfDay = ((static_cast<long>(std::floor(hour * 60.0f)) % 1440) + 1440) % 1440;
		const long  h = minuteOfDay / 60, m = minuteOfDay % 60;
		char        text[16];
		if (a_24h)
			std::snprintf(text, sizeof(text), "%02ld:%02ld", h, m);
		else
			std::snprintf(text, sizeof(text), "%ld:%02ld %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
		return text;
	}

	bool AllCharsHaveGlyphs(std::string_view a_text) noexcept
	{
		return std::ranges::all_of(a_text, [](char c) { return c == ' ' || kGlyphChars.find(c) != std::string_view::npos; });
	}

	ServiceKind ClassifyVendor(bool a_sellsAllButList, std::uint32_t a_goods) noexcept
	{
		if (a_sellsAllButList)
			return ServiceKind::kGeneralGoods;
		// First match wins: a court wizard's list also has potions, a blacksmith's has no spells.
		static constexpr std::pair<std::uint32_t, ServiceKind> kByGoods[]{
			{ kGoodsSpells, ServiceKind::kSpells },
			{ kGoodsWeaponsArmor, ServiceKind::kBlacksmith },
			{ kGoodsPotions, ServiceKind::kAlchemist },
			{ kGoodsFood, ServiceKind::kInnkeeper },
		};
		for (const auto& [goods, kind] : kByGoods) {
			if (a_goods & goods)
				return kind;
		}
		return ServiceKind::kNone;
	}

	IconShape ShapeForService(ServiceKind a_kind) noexcept
	{
		static constexpr IconShape kShapes[]{ IconShape::kCircle, IconShape::kAnvil, IconShape::kMug, IconShape::kFlask, IconShape::kBag, IconShape::kBook };
		static_assert(std::size(kShapes) == static_cast<std::size_t>(ServiceKind::kCount));
		const auto i = static_cast<std::size_t>(a_kind);
		return i < std::size(kShapes) ? kShapes[i] : IconShape::kCircle;
	}

	DeadMode DeadModeFromIni(long a_iniValue, bool a_legacyBDead) noexcept
	{
		if (a_iniValue < 0)
			return a_legacyBDead ? DeadMode::kAll : DeadMode::kOff;
		return static_cast<DeadMode>(std::min<long>(a_iniValue, kDeadModeCount - 1));
	}

	bool TrailBuffer::Offer(Vec2 a_point, float a_minSpacing)
	{
		if (_count > 0) {
			const Vec2  last = At(_count - 1);
			const float dx = a_point.x - last.x, dy = a_point.y - last.y;
			if (dx * dx + dy * dy < a_minSpacing * a_minSpacing)
				return false;
		}
		_points[static_cast<std::size_t>(_head)] = a_point;
		_head = (_head + 1) % kCapacity;
		_count = std::min(_count + 1, kCapacity);
		return true;
	}

	Vec2 TrailBuffer::At(int a_index) const noexcept
	{
		const int oldest = (_head - _count + kCapacity) % kCapacity;
		return _points[static_cast<std::size_t>((oldest + a_index) % kCapacity)];
	}

	float TrailBuffer::AgeAlpha(int a_index, int a_count) noexcept
	{
		return a_count > 0 ? static_cast<float>(a_index + 1) / static_cast<float>(a_count) : 0.0f;
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
				const float anchor = s.anchorSizePx > 0.0f ? s.anchorSizePx * a_view.iconScale : size;
				q = ClampToRim(q, half, a_view.round, anchor * 0.5f + 1.0f);
			} else if (!InsideMask(q, half, a_view.round)) {
				continue;
			}
			if (s.offsetPx.x != 0.0f || s.offsetPx.y != 0.0f) {
				Vec2 off{ s.offsetPx.x * a_view.iconScale, s.offsetPx.y * a_view.iconScale };
				// Same test for every glyph of a label (x left out), so the whole label flips together.
				if (s.flipOffsetInside && !InsideMask({ q.x, q.y + off.y }, half, a_view.round, size * 0.5f))
					off.y = -off.y;
				q = { q.x + off.x, q.y + off.y };
			}
			IconInstance inst;
			inst.x = a_view.centerPx.x + q.x;
			inst.y = a_view.centerPx.y + q.y;
			inst.size = size;
			inst.rotation = s.rotation;
			inst.clip = 1.0f;
			inst.uv = AtlasUv(s.shape);
			inst.rgba = s.rgba;
			AppendOutlinedGlyph(inst, s.outlineRgba, a_out);
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
