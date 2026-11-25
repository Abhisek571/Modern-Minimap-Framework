#pragma once

namespace MM::Minimap
{
	// True when nothing but the HUD is up: HUD shown, game not paused, no cursor/modal/loading menu.
	bool HudIsClear();

	// Render thread, once per frame. a_target: render target to draw into, or nullptr for the one bound now.
	void Draw(ID3D11DeviceContext* a_ctx, ID3D11Device* a_device, ID3D11RenderTargetView* a_target);

	// Call once from the main thread (kDataLoaded). Game state is read there each frame; Draw only draws.
	void SetMainThread();

	void CycleZoom();
	void ToggleHidden();
	void SetHoldZoom(bool a_held);  // hold-to-zoom-out key down / up

	// Town photo bake: no minimap in the picture while it is taken.
	void SetSuppressed(bool a_suppressed);

	// A baked photo tile was written: drop its cached copy so the next draw loads the new file. Any thread.
	void ForgetBakedTile(int a_cellX, int a_cellY);

	// Whole-file read through the game's resource system (loose files and BSAs). Safe off the main thread.
	bool ReadGameFile(const std::string& a_path, std::vector<std::uint8_t>& a_out);
}
