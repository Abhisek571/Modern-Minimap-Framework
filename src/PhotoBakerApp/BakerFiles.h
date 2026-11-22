#pragma once

// Everything the Photo Baker app does with files and processes, kept apart from the window code:
// the plugin's ini, its status file, finding the game, the heavy-mod check and moving the finished tiles.

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PhotoBaker
{
	namespace fs = std::filesystem;

	// Data\SKSE\Plugins\ModernMinimapBakeStatus.json, written by the plugin.
	struct BakeStatus
	{
		std::string state;  // starting | running | writing | done | cancelled | error
		std::string phase;
		std::string message;
		int         found{ -1 };
		int         done{ 0 };
		double      etaSeconds{ 0 };
		DWORD       pid{ 0 };
	};
	bool ReadStatus(const fs::path& a_path, BakeStatus& a_out);

	// Read-modify-write of one ini section in the plugin's simple format: every other line, comment and key
	// stays as it is; missing keys are added at the end of the section (or a new section at the end).
	bool SetIniValues(const fs::path& a_path, std::string_view a_section, const std::vector<std::pair<std::string, std::string>>& a_values, std::wstring& a_error);

	fs::path AppFolder();                          // folder of this exe
	bool     IsGameFolder(const fs::path& a_dir);  // holds SkyrimSE.exe
	fs::path FindGameFolder(std::wstring& a_how);  // empty when not found; a_how says where it was found
	bool     StartedFromMo2();

	// Id of a running process with this exe name, created after a_after (nullptr = any), or 0.
	DWORD FindProcess(const wchar_t* a_exe, const FILETIME* a_after);

	struct MoveResult
	{
		int moved{ 0 };       // copied and removed from the game's Data folder
		int copiedOnly{ 0 };  // copied, but the original could not be removed
		int failed{ 0 };      // not copied
	};
	// Every *.dds in a_from written after a_newerThan, copied to a_to and then removed from a_from.
	MoveResult MoveNewTiles(const fs::path& a_from, const fs::path& a_to, const FILETIME& a_newerThan);

	std::wstring Widen(std::string_view a_utf8);
}
