// Modern Minimap Photo Baker: a small window that runs the town photo bake built into the Modern Minimap plugin
// without the player touching the game. It writes the plugin's [Bake] switches, starts the game through SKSE,
// follows the plugin's status file, starts the game again after a crash, a planned restart or when it got stuck (the
// bake resumes from its progress file) and finally moves the new photo tiles into a folder the player installs as a mod.

#include "BakeRules.h"
#include "BakerFiles.h"
#include "Bc7Encode.h"
#include "MasterTone.h"
#include "TileFormats.h"
#include "resource.h"

#include <CommCtrl.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

namespace
{
	using namespace PhotoBaker;

	constexpr wchar_t  kClass[] = L"ModernMinimapPhotoBaker";
	constexpr wchar_t  kTitle[] = L"Modern Minimap Photo Baker";
	constexpr wchar_t  kAppSection[] = L"PhotoBaker";
	constexpr UINT_PTR kTimer = 1;
	constexpr UINT_PTR kEncodeTimer = 2;
	constexpr ULONGLONG kStartTimeoutMs = 120'000;  // skse64_loader -> SkyrimSE.exe
	constexpr ULONGLONG kQuitTimeoutMs = 120'000;   // final status -> game process gone
	constexpr ULONGLONG kRelaunchDelayMs = 10'000;  // let the crashed game release its files first
	constexpr ULONGLONG kStuckCloseGraceMs = 30'000;  // close request to a stuck game -> end its process
	constexpr wchar_t  kCrashAdvice[] = L"The game closed early. If this keeps happening, turn off the mods suggested above (lighting, effects, physics and NPC replacers first) and press Start again; the bake continues where it stopped.";
	constexpr wchar_t  kPreBakeNotice[] =
		L"Before baking: disable anything that shrinks or removes textures or LOD, such as VRAMr, texture downscalers, terrain LOD downscalers and mods that "
		L"remove LOD in cities. Keep your generated outputs on: xLODGen terrain LOD, TexGen, DynDOLOD, your grass cache and Synthesis. The photos show the game "
		L"exactly as it looks while baking.";
	constexpr wchar_t  kFullMapNote[] = L"Whole map is about 4,900 photos at 1x1: roughly 9-10 hours, plus the tile encode.";

	HINSTANCE instance = nullptr;

	struct Ui
	{
		HWND  wnd{};
		HWND  gameGroup{}, gameLabel{}, gameEdit{}, gameBrowse{}, gameInfo{};
		HWND  adviceGroup{}, advice{};
		HWND  setGroup{}, areaLabel{}, areaTrack{}, areaValue{}, hideNpcs{}, settleLabel{}, settleEdit{}, settleSpin{};
		HWND  restartsLabel{}, restartsEdit{}, restartsSpin{}, stallLabel{}, stallEdit{}, stallSpin{}, keep{}, gridLabel{}, gridTrack{}, gridValue{}, gridLowLabel{}, gridHighLabel{}, gridHint{};
		HWND  mapAreaLabel{}, mapAreaCombo{}, tileSizeLabel{}, tileSizeCombo{}, fullMapNote{}, formatLabel{}, formatCombo{}, formatHint{};
		HWND  outGroup{}, outLabel{}, outEdit{}, outBrowse{};
		HWND  brightLabel{}, brightTrack{}, brightValue{}, satLabel{}, satTrack{}, satValue{}, recolourHint{};
		HWND  prebakeNote{}, start{}, cancel{}, open{}, encode{}, progress{}, count{}, eta{}, phase{}, log{};
		HFONT font{};
		UINT  dpi{ 96 };
	} ui;

	enum class Outcome
	{
		kDone,
		kCancelled,
		kError,   // the plugin stopped the bake (see its message)
		kFailed   // the game kept closing early, or never started
	};

	struct Run
	{
		bool        active{ false };
		bool        switchesOn{ false };  // the ini keys this app turned on
		bool        cancelRequested{ false };
		bool        crashAdviceShown{ false };
		int         restartsLeft{ 0 };
		int         restartsAllowed{ 0 };  // in a row without progress; refilled whenever the done count grew
		int         doneAtLaunch{ 0 };     // highest done count when the game was last started
		int         bestDone{ 0 };         // highest done count seen in this run (the plugin's count includes earlier runs)
		int         lastFailedCells{ 0 };
		ULONGLONG   progressTick{ 0 };     // last launch or progress, for the stall watchdog
		ULONGLONG   stallLimitMs{ 0 };     // 0 = watchdog off
		ULONGLONG   stuckCloseAt{ 0 };     // when a stuck game that ignored the close request is ended
		bool        stuckClose{ false };   // the watchdog is closing the game
		int         stuckRestarts{ 0 };
		ULONGLONG   restartSeen{ 0 };      // status "restart" first seen
		FILETIME    start{};       // tiles written after this belong to this run
		FILETIME    launchTime{};  // only a SkyrimSE.exe created after this is ours
		ULONGLONG   launchTick{ 0 };
		ULONGLONG   relaunchAt{ 0 };
		ULONGLONG   finalSeen{ 0 };
		HANDLE      game{ nullptr };
		DWORD       gamePid{ 0 };
		BakeStatus  last;
		bool        haveLast{ false };
		std::string lastLoggedPhase;
		std::string lastLoggedMessage;
		fs::path    gameDir, loader, ini, status, cancel, tiles, output;
		TileFormat  format{ kDefaultTileFormat };
	} run;

	// BC7 encoding of the master tiles, after a bake or from "Encode existing masters".
	struct Encode
	{
		Bc7Encoder encoder;
		bool       active{ false };
		ULONGLONG  startTick{ 0 };
		fs::path   output;  // the mod folder the tiles go into
	} encode;

	fs::path openFolder;  // for the Open folder button

	// ---------------------------------------------------------------- small helpers

	std::wstring GetText(HWND a_h)
	{
		std::wstring s(static_cast<std::size_t>(GetWindowTextLengthW(a_h)) + 1, L'\0');
		s.resize(static_cast<std::size_t>(GetWindowTextW(a_h, s.data(), static_cast<int>(s.size()))));
		const auto b = s.find_first_not_of(L" \t\"");
		const auto e = s.find_last_not_of(L" \t\"");
		return b == std::wstring::npos ? std::wstring{} : s.substr(b, e - b + 1);
	}

	bool Exists(const fs::path& a_p)
	{
		std::error_code ec;
		return fs::exists(a_p, ec);
	}

	void Log(const std::wstring& a_text)
	{
		SYSTEMTIME t{};
		GetLocalTime(&t);
		const auto line = std::format(L"[{:02}:{:02}:{:02}] {}\r\n", t.wHour, t.wMinute, t.wSecond, a_text);
		const int  len = GetWindowTextLengthW(ui.log);
		SendMessageW(ui.log, EM_SETSEL, len, len);
		SendMessageW(ui.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
	}

	std::wstring FormatEta(double a_seconds)
	{
		const int minutes = std::max(1, static_cast<int>(std::ceil(a_seconds / 60.0)));
		return minutes < 60 ? std::format(L"about {} min", minutes) : std::format(L"about {} h {} min", minutes / 60, minutes % 60);
	}

	fs::path PluginsDir(const fs::path& a_game) { return a_game / "Data" / "SKSE" / "Plugins"; }
	fs::path MastersDir(const fs::path& a_game) { return a_game / "Data" / "ModernMinimapMasters" / "tamriel"; }
	fs::path TilesDir(const fs::path& a_mod) { return a_mod / "textures" / "ModernMinimap" / "tamriel"; }
	fs::path HalfSetOutput(const fs::path& a_output) { return a_output.parent_path() / (a_output.filename().wstring() + L" (512 px)"); }
	fs::path DefaultOutput() { return AppFolder() / L"Modern Minimap - My Photos"; }
	fs::path AppSettingsPath() { return AppFolder() / L"Modern Minimap Photo Baker.ini"; }

	fs::path MissingListOutput(const fs::path& a_output) { return a_output.parent_path() / (a_output.filename().wstring() + L" - missing cells.txt"); }
	fs::path ColourStampPath(const fs::path& a_mod) { return a_mod / L"PhotoBakerColour.txt"; }

	std::uint64_t FileTimeValue(const FILETIME& a_time) { return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime; }

	float AreaFromTrack() { return static_cast<float>(SendMessageW(ui.areaTrack, TBM_GETPOS, 0, 0)) / 4.0f; }

	// Re-colour sliders for "Encode existing masters", in percent: 100 = as baked.
	float    FactorFromTrack(HWND a_track) { return static_cast<float>(SendMessageW(a_track, TBM_GETPOS, 0, 0)) / 100.0f; }
	Recolour RecolourFromTracks() { return { FactorFromTrack(ui.brightTrack), FactorFromTrack(ui.satTrack) }; }
	void     UpdateFactorLabel(HWND a_track, HWND a_label) { SetWindowTextW(a_label, std::format(L"{:.2f}x", FactorFromTrack(a_track)).c_str()); }

	void UpdateRecolourLabels()
	{
		UpdateFactorLabel(ui.brightTrack, ui.brightValue);
		UpdateFactorLabel(ui.satTrack, ui.satValue);
	}

	int SpinValue(HWND a_spin)
	{
		BOOL      bad = FALSE;
		const int v = static_cast<int>(SendMessageW(a_spin, UDM_GETPOS32, 0, reinterpret_cast<LPARAM>(&bad)));
		return v;
	}

	void UpdateAreaLabel() { SetWindowTextW(ui.areaValue, std::format(L"{:.2f}x", AreaFromTrack()).c_str()); }

	// Photo grid names: cells per photo side is the index + 1 ([Bake] iPhotoGrid). The slider runs from 4x4 on the
	// left (lower map quality) to 1x1 on the right (higher map quality).
	constexpr const wchar_t* kGridChoices[]{ L"1x1 (sharpest)", L"2x2 (default)", L"3x3 (faster)", L"4x4 (fastest)" };
	constexpr int            kDefaultGrid = 2;

	int GridFromTrack() { return GridFromQuality(static_cast<int>(SendMessageW(ui.gridTrack, TBM_GETPOS, 0, 0))); }

	void UpdateGridLabel() { SetWindowTextW(ui.gridValue, kGridChoices[GridFromTrack() - 1]); }

	void SetGridTrack(int a_grid)
	{
		SendMessageW(ui.gridTrack, TBM_SETPOS, TRUE, QualityFromGrid(a_grid));
		UpdateGridLabel();
	}

	// "Area" drop-down ([Bake] bFullMap), in drop-down order.
	struct MapAreaChoice
	{
		const wchar_t* label;
		bool           fullMap;
	};
	constexpr MapAreaChoice kMapAreas[]{ { L"Towns and places", false }, { L"Whole map", true } };

	int  ComboIndex(HWND a_combo) { return std::max(0, static_cast<int>(SendMessageW(a_combo, CB_GETCURSEL, 0, 0))); }
	bool FullMapFromCombo() { return kMapAreas[std::min<std::size_t>(ComboIndex(ui.mapAreaCombo), std::size(kMapAreas) - 1)].fullMap; }
	int  TileSizeFromCombo() { return kTileSizes[std::min<std::size_t>(ComboIndex(ui.tileSizeCombo), kTileSizes.size() - 1)]; }
	TileFormat FormatFromCombo() { return TileFormatAt(ComboIndex(ui.formatCombo)); }

	void UpdateFullMapNote() { ShowWindow(ui.fullMapNote, FullMapFromCombo() ? SW_SHOW : SW_HIDE); }

	// ---------------------------------------------------------------- app settings (next to the exe)

	std::wstring ReadAppValue(const wchar_t* a_key, const wchar_t* a_default)
	{
		wchar_t buf[MAX_PATH * 2];
		GetPrivateProfileStringW(kAppSection, a_key, a_default, buf, static_cast<DWORD>(std::size(buf)), AppSettingsPath().c_str());
		return buf;
	}

	void LoadAppSettings()
	{
		const int area = std::clamp(_wtoi(ReadAppValue(L"Area4", L"4").c_str()), 2, 16);  // quarter steps: 4 = 1.0x
		SendMessageW(ui.areaTrack, TBM_SETPOS, TRUE, area);
		UpdateAreaLabel();
		Button_SetCheck(ui.hideNpcs, ReadAppValue(L"HideNpcs", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED);
		Button_SetCheck(ui.keep, ReadAppValue(L"KeepExisting", L"0") == L"1" ? BST_CHECKED : BST_UNCHECKED);
		SetGridTrack(_wtoi(ReadAppValue(L"PhotoGrid", L"2").c_str()));
		SendMessageW(ui.mapAreaCombo, CB_SETCURSEL, ReadAppValue(L"FullMap", L"0") == L"1" ? 1 : 0, 0);
		UpdateFullMapNote();
		SendMessageW(ui.tileSizeCombo, CB_SETCURSEL, static_cast<WPARAM>(TileSizeIndex(_wtoi(ReadAppValue(L"TileSize", L"512").c_str()))), 0);
		SendMessageW(ui.formatCombo, CB_SETCURSEL, static_cast<WPARAM>(TileFormatIndex(TileFormatFromKey(ReadAppValue(L"TileFormat", L"BC1")))), 0);
		// The spin boxes read their value from the edit's text, so the loaded value goes into the edit too.
		const auto loadSpin = [](HWND a_spin, HWND a_edit, const wchar_t* a_key, int a_default, int a_min, int a_max) {
			const int value = std::clamp(_wtoi(ReadAppValue(a_key, std::to_wstring(a_default).c_str()).c_str()), a_min, a_max);
			SendMessageW(a_spin, UDM_SETPOS32, 0, value);
			SetWindowTextW(a_edit, std::to_wstring(value).c_str());
		};
		loadSpin(ui.settleSpin, ui.settleEdit, L"Seconds", 7, 2, 30);
		loadSpin(ui.restartsSpin, ui.restartsEdit, L"Restarts", kDefaultRestarts, 0, kMaxRestarts);
		loadSpin(ui.stallSpin, ui.stallEdit, L"StallMinutes", kDefaultStallMinutes, 0, kMaxStallMinutes);
		const auto percent = [](const wchar_t* a_key) {
			return std::clamp(_wtoi(ReadAppValue(a_key, std::to_wstring(kRecolourNeutralPercent).c_str()).c_str()), kRecolourMinPercent, kRecolourMaxPercent);
		};
		SendMessageW(ui.brightTrack, TBM_SETPOS, TRUE, percent(L"BrightnessPercent"));
		SendMessageW(ui.satTrack, TBM_SETPOS, TRUE, percent(L"SaturationPercent"));
		UpdateRecolourLabels();
		const auto out = ReadAppValue(L"OutputFolder", L"");
		SetWindowTextW(ui.outEdit, out.empty() ? DefaultOutput().c_str() : out.c_str());
	}

	void SaveAppSettings()
	{
		const auto path = AppSettingsPath();
		const auto put = [&](const wchar_t* a_key, const std::wstring& a_value) { WritePrivateProfileStringW(kAppSection, a_key, a_value.c_str(), path.c_str()); };
		put(L"GameFolder", GetText(ui.gameEdit));
		put(L"OutputFolder", GetText(ui.outEdit));
		put(L"Area4", std::to_wstring(SendMessageW(ui.areaTrack, TBM_GETPOS, 0, 0)));
		put(L"HideNpcs", Button_GetCheck(ui.hideNpcs) == BST_CHECKED ? L"1" : L"0");
		put(L"KeepExisting", Button_GetCheck(ui.keep) == BST_CHECKED ? L"1" : L"0");
		put(L"PhotoGrid", std::to_wstring(GridFromTrack()));
		put(L"FullMap", FullMapFromCombo() ? L"1" : L"0");
		put(L"TileSize", std::to_wstring(TileSizeFromCombo()));
		put(L"TileFormat", kTileFormats[static_cast<std::size_t>(TileFormatIndex(FormatFromCombo()))].key);
		put(L"Seconds", std::to_wstring(SpinValue(ui.settleSpin)));
		put(L"Restarts", std::to_wstring(SpinValue(ui.restartsSpin)));
		put(L"StallMinutes", std::to_wstring(SpinValue(ui.stallSpin)));
		put(L"BrightnessPercent", std::to_wstring(SendMessageW(ui.brightTrack, TBM_GETPOS, 0, 0)));
		put(L"SaturationPercent", std::to_wstring(SendMessageW(ui.satTrack, TBM_GETPOS, 0, 0)));
	}

	// ---------------------------------------------------------------- game folder check and advice

	void RefreshGameInfo()
	{
		const fs::path game = GetText(ui.gameEdit);
		std::wstring   info;
		if (!IsGameFolder(game)) {
			info = L"SkyrimSE.exe is not in this folder. Choose the folder that holds SkyrimSE.exe.";
		} else {
			info = Exists(game / "skse64_loader.exe") ? L"SkyrimSE.exe and skse64_loader.exe found." : L"skse64_loader.exe is missing: install SKSE first.";
			const bool mo2 = StartedFromMo2();
			if (Exists(PluginsDir(game) / "ModernMinimap.dll"))
				info += mo2 ? L" Modern Minimap found (MO2's virtual Data folder)." : L" Modern Minimap found.";
			else
				info += mo2 ? L" Modern Minimap is not enabled in this MO2 profile." : L" Modern Minimap is not in Data. With MO2, start this app from MO2's executables list.";
		}
		SetWindowTextW(ui.gameInfo, info.c_str());

		// Generic on purpose: the photos only need terrain, towns and buildings, so everything else just slows the bake down.
		SetWindowTextW(ui.advice,
			L"Suggested to turn off while baking (faster, fewer crashes): lighting mods, visual effects mods, physics mods, "
			L"NPC and creature replacers - anything that is not terrain, towns or buildings.\r\n"
			L"Disable them in your mod manager (a separate MO2 profile is easiest), then come back.");
	}

	std::wstring PickFolder(const std::wstring& a_start)
	{
		std::wstring     result;
		IFileOpenDialog* dlg = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
			return result;
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		IShellItem* item = nullptr;
		if (!a_start.empty() && SUCCEEDED(SHCreateItemFromParsingName(a_start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
			dlg->SetFolder(item);
			item->Release();
			item = nullptr;
		}
		if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&item))) {
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
				result = path;
				CoTaskMemFree(path);
			}
			item->Release();
		}
		dlg->Release();
		return result;
	}

	// ---------------------------------------------------------------- the run

	void EnableControls(bool a_busy)
	{
		for (HWND h : { ui.gameEdit, ui.gameBrowse, ui.areaTrack, ui.hideNpcs, ui.settleEdit, ui.settleSpin, ui.restartsEdit, ui.restartsSpin, ui.stallEdit, ui.stallSpin,
				 ui.keep, ui.gridTrack, ui.mapAreaCombo, ui.tileSizeCombo, ui.formatCombo, ui.outEdit, ui.outBrowse, ui.brightTrack, ui.satTrack, ui.start, ui.encode })
			EnableWindow(h, !a_busy);
		EnableWindow(ui.cancel, a_busy);
	}

	void SetRunningUi(bool a_running)
	{
		EnableControls(a_running);
		if (a_running) {
			EnableWindow(ui.open, FALSE);
			SendMessageW(ui.progress, PBM_SETPOS, 0, 0);
			SetWindowTextW(ui.count, L"Photos: counting...");
			SetWindowTextW(ui.eta, L"");
			SetWindowTextW(ui.phase, L"Now: starting the game");
		}
	}

	// Turns the plugin's switches off again, so the next normal game start is a normal game.
	void ResetSwitches()
	{
		if (!run.switchesOn)
			return;
		std::wstring error;
		if (SetIniValues(run.ini, "Bake",
				{ { "bStartOnLoad", "false" }, { "bEnterFromMainMenu", "false" }, { "bQuitWhenDone", "false" }, { "bHideActorsOnNextStart", "false" } }, error)) {
			run.switchesOn = false;
			Log(L"Bake switches in ModernMinimap.ini turned off again.");
		} else {
			Log(error + L" Set bStartOnLoad, bEnterFromMainMenu and bQuitWhenDone in its [Bake] section back to false by hand.");
		}
	}

	// ---------------------------------------------------------------- BC7 encoding of the master tiles

	// The colour stamp of an output folder changes only when the tone or the recolour changes; tiles written before the
	// stamp are encoded again. Returns the stamp's write time (now, when it cannot be written: every tile again).
	std::uint64_t UpdateColourStamp(const fs::path& a_output, const std::string& a_stamp)
	{
		const auto  path = ColourStampPath(a_output);
		std::string previous;
		if (ReadTextFile(path, previous) && previous == a_stamp)
			return FileWriteTime(path);
		if (WriteTextFile(path, a_stamp))
			return FileWriteTime(path);
		FILETIME now{};
		GetSystemTimeAsFileTime(&now);
		return FileTimeValue(now);
	}

	// The masters are untoned: the tone the plugin saved next to them (tone.txt), then the player's recolour.
	void PrepareColour(EncodeJob& a_job, const fs::path& a_output, const Recolour& a_recolour)
	{
		std::string toneText;
		if (!ReadTextFile(a_job.masters / "tone.txt", toneText)) {
			toneText.clear();
			Log(L"No tone.txt next to the masters: using the plugin's fixed fallback colours.");
		} else if (!ParseToneText(toneText, a_job.tone)) {
			Log(L"The masters' tone.txt could not be read: using the plugin's fixed fallback colours.");
		} else {
			Log(a_job.tone.fitted ? L"Colours: the plugin's matched tone from tone.txt." : L"Colours: the plugin's fixed fallback tone (tone.txt).");
		}
		a_job.recolour = a_recolour;
		if (!IsNeutral(a_recolour))
			Log(std::format(L"Re-colour: brightness {:.2f}x, saturation {:.2f}x.", a_recolour.brightness, a_recolour.saturation));
		a_job.colourTime = UpdateColourStamp(a_output, ColourStamp(toneText, a_recolour));
	}

	void StartEncode(const EncodeJob& a_job, const fs::path& a_output)
	{
		encode.encoder.Start(a_job);
		encode.active = true;
		encode.startTick = GetTickCount64();
		encode.output = a_output;
		EnableControls(true);
		SendMessageW(ui.progress, PBM_SETPOS, 0, 0);
		SetWindowTextW(ui.count, L"Tiles: counting...");
		SetWindowTextW(ui.eta, L"");
		SetWindowTextW(ui.phase, L"Now: encoding BC7 tiles");
		Log(std::format(L"Encoding BC7 tiles ({}) from {} into {}{}.", kTileFormats[static_cast<std::size_t>(TileFormatIndex(a_job.format))].label,
			a_job.masters.wstring(), a_job.dest.wstring(), a_job.maxSide ? std::format(L" at {} px", a_job.maxSide) : L""));
		SetTimer(ui.wnd, kEncodeTimer, 500, nullptr);
	}

	void ShowEncodeProgress(const EncodeCounts& a_counts)
	{
		if (a_counts.total < 0)
			return;
		SendMessageW(ui.progress, PBM_SETRANGE32, 0, std::max(1, a_counts.total));
		SendMessageW(ui.progress, PBM_SETPOS, static_cast<WPARAM>(a_counts.done), 0);
		SetWindowTextW(ui.count, std::format(L"Tiles: {} of {} ({})", a_counts.done, a_counts.total, a_counts.gpu ? L"GPU" : L"CPU").c_str());
		// Time per encoded tile so far; skipped tiles are near free and left out.
		const double elapsed = static_cast<double>(GetTickCount64() - encode.startTick) / 1000.0;
		if (a_counts.encoded > 0 && a_counts.done < a_counts.total)
			SetWindowTextW(ui.eta, (L"Time left: " + FormatEta(elapsed / a_counts.encoded * (a_counts.total - a_counts.done))).c_str());
	}

	void FinishEncode(const EncodeCounts& a_counts)
	{
		KillTimer(ui.wnd, kEncodeTimer);
		encode.encoder.Stop();  // joins the finished worker
		encode.active = false;
		EnableControls(false);
		SetWindowTextW(ui.eta, L"");
		SetWindowTextW(ui.phase, a_counts.stopped ? L"Now: encoding stopped" : L"Now: encoding finished");
		const auto summary = std::format(L"BC7 encoding {}: {} encoded, {} already up to date, {} failed (of {}).", a_counts.stopped ? L"stopped" : L"finished",
			a_counts.encoded, a_counts.skipped, a_counts.failed, std::max(0, a_counts.total));
		Log(summary);
		if (a_counts.stopped)
			Log(L"Press Encode existing masters later to continue; finished tiles are kept.");
		if (a_counts.encoded + a_counts.skipped > 0) {
			openFolder = encode.output;
			EnableWindow(ui.open, TRUE);
		}
		MessageBoxW(ui.wnd, (summary + (a_counts.failed > 0 ? L"\r\n\r\nThe failed files are listed in the log." : L"")).c_str(), kTitle,
			a_counts.failed > 0 ? MB_ICONWARNING : MB_ICONINFORMATION);
	}

	void OnEncodeTimer()
	{
		for (const auto& failure : encode.encoder.TakeFailures())
			Log(L"Could not encode " + failure);
		const auto counts = encode.encoder.Counts();
		ShowEncodeProgress(counts);
		if (counts.finished)
			FinishEncode(counts);
	}

	// After a bake: the masters of this run (and any earlier ones not encoded yet) into the output mod.
	void EncodeAfterBake()
	{
		if (!IsBc7(run.format))
			return;
		EncodeJob job{ MastersDir(run.gameDir), TilesDir(run.output), run.format, 0 };
		PrepareColour(job, run.output, Recolour{});  // the re-colour sliders are for "Encode existing masters" only
		StartEncode(job, run.output);
	}

	// "Encode existing masters": the same encode without a bake, at full size or as a 512 px set in its own folder.
	void EncodeExistingMasters(int a_maxSide)
	{
		if (run.active || encode.active)
			return;
		const fs::path game = GetText(ui.gameEdit);
		if (!IsGameFolder(game)) {
			Log(L"Choose the game folder (the one with SkyrimSE.exe) first.");
			return;
		}
		const auto masters = MastersDir(game);
		if (!Exists(masters)) {
			Log(L"No master images in " + masters.wstring() + L". Bake with a BC7 tile format first. With MO2, start this app from MO2 so it sees the overwrite folder.");
			return;
		}
		const TileFormat format = FormatFromCombo();
		if (!IsBc7(format)) {
			Log(L"Pick one of the BC7 tile formats first; BC1 tiles come straight from the game.");
			return;
		}
		fs::path out = GetText(ui.outEdit);
		if (out.empty())
			out = DefaultOutput();
		if (a_maxSide > 0)
			out = HalfSetOutput(out);
		SaveAppSettings();
		EncodeJob job{ masters, TilesDir(out), format, a_maxSide };
		PrepareColour(job, out, RecolourFromTracks());
		StartEncode(job, out);
	}

	void ShowEncodeMenu()
	{
		const HMENU menu = CreatePopupMenu();
		AppendMenuW(menu, MF_STRING, IDM_ENCODE_FULL, L"Encode at the masters' own size");
		AppendMenuW(menu, MF_STRING, IDM_ENCODE_HALF, L"Encode a 512 px set (half size from 1024 masters, own folder)");
		RECT button{};
		GetWindowRect(ui.encode, &button);
		TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom, 0, ui.wnd, nullptr);
		DestroyMenu(menu);
	}

	bool Launch()
	{
		std::error_code ec;
		fs::remove(run.status, ec);  // a status left from the last start would point at a game that has gone
		run.game = nullptr;
		run.gamePid = 0;
		run.haveLast = false;
		run.finalSeen = 0;
		run.doneAtLaunch = run.bestDone;
		run.stuckClose = false;
		run.stuckCloseAt = 0;
		run.restartSeen = 0;
		GetSystemTimeAsFileTime(&run.launchTime);
		STARTUPINFOW        si{ sizeof(si) };
		PROCESS_INFORMATION pi{};
		std::wstring        cmd = L"\"" + run.loader.wstring() + L"\"";
		if (!CreateProcessW(run.loader.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, run.gameDir.c_str(), &si, &pi)) {
			Log(std::format(L"Could not start skse64_loader.exe (Windows error {}).", GetLastError()));
			return false;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		run.launchTick = GetTickCount64();
		run.progressTick = run.launchTick;
		Log(L"Starting the game through skse64_loader.exe. It opens on the main menu and the bake begins by itself; leave it alone.");
		return true;
	}

	// Cells the plugin could not photograph even in its retry pass: the count, and its list copied next to the output.
	// Returns the summary line, or nothing when every cell was photographed.
	std::wstring ReportMissingCells()
	{
		const auto  list = PluginsDir(run.gameDir) / "ModernMinimapMissing.txt";
		std::string text;
		const bool  fresh = FileWriteTime(list) > FileTimeValue(run.start) && ReadTextFile(list, text);  // an older list belongs to an earlier bake
		const auto  keys = fresh ? MissingCellKeys(text) : std::vector<std::string>{};
		const int   count = keys.empty() ? (run.haveLast ? run.last.failedCells : 0) : static_cast<int>(keys.size());
		if (count <= 0)
			return {};
		std::wstring summary;
		if (keys.empty()) {
			summary = std::format(L"{} cells could not be photographed (the game wrote no list of them).", count);
		} else {
			std::string lines;
			for (const auto& key : keys)
				lines += key + "\r\n";
			const auto copy = MissingListOutput(run.output);
			summary = WriteTextFile(copy, lines)
			              ? std::format(L"{} cells could not be photographed; their list is in {}.", count, copy.wstring())
			              : std::format(L"{} cells could not be photographed; their list is in {} (it could not be copied next to the output).", count, list.wstring());
		}
		Log(summary);
		return summary;
	}

	void Finish(Outcome a_outcome, const std::wstring& a_why = {})
	{
		KillTimer(ui.wnd, kTimer);
		if (run.game) {
			CloseHandle(run.game);
			run.game = nullptr;
		}
		run.active = false;

		const auto dest = TilesDir(run.output);
		const auto moved = MoveNewTiles(run.tiles, dest, run.start);
		const int  got = moved.moved + moved.copiedOnly;
		if (got > 0)
			Log(std::format(L"{} new photo tiles copied to {}.", got, dest.wstring()));
		if (moved.copiedOnly > 0)
			Log(std::format(L"{} of them could not be removed from the game's Data\\textures\\ModernMinimap\\tamriel folder. With MO2 they are in the overwrite "
							L"folder: clear it (right-click Overwrite, Clear) so they do not override your new mod.",
				moved.copiedOnly));
		if (moved.failed > 0)
			Log(std::format(L"{} photo tiles could not be copied; they are still in the game's Data\\textures\\ModernMinimap\\tamriel folder.", moved.failed));

		ResetSwitches();
		SetRunningUi(false);

		std::wstring done;
		if (got > 0) {
			openFolder = run.output;
			EnableWindow(ui.open, TRUE);
			done = L"Photos written to " + run.output.wstring() + L" - install that folder as a mod below Modern Minimap Framework.";
			Log(done);
		}
		if (const auto missing = ReportMissingCells(); !missing.empty())
			done += (done.empty() ? L"" : L"\r\n\r\n") + missing;
		// The BC1 tiles copied above stay as a fallback; each one is replaced by its BC7 tile once that is encoded.
		EncodeAfterBake();
		if (encode.active)
			done += (done.empty() ? L"" : L"\r\n\r\n") + std::wstring(L"BC7 tiles are being encoded now; the progress shows in the window.");
		switch (a_outcome) {
		case Outcome::kDone:
			SetWindowTextW(ui.phase, L"Now: finished");
			if (got == 0 && !encode.active)
				done = L"No new photos were needed: every photo was already taken by an earlier bake. To bake everything again, delete Data\\SKSE\\Plugins\\ModernMinimapBake.txt "
				       L"(with MO2: in overwrite) and press Start." +
				       (done.empty() ? std::wstring{} : L"\r\n\r\n" + done);  // here done holds only the missing cells
			Log(L"Bake finished.");
			MessageBoxW(ui.wnd, (L"Bake finished.\r\n\r\n" + done).c_str(), kTitle, MB_ICONINFORMATION);
			break;
		case Outcome::kCancelled:
			SetWindowTextW(ui.phase, L"Now: cancelled");
			Log(L"Bake cancelled. Press Start again later to continue where it stopped.");
			MessageBoxW(ui.wnd, (L"Bake cancelled. Press Start again later to continue where it stopped.\r\n\r\n" + done).c_str(), kTitle, MB_ICONINFORMATION);
			break;
		case Outcome::kError:
			SetWindowTextW(ui.phase, L"Now: stopped");
			Log(L"The bake stopped: " + a_why);
			MessageBoxW(ui.wnd, (L"The bake stopped:\r\n" + a_why + L"\r\n\r\n" + done).c_str(), kTitle, MB_ICONWARNING);
			break;
		case Outcome::kFailed:
			SetWindowTextW(ui.phase, L"Now: stopped");
			MessageBoxW(ui.wnd, (a_why + L"\r\n\r\n" + kCrashAdvice + (done.empty() ? L"" : L"\r\n\r\n" + done)).c_str(), kTitle, MB_ICONWARNING);
			break;
		}
	}

	void ShowStatus(const BakeStatus& a_s)
	{
		if (a_s.found > 0) {
			SendMessageW(ui.progress, PBM_SETRANGE32, 0, a_s.found);
			SendMessageW(ui.progress, PBM_SETPOS, static_cast<WPARAM>(std::clamp(a_s.done, 0, a_s.found)), 0);
			SetWindowTextW(ui.count, std::format(L"Photos: {} of {}", a_s.done, a_s.found).c_str());
		}
		if (a_s.etaSeconds > 0)
			SetWindowTextW(ui.eta, (L"Time left: " + FormatEta(a_s.etaSeconds)).c_str());
		else
			SetWindowTextW(ui.eta, a_s.state == "running" ? L"Time left: known after the first photos" : L"");
		SetWindowTextW(ui.phase, (L"Now: " + Widen(a_s.phase)).c_str());

		if (!run.haveLast || run.last.state != a_s.state) {
			if (a_s.state == "running")
				Log(L"The bake is running. You can leave the computer; the game quits by itself when it is done.");
			else if (a_s.state == "writing")
				Log(L"Writing the last photo tiles...");
			else if (a_s.state == "restart")
				Log(L"Planned restart: the game quits so it can start fresh; the bake continues after it.");
		}
		// One log line per stage, not per photo (those show in the labels above).
		if (a_s.phase != run.lastLoggedPhase && !a_s.phase.empty() && a_s.phase.rfind("settling at", 0) != 0) {
			run.lastLoggedPhase = a_s.phase;
			Log(Widen(a_s.phase));
		}
		if (a_s.message != run.lastLoggedMessage && !a_s.message.empty()) {
			run.lastLoggedMessage = a_s.message;
			Log(L"Game: " + Widen(a_s.message));
		}
		run.last = a_s;
		run.haveLast = true;
	}

	// Progress for the crash budget (done count) and for the stall watchdog (done count or the retry queue moving).
	void NoteProgress(const BakeStatus& a_s, ULONGLONG a_now)
	{
		if (a_s.done > run.bestDone) {
			run.bestDone = a_s.done;
			run.progressTick = a_now;
		}
		if (a_s.failedCells != run.lastFailedCells) {
			run.lastFailedCells = a_s.failedCells;
			run.progressTick = a_now;
		}
	}

	void LogGameExit(GameExit a_exit)
	{
		if (a_exit == GameExit::kPlannedRestart) {
			Log(L"The game quit for its planned restart.");
			return;
		}
		if (a_exit == GameExit::kStuck) {
			Log(L"The stuck game has closed.");
			return;
		}
		if (run.crashAdviceShown) {
			Log(L"The game closed early again.");
			return;
		}
		run.crashAdviceShown = true;
		Log(kCrashAdvice);
	}

	// After the game has gone without finishing: start it again in a moment, or stop when the restarts are used up.
	void RestartAfterExit(GameExit a_exit, ULONGLONG a_now)
	{
		LogGameExit(a_exit);
		const auto decision = DecideRestart(a_exit, run.restartsLeft, run.restartsAllowed, run.doneAtLaunch, run.bestDone);
		run.restartsLeft = decision.restartsLeft;
		if (!decision.relaunch) {
			Log(L"No automatic restarts left.");
			Finish(Outcome::kFailed, a_exit == GameExit::kStuck ? L"The game kept getting stuck without taking a photo and every automatic restart is used up."
			                                                     : L"The game closed early and every automatic restart is used up.");
			return;
		}
		if (decision.budgetRefilled)
			Log(std::format(L"Photos were taken since the last start, so all {} automatic restarts are available again.", run.restartsAllowed));
		if (decision.usedBudget)
			Log(std::format(L"Starting it again in 10 seconds ({} automatic restart{} in a row left after this one).", run.restartsLeft, run.restartsLeft == 1 ? L"" : L"s"));
		else
			Log(L"Starting it again in 10 seconds (planned restarts do not use up the automatic restarts).");
		run.relaunchAt = a_now + kRelaunchDelayMs;
		SetWindowTextW(ui.phase, L"Now: waiting to start the game again");
	}

	// Status "restart": wait for the game to quit (ending it if it hangs on the way out), then start it again.
	void FollowPlannedRestart(bool a_exited, ULONGLONG a_now)
	{
		if (!run.restartSeen)
			run.restartSeen = a_now;
		if (!a_exited) {
			if (a_now - run.restartSeen <= kQuitTimeoutMs || !run.gamePid)
				return;
			Log(L"The game did not quit within two minutes for its planned restart; ending it.");
			EndProcess(run.gamePid);
			run.restartSeen = a_now;
			return;
		}
		if (run.game) {
			CloseHandle(run.game);
			run.game = nullptr;
		}
		if (run.cancelRequested) {
			Finish(Outcome::kCancelled);
			return;
		}
		RestartAfterExit(GameExit::kPlannedRestart, a_now);
	}

	// Stall watchdog: the game runs but no photo for the set minutes. Ask it to close, end it if it does not.
	void WatchForStall(const std::string& a_state, ULONGLONG a_now)
	{
		if (run.stuckClose) {
			if (!run.stuckCloseAt || a_now < run.stuckCloseAt)
				return;
			run.stuckCloseAt = 0;
			Log(L"Watchdog: the game did not close by itself; ending its process.");
			if (!EndProcess(run.gamePid))
				Finish(Outcome::kFailed, L"The game got stuck and could not be closed. Close it with Task Manager, then press Start again.");
			return;
		}
		if (!run.gamePid || run.cancelRequested || !IsStalled(a_state, a_now, run.progressTick, run.launchTick, run.stallLimitMs))
			return;
		run.stuckClose = true;
		run.stuckCloseAt = a_now + kStuckCloseGraceMs;
		++run.stuckRestarts;
		Log(std::format(L"Watchdog restart {}: no new photo for {} minutes while the game is running; closing the game to start it again.", run.stuckRestarts,
			run.stallLimitMs / 60'000));
		SetWindowTextW(ui.phase, L"Now: closing the stuck game");
		AskProcessToClose(run.gamePid);
	}

	void OnTimer()
	{
		if (!run.active)
			return;
		const ULONGLONG now = GetTickCount64();
		if (run.relaunchAt) {
			if (now >= run.relaunchAt) {
				run.relaunchAt = 0;
				if (!Launch())
					Finish(Outcome::kFailed, L"The game could not be started again.");
			}
			return;
		}

		BakeStatus s;
		const bool have = ReadStatus(run.status, s);
		if (have) {
			ShowStatus(s);
			NoteProgress(s, now);
		}

		// Which process is the game: the pid the plugin reports, or (before the plugin has written anything)
		// the SkyrimSE.exe that skse64_loader started.
		bool gone = false;
		if (!run.game) {
			const DWORD pid = have && s.pid ? s.pid : FindProcess(L"SkyrimSE.exe", &run.launchTime);
			if (pid)
				run.game = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (run.game) {
				run.gamePid = pid;
				Log(std::format(L"Game running (process {}).", pid));
			} else if (now - run.launchTick > kStartTimeoutMs) {
				Log(L"The game did not start within two minutes.");
				gone = true;
			}
		}
		const bool exited = gone || (run.game && WaitForSingleObject(run.game, 0) == WAIT_OBJECT_0);

		const std::string state = run.haveLast ? run.last.state : "";
		if (state == "done" || state == "cancelled" || state == "error") {
			// Tiles are on disk; the game is quitting. Move the tiles once it has gone (or it takes too long).
			if (!run.finalSeen) {
				run.finalSeen = now;
				Log(L"The game is quitting...");
			}
			if (exited || !run.game || now - run.finalSeen > kQuitTimeoutMs)
				Finish(state == "done" ? Outcome::kDone : state == "cancelled" ? Outcome::kCancelled : Outcome::kError, Widen(run.last.message));
			return;
		}
		if (state == "restart") {
			FollowPlannedRestart(exited, now);
			return;
		}
		if (!exited) {
			WatchForStall(state, now);
			return;
		}

		if (run.game) {
			CloseHandle(run.game);
			run.game = nullptr;
		}
		if (run.cancelRequested) {
			Log(L"The game closed after Cancel.");
			Finish(Outcome::kCancelled);
			return;
		}
		RestartAfterExit(run.stuckClose ? GameExit::kStuck : GameExit::kCrash, now);
	}

	void StartRun()
	{
		if (run.active || encode.active)
			return;
		const fs::path game = GetText(ui.gameEdit);
		if (!IsGameFolder(game)) {
			Log(L"Choose the game folder (the one with SkyrimSE.exe) first.");
			return;
		}
		const auto loader = game / "skse64_loader.exe";
		if (!Exists(loader)) {
			Log(L"skse64_loader.exe was not found in " + game.wstring() + L". Install SKSE, then press Start again.");
			return;
		}
		const auto plugins = PluginsDir(game);
		if (!Exists(plugins / "ModernMinimap.dll")) {
			Log(L"Modern Minimap was not found in " + plugins.wstring() +
				L". Install and enable it first. With MO2, add this app to MO2's executables list and start it from there, so it sees MO2's virtual Data folder.");
			return;
		}
		if (FindProcess(L"SkyrimSE.exe", nullptr)) {
			Log(L"Skyrim is already running. Close it, then press Start again.");
			return;
		}
		fs::path out = GetText(ui.outEdit);
		if (out.empty()) {
			out = DefaultOutput();
			SetWindowTextW(ui.outEdit, out.c_str());
		}
		std::error_code ec;
		fs::create_directories(out, ec);
		if (!Exists(out)) {
			Log(L"Could not create the output folder " + out.wstring() + L".");
			return;
		}

		if (MessageBoxW(ui.wnd, kPreBakeNotice, kTitle, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
			return;

		BakeChoices choices;
		choices.area = AreaFromTrack();
		choices.seconds = std::clamp(SpinValue(ui.settleSpin), 2, 30);
		choices.hideNpcs = Button_GetCheck(ui.hideNpcs) == BST_CHECKED;
		choices.keepExisting = Button_GetCheck(ui.keep) == BST_CHECKED;
		choices.grid = GridFromTrack();
		choices.fullMap = FullMapFromCombo();
		choices.tileSize = TileSizeFromCombo();
		choices.format = FormatFromCombo();
		const int restarts = std::clamp(SpinValue(ui.restartsSpin), 0, kMaxRestarts);
		const int stallMinutes = std::clamp(SpinValue(ui.stallSpin), 0, kMaxStallMinutes);

		run = {};
		run.gameDir = game;
		run.loader = loader;
		run.ini = plugins / "ModernMinimap.ini";
		run.status = plugins / "ModernMinimapBakeStatus.json";
		run.cancel = plugins / "ModernMinimapBake.cancel";
		run.tiles = game / "Data" / "textures" / "ModernMinimap" / "tamriel";
		run.output = out;
		run.restartsLeft = restarts;
		run.restartsAllowed = restarts;
		run.stallLimitMs = static_cast<ULONGLONG>(stallMinutes) * 60'000;
		run.format = choices.format;

		std::vector<std::pair<std::string, std::string>> values{ { "bStartOnLoad", "true" }, { "bEnterFromMainMenu", "true" }, { "bQuitWhenDone", "true" } };
		const auto                                       bakeSwitches = BakeSwitches(choices);
		values.insert(values.end(), bakeSwitches.begin(), bakeSwitches.end());
		std::wstring error;
		if (!SetIniValues(run.ini, "Bake", values, error)) {
			Log(error);
			return;
		}
		run.switchesOn = true;
		fs::remove(run.cancel, ec);
		GetSystemTimeAsFileTime(&run.start);
		run.active = true;
		SaveAppSettings();
		SetRunningUi(true);
		Log(std::format(L"Bake settings written to {} (area {:.2f}x, {}x{} photo grid, {} s per photo, NPCs {}, existing photos {}, {}, {} px tiles, {}).", run.ini.wstring(),
			choices.area, choices.grid, choices.grid, choices.seconds, choices.hideNpcs ? L"hidden" : L"shown", choices.keepExisting ? L"kept" : L"replaced",
			choices.fullMap ? L"whole map" : L"towns and places", choices.tileSize, IsBc7(choices.format) ? L"BC7 from master images" : L"BC1"));
		Log(std::format(L"Up to {} automatic restarts in a row without a new photo; {}.", restarts,
			stallMinutes > 0 ? std::format(L"a game with no new photo for {} minutes is restarted", stallMinutes) : std::wstring(L"stuck game watchdog off")));
		if (!Launch()) {
			Finish(Outcome::kFailed, L"The game could not be started.");
			return;
		}
		SetTimer(ui.wnd, kTimer, 1000, nullptr);
	}

	void CancelRun()
	{
		if (encode.active) {
			EnableWindow(ui.cancel, FALSE);
			Log(L"Stopping the encode after the current tile...");
			encode.encoder.RequestStop();  // the encode timer reports the result once the worker has stopped
			return;
		}
		if (!run.active || run.cancelRequested)
			return;
		run.cancelRequested = true;
		EnableWindow(ui.cancel, FALSE);
		if (run.relaunchAt) {  // between a crash and the restart: nothing is running
			run.relaunchAt = 0;
			Finish(Outcome::kCancelled);
			return;
		}
		std::ofstream(run.cancel) << "cancel\n";
		Log(L"Cancel requested. The game writes the photos taken so far and quits.");
	}

	// ---------------------------------------------------------------- window

	HWND Make(const wchar_t* a_class, const wchar_t* a_text, DWORD a_style, int a_id = 0, DWORD a_ex = 0)
	{
		return CreateWindowExW(a_ex, a_class, a_text, WS_CHILD | WS_VISIBLE | a_style, 0, 0, 0, 0, ui.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(a_id)), instance,
			nullptr);
	}

	void ApplyFont()
	{
		if (ui.font)
			DeleteObject(ui.font);
		NONCLIENTMETRICSW ncm{ sizeof(ncm) };
		SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
		ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
		EnumChildWindows(
			ui.wnd, [](HWND a_h, LPARAM a_font) {
				SendMessageW(a_h, WM_SETFONT, static_cast<WPARAM>(a_font), TRUE);
				return TRUE;
			},
			reinterpret_cast<LPARAM>(ui.font));
	}

	void Layout()
	{
		RECT rc{};
		GetClientRect(ui.wnd, &rc);
		const auto S = [](int a_v) { return MulDiv(a_v, static_cast<int>(ui.dpi), 96); };
		const int  W = MulDiv(rc.right, 96, static_cast<int>(ui.dpi));  // layout in 96-dpi units, scaled on placement
		const int  H = MulDiv(rc.bottom, 96, static_cast<int>(ui.dpi));
		const auto put = [&](HWND a_h, int a_x, int a_y, int a_w, int a_hh) { MoveWindow(a_h, S(a_x), S(a_y), S(std::max(a_w, 10)), S(a_hh), FALSE); };
		const int  m = 10, inner = W - 2 * m, editW = inner - 110 - 12 - 92;
		int        y = 8;

		put(ui.gameGroup, m, y, inner, 96);
		put(ui.gameLabel, m + 12, y + 28, 95, 20);
		put(ui.gameEdit, m + 110, y + 24, editW, 24);
		put(ui.gameBrowse, W - m - 12 - 84, y + 23, 84, 26);
		put(ui.gameInfo, m + 12, y + 55, inner - 24, 36);  // two lines: the MO2 hint is long
		y += 104;

		put(ui.adviceGroup, m, y, inner, 98);
		put(ui.advice, m + 12, y + 22, inner - 24, 70);
		y += 106;

		put(ui.setGroup, m, y, inner, 262);
		put(ui.areaLabel, m + 12, y + 28, 125, 20);
		put(ui.areaTrack, m + 140, y + 22, 220, 30);
		put(ui.areaValue, m + 366, y + 28, 60, 20);
		put(ui.hideNpcs, m + 440, y + 26, inner - 452, 22);
		put(ui.settleLabel, m + 12, y + 62, 125, 20);
		put(ui.settleEdit, m + 140, y + 58, 64, 24);
		put(ui.restartsLabel, m + 230, y + 62, 150, 20);
		put(ui.restartsEdit, m + 384, y + 58, 64, 24);
		put(ui.stallLabel, m + 462, y + 62, 148, 20);
		put(ui.stallEdit, m + 614, y + 58, 56, 24);
		put(ui.keep, m + 12, y + 90, inner - 24, 22);
		put(ui.gridLabel, m + 12, y + 122, 125, 20);
		put(ui.gridTrack, m + 140, y + 116, 220, 30);
		put(ui.gridValue, m + 366, y + 122, 110, 20);
		put(ui.gridLowLabel, m + 140, y + 146, 110, 20);  // under the slider's ends
		put(ui.gridHighLabel, m + 250, y + 146, 110, 20);
		put(ui.gridHint, m + 366, y + 146, inner - 378, 20);
		put(ui.mapAreaLabel, m + 12, y + 178, 125, 20);
		put(ui.mapAreaCombo, m + 140, y + 174, 150, 200);  // height includes the drop-down list
		put(ui.tileSizeLabel, m + 310, y + 178, 70, 20);
		put(ui.tileSizeCombo, m + 384, y + 174, 90, 200);
		put(ui.fullMapNote, m + 140, y + 204, inner - 152, 20);
		put(ui.formatLabel, m + 12, y + 234, 125, 20);
		put(ui.formatCombo, m + 140, y + 230, 300, 200);
		put(ui.formatHint, m + 446, y + 234, inner - 458, 20);
		SendMessageW(ui.formatCombo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(S(380)), 0);  // the list shows the full notes
		y += 270;

		put(ui.outGroup, m, y, inner, 120);
		put(ui.outLabel, m + 12, y + 28, 95, 20);
		put(ui.outEdit, m + 110, y + 24, editW, 24);
		put(ui.outBrowse, W - m - 12 - 84, y + 23, 84, 26);
		put(ui.brightLabel, m + 12, y + 62, 95, 20);
		put(ui.brightTrack, m + 110, y + 56, 170, 30);
		put(ui.brightValue, m + 284, y + 62, 50, 20);
		put(ui.satLabel, m + 344, y + 62, 80, 20);
		put(ui.satTrack, m + 426, y + 56, 170, 30);
		put(ui.satValue, m + 600, y + 62, 50, 20);
		put(ui.recolourHint, m + 12, y + 92, inner - 24, 20);
		y += 130;

		put(ui.prebakeNote, m, y, inner, 64);
		y += 70;
		put(ui.start, m, y, 100, 30);
		put(ui.cancel, m + 108, y, 100, 30);
		put(ui.open, m + 216, y, 120, 30);
		put(ui.encode, m + 344, y, 180, 30);
		y += 40;
		put(ui.progress, m, y, inner, 20);
		y += 26;
		put(ui.count, m, y, inner / 2, 20);
		put(ui.eta, m + inner / 2, y, inner / 2, 20);
		y += 22;
		put(ui.phase, m, y, inner, 20);
		y += 26;
		put(ui.log, m, y, inner, std::max(60, H - y - m));

		// The spin boxes sit on their edit boxes: attach them again after the edits moved.
		SendMessageW(ui.settleSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(ui.settleEdit), 0);
		SendMessageW(ui.restartsSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(ui.restartsEdit), 0);
		SendMessageW(ui.stallSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(ui.stallEdit), 0);
		InvalidateRect(ui.wnd, nullptr, TRUE);
	}

	void CreateControls()
	{
		ui.gameGroup = Make(WC_BUTTONW, L"Game", BS_GROUPBOX);
		ui.gameLabel = Make(WC_STATICW, L"Game folder:", SS_LEFT);
		ui.gameEdit = Make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_GAME_EDIT, WS_EX_CLIENTEDGE);
		ui.gameBrowse = Make(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, IDC_GAME_BROWSE);
		ui.gameInfo = Make(WC_STATICW, L"", SS_LEFT, IDC_GAME_INFO);

		ui.adviceGroup = Make(WC_BUTTONW, L"Before you start", BS_GROUPBOX);
		ui.advice = Make(WC_STATICW, L"", SS_LEFT, IDC_ADVICE);

		ui.setGroup = Make(WC_BUTTONW, L"Settings", BS_GROUPBOX);
		ui.areaLabel = Make(WC_STATICW, L"Photo area:", SS_LEFT);
		ui.areaTrack = Make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_AREA_TRACK);
		SendMessageW(ui.areaTrack, TBM_SETRANGE, TRUE, MAKELPARAM(2, 16));  // quarter steps: 0.5x to 4.0x
		SendMessageW(ui.areaTrack, TBM_SETTICFREQ, 2, 0);
		SendMessageW(ui.areaTrack, TBM_SETPAGESIZE, 0, 2);
		ui.areaValue = Make(WC_STATICW, L"1.00x", SS_LEFT, IDC_AREA_VALUE);
		ui.hideNpcs = Make(WC_BUTTONW, L"Hide NPCs (fewer crashes)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_HIDE_NPCS);
		ui.settleLabel = Make(WC_STATICW, L"Seconds per photo:", SS_LEFT);
		ui.settleEdit = Make(WC_EDITW, L"7", WS_TABSTOP | ES_NUMBER, IDC_SETTLE_EDIT, WS_EX_CLIENTEDGE);
		ui.settleSpin = Make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, IDC_SETTLE_SPIN);
		SendMessageW(ui.settleSpin, UDM_SETRANGE32, 2, 30);
		ui.restartsLabel = Make(WC_STATICW, L"Max restarts in a row:", SS_LEFT);
		ui.restartsEdit = Make(WC_EDITW, std::to_wstring(kDefaultRestarts).c_str(), WS_TABSTOP | ES_NUMBER, IDC_RESTARTS_EDIT, WS_EX_CLIENTEDGE);
		ui.restartsSpin = Make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, IDC_RESTARTS_SPIN);
		SendMessageW(ui.restartsSpin, UDM_SETRANGE32, 0, kMaxRestarts);
		ui.stallLabel = Make(WC_STATICW, L"Restart if stuck (min):", SS_LEFT);
		ui.stallEdit = Make(WC_EDITW, std::to_wstring(kDefaultStallMinutes).c_str(), WS_TABSTOP | ES_NUMBER, IDC_STALL_EDIT, WS_EX_CLIENTEDGE);
		ui.stallSpin = Make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, IDC_STALL_SPIN);
		SendMessageW(ui.stallSpin, UDM_SETRANGE32, 0, kMaxStallMinutes);
		ui.keep = Make(WC_BUTTONW, L"Keep existing photos (only fill places that have none yet)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_KEEP);
		ui.gridLabel = Make(WC_STATICW, L"Photo grid:", SS_LEFT);
		ui.gridTrack = Make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_GRID_TRACK);
		SendMessageW(ui.gridTrack, TBM_SETRANGE, TRUE, MAKELPARAM(kQualityMin, kQualityMax));
		SendMessageW(ui.gridTrack, TBM_SETPAGESIZE, 0, 1);
		ui.gridValue = Make(WC_STATICW, L"", SS_LEFT, IDC_GRID_VALUE);
		SetGridTrack(kDefaultGrid);
		ui.gridLowLabel = Make(WC_STATICW, L"Lower map quality", SS_LEFT);
		ui.gridHighLabel = Make(WC_STATICW, L"Higher map quality", SS_RIGHT);
		ui.gridHint = Make(WC_STATICW, L"Cells per photo: smaller is sharper but slower.", SS_LEFT | SS_ENDELLIPSIS);

		ui.mapAreaLabel = Make(WC_STATICW, L"Area:", SS_LEFT);
		ui.mapAreaCombo = Make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_MAP_AREA);
		for (const auto& choice : kMapAreas)
			SendMessageW(ui.mapAreaCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
		SendMessageW(ui.mapAreaCombo, CB_SETCURSEL, 0, 0);
		ui.tileSizeLabel = Make(WC_STATICW, L"Tile size:", SS_LEFT);
		ui.tileSizeCombo = Make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_TILE_SIZE);
		for (const int size : kTileSizes)
			SendMessageW(ui.tileSizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(size).c_str()));
		SendMessageW(ui.tileSizeCombo, CB_SETCURSEL, 0, 0);
		ui.fullMapNote = Make(WC_STATICW, kFullMapNote, SS_LEFT | SS_ENDELLIPSIS, IDC_FULL_MAP_NOTE);
		ShowWindow(ui.fullMapNote, SW_HIDE);
		ui.formatLabel = Make(WC_STATICW, L"Tile format:", SS_LEFT);
		ui.formatCombo = Make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_TILE_FORMAT);
		for (const auto& choice : kTileFormats)
			SendMessageW(ui.formatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
		SendMessageW(ui.formatCombo, CB_SETCURSEL, static_cast<WPARAM>(TileFormatIndex(kDefaultTileFormat)), 0);
		ui.formatHint = Make(WC_STATICW, L"(top to bottom: longer encoding time)", SS_LEFT | SS_ENDELLIPSIS);

		ui.outGroup = Make(WC_BUTTONW, L"Output", BS_GROUPBOX);
		ui.outLabel = Make(WC_STATICW, L"Output folder:", SS_LEFT);
		ui.outEdit = Make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_OUT_EDIT, WS_EX_CLIENTEDGE);
		ui.outBrowse = Make(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, IDC_OUT_BROWSE);
		ui.brightLabel = Make(WC_STATICW, L"Brightness:", SS_LEFT);
		ui.brightTrack = Make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_BRIGHT_TRACK);
		ui.brightValue = Make(WC_STATICW, L"1.00x", SS_LEFT, IDC_BRIGHT_VALUE);
		ui.satLabel = Make(WC_STATICW, L"Saturation:", SS_LEFT);
		ui.satTrack = Make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, IDC_SAT_TRACK);
		ui.satValue = Make(WC_STATICW, L"1.00x", SS_LEFT, IDC_SAT_VALUE);
		for (HWND track : { ui.brightTrack, ui.satTrack }) {
			SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(kRecolourMinPercent, kRecolourMaxPercent));
			SendMessageW(track, TBM_SETTICFREQ, 5, 0);
			SendMessageW(track, TBM_SETPAGESIZE, 0, 5);
			SendMessageW(track, TBM_SETPOS, TRUE, kRecolourNeutralPercent);
		}
		ui.recolourHint = Make(WC_STATICW, L"Brightness and saturation apply to \"Encode existing masters\" only; 1.00x keeps the baked colours.", SS_LEFT | SS_ENDELLIPSIS);

		ui.prebakeNote = Make(WC_STATICW, kPreBakeNotice, SS_LEFT, IDC_PREBAKE_NOTE);
		ui.start = Make(WC_BUTTONW, L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_START);
		ui.cancel = Make(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_CANCEL);
		ui.open = Make(WC_BUTTONW, L"Open folder", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_OPEN);
		ui.encode = Make(WC_BUTTONW, L"Encode existing masters...", WS_TABSTOP | BS_PUSHBUTTON, IDC_ENCODE);
		ui.progress = Make(PROGRESS_CLASSW, L"", 0, IDC_PROGRESS);
		ui.count = Make(WC_STATICW, L"Photos: -", SS_LEFT, IDC_COUNT);
		ui.eta = Make(WC_STATICW, L"", SS_LEFT, IDC_ETA);
		ui.phase = Make(WC_STATICW, L"Now: idle", SS_LEFT | SS_ENDELLIPSIS, IDC_PHASE);
		ui.log = Make(WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
		SendMessageW(ui.log, EM_SETLIMITTEXT, 0, 0);
	}

	void Init()
	{
		CreateControls();
		ApplyFont();
		LoadAppSettings();

		// Under MO2 the detected folder wins: a remembered one may belong to another instance.
		std::wstring how;
		fs::path     game;
		const auto   saved = ReadAppValue(L"GameFolder", L"");
		if (!StartedFromMo2() && IsGameFolder(saved)) {
			game = saved;
			how = L"remembered from last time";
		} else {
			game = FindGameFolder(how);
		}
		SetWindowTextW(ui.gameEdit, game.c_str());  // EN_CHANGE runs the folder check and the advice
		RefreshGameInfo();
		Log(game.empty() ? L"Game folder not found: press Browse and choose the folder that holds SkyrimSE.exe." : L"Game folder " + how + L": " + game.wstring());
		if (StartedFromMo2())
			Log(L"Started from Mod Organizer 2: the game is started with your current MO2 profile.");
		Log(L"Press Start. The game starts, enters the world from the main menu and photographs every town; it quits by itself when it is done.");
		Layout();
	}

	bool ConfirmClose()
	{
		if (encode.active) {
			if (MessageBoxW(ui.wnd, L"BC7 tiles are being encoded. Close anyway?\r\n\r\nFinished tiles are kept; press Encode existing masters later to continue.", kTitle,
					MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
				return false;
			encode.encoder.Stop();
			return true;
		}
		if (!run.active)
			return true;
		if (MessageBoxW(ui.wnd,
				L"A bake is running. Close anyway?\r\n\r\nThe game stops the bake and quits. Photos taken so far stay in the game's Data\\textures\\ModernMinimap\\tamriel "
				L"folder (with MO2: in overwrite); press Start again later to continue.",
				kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
			return false;
		if (!run.relaunchAt && !run.cancelRequested)
			std::ofstream(run.cancel) << "cancel\n";
		return true;
	}

	LRESULT CALLBACK WndProc(HWND a_wnd, UINT a_msg, WPARAM a_wp, LPARAM a_lp)
	{
		switch (a_msg) {
		case WM_CREATE:
			ui.wnd = a_wnd;
			ui.dpi = GetDpiForWindow(a_wnd);
			Init();
			return 0;
		case WM_SIZE:
			Layout();
			return 0;
		case WM_GETMINMAXINFO:
			{
				auto* mm = reinterpret_cast<MINMAXINFO*>(a_lp);
				const UINT dpi = ui.wnd ? ui.dpi : GetDpiForWindow(a_wnd);
				mm->ptMinTrackSize = { MulDiv(700, static_cast<int>(dpi), 96), MulDiv(940, static_cast<int>(dpi), 96) };
				return 0;
			}
		case WM_DPICHANGED:
			{
				ui.dpi = HIWORD(a_wp);
				ApplyFont();
				const auto* r = reinterpret_cast<const RECT*>(a_lp);
				SetWindowPos(a_wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
				Layout();
				return 0;
			}
		case WM_HSCROLL:
			if (reinterpret_cast<HWND>(a_lp) == ui.areaTrack)
				UpdateAreaLabel();
			else if (reinterpret_cast<HWND>(a_lp) == ui.gridTrack)
				UpdateGridLabel();
			else if (reinterpret_cast<HWND>(a_lp) == ui.brightTrack || reinterpret_cast<HWND>(a_lp) == ui.satTrack)
				UpdateRecolourLabels();
			return 0;
		case WM_TIMER:
			if (a_wp == kTimer)
				OnTimer();
			else if (a_wp == kEncodeTimer)
				OnEncodeTimer();
			return 0;
		case WM_COMMAND:
			switch (LOWORD(a_wp)) {
			case IDC_GAME_EDIT:
				if (HIWORD(a_wp) == EN_CHANGE)
					RefreshGameInfo();
				break;
			case IDC_GAME_BROWSE:
				if (const auto p = PickFolder(GetText(ui.gameEdit)); !p.empty())
					SetWindowTextW(ui.gameEdit, p.c_str());
				break;
			case IDC_OUT_BROWSE:
				if (const auto p = PickFolder(GetText(ui.outEdit)); !p.empty())
					SetWindowTextW(ui.outEdit, p.c_str());
				break;
			case IDC_START:
			case IDOK:  // Enter
				StartRun();
				break;
			case IDC_CANCEL:
				CancelRun();
				break;
			case IDC_OPEN:
				if (!openFolder.empty())
					ShellExecuteW(a_wnd, L"open", openFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				break;
			case IDC_MAP_AREA:
				if (HIWORD(a_wp) == CBN_SELCHANGE)
					UpdateFullMapNote();
				break;
			case IDC_ENCODE:
				ShowEncodeMenu();
				break;
			case IDM_ENCODE_FULL:
				EncodeExistingMasters(0);
				break;
			case IDM_ENCODE_HALF:
				EncodeExistingMasters(kHalfSetSide);
				break;
			}
			return 0;
		case WM_CLOSE:
			if (ConfirmClose()) {
				SaveAppSettings();
				ResetSwitches();  // never leave the game set to bake on its next start
				DestroyWindow(a_wnd);
			}
			return 0;
		case WM_ENDSESSION:
			if (a_wp)
				ResetSwitches();
			return 0;
		case WM_DESTROY:
			ResetSwitches();
			if (ui.font)
				DeleteObject(ui.font);
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(a_wnd, a_msg, a_wp, a_lp);
	}
}

int WINAPI wWinMain(HINSTANCE a_instance, HINSTANCE, PWSTR, int a_show)
{
	instance = a_instance;
	// The manifest asks for per-monitor DPI awareness; this covers a missing or overridden manifest.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS };
	InitCommonControlsEx(&icc);

	WNDCLASSEXW wc{ sizeof(wc) };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = a_instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
	wc.lpszClassName = kClass;
	wc.hIcon = LoadIconW(a_instance, MAKEINTRESOURCEW(1));
	if (!wc.hIcon)
		wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);

	// Size for the monitor the window opens on.
	const UINT  dpi = GetDpiForSystem();
	RECT        r{ 0, 0, MulDiv(700, static_cast<int>(dpi), 96), MulDiv(1010, static_cast<int>(dpi), 96) };
	const DWORD style = WS_OVERLAPPEDWINDOW;
	AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dpi);
	HWND wnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, a_instance, nullptr);
	if (!wnd)
		return 1;
	ShowWindow(wnd, a_show);
	UpdateWindow(wnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (IsDialogMessageW(wnd, &msg))  // Tab between controls, Enter for Start
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	CoUninitialize();
	return static_cast<int>(msg.wParam);
}
