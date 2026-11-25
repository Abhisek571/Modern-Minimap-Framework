#pragma once

#include "Icons.h"
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
		std::uint32_t neutral{ 0xFFF08C46 };     // 70,140,240
		std::uint32_t dead{ 0xFF8C8CA0 };        // 160,140,140
		std::uint32_t door{ 0xFF80C8E8 };        // 232,200,128
		std::uint32_t custom{ 0xFFFFC864 };      // 100,200,255
		std::uint32_t arrow{ 0xFFE6F7FF };       // 255,247,230
		std::uint32_t horse{ 0xFF6EA0C8 };       // 200,160,110
		std::uint32_t trail{ 0x8CDCF0FF };       // 255,240,220,140
		std::uint32_t nearby{ 0xA0C8C8C8 };      // 200,200,200,160: undiscovered places near you
		std::uint32_t compass{ 0xFF141414 };        // 20,20,20: N/E/S/W
		std::uint32_t infoText{ 0xFF141414 };       // 20,20,20: clock and location line
		std::uint32_t questDistance{ 0xFF141414 };  // 20,20,20: distance label at quest stars
		std::uint32_t textOutline{ 0xC8FFFFFF };    // 255,255,255,200: outline behind all text
		std::uint32_t service{ 0xFF8CD2F0 };     // 240,210,140
		std::uint32_t guard{ 0xFFB4A0A0 };       // 160,160,180
		std::uint32_t dragon{ 0xFF1E8CFF };      // 255,140,30
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
		bool       neutral{ true };
		DeadMode   dead{ DeadMode::kPlayerKills };
		DoorMode   doors{ DoorMode::kInteriors };
		bool       customMarker{ true };
		bool       questDistance{ true };       // distance label next to each quest target
		CompassMode compassLetters{ CompassMode::kOutside };  // N/E/S/W around the rim
		bool       horse{ true };               // your last ridden horse
		bool       trail{ false };              // faint dots where you walked
		bool       undiscoveredNearby{ true };  // '?' for unknown places close by (no name or type)
		float      undiscoveredRadius{ 4000.0f };  // used only when the game setting is missing
		bool       services{ true };            // merchants and innkeepers get a service icon
		bool       dimCleared{ true };          // cleared places drawn faded
		IconColors colors;
	};

	// Town photo bake (settings page button). The hide-NPCs flag is not here: it is read once at start-up.
	struct BakeValues
	{
		float settleSeconds{ 15.0f };  // longest wait after each move before the picture (cells + LOD fade-in); the whole wait without bAdaptiveSettle
		bool  adaptiveSettle{ true };   // take the picture as soon as the screen stops changing (between the min and the longest wait)
		float settleMinSeconds{ 1.5f }; // adaptive settle: never sooner than this
		float settleThreshold{ 0.75f }; // adaptive settle: mean luma change (0..255) between two looks 0.5 s apart that counts as still
		bool  spreadToneFit{ true };    // fit the colour match from photos spread over the whole shot list, taken first
		bool  lockExposure{ false };    // eye adaptation off during the bake (restored when it ends); off: snow photos blow out to white
		float photoExposure{ 0.7f };    // factor on the picture brightness during the bake (restored when it ends); below 1 keeps snow from clipping
		int   restartEvery{ 500 };      // Photo Baker app runs: quit and restart the game after this many photos (0 = off)
		bool  retryFailed{ true };      // take failed photos once more at the end, then list the missing cells
		float areaScale{ 1.0f };      // multiplies the photo radius around every map marker (bigger = more of the map, longer bake)
		bool  keepExisting{ false };  // skip cells that already have a photo tile (e.g. from another mod)
		int   photoGrid{ 2 };         // cells per photo side, 1..4: 1 = sharpest and slowest, 4 = fastest and softest
		bool  fullMap{ false };       // photograph every Tamriel cell with land instead of the areas around map markers
		int   tileSize{ 512 };        // output tile side, px: 512, 1024 or 2048
		bool  saveMasters{ true };    // also write each finished tile untoned as a lossless .bmp outside textures, plus tone.txt (for BC7 conversion)
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
		bool               hideInCombat{ false };
		bool               clock{ false };          // game time line next to the minimap
		bool               locationName{ false };   // current location name on that line
		bool               clock24h{ true };
		bool               fogOfWar{ false };       // unexplored land drawn darker (saved with the game)
		float              fogDarkness{ 0.5f };
		bool               fogInTowns{ false };     // fog over the baked town photos too
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
		std::uint32_t      holdZoomKey{ 0 };  // 0 = off; held: zoom out by holdZoomFactor
		float              holdZoomFactor{ 3.0f };
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
