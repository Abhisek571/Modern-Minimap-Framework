// Modern Minimap Photo Baker: a small window that runs the town photo bake built into the Modern Minimap plugin
// without the player touching the game. It writes the plugin's [Bake] switches, starts the game through SKSE,
// follows the plugin's status file, starts the game again after a crash (the bake resumes from its progress
// file) and finally moves the new photo tiles into a folder the player installs as a mod.

#include "BakerFiles.h"
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
	constexpr ULONGLONG kStartTimeoutMs = 120'000;  // skse64_loader -> SkyrimSE.exe
	constexpr ULONGLONG kQuitTimeoutMs = 120'000;   // final status -> game process gone
	constexpr ULONGLONG kRelaunchDelayMs = 10'000;  // let the crashed game release its files first
	constexpr wchar_t  kCrashAdvice[] = L"The game closed early. If this keeps happening, turn off the mods suggested above (lighting, effects, physics and NPC replacers first) and press Start again; the bake continues where it stopped.";

	HINSTANCE instance = nullptr;

	struct Ui
	{
		HWND  wnd{};
		HWND  gameGroup{}, gameLabel{}, gameEdit{}, gameBrowse{}, gameInfo{};
		HWND  adviceGroup{}, advice{};
		HWND  setGroup{}, areaLabel{}, areaTrack{}, areaValue{}, hideNpcs{}, settleLabel{}, settleEdit{}, settleSpin{};
		HWND  restartsLabel{}, restartsEdit{}, restartsSpin{}, keep{}, gridLabel{}, gridCombo{}, gridHint{};
		HWND  outGroup{}, outLabel{}, outEdit{}, outBrowse{};
		HWND  start{}, cancel{}, open{}, progress{}, count{}, eta{}, phase{}, log{};
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
	} run;

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
	fs::path DefaultOutput() { return AppFolder() / L"Modern Minimap - My Photos"; }
	fs::path AppSettingsPath() { return AppFolder() / L"Modern Minimap Photo Baker.ini"; }

	float AreaFromTrack() { return static_cast<float>(SendMessageW(ui.areaTrack, TBM_GETPOS, 0, 0)) / 4.0f; }

	int SpinValue(HWND a_spin)
	{
		BOOL      bad = FALSE;
		const int v = static_cast<int>(SendMessageW(a_spin, UDM_GETPOS32, 0, reinterpret_cast<LPARAM>(&bad)));
		return v;
	}

	void UpdateAreaLabel() { SetWindowTextW(ui.areaValue, std::format(L"{:.2f}x", AreaFromTrack()).c_str()); }

	// Photo grid choices, in combo box order: cells per photo side is the index + 1 ([Bake] iPhotoGrid).
	constexpr const wchar_t* kGridChoices[]{ L"1x1 (sharpest)", L"2x2 (default)", L"3x3 (faster)", L"4x4 (fastest)" };
	constexpr int            kDefaultGrid = 2;

	int GridFromCombo()
	{
		const auto index = static_cast<int>(SendMessageW(ui.gridCombo, CB_GETCURSEL, 0, 0));
		return index == CB_ERR ? kDefaultGrid : index + 1;
	}

	void SetGridCombo(int a_grid)
	{
		const int grid = std::clamp(a_grid, 1, static_cast<int>(std::size(kGridChoices)));
		SendMessageW(ui.gridCombo, CB_SETCURSEL, static_cast<WPARAM>(grid - 1), 0);
	}

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
		SetGridCombo(_wtoi(ReadAppValue(L"PhotoGrid", L"2").c_str()));
		SendMessageW(ui.settleSpin, UDM_SETPOS32, 0, std::clamp(_wtoi(ReadAppValue(L"Seconds", L"7").c_str()), 2, 30));
		SendMessageW(ui.restartsSpin, UDM_SETPOS32, 0, std::clamp(_wtoi(ReadAppValue(L"Restarts", L"3").c_str()), 0, 20));
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
		put(L"PhotoGrid", std::to_wstring(GridFromCombo()));
		put(L"Seconds", std::to_wstring(SpinValue(ui.settleSpin)));
		put(L"Restarts", std::to_wstring(SpinValue(ui.restartsSpin)));
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

	void SetRunningUi(bool a_running)
	{
		for (HWND h : { ui.gameEdit, ui.gameBrowse, ui.areaTrack, ui.hideNpcs, ui.settleEdit, ui.settleSpin, ui.restartsEdit, ui.restartsSpin, ui.keep, ui.gridCombo, ui.outEdit,
				 ui.outBrowse, ui.start })
			EnableWindow(h, !a_running);
		EnableWindow(ui.cancel, a_running);
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

	bool Launch()
	{
		std::error_code ec;
		fs::remove(run.status, ec);  // a status left from the last start would point at a game that has gone
		run.game = nullptr;
		run.gamePid = 0;
		run.haveLast = false;
		run.finalSeen = 0;
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
		Log(L"Starting the game through skse64_loader.exe. It opens on the main menu and the bake begins by itself; leave it alone.");
		return true;
	}

	void Finish(Outcome a_outcome, const std::wstring& a_why = {})
	{
		KillTimer(ui.wnd, kTimer);
		if (run.game) {
			CloseHandle(run.game);
			run.game = nullptr;
		}
		run.active = false;

		const auto dest = run.output / "textures" / "ModernMinimap" / "tamriel";
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
		switch (a_outcome) {
		case Outcome::kDone:
			SetWindowTextW(ui.phase, L"Now: finished");
			if (got == 0)
				done = L"No new photos were needed: every photo was already taken by an earlier bake. To bake everything again, delete Data\\SKSE\\Plugins\\ModernMinimapBake.txt "
				       L"(with MO2: in overwrite) and press Start.";
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
		if (have)
			ShowStatus(s);

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
		if (!exited)
			return;

		if (run.game) {
			CloseHandle(run.game);
			run.game = nullptr;
		}
		if (run.cancelRequested) {
			Log(L"The game closed after Cancel.");
			Finish(Outcome::kCancelled);
			return;
		}
		if (!run.crashAdviceShown) {
			run.crashAdviceShown = true;
			Log(kCrashAdvice);
		} else {
			Log(L"The game closed early again.");
		}
		if (run.restartsLeft > 0) {
			--run.restartsLeft;
			Log(std::format(L"Starting it again in 10 seconds ({} automatic restart{} left after this one).", run.restartsLeft, run.restartsLeft == 1 ? L"" : L"s"));
			run.relaunchAt = now + kRelaunchDelayMs;
			SetWindowTextW(ui.phase, L"Now: waiting to start the game again");
		} else {
			Log(L"No automatic restarts left.");
			Finish(Outcome::kFailed, L"The game closed early and every automatic restart is used up.");
		}
	}

	void StartRun()
	{
		if (run.active)
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

		const float area = AreaFromTrack();
		const int   seconds = std::clamp(SpinValue(ui.settleSpin), 2, 30);
		const bool  hide = Button_GetCheck(ui.hideNpcs) == BST_CHECKED;
		const bool  keep = Button_GetCheck(ui.keep) == BST_CHECKED;
		const int   restarts = std::clamp(SpinValue(ui.restartsSpin), 0, 20);
		const int   grid = GridFromCombo();

		run = {};
		run.gameDir = game;
		run.loader = loader;
		run.ini = plugins / "ModernMinimap.ini";
		run.status = plugins / "ModernMinimapBakeStatus.json";
		run.cancel = plugins / "ModernMinimapBake.cancel";
		run.tiles = game / "Data" / "textures" / "ModernMinimap" / "tamriel";
		run.output = out;
		run.restartsLeft = restarts;

		std::wstring error;
		if (!SetIniValues(run.ini, "Bake",
				{ { "bStartOnLoad", "true" }, { "bEnterFromMainMenu", "true" }, { "bQuitWhenDone", "true" }, { "bHideActorsOnNextStart", hide ? "true" : "false" },
					{ "fAreaScale", std::format("{:.2f}", area) }, { "fSettleSeconds", std::format("{}.0", seconds) }, { "bKeepExistingTiles", keep ? "true" : "false" },
					{ "iPhotoGrid", std::to_string(grid) } },
				error)) {
			Log(error);
			return;
		}
		run.switchesOn = true;
		fs::remove(run.cancel, ec);
		GetSystemTimeAsFileTime(&run.start);
		run.active = true;
		SaveAppSettings();
		SetRunningUi(true);
		Log(std::format(L"Bake settings written to {} (area {:.2f}x, {}x{} photo grid, {} s per photo, NPCs {}, existing photos {}).", run.ini.wstring(), area, grid, grid,
			seconds, hide ? L"hidden" : L"shown", keep ? L"kept" : L"replaced"));
		if (!Launch()) {
			Finish(Outcome::kFailed, L"The game could not be started.");
			return;
		}
		SetTimer(ui.wnd, kTimer, 1000, nullptr);
	}

	void CancelRun()
	{
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

		put(ui.setGroup, m, y, inner, 150);
		put(ui.areaLabel, m + 12, y + 28, 125, 20);
		put(ui.areaTrack, m + 140, y + 22, 220, 30);
		put(ui.areaValue, m + 366, y + 28, 60, 20);
		put(ui.hideNpcs, m + 440, y + 26, inner - 452, 22);
		put(ui.settleLabel, m + 12, y + 62, 125, 20);
		put(ui.settleEdit, m + 140, y + 58, 64, 24);
		put(ui.restartsLabel, m + 230, y + 62, 150, 20);
		put(ui.restartsEdit, m + 384, y + 58, 64, 24);
		put(ui.keep, m + 12, y + 90, inner - 24, 22);
		put(ui.gridLabel, m + 12, y + 122, 125, 20);
		put(ui.gridCombo, m + 140, y + 118, 150, 200);  // height includes the drop-down list
		put(ui.gridHint, m + 300, y + 122, inner - 312, 20);
		y += 158;

		put(ui.outGroup, m, y, inner, 62);
		put(ui.outLabel, m + 12, y + 28, 95, 20);
		put(ui.outEdit, m + 110, y + 24, editW, 24);
		put(ui.outBrowse, W - m - 12 - 84, y + 23, 84, 26);
		y += 72;

		put(ui.start, m, y, 100, 30);
		put(ui.cancel, m + 108, y, 100, 30);
		put(ui.open, m + 216, y, 120, 30);
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
		ui.restartsLabel = Make(WC_STATICW, L"Max automatic restarts:", SS_LEFT);
		ui.restartsEdit = Make(WC_EDITW, L"3", WS_TABSTOP | ES_NUMBER, IDC_RESTARTS_EDIT, WS_EX_CLIENTEDGE);
		ui.restartsSpin = Make(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, IDC_RESTARTS_SPIN);
		SendMessageW(ui.restartsSpin, UDM_SETRANGE32, 0, 20);
		ui.keep = Make(WC_BUTTONW, L"Keep existing photos (only fill places that have none yet)", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_KEEP);
		ui.gridLabel = Make(WC_STATICW, L"Photo grid:", SS_LEFT);
		ui.gridCombo = Make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_GRID_COMBO);
		for (const wchar_t* choice : kGridChoices)
			SendMessageW(ui.gridCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
		SetGridCombo(kDefaultGrid);
		ui.gridHint = Make(WC_STATICW, L"Cells per photo: smaller is sharper but slower.", SS_LEFT | SS_ENDELLIPSIS);

		ui.outGroup = Make(WC_BUTTONW, L"Output", BS_GROUPBOX);
		ui.outLabel = Make(WC_STATICW, L"Output folder:", SS_LEFT);
		ui.outEdit = Make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_OUT_EDIT, WS_EX_CLIENTEDGE);
		ui.outBrowse = Make(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, IDC_OUT_BROWSE);

		ui.start = Make(WC_BUTTONW, L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_START);
		ui.cancel = Make(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_CANCEL);
		ui.open = Make(WC_BUTTONW, L"Open folder", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_OPEN);
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
				mm->ptMinTrackSize = { MulDiv(620, static_cast<int>(dpi), 96), MulDiv(720, static_cast<int>(dpi), 96) };
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
			return 0;
		case WM_TIMER:
			if (a_wp == kTimer)
				OnTimer();
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
	RECT        r{ 0, 0, MulDiv(660, static_cast<int>(dpi), 96), MulDiv(850, static_cast<int>(dpi), 96) };
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
