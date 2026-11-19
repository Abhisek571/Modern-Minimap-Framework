#include "PCH.h"

#include "Hooks.h"

#include "PhotoBake.h"
#include "Minimap.h"
#include "Settings.h"

namespace MM::Hooks
{
	namespace
	{
		ID3D11DeviceContext* Context()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11DeviceContext*>(r->GetRuntimeData().context) : nullptr;
		}

		ID3D11Device* Device()
		{
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11Device*>(r->GetRuntimeData().forwarder) : nullptr;
		}

		// No C++ exception may unwind into the game's frames.
		void SafeDraw(ID3D11RenderTargetView* a_target)
		{
			static bool logged = false;
			try {
				Minimap::Draw(Context(), Device(), a_target);
			} catch (const std::exception& e) {
				if (!std::exchange(logged, true))
					logger::error("draw failed: {}", e.what());
			} catch (...) {
				if (!std::exchange(logged, true))
					logger::error("draw failed: unknown exception");
			}
		}

		// HUDMenu::PostDisplay (IMenu vfunc 0x06): runs right after the HUD movie is drawn, in the UI pass.
		struct HudPostDisplay
		{
			static void thunk(RE::HUDMenu* a_this)
			{
				func(a_this);
				SafeDraw(nullptr);
			}
			static inline REL::Relocation<decltype(thunk)> func;

			static void Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
				func = vtbl.write_vfunc(0x6, thunk);
				logger::info("hooked HUDMenu::PostDisplay");
			}
		};

		// Fallback: IDXGISwapChain::Present (vfunc 8), drawing straight into the back buffer.
		struct SwapChainPresent
		{
			static HRESULT thunk(IDXGISwapChain* a_this, UINT a_syncInterval, UINT a_flags)
			{
				if (!(a_flags & DXGI_PRESENT_TEST)) {
					auto* r = RE::BSGraphics::Renderer::GetSingleton();
					auto* rtv = r ? reinterpret_cast<ID3D11RenderTargetView*>(r->GetRuntimeData().renderWindows[0].renderView) : nullptr;
					if (rtv)
						SafeDraw(rtv);
				}
				return func(a_this, a_syncInterval, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;

			static bool Install()
			{
				auto* r = RE::BSGraphics::Renderer::GetSingleton();
				auto* swap = r ? reinterpret_cast<IDXGISwapChain*>(r->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
				if (!swap) {
					logger::error("no swap chain; Present hook not installed");
					return false;
				}
				REL::Relocation<std::uintptr_t> vtbl{ *reinterpret_cast<std::uintptr_t*>(swap) };
				func = vtbl.write_vfunc(8, thunk);
				logger::info("hooked IDXGISwapChain::Present");
				return true;
			}
		};

		// Separate from the fallback draw hook: it only exists while the player uses the photo bake.
		struct BakePresent
		{
			static HRESULT thunk(IDXGISwapChain* a_this, UINT a_syncInterval, UINT a_flags)
			{
				if (!(a_flags & DXGI_PRESENT_TEST)) {
					try {
						Bake::OnPresent(a_this);
					} catch (...) {
						static bool logged = false;
						if (!std::exchange(logged, true))
							logger::error("bake present step failed");
					}
				}
				return func(a_this, a_syncInterval, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* GetSingleton()
			{
				static InputSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_event)
					return RE::BSEventNotifyControl::kContinue;
				const auto  cfgPtr = Settings::Get();
				const auto& cfg = *cfgPtr;
				for (auto* e = *a_event; e; e = e->next) {
					if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || e->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
						continue;
					const auto* button = e->AsButtonEvent();
					if (!button || !button->IsDown())
						continue;
					const auto key = button->GetIDCode();
					if ((key == cfg.zoomKey || key == cfg.hideKey) && Minimap::HudIsClear()) {
						if (key == cfg.zoomKey)
							Minimap::CycleZoom();
						else
							Minimap::ToggleHidden();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		if (Settings::Get()->drawHook == Settings::DrawHook::kPresent)
			SwapChainPresent::Install();
		else
			HudPostDisplay::Install();
	}

	void InstallBakePresent()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			auto* r = RE::BSGraphics::Renderer::GetSingleton();
			auto* swap = r ? reinterpret_cast<IDXGISwapChain*>(r->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
			if (!swap) {
				logger::error("bake: no swap chain; Present hook not installed");
				return;
			}
			REL::Relocation<std::uintptr_t> vtbl{ *reinterpret_cast<std::uintptr_t*>(swap) };
			BakePresent::func = vtbl.write_vfunc(8, BakePresent::thunk);
			logger::info("bake: hooked IDXGISwapChain::Present");
		});
	}

	void RegisterInput()
	{
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(InputSink::GetSingleton());
			logger::info("input sink registered");
		}
	}
}
