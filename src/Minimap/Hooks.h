#pragma once

namespace MM::Hooks
{
	void Install();        // draw hook (HUDMenu::PostDisplay, or Present if the INI asks)
	void RegisterInput();  // zoom / hide keys

	// Town photo bake: IDXGISwapChain::Present hook for back buffer captures and the per-frame bake step.
	// Installed only when a bake is started; later calls do nothing.
	void InstallBakePresent();
}
