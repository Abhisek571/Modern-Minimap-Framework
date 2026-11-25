#include "PCH.h"

#include "SettingsMenu.h"

#include "PhotoBake.h"
#include "PhotoProcessing.h"
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

		template <class Enum>
		bool EnumCombo(const char* a_label, Enum* a_value, const char* const* a_names, int a_count)
		{
			int index = static_cast<int>(*a_value);
			ImGuiMCP::SetNextItemWidth(kWidth);
			if (!ImGuiMCP::Combo(a_label, &index, a_names, a_count))
				return false;
			*a_value = static_cast<Enum>(index);
			return true;
		}

		bool PageGeneral(Settings::Values& v)
		{
			bool changed = false;
			changed |= ImGuiMCP::Checkbox("Show minimap", &v.enabled);
			changed |= Key("Show/hide key (scan code)", &v.hideKey);
			changed |= SliderI("Size (px at 1080p)", &v.sizePx, 64, 1024);
			changed |= ImGuiMCP::Checkbox("Scale with resolution", &v.scaleWithResolution);
			const char* corners[]{ "Top left", "Top right", "Bottom left", "Bottom right" };
			changed |= EnumCombo("Corner", &v.corner, corners, 4);
			changed |= SliderI("Offset X", &v.offsetX, -2000, 2000);
			changed |= SliderI("Offset Y", &v.offsetY, -2000, 2000);
			bool square = !v.round;
			if (ImGuiMCP::Checkbox("Square shape", &square)) {
				v.round = !square;
				changed = true;
			}
			changed |= Slider("Opacity", &v.opacity, 0.0f, 1.0f);
			changed |= Slider("Brightness", &v.brightness, 0.3f, 3.0f);
			ImGuiMCP::SeparatorText("Turning");
			changed |= ImGuiMCP::Checkbox("Rotate with player", &v.rotateWithPlayer);
			changed |= ImGuiMCP::Checkbox("Use camera direction", &v.useCameraYaw);
			ImGuiMCP::SeparatorText("Hide");
			changed |= ImGuiMCP::Checkbox("In interiors", &v.hideInInteriors);
			changed |= ImGuiMCP::Checkbox("In combat", &v.hideInCombat);
			changed |= ImGuiMCP::Checkbox("In kill moves and death camera", &v.hideInKillCam);
			ImGuiMCP::SeparatorText("Colour output");
			const char* colorModes[]{ "Auto", "sRGB as is", "Always linear" };
			ImGuiMCP::SetNextItemWidth(kWidth);
			changed |= ImGuiMCP::Combo("Colour mode", &v.colorMode, colorModes, 3);
			changed |= Slider("HDR brightness", &v.hdrBrightness, 0.1f, 10.0f);
			return changed;
		}

		bool PageZoom(Settings::Values& v)
		{
			bool                 changed = false;
			static char          radii[128]{};
			static std::uint32_t shownGen = 0;
			if (shownGen != Settings::Generation()) {
				std::snprintf(radii, sizeof(radii), "%s", Settings::FormatRadii(v.zoomRadii).c_str());
				shownGen = Settings::Generation();
			}
			ImGuiMCP::SetNextItemWidth(kWidth);
			if (ImGuiMCP::InputText("Zoom steps (Enter)", radii, sizeof(radii), ImGuiMCP::ImGuiInputTextFlags_EnterReturnsTrue)) {
				if (auto parsed = Settings::ParseRadii(radii); !parsed.empty()) {
					v.zoomRadii = std::move(parsed);
					changed = true;
				}
			}
			ImGuiMCP::TextDisabled("Comma list of world units from the centre to the edge. 4096 units = one cell.");
			changed |= Key("Zoom key (scan code)", &v.zoomKey);
			ImGuiMCP::SeparatorText("Auto zoom");
			changed |= ImGuiMCP::Checkbox("Zoom out while sprinting or riding", &v.autoZoom);
			changed |= Slider("Strength", &v.autoZoomStrength, 0.25f, 3.0f);
			ImGuiMCP::TextDisabled("Outdoors only. At a gallop the map shows up to 1 + strength times the zoom step.");
			ImGuiMCP::SeparatorText("Hold to zoom out");
			changed |= Key("Hold key (scan code, 0 = off)", &v.holdZoomKey);
			changed |= Slider("Factor", &v.holdZoomFactor, kHoldZoomMin, kHoldZoomMax, "%.1fx");
			ImGuiMCP::SeparatorText("Interiors");
			changed |= Slider("Radar zoom", &v.radarZoomScale, 0.1f, 4.0f);
			return changed;
		}

		bool PageMapLook(Settings::Values& v)
		{
			bool        changed = false;
			const char* styles[]{ "Natural", "Muted", "Black and white", "Parchment" };
			changed |= EnumCombo("Map style", &v.style, styles, 4);
			ImGuiMCP::SeparatorText("Shading");
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
			ImGuiMCP::SeparatorText("Water");
			changed |= ImGuiMCP::Checkbox("Water tint", &s.water);
			float water[3]{ s.waterR, s.waterG, s.waterB };
			if (ImGuiMCP::ColorEdit3("Water colour", water)) {
				s.waterR = water[0];
				s.waterG = water[1];
				s.waterB = water[2];
				changed = true;
			}
			changed |= Slider("Water strength", &s.waterAlpha, 0.0f, 1.0f);
			ImGuiMCP::SeparatorText("Interior map");
			changed |= Slider("Floor reaches below you", &v.floorBelow, 0.0f, 1000.0f, "%.0f");
			changed |= Slider("Floor reaches above you", &v.floorAbove, 0.0f, 1000.0f, "%.0f");
			changed |= Slider("Other floors", &v.otherFloorAlpha, 0.0f, 1.0f);
			ImGuiMCP::TextDisabled("Interiors are drawn from the navmesh. Heights are world units.");
			ImGuiMCP::SeparatorText("Fog of war");
			changed |= ImGuiMCP::Checkbox("Fog of war", &v.fogOfWar);
			changed |= Slider("Fog darkness", &v.fogDarkness, 0.0f, 1.0f);
			changed |= ImGuiMCP::Checkbox("Fog over town photos too", &v.fogInTowns);
			ImGuiMCP::TextDisabled("Unexplored land is darker. Land within about one cell of you counts as explored. Saved with your game.");
			return changed;
		}

		bool PageIcons(Settings::Values& v)
		{
			bool  changed = false;
			auto& ic = v.icons;
			changed |= ImGuiMCP::Checkbox("Show icons", &ic.enabled);
			changed |= Slider("Icon size", &ic.scale, 0.25f, 4.0f);
			ImGuiMCP::SeparatorText("Map markers");
			changed |= ImGuiMCP::Checkbox("Map markers", &ic.mapMarkers);
			changed |= ImGuiMCP::Checkbox("Seen but not visited (faded)", &ic.undiscovered);
			changed |= ImGuiMCP::Checkbox("Cleared places faded", &ic.dimCleared);
			changed |= ImGuiMCP::Checkbox("Undiscovered places nearby (?)", &ic.undiscoveredNearby);
			changed |= Slider("Nearby radius (if the game has none)", &ic.undiscoveredRadius, 500.0f, 50000.0f, "%.0f");
			ImGuiMCP::TextDisabled("The radius is the game's compass distance (iMapMarkerVisibleDistance) when it exists.");
			ImGuiMCP::SeparatorText("Quests");
			changed |= ImGuiMCP::Checkbox("Quest targets", &ic.quest);
			changed |= ImGuiMCP::Checkbox("Distance to quest targets", &ic.questDistance);
			ImGuiMCP::SeparatorText("People");
			changed |= ImGuiMCP::Checkbox("Enemies", &ic.hostile);
			changed |= ImGuiMCP::Checkbox("Followers", &ic.follower);
			changed |= ImGuiMCP::Checkbox("Other people and animals", &ic.neutral);
			changed |= ImGuiMCP::Checkbox("Merchants and innkeepers", &ic.services);
			const char* deadModes[]{ "Off", "Only ones you killed", "All" };
			changed |= EnumCombo("Dead bodies", &ic.dead, deadModes, kDeadModeCount);
			changed |= ImGuiMCP::Checkbox("Your horse", &ic.horse);
			ImGuiMCP::SeparatorText("Other");
			const char* doorModes[]{ "Off", "Interiors only", "Everywhere" };
			changed |= EnumCombo("Doors", &ic.doors, doorModes, 3);
			changed |= ImGuiMCP::Checkbox("Custom map marker", &ic.customMarker);
			changed |= ImGuiMCP::Checkbox("Path trail", &ic.trail);
			return changed;
		}

		bool PageText(Settings::Values& v)
		{
			bool        changed = false;
			const char* compassModes[]{ "Off", "Inside", "Outside" };
			changed |= EnumCombo("Compass letters", &v.icons.compassLetters, compassModes, kCompassModeCount);
			ImGuiMCP::TextDisabled("Inside or just outside the rim. Needs icons on.");
			ImGuiMCP::SeparatorText("Info line");
			changed |= ImGuiMCP::Checkbox("Clock", &v.clock);
			changed |= ImGuiMCP::Checkbox("24 hour clock", &v.clock24h);
			changed |= ImGuiMCP::Checkbox("Location name", &v.locationName);
			ImGuiMCP::TextDisabled("Below the minimap in top corners, above it in bottom corners.");
			return changed;
		}

		bool PageColours(Settings::Values& v)
		{
			bool  changed = false;
			auto& c = v.icons.colors;
			ImGuiMCP::SeparatorText("Markers");
			changed |= Color("Cities", &c.city);
			changed |= Color("Towns and farms", &c.settlement);
			changed |= Color("Caves, forts, ruins", &c.dungeon);
			changed |= Color("Other places", &c.other);
			changed |= Color("Undiscovered places nearby", &c.nearby);
			changed |= Color("Quest targets", &c.quest);
			changed |= Color("Doors", &c.door);
			changed |= Color("Custom marker", &c.custom);
			ImGuiMCP::SeparatorText("People");
			changed |= Color("Enemies", &c.hostile);
			changed |= Color("Followers", &c.follower);
			changed |= Color("Other people and animals", &c.neutral);
			changed |= Color("Guards", &c.guard);
			changed |= Color("Merchants and innkeepers", &c.service);
			changed |= Color("Dragons", &c.dragon);
			changed |= Color("Dead bodies", &c.dead);
			changed |= Color("Your horse", &c.horse);
			ImGuiMCP::SeparatorText("Text");
			changed |= Color("Compass letters", &c.compass);
			changed |= Color("Clock and location", &c.infoText);
			changed |= Color("Quest distance", &c.questDistance);
			changed |= Color("Text outline", &c.textOutline);
			ImGuiMCP::TextDisabled("Light text with a dark outline reads everywhere; dark text needs a light outline.");
			ImGuiMCP::SeparatorText("Other");
			changed |= Color("Player arrow", &c.arrow);
			changed |= Color("Path trail", &c.trail);
			return changed;
		}

		bool PagePhotoBake(Settings::Values& v)
		{
			if (!Bake::Supported()) {
				ImGuiMCP::TextWrapped("Not available in Skyrim VR. Use the included photos or photos baked in Skyrim SE or AE.");
				return false;
			}
			bool        changed = false;
			static bool scanned = false;
			if (!std::exchange(scanned, true))
				Bake::RequestScan();
			const auto st = Bake::GetStatus();
			ImGuiMCP::TextWrapped(
				"Photographs every town, city, fort and farm in your load order from straight above and makes the minimap's "
				"close-up tiles from them. Takes 1-3 hours. The camera stays in free mode afterwards: load a save when done. "
				"Best on a new or throwaway save. Close this menu after pressing Start.");
			ImGuiMCP::TextDisabled(
				"Before baking, disable anything that shrinks or removes textures or LOD (VRAMr, texture or terrain LOD downscalers, "
				"city LOD removers). Keep xLODGen terrain LOD, TexGen, DynDOLOD, your grass cache and Synthesis on: the photos show "
				"the game exactly as it looks while baking.");
			bool hide = Settings::BakeHideActorsRequested();
			if (ImGuiMCP::Checkbox("Hide NPCs while baking (restart the game to apply)", &hide))
				Settings::SetBakeHideActors(hide);
			if (hide != Bake::ActorsHidden())
				ImGuiMCP::TextDisabled(hide ? "Restart the game, load your save, then press Start." : "NPCs stay hidden until you restart the game.");
			else if (Bake::ActorsHidden())
				ImGuiMCP::TextDisabled("NPCs are hidden this session.");
			changed |= Slider("Longest settle seconds per photo",&v.bake.settleSeconds, 2.0f, 30.0f, "%.0f");
			if (Slider("Photo area around each place", &v.bake.areaScale, 0.5f, 4.0f, "%.1fx")) {
				changed = true;
				if (!st.running)
					Bake::RequestScan();  // recount the photos for the new area (used from the next Start)
			}
			ImGuiMCP::TextDisabled("1.0x covers the town itself. Higher values also photograph the land around it; 2x is roughly 4 times the photos.");
			int         gridIndex = v.bake.photoGrid - 1;
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
			if (ImGuiMCP::Checkbox("Whole map (every cell with land, takes days)", &v.bake.fullMap)) {
				changed = true;
				if (!st.running)
					Bake::RequestScan();  // recount the photos for the new area (used from the next Start)
			}
			int         tileIndex = v.bake.tileSize >= 2048 ? 2 : v.bake.tileSize >= 1024 ? 1 : 0;
			const char* tileNames[]{ "512 px (default)", "1024 px", "2048 px" };
			ImGuiMCP::SetNextItemWidth(kWidth);
			if (ImGuiMCP::Combo("Tile size", &tileIndex, tileNames, 3)) {
				v.bake.tileSize = BakeImage::kMinTileSize << tileIndex;
				changed = true;
			}
			ImGuiMCP::TextDisabled("1024 px needs the 1x1 grid on a 1080p or taller screen to add real detail. 4x the disk space of 512.");
			changed |= ImGuiMCP::Checkbox("Also save lossless master copies (for BC7 conversion)", &v.bake.saveMasters);
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
			return changed;
		}

		bool PageAdvanced(Settings::Values& v)
		{
			bool changed = false;
			changed |= SliderI("Tile cache MB (restart)", &v.cacheMB, 4, 512);
			int         hook = v.drawHook == Settings::DrawHook::kPresent ? 1 : 0;
			const char* hooks[]{ "HUD pass", "Present (fallback)" };
			ImGuiMCP::SetNextItemWidth(kWidth);
			if (ImGuiMCP::Combo("Draw hook (restart)", &hook, hooks, 2)) {
				v.drawHook = hook == 1 ? Settings::DrawHook::kPresent : Settings::DrawHook::kHudMenu;
				changed = true;
			}
			changed |= ImGuiMCP::Checkbox("Debug log (cost line once a minute)", &v.debugLog);
			return changed;
		}

		// Save / reload / defaults, the same on every page.
		void Footer()
		{
			ImGuiMCP::Separator();
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

		// One menu page: edits a copy of the settings, applies it when something changed, then the footer.
		template <bool (*Page)(Settings::Values&)>
		void __stdcall RenderPage()
		{
			Settings::Values v = *Settings::Get();
			if (Page(v))
				Settings::Set(std::move(v));
			Footer();
		}

		// The "Settings" page: every section but colours and the bake, as collapsing headers.
		bool PageSettings(Settings::Values& v)
		{
			struct Section
			{
				const char* header;
				bool (*draw)(Settings::Values&);
				bool        open;
			};
			static constexpr Section kSections[]{
				{ "Minimap", PageGeneral, true },
				{ "Zoom", PageZoom, false },
				{ "Map look", PageMapLook, false },
				{ "Icons", PageIcons, false },
				{ "Text and compass", PageText, false },
				{ "Advanced", PageAdvanced, false },
			};
			bool changed = false;
			for (const auto& section : kSections) {
				if (ImGuiMCP::CollapsingHeader(section.header, section.open ? ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen : 0))
					changed |= section.draw(v);
			}
			return changed;
		}

		struct PageEntry
		{
			const char*                              name;
			SKSEMenuFramework::Model::RenderFunction render;
		};
		constexpr PageEntry kPages[]{
			{ "Settings", RenderPage<PageSettings> },
			{ "Colours", RenderPage<PageColours> },
			{ "Photo bake", RenderPage<PagePhotoBake> },
		};
	}

	void Register()
	{
		if (!GetMenuFrameworkModule()) {
			logger::info("SKSE Menu Framework not loaded; edit ModernMinimap.ini instead");
			return;
		}
		SKSEMenuFramework::SetSection("Modern Minimap");
		for (const auto& page : kPages)
			SKSEMenuFramework::AddSectionItem(page.name, page.render);
		logger::info("settings menu registered ({} pages)", std::size(kPages));
	}
}
