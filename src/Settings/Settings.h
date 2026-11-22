#pragma once

#include "MapMath.h"

namespace MM::Settings
{
	enum class Corner
	{
		kTopLeft,
		kTopRight,
		kBottomLeft,
		kBottomRight
	};

	enum class DrawHook
	{
		kHudMenu,  // HUDMenu::PostDisplay (default, draws inside the UI pass)
		kPresent   // IDXGISwapChain::Present (fallback)
	};

	enum class DoorMode
	{
		kOff,
		kInteriors,
		kAlways
	};

	// Icon colours, RGBA packed (see MM::PackRgba).
	struct IconColors
	{
		std::uint32_t city{ 0xFF8CE6F5 };        // 245,230,140
		std::uint32_t settlement{ 0xFFC8E6F0 };  // 240,230,200
		std::uint32_t dungeon{ 0xFFA0B4C8 };     // 200,180,160
		std::uint32_t other{ 0xFFC8C8C8 };       // 200,200,200
		std::uint32_t quest{ 0xFF30C8FF };       // 255,200,48
		std::uint32_t hostile{ 0xFF2832E6 };     // 230,50,40
		std::uint32_t follower{ 0xFF50DC50 };    // 80,220,80
		std::uint32_t neutral{ 0xFFB4B4B4 };     // 180,180,180
		std::uint32_t dead{ 0xFF8C8CA0 };        // 160,140,140
		std::uint32_t door{ 0xFF80C8E8 };        // 232,200,128
		std::uint32_t custom{ 0xFFFFC864 };      // 100,200,255
		std::uint32_t arrow{ 0xFFE6F7FF };       // 255,247,230
	};

	struct IconValues
	{
		bool       enabled{ true };
		float      scale{ 1.0f };
		bool       mapMarkers{ true };
		bool       undiscovered{ true };  // markers seen but not visited, drawn faded (like the world map)
		bool       quest{ true };
		bool       hostile{ true };
		bool       follower{ true };
		bool       neutral{ false };
		bool       dead{ false };
		DoorMode   doors{ DoorMode::kInteriors };
		bool       customMarker{ true };
		IconColors colors;
	};

	// Town photo bake (settings page button). The hide-NPCs flag is not here: it is read once at start-up.
	struct BakeValues
	{
		float settleSeconds{ 7.0f };  // wait after each move before the picture (cells + LOD fade-in)
		float areaScale{ 1.0f };      // multiplies the photo radius around every map marker (bigger = more of the map, longer bake)
		bool  keepExisting{ false };  // skip cells that already have a photo tile (e.g. from another mod)
		int   photoGrid{ 2 };         // cells per photo side, 1..4: 1 = sharpest and slowest, 4 = fastest and softest
	};

	struct Values
	{
		bool               enabled{ true };
		int                sizePx{ 300 };  // at 1080p
		bool               scaleWithResolution{ true };
		Corner             corner{ Corner::kBottomRight };
		int                offsetX{ 40 };
		int                offsetY{ 60 };
		bool               round{ true };
		float              opacity{ 0.9f };
		float              brightness{ 1.0f };  // whole minimap (map + icons), every colour mode
		MapStyle           style{ MapStyle::kNatural };  // look of the map (not the icons)
		bool               rotateWithPlayer{ true };
		bool               useCameraYaw{ false };
		bool               hideInInteriors{ false };
		bool               hideInKillCam{ true };
		int                colorMode{ 0 };      // 0 = auto (by HUD target format), 1 = sRGB values as is, 2 = always linear
		float              hdrBrightness{ 1.0f };  // brightness scale when the HUD target is float (HDR)
		float              radarZoomScale{ 0.5f };  // interiors / no-tile worldspaces: zoom radius multiplier
		float              floorBelow{ 120.0f };    // interior map: player's floor reaches this far below the player
		float              floorAbove{ 220.0f };    // ... and this far above
		float              otherFloorAlpha{ 0.0f }; // interior map: other floors drawn this faint (0 = hidden)
		bool               debugLog{ false };       // debug level: once-a-minute cost line
		std::vector<float> zoomRadii{ 3000.0f, 6000.0f, 12000.0f };
		bool               autoZoom{ true };           // outdoors: zoom out while sprinting or riding
		float              autoZoomStrength{ 1.0f };  // how far: up to 1 + this times the zoom step at a gallop
		std::uint32_t      zoomKey{ 0x4E };  // numpad +
		std::uint32_t      hideKey{ 0x4A };  // numpad -
		int                cacheMB{ 48 };
		DrawHook           drawHook{ DrawHook::kHudMenu };
		ShadeParams        shade;
		IconValues         icons;
		BakeValues         bake;
	};

	void Load();
	bool Save();  // writes the current values to the INI; false on error

	// Snapshot of the current values. Hold it for the whole frame / event, not just one field.
	std::shared_ptr<const Values> Get();

	// Replaces the values (clamped to the same ranges as the INI). Bumps Generation().
	void          Set(Values a_values);
	std::uint32_t Generation();

	// [Bake] bHideActorsOnNextStart: read straight from the INI (start-up) and written as that one key only,
	// so the menu's Save never touches it.
	bool BakeHideActorsRequested();
	void SetBakeHideActors(bool a_on);
	bool BakeStartOnLoadRequested();  // [Bake] bStartOnLoad
	// Set by the Photo Baker app together with bStartOnLoad, so it can run a bake unattended.
	bool BakeEnterFromMainMenuRequested();  // [Bake] bEnterFromMainMenu: coc from the main menu instead of waiting for a save
	bool BakeQuitWhenDoneRequested();       // [Bake] bQuitWhenDone: quit the game once the tiles are written
	void ClearBakeStartOnLoad();       // when a bake finishes or is cancelled: clears the three keys above

	// "3000,6000,12000" <-> radii (500..200000 units each). Empty result = keep the old list.
	std::vector<float> ParseRadii(const std::string& a_list);
	std::string        FormatRadii(const std::vector<float>& a_radii);
}
