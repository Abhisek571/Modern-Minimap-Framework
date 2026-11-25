#pragma once

// Colour of the BC7 tiles made from the plugin's untoned master images (no window, no DirectXTex, no files), so the
// offline test tool can check it: the tone the plugin saved in tone.txt, applied with the plugin's own ApplyTone so
// the tiles match its BC1 tiles, then the player's optional brightness and saturation from "Encode existing masters".

#include "../PhotoBake/PhotoProcessing.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace PhotoBaker
{
	// <masters folder>\tone.txt, one key per line: "fitted 1|0", "gain <r> <g> <b>", "lut <256 numbers>".
	// False when the text is not a valid tone (a_out is then the unfitted fallback tone, like a missing file).
	bool ParseToneText(std::string_view a_text, MM::BakeImage::Tone& a_out);

	// Brightness and saturation sliders, as factors; 1.0 each leaves the toned colours exactly as they are.
	inline constexpr int   kRecolourMinPercent = 80;
	inline constexpr int   kRecolourMaxPercent = 120;
	inline constexpr int   kRecolourNeutralPercent = 100;
	struct Recolour
	{
		float brightness{ 1.0f };
		float saturation{ 1.0f };
	};
	bool IsNeutral(const Recolour& a_recolour);

	// RGB bytes in place: scaled by brightness, then pushed from or towards their grey by saturation.
	void ApplyRecolour(const Recolour& a_recolour, std::span<std::uint8_t> a_rgb);

	// One master's RGBA bytes in place: the tone, then the recolour. Alpha stays as it is.
	void ToneMasterRgba(const MM::BakeImage::Tone& a_tone, const Recolour& a_recolour, std::span<std::uint8_t> a_rgba);

	// What the colour of an output folder's tiles depends on; the tiles are encoded again when this text changes.
	std::string ColourStamp(std::string_view a_toneText, const Recolour& a_recolour);
}
