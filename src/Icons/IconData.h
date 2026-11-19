#pragma once

#include "Icons.h"
#include "Settings.h"

// Game-side icon data: map markers, quest targets, actors, doors and the player's custom marker.
// Gathered on fixed timers from the draw call (main thread) only while the minimap is visible;
// every frame only projects the cached lists.

namespace MM::IconData
{
	// Worldspace whose map (LOD tiles, map markers) this one shows: city worlds use their parent's.
	RE::TESWorldSpace* MapRootWorld(RE::TESWorldSpace* a_world);

	// Event sinks (cell attach/detach for doors, loading menu for flushing). Call at kDataLoaded.
	void RegisterEvents();

	// Drop every cached list on the next update (load game, new game, fast travel, coc).
	void RequestFlush();

	// Main thread, once per visible frame: runs the timers that are due. a_radius = world units centre-to-edge.
	void Update(double a_now, const Settings::Values& a_cfg, RE::PlayerCharacter* a_player, float a_radius);

	// Appends this frame's icon sources in draw order (markers, doors, custom marker, actors, quest targets).
	void Collect(double a_now, std::vector<IconSource>& a_out);

	// Counts for the perf/debug log.
	struct Counts
	{
		std::size_t markers, doors, actors, quests;
	};
	Counts GetCounts();
}
