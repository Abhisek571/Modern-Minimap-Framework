#include "PCH.h"

#include "SettingsMenu.h"

#include "PhotoBake.h"
#include "Icons.h"
#include "Settings.h"

#include <SKSEMenuFramework.h>

namespace MM::UI
{
	namespace
	{
		constexpr float kWidth = 260.0f;

		bool Slider(const char* a_label, float* a_v, float a_min, float a_max, const char* a_fmt = "%.2f")
		{
			ImGuiMCP::SetNextItemWidth(kWidth);
			return ImGuiMCP::SliderFloat(a_label, a_v, a_min, a_max, a_fmt);
		}

		bool SliderI(const char* a_label, int* a_v, int a_min, int a_max)
		{
			ImGuiMCP::SetNextItemWidth(kWidth);
			return ImGuiMCP::SliderInt(a_label, a_v, a_min, a_max);
		}

		bool Key(const char* a_label, std::uint32_t* a_key)
		{
			int k = static_cast<int>(*a_key);
			ImGuiMCP::SetNextItemWidth(kWidth);
			const bool changed = ImGuiMCP::InputInt(a_label, &k, 1, 10);
			if (changed)
				*a_key = static_cast<std::uint32_t>(std::clamp(k, 0, 255));
			return changed;
		}

		bool Color(const char* a_label, std::uint32_t* a_rgba)
		{
			float c[4]{ static_cast<float>(*a_rgba & 255) / 255.0f, static_cast<float>((*a_rgba >> 8) & 255) / 255.0f,
				static_cast<float>((*a_rgba >> 16) & 255) / 255.0f, static_cast<float>((*a_rgba >> 24) & 255) / 255.0f };
			if (!ImGuiMCP::ColorEdit4(a_label, c))
				return false;
			auto b = [](float a_f) { return static_cast<int>(std::lround(std::clamp(a_f, 0.0f, 1.0f) * 255.0f)); };
			*a_rgba = PackRgba(b(c[0]), b(c[1]), b(c[2]), b(c[3]));
			return true;
		}

		void __stdcall Render()
		{
			Settings::Values v = *Settings::Get();
			bool             changed = false;

			if (ImGuiMCP::CollapsingHeader("Minimap", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
				changed |= ImGuiMCP::Checkbox("Enabled", &v.enabled);
				changed |= SliderI("Size (px at 1080p)", &v.sizePx, 64, 1024);
				changed |= ImGuiMCP::Checkbox("Scale with resolution", &v.scaleWithResolution);
				int corner = static_cast<int>(v.corner);
				const char* corners[]{ "Top left", "Top right", "Bottom left", "Bottom right" };
				ImGuiMCP::SetNextItemWidth(kWidth);
				if (ImGuiMCP::Combo("Corner", &corner, corners, 4)) {
					v.corner = static_cast<Settings::Corner>(corner);
					changed = true;
				}
				changed |= SliderI("Offset X", &v.offsetX, -2000, 2000);
				changed |= SliderI("Offset Y", &v.offsetY, -2000, 2000);
				bool square = !v.round;
				if (ImGuiMCP::Checkbox("Square shape", &square)) {
					v.round = !square;
					changed = true;
				}
				changed |= Slider("Opacity", &v.opacity, 0.0f, 1.0f);
				changed |= Slider("Overall brightness", &v.brightness, 0.3f, 3.0f);
				int         style = static_cast<int>(v.style);
				const char* styles[]{ "Natural", "Muted", "Black and white", "Parchment" };
				ImGuiMCP::SetNextItemWidth(kWidth);
				if (ImGuiMCP::Combo("Map style", &style, styles, 4)) {
					v.style = static_cast<MapStyle>(style);
					changed = true;
				}
				changed |= ImGuiMCP::Checkbox("Rotate with player", &v.rotateWithPlayer);
				changed |= ImGuiMCP::Checkbox("Use camera direction", &v.useCameraYaw);
				changed |= ImGuiMCP::Checkbox("Hide in interiors", &v.hideInInteriors);
				changed |= ImGuiMCP::Checkbox("Hide in kill moves and death camera", &v.hideInKillCam);
				changed |= Key("Hide key (scan code)", &v.hideKey);
			}

			if (ImGuiMCP::CollapsingHeader("Zoom")) {
				static char          radii[128]{};
				static std::uint32_t shownGen = 0;
				if (shownGen != Settings::Generation()) {
					std::snprintf(radii, sizeof(radii), "%s", Settings::FormatRadii(v.zoomRadii).c_str());
					shownGen = Settings::Generation();
				}
				ImGuiMCP::SetNextItemWidth(kWidth);
				if (ImGuiMCP::InputText("Zoom steps (units, comma list, Enter)", radii, sizeof(radii), ImGuiMCP::ImGuiInputTextFlags_EnterReturnsTrue)) {
					if (auto parsed = Settings::ParseRadii(radii); !parsed.empty()) {
						v.zoomRadii = std::move(parsed);
						changed = true;
					}
				}
				ImGuiMCP::TextDisabled("4096 units = one cell. Each step is the distance from the centre to the edge.");
				changed |= Key("Zoom key (scan code)", &v.zoomKey);
				changed |= ImGuiMCP::Checkbox("Auto zoom (zoom out while sprinting or riding)", &v.autoZoom);
				changed |= Slider("Auto zoom strength", &v.autoZoomStrength, 0.25f, 3.0f);
				ImGuiMCP::TextDisabled("Outdoors only. At a horse gallop the map shows up to 1 + strength times the zoom step.");
				changed |= Slider("Radar zoom (interiors)", &v.radarZoomScale, 0.1f, 4.0f);
			}

			if (ImGuiMCP::CollapsingHeader("Interior map")) {
				changed |= Slider("Floor reaches below you", &v.floorBelow, 0.0f, 1000.0f, "%.0f");
				changed |= Slider("Floor reaches above you", &v.floorAbove, 0.0f, 1000.0f, "%.0f");
				changed |= Slider("Other floors", &v.otherFloorAlpha, 0.0f, 1.0f);
				ImGuiMCP::TextDisabled("Interiors are drawn from the navmesh. Heights are world units.");
			}

			if (ImGuiMCP::CollapsingHeader("Icons")) {
				auto& ic = v.icons;
				changed |= ImGuiMCP::Checkbox("Show icons", &ic.enabled);
				changed |= Slider("Icon size", &ic.scale, 0.25f, 4.0f);
				changed |= ImGuiMCP::Checkbox("Map markers", &ic.mapMarkers);
				changed |= ImGuiMCP::Checkbox("Undiscovered map markers (faded)", &ic.undiscovered);
				changed |= ImGuiMCP::Checkbox("Quest targets", &ic.quest);
				changed |= ImGuiMCP::Checkbox("Enemies", &ic.hostile);
				changed |= ImGuiMCP::Checkbox("Followers", &ic.follower);
				changed |= ImGuiMCP::Checkbox("Other people", &ic.neutral);
				changed |= ImGuiMCP::Checkbox("Dead bodies", &ic.dead);
				changed |= ImGuiMCP::Checkbox("Custom map marker", &ic.customMarker);
				int         doors = static_cast<int>(ic.doors);
				const char* doorModes[]{ "Off", "Interiors only", "Everywhere" };
				ImGuiMCP::SetNextItemWidth(kWidth);
				if (ImGuiMCP::Combo("Doors", &doors, doorModes, 3)) {
					ic.doors = static_cast<Settings::DoorMode>(doors);
					changed = true;
				}
			}

			if (ImGuiMCP::CollapsingHeader("Colours")) {
				auto& c = v.icons.colors;
				changed |= Color("Cities", &c.city);
				changed |= Color("Towns and farms", &c.settlement);
				changed |= Color("Caves, forts, ruins", &c.dungeon);
				changed |= Color("Other places", &c.other);
				changed |= Color("Quest targets", &c.quest);
				changed |= Color("Enemies", &c.hostile);
				changed |= Color("Followers", &c.follower);
				changed |= Color("Other people", &c.neutral);
				changed |= Color("Dead bodies", &c.dead);
				changed |= Color("Doors", &c.door);
				changed |= Color("Custom marker", &c.custom);
				changed |= Color("Player arrow", &c.arrow);
			}

			if (ImGuiMCP::CollapsingHeader("Map look")) {
				auto& s = v.shade;
				changed |= Slider("Light east", &s.lightX, -10.0f, 10.0f);
				changed |= Slider("Light north", &s.lightY, -10.0f, 10.0f);
				changed |= Slider("Light height", &s.lightZ, 0.05f, 10.0f);
				changed |= Slider("Ambient", &s.ambient, 0.0f, 4.0f);
				changed |= Slider("Diffuse", &s.diffuse, 0.0f, 4.0f);
				changed |= Slider("Shade min", &s.shadeMin, 0.0f, 4.0f);
				changed |= Slider("Shade max", &s.shadeMax, 0.0f, 8.0f);
				changed |= Slider("Brightness", &s.brightness, 0.0f, 4.0f);
				changed |= Slider("Saturation", &s.saturation, 0.0f, 4.0f);
				changed |= ImGuiMCP::Checkbox("Water tint", &s.water);
				float water[3]{ s.waterR, s.waterG, s.waterB };
				if (ImGuiMCP::ColorEdit3("Water colour", water)) {
					s.waterR = water[0];
					s.waterG = water[1];
					s.waterB = water[2];
					changed = true;
				}
				changed |= Slider("Water strength", &s.waterAlpha, 0.0f, 1.0f);
			}

			if (ImGuiMCP::CollapsingHeader("Advanced")) {
				changed |= SliderI("Tile cache MB (restart)", &v.cacheMB, 4, 512);
				int hook = v.drawHook == Settings::DrawHook::kPresent ? 1 : 0;
				const char* hooks[]{ "HUD pass", "Present (fallback)" };
				ImGuiMCP::SetNextItemWidth(kWidth);
				if (ImGuiMCP::Combo("Draw hook (restart)", &hook, hooks, 2)) {
					v.drawHook = hook == 1 ? Settings::DrawHook::kPresent : Settings::DrawHook::kHudMenu;
					changed = true;
				}
				changed |= ImGuiMCP::Checkbox("Debug log (cost line once a minute)", &v.debugLog);
			}

			if (!Bake::Supported()) {
				if (ImGuiMCP::CollapsingHeader("Bake town photos"))
					ImGuiMCP::TextWrapped("Not available in Skyrim VR. Use the included photos or photos baked in Skyrim SE or AE.");
			} else if (ImGuiMCP::CollapsingHeader("Bake town photos")) {
				static bool scanned = false;
				if (!std::exchange(scanned, true))
					Bake::RequestScan();
				const auto st = Bake::GetStatus();
				ImGuiMCP::TextWrapped(
					"Photographs every town, city, fort and farm in your load order from straight above and makes the minimap's "
					"close-up tiles from them. Takes 1-3 hours. The camera stays in free mode afterwards: load a save when done. "
					"Best on a new or throwaway save. Close this menu after pressing Start.");
				bool hide = Settings::BakeHideActorsRequested();
				if (ImGuiMCP::Checkbox("Hide NPCs while baking (restart the game to apply)", &hide))
					Settings::SetBakeHideActors(hide);
				if (hide != Bake::ActorsHidden())
					ImGuiMCP::TextDisabled(hide ? "Restart the game, load your save, then press Start." : "NPCs stay hidden until you restart the game.");
				else if (Bake::ActorsHidden())
					ImGuiMCP::TextDisabled("NPCs are hidden this session.");
				changed |= Slider("Settle seconds per photo", &v.bake.settleSeconds, 2.0f, 30.0f, "%.0f");
				if (Slider("Photo area around each place", &v.bake.areaScale, 0.5f, 4.0f, "%.1fx")) {
					changed = true;
					if (!st.running)
						Bake::RequestScan();  // recount the photos for the new area (used from the next Start)
				}
				ImGuiMCP::TextDisabled("1.0x covers the town itself. Higher values also photograph the land around it; 2x is roughly 4 times the photos.");
				int gridIndex = v.bake.photoGrid - 1;
				const char* gridNames[]{ "1x1 (sharpest)", "2x2 (default)", "3x3 (faster)", "4x4 (fastest)" };
				ImGuiMCP::SetNextItemWidth(kWidth);
				if (ImGuiMCP::Combo("Photo grid", &gridIndex, gridNames, 4)) {
					v.bake.photoGrid = gridIndex + 1;
					changed = true;
					if (!st.running)
						Bake::RequestScan();  // recount the photos for the new grid (used from the next Start)
				}
				ImGuiMCP::TextDisabled("Cells per photo. 1x1 is sharpest but takes about 4x the photos; 3x3 and 4x4 are faster with softer town detail.");
				changed |= ImGuiMCP::Checkbox("Keep existing photo tiles (only fill gaps)", &v.bake.keepExisting);
				if (st.found >= 0)
					ImGuiMCP::Text("Photos found: %d, done: %d", st.found, st.done);
				else
					ImGuiMCP::Text("Looking for map markers...");
				if (st.running) {
					const int eta = static_cast<int>(st.etaSeconds / 60.0);
					if (st.etaSeconds > 0)
						ImGuiMCP::Text("Progress: %d / %d, about %d h %02d min left", st.done, st.found, eta / 60, eta % 60);
					else
						ImGuiMCP::Text("Progress: %d / %d, time left shown after the first photo", st.done, st.found);
					ImGuiMCP::Text("Now: %s", st.phase.c_str());
				}
				if (!st.tone.empty())
					ImGuiMCP::Text("Colour: %s", st.tone.c_str());
				if (st.writing && !st.running)
					ImGuiMCP::TextDisabled("Writing the last tiles...");
				if (!st.running) {
					if (ImGuiMCP::Button("Start"))
						Bake::RequestStart();
					ImGuiMCP::SameLine();
					if (ImGuiMCP::Button("Reset progress"))
						Bake::RequestReset();
				} else if (ImGuiMCP::Button("Cancel")) {
					Bake::RequestCancel();
				}
				if (!st.message.empty())
					ImGuiMCP::TextWrapped("%s", st.message.c_str());
			}

			ImGuiMCP::Separator();
			if (changed)
				Settings::Set(std::move(v));
			if (ImGuiMCP::Button("Save"))
				Settings::Save();
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Reload from file"))
				Settings::Load();
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Defaults"))
				Settings::Set(Settings::Values{});
			ImGuiMCP::TextDisabled("Changes apply at once. Save writes them to ModernMinimap.ini.");
		}
	}

	void Register()
	{
		if (!GetMenuFrameworkModule()) {
			logger::info("SKSE Menu Framework not loaded; edit ModernMinimap.ini instead");
			return;
		}
		SKSEMenuFramework::SetSection("Modern Minimap");
		SKSEMenuFramework::AddSectionItem("Settings", Render);
		logger::info("settings menu registered");
	}
}
