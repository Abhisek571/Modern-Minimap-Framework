#pragma once

// In-game town photo bake: flies a straight-down free camera over every settlement found from the load order's
// map markers (or every Tamriel cell with land, [Bake] bFullMap), captures the back buffer and writes finished BC1
// tiles (512 px by default, [Bake] iTileSize) to Data\textures\ModernMinimap\tamriel.
// Needs no outside tools.

namespace MM::Bake
{
	// SKSEPlugin_Load: keeps NPCs from loading their 3D, but only when the settings page asked for it before this
	// start ([Bake] bHideActorsOnNextStart). Never installed otherwise.
	void InstallActorHooksIfRequested();
	[[nodiscard]] bool ActorsHidden();

	// kDataLoaded: with [Bake] bStartOnLoad set in the ini, starts (or resumes) the bake by itself once a game is
	// loaded and the player has stood in the world for a while. For players without SKSE Menu Framework.
	// False in Skyrim VR: the bake drives the flat free camera and copies the flat screen image.
	inline bool Supported() { return !REL::Module::IsVR(); }

	void StartOnLoadIfRequested();

	// Any thread (settings page): the work runs on the main thread.
	void RequestScan();    // finds the shots and counts what is already done
	void RequestStart();   // starts or resumes
	void RequestCancel();  // stops after the current step; progress is kept
	void RequestReset();   // forgets progress and the colour fit (not while running)

	// Render thread, from the bake's Present hook: back buffer copies, reference renders, per-frame step.
	void OnPresent(IDXGISwapChain* a_swapChain);

	struct Status
	{
		bool        running{ false };
		bool        writing{ false };  // tiles still being encoded / written
		int         found{ -1 };       // shots in the list (-1 = not scanned yet)
		int         done{ 0 };         // shots finished, earlier runs included
		int         failedCells{ 0 };  // cells of failed shots: queued for the retry pass, after it the ones still missing
		double      etaSeconds{ 0 };
		std::string phase;
		std::string tone;
		std::string message;
		std::string state;  // for the status file: "starting", "done", "cancelled", "error", "restart"; empty = running / writing
	};
	Status GetStatus();
}
