#include "PhotoProcessing.h"

#include "MapMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace MM::BakeImage
{
	namespace
	{
		constexpr float kLuma[3]{ 0.299f, 0.587f, 0.114f };

		// Fallback grade: straight-down noon shots come out hazy and use only part of the range; stretch it and add colour.
		constexpr float kGradeBlack = 0.30f, kGradeWhite = 0.68f, kGradeGamma = 1.05f, kGradeSat = 1.3f;
		constexpr float kFitSaturation = 0.9f;  // takes a little saturation off the matched photos
		constexpr float kTintStrength = 0.5f;

		float HalfToFloat(std::uint16_t a_h)
		{
			const std::uint32_t sign = (a_h & 0x8000u) << 16;
			std::uint32_t       exp = (a_h >> 10) & 0x1F;
			std::uint32_t       man = a_h & 0x3FF;
			std::uint32_t       bits;
			if (exp == 0) {
				if (man == 0) {
					bits = sign;
				} else {  // subnormal: normalise
					exp = 1;
					while (!(man & 0x400)) {
						man <<= 1;
						--exp;
					}
					man &= 0x3FF;
					bits = sign | ((exp + 112) << 23) | (man << 13);
				}
			} else if (exp == 31) {
				bits = sign | 0x7F800000u | (man << 13);
			} else {
				bits = sign | ((exp + 112) << 23) | (man << 13);
			}
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		}

		float LinearToSrgb255(float a_v)
		{
			if (!(a_v > 0.0f))  // also catches NaN
				return 0.0f;
			if (a_v >= 1.0f)
				return 255.0f;
			const float s = a_v <= 0.0031308f ? a_v * 12.92f : 1.055f * std::pow(a_v, 1.0f / 2.4f) - 0.055f;
			return s * 255.0f;
		}

		float Sinc(float a_x)
		{
			if (std::fabs(a_x) < 1e-6f)
				return 1.0f;
			const float p = std::numbers::pi_v<float> * a_x;
			return std::sin(p) / p;
		}

		float Lanczos3(float a_x) { return (a_x > -3.0f && a_x < 3.0f) ? Sinc(a_x) * Sinc(a_x / 3.0f) : 0.0f; }

		// Weights of one output pixel along one axis, PIL style: the filter widens by the scale when shrinking,
		// and taps outside the image are dropped and the rest renormalised.
		struct Taps
		{
			int                first{ 0 };
			std::vector<float> w;
		};

		std::vector<Taps> AxisTaps(double a_begin, double a_end, int a_out, int a_inSize)
		{
			std::vector<Taps> taps(static_cast<std::size_t>(a_out));
			const double      scale = (a_end - a_begin) / a_out;
			const double      fscale = std::max(scale, 1.0);
			const double      support = 3.0 * fscale;
			for (int i = 0; i < a_out; ++i) {
				const double centre = a_begin + (i + 0.5) * scale;
				const int    lo = std::max(0, static_cast<int>(std::floor(centre - support)));
				const int    hi = std::min(a_inSize - 1, static_cast<int>(std::ceil(centre + support)));
				auto&        t = taps[static_cast<std::size_t>(i)];
				t.first = lo;
				float sum = 0.0f;
				for (int j = lo; j <= hi; ++j) {
					const float v = Lanczos3(static_cast<float>((j + 0.5 - centre) / fscale));
					t.w.push_back(v);
					sum += v;
				}
				if (std::fabs(sum) > 1e-6f) {
					for (auto& v : t.w)
						v /= sum;
				}
			}
			return taps;
		}

		std::uint16_t To565(const float (&a_c)[3])
		{
			const auto q = [](float a_v, int a_max) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(a_v * a_max / 255.0f)), 0, a_max)); };
			return static_cast<std::uint16_t>((q(a_c[0], 31) << 11) | (q(a_c[1], 63) << 5) | q(a_c[2], 31));
		}

		void From565(std::uint16_t a_c, float (&a_out)[3])
		{
			const int r = (a_c >> 11) & 31, g = (a_c >> 5) & 63, b = a_c & 31;
			a_out[0] = static_cast<float>((r << 3) | (r >> 2));
			a_out[1] = static_cast<float>((g << 2) | (g >> 4));
			a_out[2] = static_cast<float>((b << 3) | (b >> 2));
		}

		std::uint8_t Byte(float a_v) { return static_cast<std::uint8_t>(std::clamp(a_v, 0.0f, 255.0f)); }  // truncates like numpy astype
	}

	bool IsSupportedFormat(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return true;
		default:
			return false;
		}
	}

	namespace
	{
		// One pixel of a supported back buffer row -> RGB 0..255 display values.
		void DecodePixel(const std::uint8_t* a_row, int a_x, DXGI_FORMAT a_format, float* a_out)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				a_out[0] = a_row[a_x * 4 + 0];
				a_out[1] = a_row[a_x * 4 + 1];
				a_out[2] = a_row[a_x * 4 + 2];
				break;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				{
					std::uint32_t v;
					std::memcpy(&v, a_row + a_x * 4, 4);
					a_out[0] = static_cast<float>(v & 1023) * (255.0f / 1023.0f);
					a_out[1] = static_cast<float>((v >> 10) & 1023) * (255.0f / 1023.0f);
					a_out[2] = static_cast<float>((v >> 20) & 1023) * (255.0f / 1023.0f);
				}
				break;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				{
					std::uint16_t h[3];
					std::memcpy(h, a_row + a_x * 8, 6);
					for (int c = 0; c < 3; ++c)
						a_out[c] = LinearToSrgb255(HalfToFloat(h[c]));
				}
				break;
			default:  // the B8G8R8A8 / B8G8R8X8 family
				a_out[0] = a_row[a_x * 4 + 2];
				a_out[1] = a_row[a_x * 4 + 1];
				a_out[2] = a_row[a_x * 4 + 0];
				break;
			}
		}
	}

	bool ConvertPhoto(const std::uint8_t* a_data, int a_w, int a_h, std::size_t a_rowPitch, DXGI_FORMAT a_format, RgbImage& a_out)
	{
		if (!a_data || a_w <= 0 || a_h <= 0 || !IsSupportedFormat(a_format))
			return false;
		a_out.w = a_w;
		a_out.h = a_h;
		a_out.px.resize(static_cast<std::size_t>(a_w) * a_h * 3);
		float* o = a_out.px.data();
		for (int y = 0; y < a_h; ++y) {
			const std::uint8_t* row = a_data + a_rowPitch * static_cast<std::size_t>(y);
			for (int x = 0; x < a_w; ++x, o += 3)
				DecodePixel(row, x, a_format, o);
		}
		return true;
	}

	std::vector<float> LumaThumbnail(const std::uint8_t* a_data, int a_w, int a_h, std::size_t a_rowPitch, DXGI_FORMAT a_format, int a_size)
	{
		if (!a_data || a_w <= 0 || a_h <= 0 || a_size <= 0 || !IsSupportedFormat(a_format))
			return {};
		constexpr int      kSamplesPerSide = 4;  // per thumbnail pixel: 4 x 4 photo pixels averaged
		const int          side = std::min(a_w, a_h);
		const int          x0 = (a_w - side) / 2, y0 = (a_h - side) / 2;
		const int          samples = a_size * kSamplesPerSide;
		std::vector<float> thumb(static_cast<std::size_t>(a_size) * a_size, 0.0f);
		for (int sy = 0; sy < samples; ++sy) {
			const int           py = y0 + static_cast<int>((static_cast<long long>(sy) * 2 + 1) * side / (samples * 2));
			const std::uint8_t* row = a_data + a_rowPitch * static_cast<std::size_t>(py);
			for (int sx = 0; sx < samples; ++sx) {
				const int px = x0 + static_cast<int>((static_cast<long long>(sx) * 2 + 1) * side / (samples * 2));
				float     rgb[3];
				DecodePixel(row, px, a_format, rgb);
				thumb[static_cast<std::size_t>(sy / kSamplesPerSide) * a_size + sx / kSamplesPerSide] += rgb[0] * kLuma[0] + rgb[1] * kLuma[1] + rgb[2] * kLuma[2];
			}
		}
		for (auto& v : thumb)
			v /= static_cast<float>(kSamplesPerSide * kSamplesPerSide);
		return thumb;
	}

	float MeanAbsDiff(std::span<const float> a_lhs, std::span<const float> a_rhs)
	{
		if (a_lhs.empty() || a_lhs.size() != a_rhs.size())
			return -1.0f;
		double sum = 0;
		for (std::size_t i = 0; i < a_lhs.size(); ++i)
			sum += std::abs(a_lhs[i] - a_rhs[i]);
		return static_cast<float>(sum / static_cast<double>(a_lhs.size()));
	}

	float CentreContrast(const RgbImage& a_img)
	{
		double sum = 0, sum2 = 0;
		std::size_t n = 0;
		for (int y = a_img.h / 4; y < 3 * a_img.h / 4; ++y) {
			const float* p = a_img.px.data() + (static_cast<std::size_t>(y) * a_img.w + a_img.w / 4) * 3;
			for (int x = a_img.w / 4; x < 3 * a_img.w / 4; ++x, p += 3) {
				const double l = p[0] * kLuma[0] + p[1] * kLuma[1] + p[2] * kLuma[2];
				sum += l;
				sum2 += l * l;
				++n;
			}
		}
		if (n == 0)
			return 0.0f;
		const double mean = sum / static_cast<double>(n);
		return static_cast<float>(std::sqrt(std::max(0.0, sum2 / static_cast<double>(n) - mean * mean)));
	}

	float ReferenceContrast(std::span<const std::uint8_t> a_rgba, int a_size)
	{
		if (a_size <= 0 || a_rgba.size() < static_cast<std::size_t>(a_size) * a_size * 4)
			return -1.0f;
		RgbImage img;
		img.w = img.h = a_size;
		img.px.resize(static_cast<std::size_t>(a_size) * a_size * 3);
		for (std::size_t p = 0; p < static_cast<std::size_t>(a_size) * a_size; ++p)
			for (int c = 0; c < 3; ++c)
				img.px[p * 3 + c] = a_rgba[p * 4 + c];
		return CentreContrast(img);
	}

	bool NeedsReferenceContrast(float a_photo) { return a_photo >= kEmptyContrast && a_photo < kFlatContrast; }

	ContrastVerdict JudgeContrast(float a_photo, float a_reference)
	{
		if (a_photo < 0.0f || a_photo >= kFlatContrast)
			return ContrastVerdict::kKeep;  // no contrast measured (the photo could not be read) is not a reason to drop it
		if (a_photo < kEmptyContrast)
			return ContrastVerdict::kEmpty;
		if (a_reference >= 0.0f && a_photo >= a_reference * kPlainGroundRatio)
			return ContrastVerdict::kKeep;  // plain snow, ice or sand: the reference is plain there too
		return ContrastVerdict::kFlat;
	}

	float ClippedWhiteFraction(const RgbImage& a_img)
	{
		const std::size_t pixels = static_cast<std::size_t>(std::max(a_img.w, 0)) * static_cast<std::size_t>(std::max(a_img.h, 0));
		if (pixels == 0 || a_img.px.size() < pixels * 3)
			return 0.0f;
		std::size_t clipped = 0;
		for (std::size_t p = 0; p < pixels; ++p) {
			const float* c = a_img.px.data() + p * 3;
			if (c[0] * kLuma[0] + c[1] * kLuma[1] + c[2] * kLuma[2] >= kClippedLuma)
				++clipped;
		}
		return static_cast<float>(static_cast<double>(clipped) / static_cast<double>(pixels));
	}

	float BakeFovDeg(int a_w, int a_h)
	{
		const double aspect = a_h > 0 ? static_cast<double>(a_w) / a_h : 16.0 / 9.0;
		const double widen = std::max(1.0, aspect / (16.0 / 9.0));
		const double half = std::atan(std::tan(kBaseFovDeg * std::numbers::pi / 360.0) * widen);
		return static_cast<float>(half * 360.0 / std::numbers::pi);
	}

	int ClampTileSize(int a_size)
	{
		int size = kMinTileSize;
		while (size * 2 <= std::min(a_size, kMaxTileSize))
			size *= 2;
		return size;
	}

	float ClampPhotoExposure(float a_exposure) { return std::isfinite(a_exposure) ? std::clamp(a_exposure, 0.25f, 1.0f) : 1.0f; }

	float CameraHeight(int a_grid) { return kCameraHeight * static_cast<float>(a_grid) / 2.0f; }

	int ReferenceSize(int a_grid) { return kSampleSize * a_grid; }

	double UnitsPerPixel(int a_w, float a_fovDeg, int a_grid)
	{
		return 2.0 * CameraHeight(a_grid) * std::tan(a_fovDeg * std::numbers::pi / 360.0) / std::max(a_w, 1);
	}

	double PhotoPixelsPerCell(int a_w, float a_fovDeg, int a_grid) { return kCellUnits / UnitsPerPixel(a_w, a_fovDeg, a_grid); }

	bool TooFewPixelsPerCell(int a_w, float a_fovDeg, int a_grid, int a_tileSize) { return a_w > 0 && PhotoPixelsPerCell(a_w, a_fovDeg, a_grid) < a_tileSize; }

	std::vector<std::uint8_t> CropCell(const RgbImage& a_photo, float a_fovDeg, int a_grid, int a_dx, int a_dy, int a_tileSize)
	{
		// The block's south-west corner sits half a block left of and below the picture centre.
		const double cellPx = PhotoPixelsPerCell(a_photo.w, a_fovDeg, a_grid);
		const double x0 = a_photo.w / 2.0 - a_grid * cellPx / 2 + a_dx * cellPx;
		const double y1 = a_photo.h / 2.0 + a_grid * cellPx / 2 - a_dy * cellPx;  // south edge of the cell
		const double y0 = y1 - cellPx;
		const auto   tx = AxisTaps(x0, x0 + cellPx, a_tileSize, a_photo.w);
		const auto   ty = AxisTaps(y0, y1, a_tileSize, a_photo.h);

		// Horizontal pass over the source rows the vertical taps need, then the vertical pass.
		int rowLo = a_photo.h, rowHi = -1;
		for (const auto& t : ty) {
			rowLo = std::min(rowLo, t.first);
			rowHi = std::max(rowHi, t.first + static_cast<int>(t.w.size()) - 1);
		}
		std::vector<std::uint8_t> out(static_cast<std::size_t>(a_tileSize) * a_tileSize * 3, 0);
		if (rowHi < rowLo)
			return out;
		const int          rows = rowHi - rowLo + 1;
		std::vector<float> mid(static_cast<std::size_t>(rows) * a_tileSize * 3, 0.0f);
		for (int r = 0; r < rows; ++r) {
			const float* src = a_photo.px.data() + static_cast<std::size_t>(rowLo + r) * a_photo.w * 3;
			float*       dst = mid.data() + static_cast<std::size_t>(r) * a_tileSize * 3;
			for (int i = 0; i < a_tileSize; ++i) {
				const auto& t = tx[static_cast<std::size_t>(i)];
				float       c[3]{};
				for (std::size_t k = 0; k < t.w.size(); ++k) {
					const float* p = src + (static_cast<std::size_t>(t.first) + k) * 3;
					c[0] += p[0] * t.w[k];
					c[1] += p[1] * t.w[k];
					c[2] += p[2] * t.w[k];
				}
				dst[i * 3 + 0] = c[0];
				dst[i * 3 + 1] = c[1];
				dst[i * 3 + 2] = c[2];
			}
		}
		for (int j = 0; j < a_tileSize; ++j) {
			const auto& t = ty[static_cast<std::size_t>(j)];
			for (int i = 0; i < a_tileSize; ++i) {
				float c[3]{};
				for (std::size_t k = 0; k < t.w.size(); ++k) {
					const float* p = mid.data() + ((static_cast<std::size_t>(t.first - rowLo) + k) * a_tileSize + i) * 3;
					c[0] += p[0] * t.w[k];
					c[1] += p[1] * t.w[k];
					c[2] += p[2] * t.w[k];
				}
				auto* o = out.data() + (static_cast<std::size_t>(j) * a_tileSize + i) * 3;
				for (int ch = 0; ch < 3; ++ch)
					o[ch] = static_cast<std::uint8_t>(std::clamp(std::lround(c[ch]), 0L, 255L));
			}
		}
		return out;
	}

	std::vector<std::uint8_t> ReferenceCell(std::span<const std::uint8_t> a_rgba, int a_grid, int a_dx, int a_dy)
	{
		constexpr int             q = kSampleSize;
		const int                 refSize = ReferenceSize(a_grid);
		std::vector<std::uint8_t> cell(static_cast<std::size_t>(q) * q * 3);
		if (a_rgba.size() < static_cast<std::size_t>(refSize) * refSize * 4)
			return cell;
		for (int y = 0; y < q; ++y) {
			for (int x = 0; x < q; ++x) {
				// Reference row 0 is north, so the block's northern cells (high dy) are the top rows.
				const auto* p = a_rgba.data() + (static_cast<std::size_t>((a_grid - 1 - a_dy) * q + y) * refSize + static_cast<std::size_t>(a_dx * q + x)) * 4;
				auto*       o = cell.data() + (static_cast<std::size_t>(y) * q + x) * 3;
				o[0] = p[0];
				o[1] = p[1];
				o[2] = p[2];
			}
		}
		return cell;
	}

	std::vector<std::uint8_t> BoxDown(std::span<const std::uint8_t> a_rgb, int a_size, int a_out)
	{
		const int                 f = a_size / a_out;
		std::vector<std::uint8_t> out(static_cast<std::size_t>(a_out) * a_out * 3);
		for (int y = 0; y < a_out; ++y) {
			for (int x = 0; x < a_out; ++x) {
				std::uint32_t c[3]{};
				for (int v = 0; v < f; ++v) {
					const auto* p = a_rgb.data() + ((static_cast<std::size_t>(y) * f + v) * a_size + static_cast<std::size_t>(x) * f) * 3;
					for (int u = 0; u < f; ++u, p += 3) {
						c[0] += p[0];
						c[1] += p[1];
						c[2] += p[2];
					}
				}
				const std::uint32_t n = static_cast<std::uint32_t>(f * f);
				for (int ch = 0; ch < 3; ++ch)
					out[(static_cast<std::size_t>(y) * a_out + x) * 3 + ch] = static_cast<std::uint8_t>((c[ch] + n / 2) / n);
			}
		}
		return out;
	}

	void CapHighlights(std::array<float, 256>& a_lut)
	{
		constexpr float knee = static_cast<float>(kHighlightKnee);
		const float     kneeOut = std::min(a_lut[kHighlightKnee], knee + (255.0f - knee) / 2.0f);
		for (int v = 0; v < kHighlightKnee; ++v)
			a_lut[v] = std::min(a_lut[v], kneeOut);  // keeps the curve monotonic when the knee was lowered
		for (int v = kHighlightKnee; v < 256; ++v) {
			const float line = kneeOut + (255.0f - kneeOut) * (static_cast<float>(v) - knee) / (255.0f - knee);
			a_lut[v] = std::min(a_lut[v], line);
		}
	}

	Tone FitMatch(std::span<const std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> a_pairs)
	{
		Tone               tone;
		std::vector<float> ya, yb;
		double             sumA[3]{}, sumB[3]{}, sumYa = 0, sumYb = 0;
		for (const auto& [a, b] : a_pairs) {
			const std::size_t n = std::min(a.size(), b.size()) / 3;
			for (std::size_t i = 0; i < n; ++i) {
				float la = 0, lb = 0;
				for (int c = 0; c < 3; ++c) {
					la += a[i * 3 + c] * kLuma[c];
					lb += b[i * 3 + c] * kLuma[c];
					sumA[c] += a[i * 3 + c];
					sumB[c] += b[i * 3 + c];
				}
				ya.push_back(la);
				yb.push_back(lb);
				sumYa += la;
				sumYb += lb;
			}
		}
		if (ya.size() < 2)
			return tone;
		std::ranges::sort(ya);
		std::ranges::sort(yb);
		// numpy.percentile (linear) at 0..100, made strictly increasing so the curve can be inverted.
		const auto pct = [](const std::vector<float>& a_v, int a_q) {
			const double pos = a_q / 100.0 * static_cast<double>(a_v.size() - 1);
			const auto   lo = static_cast<std::size_t>(pos);
			const auto   hi = std::min(lo + 1, a_v.size() - 1);
			return static_cast<float>(a_v[lo] + (a_v[hi] - a_v[lo]) * (pos - static_cast<double>(lo)));
		};
		std::array<float, 101> pa{}, pb{};
		for (int q = 0; q <= 100; ++q) {
			pa[q] = pct(ya, q) + static_cast<float>(q) * 1e-4f;
			pb[q] = pct(yb, q);
			if (q > 0)
				pa[q] = std::max(pa[q], pa[q - 1]);
		}
		// numpy.interp: clamped at both ends.
		for (int v = 0; v < 256; ++v) {
			const float x = static_cast<float>(v);
			float       y;
			if (x <= pa[0]) {
				y = pb[0];
			} else if (x >= pa[100]) {
				y = pb[100];
			} else {
				int k = 0;
				while (k < 99 && pa[k + 1] < x)
					++k;
				const float span = pa[k + 1] - pa[k];
				y = span > 0 ? pb[k] + (pb[k + 1] - pb[k]) * (x - pa[k]) / span : pb[k + 1];
			}
			tone.lut[v] = y;
		}
		CapHighlights(tone.lut);
		const double n = static_cast<double>(ya.size());
		const double meanYa = std::max(sumYa / n, 1e-3), meanYb = std::max(sumYb / n, 1e-3);
		for (int c = 0; c < 3; ++c) {
			const double ta = sumA[c] / n / meanYa, tb = sumB[c] / n / meanYb;
			tone.gain[c] = static_cast<float>(ta > 1e-6 ? 1.0 + kTintStrength * (tb / ta - 1.0) : 1.0);
		}
		tone.fitted = true;
		return tone;
	}

	void ApplyTone(const Tone& a_tone, std::span<std::uint8_t> a_rgb)
	{
		for (std::size_t i = 0; i + 2 < a_rgb.size(); i += 3) {
			float c[3]{ static_cast<float>(a_rgb[i]), static_cast<float>(a_rgb[i + 1]), static_cast<float>(a_rgb[i + 2]) };
			if (a_tone.fitted) {
				const float y = c[0] * kLuma[0] + c[1] * kLuma[1] + c[2] * kLuma[2];
				const float ny = a_tone.lut[static_cast<std::size_t>(std::clamp(y, 0.0f, 255.0f))];
				const float k = ny / std::max(y, 1.0f);
				for (int ch = 0; ch < 3; ++ch)
					c[ch] *= k * a_tone.gain[ch];
				const float g = (c[0] + c[1] + c[2]) / 3.0f;
				for (int ch = 0; ch < 3; ++ch)
					a_rgb[i + ch] = Byte(g + (c[ch] - g) * kFitSaturation);
			} else {
				for (auto& v : c)
					v = std::pow(std::clamp((v / 255.0f - kGradeBlack) / (kGradeWhite - kGradeBlack), 0.0f, 1.0f), kGradeGamma);
				const float g = (c[0] + c[1] + c[2]) / 3.0f;
				for (int ch = 0; ch < 3; ++ch)
					a_rgb[i + ch] = Byte(std::clamp(g + (c[ch] - g) * kGradeSat, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
	}

	void EncodeBc1Block(const float (&a_px)[16][3], std::uint8_t (&a_out)[8])
	{
		// Range fit: endpoints at the extremes along the block's principal colour axis, pulled in by 1/16.
		float mean[3]{};
		for (const auto& p : a_px)
			for (int c = 0; c < 3; ++c)
				mean[c] += p[c] / 16.0f;
		float cov[6]{};  // rr rg rb gg gb bb
		for (const auto& p : a_px) {
			const float d[3]{ p[0] - mean[0], p[1] - mean[1], p[2] - mean[2] };
			cov[0] += d[0] * d[0];
			cov[1] += d[0] * d[1];
			cov[2] += d[0] * d[2];
			cov[3] += d[1] * d[1];
			cov[4] += d[1] * d[2];
			cov[5] += d[2] * d[2];
		}
		float axis[3]{ 0.577f, 0.577f, 0.577f };
		for (int it = 0; it < 8; ++it) {
			const float v[3]{ cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
				cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (len < 1e-6f)
				break;
			for (int c = 0; c < 3; ++c)
				axis[c] = v[c] / len;
		}
		float lo = 1e9f, hi = -1e9f;
		for (const auto& p : a_px) {
			const float t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2];
			lo = std::min(lo, t);
			hi = std::max(hi, t);
		}
		const float inset = (hi - lo) / 16.0f;
		lo += inset;
		hi -= inset;
		float e0[3], e1[3];
		for (int c = 0; c < 3; ++c) {
			e0[c] = std::clamp(mean[c] + axis[c] * hi, 0.0f, 255.0f);
			e1[c] = std::clamp(mean[c] + axis[c] * lo, 0.0f, 255.0f);
		}
		std::uint16_t c0 = To565(e0), c1 = To565(e1);
		if (c0 < c1)
			std::swap(c0, c1);
		std::uint32_t bits = 0;
		if (c0 != c1) {  // c0 > c1: four-colour mode
			float pal[4][3];
			From565(c0, pal[0]);
			From565(c1, pal[1]);
			for (int c = 0; c < 3; ++c) {
				pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3.0f;
				pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3.0f;
			}
			for (int i = 0; i < 16; ++i) {
				int   best = 0;
				float bestD = 1e30f;
				for (int k = 0; k < 4; ++k) {
					const float dr = a_px[i][0] - pal[k][0], dg = a_px[i][1] - pal[k][1], db = a_px[i][2] - pal[k][2];
					const float d = dr * dr + dg * dg + db * db;
					if (d < bestD) {
						bestD = d;
						best = k;
					}
				}
				bits |= static_cast<std::uint32_t>(best) << (i * 2);
			}
		}
		a_out[0] = static_cast<std::uint8_t>(c0 & 0xFF);
		a_out[1] = static_cast<std::uint8_t>(c0 >> 8);
		a_out[2] = static_cast<std::uint8_t>(c1 & 0xFF);
		a_out[3] = static_cast<std::uint8_t>(c1 >> 8);
		for (int b = 0; b < 4; ++b)
			a_out[4 + b] = static_cast<std::uint8_t>((bits >> (b * 8)) & 0xFF);
	}

	void DecodeBc1Block(const std::uint8_t (&a_in)[8], std::uint8_t (&a_px)[16][3])
	{
		const auto c0 = static_cast<std::uint16_t>(a_in[0] | (a_in[1] << 8)), c1 = static_cast<std::uint16_t>(a_in[2] | (a_in[3] << 8));
		float      pal[4][3];
		From565(c0, pal[0]);
		From565(c1, pal[1]);
		for (int c = 0; c < 3; ++c) {
			if (c0 > c1) {
				pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3.0f;
				pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3.0f;
			} else {
				pal[2][c] = (pal[0][c] + pal[1][c]) / 2.0f;
				pal[3][c] = 0.0f;
			}
		}
		const std::uint32_t bits = a_in[4] | (a_in[5] << 8) | (a_in[6] << 16) | (static_cast<std::uint32_t>(a_in[7]) << 24);
		for (int i = 0; i < 16; ++i)
			for (int c = 0; c < 3; ++c)
				a_px[i][c] = static_cast<std::uint8_t>(std::lround(pal[(bits >> (i * 2)) & 3][c]));
	}

	std::vector<std::uint8_t> EncodeDdsBc1(std::span<const std::uint8_t> a_rgb, int a_size)
	{
		int mips = 1;
		while ((a_size >> (mips - 1)) > 1)
			++mips;

		std::vector<std::uint8_t> out(4 + 124, 0);
		const auto                put = [&](std::size_t a_at, std::uint32_t a_v) { std::memcpy(out.data() + a_at, &a_v, 4); };
		std::memcpy(out.data(), "DDS ", 4);
		const auto blocks = static_cast<std::uint32_t>(std::max(1, a_size / 4));
		put(4, 124);                                           // header size
		put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);  // CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE
		put(12, static_cast<std::uint32_t>(a_size));           // height
		put(16, static_cast<std::uint32_t>(a_size));           // width
		put(20, blocks * blocks * 8);                          // byte size of the top mip
		put(28, static_cast<std::uint32_t>(mips));
		put(76, 32);   // pixel format size
		put(80, 0x4);  // DDPF_FOURCC
		std::memcpy(out.data() + 84, "DXT1", 4);
		put(108, 0x1000 | 0x400000 | 0x8);  // TEXTURE MIPMAP COMPLEX

		std::vector<float> level(a_rgb.begin(), a_rgb.end());
		int                size = a_size;
		for (int m = 0; m < mips; ++m) {
			const int nb = std::max(1, size / 4);
			for (int by = 0; by < nb; ++by) {
				for (int bx = 0; bx < nb; ++bx) {
					float px[16][3];
					for (int y = 0; y < 4; ++y) {
						for (int x = 0; x < 4; ++x) {
							// Mips under 4 px repeat their edge pixels.
							const int sx = std::min(bx * 4 + x, size - 1), sy = std::min(by * 4 + y, size - 1);
							for (int c = 0; c < 3; ++c)
								px[y * 4 + x][c] = level[(static_cast<std::size_t>(sy) * size + sx) * 3 + c];
						}
					}
					std::uint8_t block[8];
					EncodeBc1Block(px, block);
					out.insert(out.end(), block, block + 8);
				}
			}
			if (size == 1)
				break;
			const int          half = size / 2;
			std::vector<float> next(static_cast<std::size_t>(half) * half * 3);
			for (int y = 0; y < half; ++y) {
				for (int x = 0; x < half; ++x) {
					for (int c = 0; c < 3; ++c) {
						const auto at = [&](int a_x, int a_y) { return level[(static_cast<std::size_t>(a_y) * size + a_x) * 3 + c]; };
						next[(static_cast<std::size_t>(y) * half + x) * 3 + c] = (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) * 0.25f;
					}
				}
			}
			level = std::move(next);
			size = half;
		}
		return out;
	}

	std::vector<std::uint8_t> EncodeBmp24(std::span<const std::uint8_t> a_rgb, int a_w, int a_h)
	{
		const int                 row = (a_w * 3 + 3) & ~3;  // rows are padded to 4 bytes
		std::vector<std::uint8_t> out(54 + static_cast<std::size_t>(row) * a_h, 0);
		if (a_w <= 0 || a_h <= 0 || a_rgb.size() < static_cast<std::size_t>(a_w) * a_h * 3)
			return {};
		const auto put = [&](std::size_t a_at, std::uint32_t a_v) { std::memcpy(out.data() + a_at, &a_v, 4); };
		out[0] = 'B';
		out[1] = 'M';
		put(2, static_cast<std::uint32_t>(out.size()));
		put(10, 54);  // pixel data offset
		put(14, 40);  // BITMAPINFOHEADER size
		put(18, static_cast<std::uint32_t>(a_w));
		put(22, static_cast<std::uint32_t>(a_h));  // positive height: bottom row first
		out[26] = 1;   // planes
		out[28] = 24;  // bits per pixel
		for (int y = 0; y < a_h; ++y) {
			const auto* src = a_rgb.data() + static_cast<std::size_t>(y) * a_w * 3;
			auto*       dst = out.data() + 54 + static_cast<std::size_t>(a_h - 1 - y) * row;
			for (int x = 0; x < a_w; ++x) {
				dst[x * 3 + 0] = src[x * 3 + 2];
				dst[x * 3 + 1] = src[x * 3 + 1];
				dst[x * 3 + 2] = src[x * 3 + 0];
			}
		}
		return out;
	}
}
