#pragma once

#include "FogOfWar.h"
#include "Settings.h"

// Fog of war in the game: explored blocks per worldspace, updated on a timer from the main thread and saved
// in the SKSE co-save. The render thread only gets a finished mask (rebuilt when a bit flips or the window moves).

namespace MM::FogState
{
	struct Mask
	{
		std::vector<std::uint8_t> pixels;  // Fog::kMaskSize^2, R8, row 0 = north
		Rect                      world;   // world rect the mask covers
		std::uint32_t             generation{ 0 };
	};

	// Co-save callbacks (unique ID 'MMFW', record 'FOGW'). Call once from SKSEPlugin_Load.
	void RegisterSerialization();

	// Main thread, every visible exterior frame. a_mapWorld = the worldspace whose map is shown (city worlds use
	// their parent's). Returns the current mask, or null when fog is off or there is no world.
	std::shared_ptr<const Mask> Update(double a_now, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, RE::TESWorldSpace* a_mapWorld);
}
