#include "PCH.h"

#include "Settings.h"
#include "Icons.h"

#include <SimpleIni.h>

namespace MM::Settings
{
	namespace
	{
		constexpr auto kPath = "Data/SKSE/Plugins/ModernMinimap.ini";

		std::atomic<std::shared_ptr<const Values>> current{ std::make_shared<const Values>() };
		std::atomic<std::uint32_t>                 generation{ 1 };

		std::string Upper(std::string a_s)
		{
			for (auto& c : a_s)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return a_s;
		}

		float Fin(float a_v, float a_default) { return std::isfinite(a_v) ? a_v : a_default; }

		void ClampF(float& a_v, float a_min, float a_max, float a_default) { a_v = std::clamp(Fin(a_v, a_default), a_min, a_max); }

		// Every range check in one place: used for the INI and for values set from the menu.
		void Clamp(Values& a_v)
		{
			const Values d;
			a_v.sizePx = std::clamp(a_v.sizePx, 64, 1024);
			a_v.offsetX = std::clamp(a_v.offsetX, -4000, 4000);
			a_v.offsetY = std::clamp(a_v.offsetY, -4000, 4000);
			ClampF(a_v.opacity, 0.0f, 1.0f, d.opacity);
			ClampF(a_v.brightness, 0.3f, 3.0f, d.brightness);
			ClampF(a_v.floorBelow, 0.0f, 4000.0f, d.floorBelow);
			ClampF(a_v.floorAbove, 0.0f, 4000.0f, d.floorAbove);
			ClampF(a_v.otherFloorAlpha, 0.0f, 1.0f, d.otherFloorAlpha);
			ClampF(a_v.radarZoomScale, 0.1f, 4.0f, d.radarZoomScale);
			a_v.cacheMB = std::clamp(a_v.cacheMB, 4, 512);
			a_v.colorMode = std::clamp(a_v.colorMode, 0, 2);
			ClampF(a_v.hdrBrightness, 0.1f, 10.0f, d.hdrBrightness);
			a_v.zoomKey = std::min<std::uint32_t>(a_v.zoomKey, 255);
			a_v.hideKey = std::min<std::uint32_t>(a_v.hideKey, 255);
			std::erase_if(a_v.zoomRadii, [](float r) { return !std::isfinite(r) || r < 500.0f || r > 200000.0f; });
			if (a_v.zoomRadii.empty())
				a_v.zoomRadii = d.zoomRadii;

			auto& s = a_v.shade;
			ClampF(s.lightX, -10.0f, 10.0f, d.shade.lightX);
			ClampF(s.lightY, -10.0f, 10.0f, d.shade.lightY);
			ClampF(s.lightZ, 0.05f, 10.0f, d.shade.lightZ);
			ClampF(s.ambient, 0.0f, 4.0f, d.shade.ambient);
			ClampF(s.diffuse, 0.0f, 4.0f, d.shade.diffuse);
			ClampF(s.shadeMin, 0.0f, 4.0f, d.shade.shadeMin);
			ClampF(s.shadeMax, s.shadeMin, 8.0f, d.shade.shadeMax);
			ClampF(s.brightness, 0.0f, 4.0f, d.shade.brightness);
			ClampF(s.saturation, 0.0f, 4.0f, d.shade.saturation);
			ClampF(s.waterR, 0.0f, 1.0f, d.shade.waterR);
			ClampF(s.waterG, 0.0f, 1.0f, d.shade.waterG);
			ClampF(s.waterB, 0.0f, 1.0f, d.shade.waterB);
			ClampF(s.waterAlpha, 0.0f, 1.0f, d.shade.waterAlpha);

			ClampF(a_v.icons.scale, 0.25f, 4.0f, d.icons.scale);
			ClampF(a_v.bake.settleSeconds, 2.0f, 60.0f, d.bake.settleSeconds);
			ClampF(a_v.bake.areaScale, 0.5f, 4.0f, d.bake.areaScale);
		}

		void ApplyLogLevel(bool a_debug)
		{
			if (auto log = spdlog::default_logger()) {
				log->set_level(a_debug ? spdlog::level::debug : spdlog::level::info);
				log->flush_on(a_debug ? spdlog::level::debug : spdlog::level::info);
			}
		}

		// "R,G,B" or "R,G,B,A", 0..255 each.
		std::uint32_t ParseColor(const char* a_text, std::uint32_t a_default)
		{
			if (!a_text)
				return a_default;
			int         c[4]{ 0, 0, 0, 255 };
			int         n = 0;
			std::string s(a_text);
			for (std::size_t pos = 0; pos <= s.size() && n < 4;) {
				const auto end = std::min(s.find(',', pos), s.size());
				try {
					c[n++] = std::clamp(std::stoi(s.substr(pos, end - pos)), 0, 255);
				} catch (...) {
					return a_default;
				}
				pos = end + 1;
			}
			return n >= 3 ? MM::PackRgba(c[0], c[1], c[2], c[3]) : a_default;
		}

		std::string FormatColor(std::uint32_t a_rgba)
		{
			return std::format("{},{},{},{}", a_rgba & 255, (a_rgba >> 8) & 255, (a_rgba >> 16) & 255, (a_rgba >> 24) & 255);
		}

		struct ColorKey
		{
			const char*                        key;
			std::uint32_t IconColors::*member;
		};
		constexpr ColorKey kColorKeys[]{
			{ "sCity", &IconColors::city },
			{ "sSettlement", &IconColors::settlement },
			{ "sDungeon", &IconColors::dungeon },
			{ "sOther", &IconColors::other },
			{ "sQuest", &IconColors::quest },
			{ "sHostile", &IconColors::hostile },
			{ "sFollower", &IconColors::follower },
			{ "sNeutral", &IconColors::neutral },
			{ "sDead", &IconColors::dead },
			{ "sDoor", &IconColors::door },
			{ "sCustomMarker", &IconColors::custom },
			{ "sArrow", &IconColors::arrow },
		};

		const char* CornerName(Corner a_c)
		{
			switch (a_c) {
			case Corner::kTopLeft:
				return "TL";
			case Corner::kBottomLeft:
				return "BL";
			case Corner::kBottomRight:
				return "BR";
			default:
				return "TR";
			}
		}
	}

	std::vector<float> ParseRadii(const std::string& a_list)
	{
		std::vector<float> radii;
		for (std::size_t pos = 0; pos <= a_list.size();) {
			const auto end = std::min(a_list.find(',', pos), a_list.size());
			try {
				const float r = std::stof(a_list.substr(pos, end - pos));
				if (std::isfinite(r) && r >= 500.0f && r <= 200000.0f)
					radii.push_back(r);
			} catch (...) {
			}
			pos = end + 1;
		}
		return radii;
	}

	std::string FormatRadii(const std::vector<float>& a_radii)
	{
		std::string s;
		for (const float r : a_radii)
			s += (s.empty() ? "" : ",") + std::format("{:.0f}", r);
		return s;
	}

	void Load()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(kPath) < 0) {
			logger::info("{} not found, using defaults", kPath);
			return;
		}

		Values v;
		auto   b = [&](const char* a_sec, const char* a_key, bool& a_out) { a_out = ini.GetBoolValue(a_sec, a_key, a_out); };
		auto   i = [&](const char* a_sec, const char* a_key, int& a_out) { a_out = static_cast<int>(ini.GetLongValue(a_sec, a_key, a_out)); };
		auto   f = [&](const char* a_sec, const char* a_key, float& a_out) { a_out = static_cast<float>(ini.GetDoubleValue(a_sec, a_key, a_out)); };

		b("General", "bEnabled", v.enabled);
		i("General", "iSize", v.sizePx);
		b("General", "bScaleWithResolution", v.scaleWithResolution);
		const auto corner = Upper(ini.GetValue("General", "sCorner", "BR"));
		v.corner = corner == "TL" ? Corner::kTopLeft : corner == "BL" ? Corner::kBottomLeft : corner == "BR" ? Corner::kBottomRight : Corner::kTopRight;
		i("General", "iOffsetX", v.offsetX);
		i("General", "iOffsetY", v.offsetY);
		v.round = Upper(ini.GetValue("General", "sShape", "round")) != "SQUARE";
		f("General", "fOpacity", v.opacity);
		f("General", "fBrightness", v.brightness);
		b("General", "bRotateWithPlayer", v.rotateWithPlayer);
		b("General", "bUseCameraYaw", v.useCameraYaw);
		b("General", "bHideInInteriors", v.hideInInteriors);
		b("General", "bHideInKillCam", v.hideInKillCam);
		i("General", "iColorMode", v.colorMode);
		f("General", "fHdrBrightness", v.hdrBrightness);
		f("General", "fRadarZoomScale", v.radarZoomScale);
		f("Interior", "fFloorBelow", v.floorBelow);
		f("Interior", "fFloorAbove", v.floorAbove);
		f("Interior", "fOtherFloorAlpha", v.otherFloorAlpha);
		b("General", "bDebugLog", v.debugLog);
		v.drawHook = ini.GetLongValue("General", "iDrawHook", 0) == 1 ? DrawHook::kPresent : DrawHook::kHudMenu;
		i("General", "iCacheMB", v.cacheMB);
		int hideKey = static_cast<int>(v.hideKey), zoomKey = static_cast<int>(v.zoomKey);
		i("General", "iHideKey", hideKey);
		i("Zoom", "iZoomKey", zoomKey);
		v.hideKey = static_cast<std::uint32_t>(std::clamp(hideKey, 0, 255));
		v.zoomKey = static_cast<std::uint32_t>(std::clamp(zoomKey, 0, 255));
		if (auto radii = ParseRadii(ini.GetValue("Zoom", "sRadii", "")); !radii.empty())
			v.zoomRadii = std::move(radii);

		auto& s = v.shade;
		f("Shading", "fLightX", s.lightX);
		f("Shading", "fLightY", s.lightY);
		f("Shading", "fLightZ", s.lightZ);
		f("Shading", "fAmbient", s.ambient);
		f("Shading", "fDiffuse", s.diffuse);
		f("Shading", "fShadeMin", s.shadeMin);
		f("Shading", "fShadeMax", s.shadeMax);
		f("Shading", "fBrightness", s.brightness);
		f("Shading", "fSaturation", s.saturation);
		b("Water", "bEnabled", s.water);
		s.waterR = static_cast<float>(ini.GetDoubleValue("Water", "iRed", 60.0)) / 255.0f;
		s.waterG = static_cast<float>(ini.GetDoubleValue("Water", "iGreen", 110.0)) / 255.0f;
		s.waterB = static_cast<float>(ini.GetDoubleValue("Water", "iBlue", 120.0)) / 255.0f;
		f("Water", "fStrength", s.waterAlpha);

		auto& ic = v.icons;
		b("Icons", "bEnabled", ic.enabled);
		f("Icons", "fScale", ic.scale);
		b("Icons", "bMapMarkers", ic.mapMarkers);
		b("Icons", "bUndiscovered", ic.undiscovered);
		b("Icons", "bQuestTargets", ic.quest);
		b("Icons", "bHostile", ic.hostile);
		b("Icons", "bFollowers", ic.follower);
		b("Icons", "bNeutral", ic.neutral);
		b("Icons", "bDead", ic.dead);
		b("Icons", "bCustomMarker", ic.customMarker);
		const auto doors = std::clamp(static_cast<int>(ini.GetLongValue("Icons", "iDoors", 1)), 0, 2);
		ic.doors = static_cast<DoorMode>(doors);
		for (const auto& c : kColorKeys)
			ic.colors.*c.member = ParseColor(ini.GetValue("Colors", c.key, nullptr), ic.colors.*c.member);

		f("Bake", "fSettleSeconds", v.bake.settleSeconds);
		f("Bake", "fAreaScale", v.bake.areaScale);
		b("Bake", "bKeepExistingTiles", v.bake.keepExisting);

		Set(std::move(v));
		const auto now = Get();
		logger::info("settings: size {} corner {} shape {} rotate {} zoom steps {} hook {} icons {}", now->sizePx, CornerName(now->corner), now->round ? "round" : "square",
			now->rotateWithPlayer, now->zoomRadii.size(), now->drawHook == DrawHook::kPresent ? "Present" : "HUDMenu", now->icons.enabled);
	}

	bool Save()
	{
		const auto  vp = Get();
		const auto& v = *vp;
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(kPath);  // keep comments and unknown keys when the file exists

		auto b = [&](const char* a_sec, const char* a_key, bool a_val) { ini.SetBoolValue(a_sec, a_key, a_val); };
		auto i = [&](const char* a_sec, const char* a_key, long a_val) { ini.SetLongValue(a_sec, a_key, a_val); };
		auto f = [&](const char* a_sec, const char* a_key, float a_val) { ini.SetValue(a_sec, a_key, std::format("{}", a_val).c_str()); };
		auto t = [&](const char* a_sec, const char* a_key, const std::string& a_val) { ini.SetValue(a_sec, a_key, a_val.c_str()); };

		b("General", "bEnabled", v.enabled);
		i("General", "iSize", v.sizePx);
		b("General", "bScaleWithResolution", v.scaleWithResolution);
		t("General", "sCorner", CornerName(v.corner));
		i("General", "iOffsetX", v.offsetX);
		i("General", "iOffsetY", v.offsetY);
		t("General", "sShape", v.round ? "round" : "square");
		f("General", "fOpacity", v.opacity);
		f("General", "fBrightness", v.brightness);
		b("General", "bRotateWithPlayer", v.rotateWithPlayer);
		b("General", "bUseCameraYaw", v.useCameraYaw);
		b("General", "bHideInInteriors", v.hideInInteriors);
		b("General", "bHideInKillCam", v.hideInKillCam);
		i("General", "iColorMode", v.colorMode);
		f("General", "fHdrBrightness", v.hdrBrightness);
		f("General", "fRadarZoomScale", v.radarZoomScale);
		f("Interior", "fFloorBelow", v.floorBelow);
		f("Interior", "fFloorAbove", v.floorAbove);
		f("Interior", "fOtherFloorAlpha", v.otherFloorAlpha);
		b("General", "bDebugLog", v.debugLog);
		i("General", "iHideKey", static_cast<long>(v.hideKey));
		i("General", "iCacheMB", v.cacheMB);
		i("General", "iDrawHook", v.drawHook == DrawHook::kPresent ? 1 : 0);
		t("Zoom", "sRadii", FormatRadii(v.zoomRadii));
		i("Zoom", "iZoomKey", static_cast<long>(v.zoomKey));

		const auto& s = v.shade;
		f("Shading", "fLightX", s.lightX);
		f("Shading", "fLightY", s.lightY);
		f("Shading", "fLightZ", s.lightZ);
		f("Shading", "fAmbient", s.ambient);
		f("Shading", "fDiffuse", s.diffuse);
		f("Shading", "fShadeMin", s.shadeMin);
		f("Shading", "fShadeMax", s.shadeMax);
		f("Shading", "fBrightness", s.brightness);
		f("Shading", "fSaturation", s.saturation);
		b("Water", "bEnabled", s.water);
		i("Water", "iRed", std::lround(s.waterR * 255.0f));
		i("Water", "iGreen", std::lround(s.waterG * 255.0f));
		i("Water", "iBlue", std::lround(s.waterB * 255.0f));
		f("Water", "fStrength", s.waterAlpha);

		const auto& ic = v.icons;
		b("Icons", "bEnabled", ic.enabled);
		f("Icons", "fScale", ic.scale);
		b("Icons", "bMapMarkers", ic.mapMarkers);
		b("Icons", "bUndiscovered", ic.undiscovered);
		b("Icons", "bQuestTargets", ic.quest);
		b("Icons", "bHostile", ic.hostile);
		b("Icons", "bFollowers", ic.follower);
		b("Icons", "bNeutral", ic.neutral);
		b("Icons", "bDead", ic.dead);
		b("Icons", "bCustomMarker", ic.customMarker);
		i("Icons", "iDoors", static_cast<long>(ic.doors));
		for (const auto& c : kColorKeys)
			t("Colors", c.key, FormatColor(ic.colors.*c.member));

		f("Bake", "fSettleSeconds", v.bake.settleSeconds);
		f("Bake", "fAreaScale", v.bake.areaScale);
		b("Bake", "bKeepExistingTiles", v.bake.keepExisting);

		const bool ok = ini.SaveFile(kPath) >= 0;
		if (ok)
			logger::info("settings saved to {}", kPath);
		else
			logger::error("could not write {}", kPath);
		return ok;
	}

	namespace
	{
		std::atomic<int> hideActorsCache{ -1 };  // -1 = not read yet (the settings page asks every frame)
	}

	bool BakeHideActorsRequested()
	{
		if (const int c = hideActorsCache.load(); c >= 0)
			return c == 1;
		CSimpleIniA ini;
		ini.SetUnicode();
		const bool on = ini.LoadFile(kPath) >= 0 && ini.GetBoolValue("Bake", "bHideActorsOnNextStart", false);
		hideActorsCache = on ? 1 : 0;
		return on;
	}

	bool BakeStartOnLoadRequested()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		return ini.LoadFile(kPath) >= 0 && ini.GetBoolValue("Bake", "bStartOnLoad", false);
	}

	void ClearBakeStartOnLoad()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(kPath) < 0 || !ini.GetBoolValue("Bake", "bStartOnLoad", false))
			return;
		ini.SetBoolValue("Bake", "bStartOnLoad", false);
		if (ini.SaveFile(kPath) < 0)
			logger::error("could not write {}", kPath);
	}

	void SetBakeHideActors(bool a_on)
	{
		hideActorsCache = a_on ? 1 : 0;
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(kPath);
		ini.SetBoolValue("Bake", "bHideActorsOnNextStart", a_on);
		if (ini.SaveFile(kPath) < 0)
			logger::error("could not write {}", kPath);
		else
			logger::info("bake: hide NPCs on next start {}", a_on ? "on" : "off");
	}

	std::shared_ptr<const Values> Get() { return current.load(std::memory_order_acquire); }

	void Set(Values a_values)
	{
		Clamp(a_values);
		const bool debug = a_values.debugLog;
		current.store(std::make_shared<const Values>(std::move(a_values)), std::memory_order_release);
		generation.fetch_add(1, std::memory_order_acq_rel);
		ApplyLogLevel(debug);
	}

	std::uint32_t Generation() { return generation.load(std::memory_order_acquire); }
}
