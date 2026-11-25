#include "MasterTone.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <vector>

namespace PhotoBaker
{
	namespace
	{
		using MM::BakeImage::Tone;

		std::string_view Trim(std::string_view a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r");
			if (b == std::string_view::npos)
				return {};
			const auto e = a_s.find_last_not_of(" \t\r");
			return a_s.substr(b, e - b + 1);
		}

		// Exactly a_out.size() numbers separated by spaces; locale-independent.
		bool ReadFloats(std::string_view a_text, std::span<float> a_out)
		{
			std::size_t count = 0;
			const char* at = a_text.data();
			const char* end = a_text.data() + a_text.size();
			while (true) {
				while (at < end && (*at == ' ' || *at == '\t'))
					++at;
				if (at == end)
					break;
				if (count == a_out.size())
					return false;
				const auto [next, error] = std::from_chars(at, end, a_out[count]);
				if (error != std::errc{} || next == at)
					return false;
				at = next;
				++count;
			}
			return count == a_out.size();
		}

		bool ReadFitted(std::string_view a_value, Tone& a_tone)
		{
			if (a_value != "0" && a_value != "1")
				return false;
			a_tone.fitted = a_value == "1";
			return true;
		}
		bool ReadGain(std::string_view a_value, Tone& a_tone) { return ReadFloats(a_value, a_tone.gain); }
		bool ReadLut(std::string_view a_value, Tone& a_tone) { return ReadFloats(a_value, a_tone.lut); }

		struct ToneKey
		{
			std::string_view name;
			bool (*read)(std::string_view, Tone&);
		};
		constexpr std::array<ToneKey, 3> kToneKeys{ { { "fitted", ReadFitted }, { "gain", ReadGain }, { "lut", ReadLut } } };

		std::uint8_t RoundByte(float a_v) { return static_cast<std::uint8_t>(std::clamp(a_v + 0.5f, 0.0f, 255.0f)); }
	}

	bool ParseToneText(std::string_view a_text, Tone& a_out)
	{
		a_out = Tone{};
		Tone                    tone;
		std::array<bool, 3>     seen{};
		while (!a_text.empty()) {
			const auto eol = a_text.find('\n');
			const auto line = Trim(a_text.substr(0, eol));
			a_text.remove_prefix(eol == std::string_view::npos ? a_text.size() : eol + 1);
			if (line.empty())
				continue;
			const auto space = line.find_first_of(" \t");
			const auto name = line.substr(0, space);
			const auto value = space == std::string_view::npos ? std::string_view{} : Trim(line.substr(space));
			const auto key = std::ranges::find(kToneKeys, name, &ToneKey::name);
			if (key == kToneKeys.end())
				continue;  // a key from a newer plugin
			if (!key->read(value, tone))
				return false;
			seen[static_cast<std::size_t>(key - kToneKeys.begin())] = true;
		}
		if (!seen[0])
			return false;
		if (tone.fitted && !(seen[1] && seen[2]))
			return false;
		a_out = tone;
		return true;
	}

	bool IsNeutral(const Recolour& a_recolour) { return a_recolour.brightness == 1.0f && a_recolour.saturation == 1.0f; }

	void ApplyRecolour(const Recolour& a_recolour, std::span<std::uint8_t> a_rgb)
	{
		if (IsNeutral(a_recolour))
			return;
		for (std::size_t i = 0; i + 2 < a_rgb.size(); i += 3) {
			float c[3]{};
			for (int ch = 0; ch < 3; ++ch)
				c[ch] = static_cast<float>(a_rgb[i + ch]) * a_recolour.brightness;
			const float grey = (c[0] + c[1] + c[2]) / 3.0f;  // the same grey ApplyTone saturates around
			for (int ch = 0; ch < 3; ++ch)
				a_rgb[i + ch] = RoundByte(grey + (c[ch] - grey) * a_recolour.saturation);
		}
	}

	void ToneMasterRgba(const Tone& a_tone, const Recolour& a_recolour, std::span<std::uint8_t> a_rgba)
	{
		const std::size_t         pixels = a_rgba.size() / 4;
		std::vector<std::uint8_t> rgb(pixels * 3);
		for (std::size_t p = 0; p < pixels; ++p)
			std::copy_n(a_rgba.data() + p * 4, 3, rgb.data() + p * 3);
		MM::BakeImage::ApplyTone(a_tone, rgb);
		ApplyRecolour(a_recolour, rgb);
		for (std::size_t p = 0; p < pixels; ++p)
			std::copy_n(rgb.data() + p * 3, 3, a_rgba.data() + p * 4);
	}

	std::string ColourStamp(std::string_view a_toneText, const Recolour& a_recolour)
	{
		return std::format("brightness {:.2f}\nsaturation {:.2f}\n{}", a_recolour.brightness, a_recolour.saturation, a_toneText);
	}
}
